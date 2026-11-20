
#include <sal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"
#include "core.h"
#include "scaler.h"
#include "scaler_fast.h"

#define MAX_AUDIO_FRAMESKIP 5
#define SRAM_CHECK_FRAMES   60   /* autosave: look for SRAM changes once per second */

static struct MENU_OPTIONS mMenuOptions;
static char mRomName[SAL_MAX_PATH]={""};

static s8 mFpsDisplay[16]={""};
static s8 mVolumeDisplay[16]={""};
static s8 mQuickStateDisplay[16]={""};
static u32 mFps=0;
static u32 mLastTimer=0;
static u32 mEnterMenu=0;
static u32 mQuickStateTimer=0;
static u32 mVolumeDisplayTimer=0;
static u32 mFramesCleared=0;
static u32 mInMenu=0;
static u32 mSkippedFrames=0;
static u32 mSramCheck=0;

extern SDL_Surface *mScreen;

/* While the save state menu previews a state, frames go here (256 wide) */
u16 *gPreviewTarget = NULL;

static bool LastPAL; /* Whether the last frame's height was 239 (true) or 224. */

extern "C"
{

const char *S9xGetFilename (const char *ex)
{
	static char dir [SAL_MAX_PATH];
	char fname [SAL_MAX_PATH];
	char ext [SAL_MAX_PATH];

	sal_DirectorySplitFilename(mRomName, dir, fname, ext);
	strcpy(dir, sal_DirectoryGetHome());
	sal_DirectoryCombine(dir,fname);
	strcat (dir, ex);

	return (dir);
}

void PSNESForceSaveSRAM (void)
{
	if(mRomName[0] != 0)
		CoreSramSave(S9xGetFilename (".srm"));
}

void S9xSaveSRAM (int showWarning)
{
	int r = CoreSramSave(S9xGetFilename (".srm"));
	if (r < 0)
		MenuMessageBox("Saving SRAM","Failed!","",MENU_MESSAGE_BOX_MODE_PAUSE);
	else if (r == 0 && showWarning)
		MenuMessageBox("SRAM saving ignored","No changes have been made to SRAM","",MENU_MESSAGE_BOX_MODE_MSG);
}

/* ---------------------------------------------------------------- */
/* Core -> frontend                                                  */

uint32_t CoreFrontendInput(void)
{
	if (mInMenu) return 0;

	u32 joy = sal_InputPoll();

	if (((joy & SAL_INPUT_SELECT) && (joy & SAL_INPUT_START))
	 || (joy & SAL_INPUT_MENU))
	{
		mEnterMenu = 1;
		return 0;
	}
	return joy;
}

void CoreFrontendVideo(const uint16_t *pixels, int pal, unsigned pitch)
{
	u32 h = pal ? CORE_HEIGHT_EXTENDED : CORE_HEIGHT;

	if (gPreviewTarget)
	{
		for (u32 y = 0; y < h; y++)
			memcpy(gPreviewTarget + y * CORE_WIDTH, pixels + y * pitch, CORE_WIDTH * sizeof(u16));
		return;
	}
	if (mInMenu) return;

	// After returning from the menu, clear the background of 3 frames.
	// This prevents remnants of the menu from appearing.
	if (mFramesCleared < 3)
	{
		sal_VideoClear(0);
		mFramesCleared++;
	}

	// If the height changed from 224 to 239, or from 239 to 224,
	// possibly change the resolution.
	if ((bool) pal != LastPAL)
	{
		sal_VideoSetPAL(mMenuOptions.fullScreen, pal);
		LastPAL = pal;
	}

	uint32_t *src = (uint32_t *) pixels;
	switch (mMenuOptions.fullScreen)
	{
		case 0: /* No scaling */
		case 3: /* Hardware scaling: the video mode is 256 x h */
		{
			u32 y, dpitch = sal_VideoGetPitch();
			const u8 *s = (const u8 *) pixels;
			u8 *dst = (u8*) sal_VideoGetBuffer()
				+ ((sal_VideoGetWidth() - CORE_WIDTH) / 2) * sizeof(u16)
				+ ((sal_VideoGetHeight() - h) / 2) * dpitch;
			for (y = 0; y < h; y++)
			{
				memcpy(dst, s, CORE_WIDTH * sizeof(u16));
				s += pitch * sizeof(u16);
				dst += dpitch;
			}
			break;
		}
		case 1: /* Fast software scaling */
			if (pal)
				upscale_256x240_to_320x240((uint32_t*) sal_VideoGetBuffer(), src, pitch);
			else
				upscale_256x224_to_320x240((uint32_t*) sal_VideoGetBuffer(), src, pitch);
			break;

		case 2: /* Smooth software scaling (NEON, bit-exact with the C version) */
		{
			u32 dpitch = sal_VideoGetPitch() / sizeof(u16);
			if (pal)
				scale_smooth_239((uint16_t*) sal_VideoGetBuffer() + dpitch, dpitch, pixels, pitch);
			else
				/* 14 x 17 = 238 lines, one line down as the original */
				scale_smooth_224((uint16_t*) sal_VideoGetBuffer() + dpitch, dpitch, pixels, pitch);
			break;
		}
	}

	if (mMenuOptions.showFps)
	{
		mFps++;
		u32 newTimer=sal_TimerRead();
		if(newTimer-mLastTimer>CoreFps())
		{
			mLastTimer=newTimer;
			sprintf(mFpsDisplay,"%2d/%2d", mFps, CoreFps());
			mFps=0;
		}

		sal_VideoDrawRect(0,0,5*8,8,SAL_RGB(0,0,0));
		sal_VideoPrint(0,0,mFpsDisplay,SAL_RGB(31,31,31));
	}

	if(mVolumeDisplayTimer>0)
	{
		sal_VideoDrawRect(100,0,8*8,8,SAL_RGB(0,0,0));
		sal_VideoPrint(100,0,mVolumeDisplay,SAL_RGB(31,31,31));
	}

	if(mQuickStateTimer>0)
	{
		sal_VideoDrawRect(200,0,8*8,8,SAL_RGB(0,0,0));
		sal_VideoPrint(200,0,mQuickStateDisplay,SAL_RGB(31,31,31));
	}

	sal_VideoFlip(0);
}

/* Linear resampler, core rate (32040 Hz) -> menu sound rate, stereo or mono.
 * 16.16 fixed point; keeps the last input frame between calls. */
static u32 mResampleStep = 0;   /* input frames per output frame, 16.16 */
static u32 mResamplePos = 0;    /* position between mPrev and the next input, 16.16 */
static s32 mPrevL = 0, mPrevR = 0;
static s16 mResampleOut[4096 * 2];

static void ResampleInit(u32 inRate, u32 outRate)
{
	mResampleStep = (u32) (((uint64_t) inRate << 16) / outRate);
	mResamplePos = 0;
	mPrevL = mPrevR = 0;
}

void CoreFrontendAudio(const int16_t *in, unsigned frames)
{
	if (gPreviewTarget || mResampleStep == 0) return;

	const int stereo = mMenuOptions.stereo;
	u32 out = 0, i = 0;
	const u32 maxOut = sizeof(mResampleOut) / sizeof(mResampleOut[0]) / 2;

	while (i < frames)
	{
		s32 curL = in[i * 2], curR = in[i * 2 + 1];
		/* emit every output sample that falls between prev and cur */
		while (mResamplePos < 0x10000 && out < maxOut)
		{
			s32 f = mResamplePos;
			s32 l = mPrevL + (((curL - mPrevL) * f) >> 16);
			s32 r = mPrevR + (((curR - mPrevR) * f) >> 16);
			if (stereo)
			{
				mResampleOut[out * 2] = (s16) l;
				mResampleOut[out * 2 + 1] = (s16) r;
			}
			else
				mResampleOut[out] = (s16) ((l + r) >> 1);
			out++;
			mResamplePos += mResampleStep;
		}
		mResamplePos -= 0x10000;
		mPrevL = curL;
		mPrevR = curR;
		i++;
	}
	sal_AudioWrite(mResampleOut, out);
}

}

/* ---------------------------------------------------------------- */

static int RenderThisFrame(void)
{
	if (mMenuOptions.frameSkip == 0) /* auto */
	{
		if (sal_AudioGetFramesBuffered() < sal_AudioGetMinFrames()
		 && ++mSkippedFrames < MAX_AUDIO_FRAMESKIP)
			return 0;
		mSkippedFrames = 0;
		return 1;
	}
	/* frameSkip 1 = no skip, 2 = render 1 of 2, ... */
	if (++mSkippedFrames >= mMenuOptions.frameSkip)
	{
		mSkippedFrames = 0;
		return 1;
	}
	return 0;
}

static u32 LastAudioRate = 0;
static u32 LastStereo = 0;
static u32 LastHz = 0;

static
int Run(int sound)
{
	bool PAL = CoreIsPal();

	sal_VideoEnterGame(mMenuOptions.fullScreen, PAL, CoreFps());
	LastPAL = PAL;
	sal_TimerInit(1000000 / CoreFps());

	/* The audio device is opened even with sound disabled (muted): its
	 * clock is what paces the emulation. */
	if (LastAudioRate != mMenuOptions.soundRate || LastStereo != mMenuOptions.stereo || LastHz != CoreFps())
	{
		if (LastAudioRate != 0)
			sal_AudioClose();
		sal_AudioInit(mMenuOptions.soundRate, 16, mMenuOptions.stereo, CoreFps());
		LastAudioRate = mMenuOptions.soundRate;
		LastStereo = mMenuOptions.stereo;
		LastHz = CoreFps();
	}
	ResampleInit(CoreAudioRate(), mMenuOptions.soundRate);
	sal_AudioSetMuted(!sound);
	sal_AudioResume();

	while(!mEnterMenu)
	{
		CoreRunFrame(RenderThisFrame());

		if (mMenuOptions.autoSaveSram && ++mSramCheck >= SRAM_CHECK_FRAMES)
		{
			mSramCheck = 0;
			CoreSramSave(S9xGetFilename (".srm"));
		}

		sal_AudioWaitSpace();
	}

	sal_AudioPause();

	sal_VideoExitGame();

	mEnterMenu=0;
	return mEnterMenu;
}

static
int SnesRomLoad()
{
	MenuMessageBox("Loading ROM...",mRomName,"",MENU_MESSAGE_BOX_MODE_MSG);
	if (!CoreLoadRom(mRomName))
	{
		MenuMessageBox("Loading ROM",mRomName,"Failed!",MENU_MESSAGE_BOX_MODE_PAUSE);
		return SAL_ERROR;
	}
	MenuMessageBox("Done loading the ROM",mRomName,"",MENU_MESSAGE_BOX_MODE_MSG);

	CoreSramLoad(S9xGetFilename (".srm"));
	CoreCheatsLoad(S9xGetFilename (".cht"));
	return SAL_OK;
}

extern "C"
{

int mainEntry(int argc, char* argv[])
{
	s32 event=EVENT_NONE;

	sal_Init();
	sal_VideoInit(16);

	mRomName[0]=0;
	if (argc >= 2)
		strcpy(mRomName, argv[1]); // Record ROM name

	MenuInit(sal_DirectoryGetHome(), &mMenuOptions);

	CoreInit();

	while(1)
	{
		mInMenu=1;
		event=MenuRun(mRomName);
		mInMenu=0;

		if(event==EVENT_LOAD_ROM)
		{
			if (mRomName[0] != 0)
			{
				MenuMessageBox("Saving SRAM...","","",MENU_MESSAGE_BOX_MODE_MSG);
				PSNESForceSaveSRAM();
			}
			if(SnesRomLoad() == SAL_ERROR)
			{
				mRomName[0] = 0;
				MenuMessageBox("Failed to load ROM",mRomName,"Press any button to continue", MENU_MESSAGE_BOX_MODE_PAUSE);
				sal_Reset();
				return 0;
			}
			else
			{
				MenuMessageBox("Loading game settings...",mRomName,"",MENU_MESSAGE_BOX_MODE_MSG);
				LoadCurrentOptions();

				event=EVENT_RUN_ROM;
			}
		}

		if(event==EVENT_RESET_ROM)
		{
			CoreReset();
			event=EVENT_RUN_ROM;
		}

		if(event==EVENT_RUN_ROM)
		{
			sal_AudioSetVolume(mMenuOptions.volume,mMenuOptions.volume);
			sal_CpuSpeedSet(mMenuOptions.cpuSpeed);
			mFramesCleared = 0;
			Run(mMenuOptions.soundEnabled);
			event=EVENT_NONE;
		}

		if(event==EVENT_EXIT_APP) break;
	}

	MenuMessageBox("Saving SRAM...","","",MENU_MESSAGE_BOX_MODE_MSG);
	PSNESForceSaveSRAM();

	CoreDeinit();

	sal_Reset();
	return 0;
}

}
