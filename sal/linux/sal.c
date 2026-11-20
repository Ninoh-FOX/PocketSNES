#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <SDL.h>
#include <sys/time.h>
#include "sal.h"

#define PALETTE_BUFFER_LENGTH	256*2*4
#define SNES_WIDTH  256
#define SNES_HEIGHT 239

SDL_Surface *mScreen = NULL;
/* The menu is drawn into this RAM copy and written to the screen in one pass
 * at flip time. Drawing it straight on the triple-buffered framebuffer
 * (background first, then the text) could show a page between the two steps,
 * which made the menu text flicker. The game (direct mode) writes each pixel
 * once, so it keeps drawing straight to the screen with no extra copy. */
static u8 *mShadow = NULL;
static u32 mShadowSize = 0;
static int mDirect = 0;

static void ShadowUpdate(void)
{
	u32 size = mScreen ? mScreen->pitch * mScreen->h : 0;
	if (size > mShadowSize)
	{
		free(mShadow);
		mShadow = (u8 *) malloc(size);
		mShadowSize = mShadow ? size : 0;
	}
	if (mShadow && mScreen)
		memcpy(mShadow, mScreen->pixels, size);
}
static u32 mSoundThreadFlag=0;
static u32 mSoundLastCpuSpeed=0;
static u32 mPaletteBuffer[PALETTE_BUFFER_LENGTH];
static u32 *mPaletteCurr=(u32*)&mPaletteBuffer[0];
static u32 *mPaletteLast=(u32*)&mPaletteBuffer[0];
static u32 *mPaletteEnd=(u32*)&mPaletteBuffer[PALETTE_BUFFER_LENGTH];
static u32 mInputFirst=0;

s32 mCpuSpeedLookup[1]={0};

#include <sal_common.h>

static u32 inputHeld = 0;

static u32 sal_Input(int held)
{
	SDL_Event event;
	if (!SDL_PollEvent(&event)) {
		if (held) return inputHeld;
		return 0;
	}

	u32 extraKeys = 0;
	do {
		switch (event.type) {
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym) {
#ifdef RG350
			case SDLK_HOME:
#endif				
#if defined(PG) && !defined(MIYOOMINI)
			case SDLK_RCTRL:
#endif
#ifdef MIYOOMINI
			case SDLK_ESCAPE: /* MENU */
#endif
				extraKeys |= SAL_INPUT_MENU;
				break;
			}
			break;
		}
	} while(SDL_PollEvent(&event));

	inputHeld = 0;

	u8 *keystate = SDL_GetKeyState(NULL);
#ifdef MIYOOMINI
	/* Miyoo Mini keys (Koriki SDL 1.2), same face layout as the SNES pad */
	if ( keystate[SDLK_SPACE] )		inputHeld |= SAL_INPUT_A;	/* A */
	if ( keystate[SDLK_LCTRL] )		inputHeld |= SAL_INPUT_B;	/* B */
	if ( keystate[SDLK_LSHIFT] )	inputHeld |= SAL_INPUT_X;	/* X */
	if ( keystate[SDLK_LALT] )		inputHeld |= SAL_INPUT_Y;	/* Y */
	if ( keystate[SDLK_e] )			inputHeld |= SAL_INPUT_L;	/* L1 */
	if ( keystate[SDLK_t] )			inputHeld |= SAL_INPUT_R;	/* R1 */
	if ( keystate[SDLK_TAB] )		inputHeld |= SAL_INPUT_L;	/* L2 = L */
	if ( keystate[SDLK_BACKSPACE] )	inputHeld |= SAL_INPUT_R;	/* R2 = R */
	if ( keystate[SDLK_RETURN] )	inputHeld |= SAL_INPUT_START;	/* START */
	if ( keystate[SDLK_RCTRL] )		inputHeld |= SAL_INPUT_SELECT;	/* SELECT */
	if ( keystate[SDLK_UP] )		inputHeld |= SAL_INPUT_UP;	/* UP */
	if ( keystate[SDLK_DOWN] )		inputHeld |= SAL_INPUT_DOWN;	/* DOWN */
	if ( keystate[SDLK_LEFT] )		inputHeld |= SAL_INPUT_LEFT;	/* LEFT */
	if ( keystate[SDLK_RIGHT] )		inputHeld |= SAL_INPUT_RIGHT;	/* RIGHT */
	if ( keystate[SDLK_ESCAPE] )	inputHeld |= SAL_INPUT_MENU;	/* MENU */
#else
	if ( keystate[SDLK_LCTRL] )		inputHeld |= SAL_INPUT_A;
	if ( keystate[SDLK_LALT] )		inputHeld |= SAL_INPUT_B;
	if ( keystate[SDLK_SPACE] )		inputHeld |= SAL_INPUT_X;
	if ( keystate[SDLK_LSHIFT] )	inputHeld |= SAL_INPUT_Y;
	if ( keystate[SDLK_TAB] )		inputHeld |= SAL_INPUT_L;
	if ( keystate[SDLK_BACKSPACE] )	inputHeld |= SAL_INPUT_R;
	if ( keystate[SDLK_RETURN] )	inputHeld |= SAL_INPUT_START;
	if ( keystate[SDLK_ESCAPE] )	inputHeld |= SAL_INPUT_SELECT;
	if ( keystate[SDLK_UP] )		inputHeld |= SAL_INPUT_UP;
	if ( keystate[SDLK_DOWN] )		inputHeld |= SAL_INPUT_DOWN;
	if ( keystate[SDLK_LEFT] )		inputHeld |= SAL_INPUT_LEFT;
	if ( keystate[SDLK_RIGHT] )		inputHeld |= SAL_INPUT_RIGHT;
#ifdef RG350
	if ( keystate[SDLK_HOME] )		inputHeld |= SAL_INPUT_MENU;
#endif
#ifdef PG
	if ( keystate[SDLK_RCTRL] )		inputHeld |= SAL_INPUT_MENU;
#endif
#endif /* MIYOOMINI */
	mInputRepeat = inputHeld;
	return inputHeld | extraKeys;
}

static int key_repeat_enabled = 1;

u32 sal_InputPollRepeat()
{
	if (!key_repeat_enabled) {
		SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
		key_repeat_enabled = 1;
	}
	return sal_Input(0);
}

u32 sal_InputPoll()
{
	if (key_repeat_enabled) {
		SDL_EnableKeyRepeat(0, 0);
		key_repeat_enabled = 0;
	}
	return sal_Input(1);
}

const char* sal_DirectoryGetTemp(void)
{
	return "/tmp";
}

void sal_CpuSpeedSet(u32 mhz)
{
}

u32 sal_CpuSpeedNext(u32 currSpeed)
{
	u32 newSpeed=currSpeed+1;
	if(newSpeed > 500) newSpeed = 500;
	return newSpeed;
}

u32 sal_CpuSpeedPrevious(u32 currSpeed)
{
	u32 newSpeed=currSpeed-1;
	if(newSpeed > 500) newSpeed = 0;
	return newSpeed;
}

u32 sal_CpuSpeedNextFast(u32 currSpeed)
{
	u32 newSpeed=currSpeed+10;
	if(newSpeed > 500) newSpeed = 500;
	return newSpeed;
}

u32 sal_CpuSpeedPreviousFast(u32 currSpeed)
{
	u32 newSpeed=currSpeed-10;
	if(newSpeed > 500) newSpeed = 0;
	return newSpeed;
}

s32 sal_Init(void)
{
	setenv("SDL_NOMOUSE", "1", 1);
	if( SDL_Init( SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_JOYSTICK ) == -1 )
	{
		return SAL_ERROR;
	}
	sal_TimerInit(60);

	memset(mInputRepeatTimer,0,sizeof(mInputRepeatTimer));

	SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);

	return SAL_OK;
}

u32 sal_VideoInit(u32 bpp)
{
	SDL_ShowCursor(0);
	
	mBpp=bpp;

	//Set up the screen
	mScreen = SDL_SetVideoMode(SAL_SCREEN_WIDTH, SAL_SCREEN_HEIGHT, bpp, SDL_HWSURFACE | 
#ifdef SDL_TRIPLEBUF
	SDL_TRIPLEBUF
#else
	SDL_DOUBLEBUF
#endif
	);

	//If there was an error in setting up the screen
	if( mScreen == NULL )
	{
	sal_LastErrorSet("SDL_SetVideoMode failed");
	return SAL_ERROR;
	}

	mDirect = 0;
	ShadowUpdate();
	return SAL_OK;
}

u32 sal_VideoGetWidth()
{
	return mScreen->w;
}

u32 sal_VideoGetHeight()
{
	return mScreen->h;
}

u32 sal_VideoGetPitch()
{
	return mScreen->pitch;
}

void sal_VideoEnterGame(u32 fullscreenOption, u32 pal, u32 refreshRate)
{
#ifdef GCW_ZERO
	/* Copied from C++ headers which we can't include in C */
	unsigned int Width = 256 /* SNES_WIDTH */,
	             Height = pal ? 239 /* SNES_HEIGHT_EXTENDED */ : 224 /* SNES_HEIGHT */;
	if (fullscreenOption != 3)
	{
		Width = SAL_SCREEN_WIDTH;
		Height = SAL_SCREEN_HEIGHT;
	}
	if (SDL_MUSTLOCK(mScreen)) SDL_UnlockSurface(mScreen);
	mScreen = SDL_SetVideoMode(Width, Height, mBpp, SDL_HWSURFACE |
#ifdef SDL_TRIPLEBUF
		SDL_TRIPLEBUF
#else
		SDL_DOUBLEBUF
#endif
		);
	mRefreshRate = refreshRate;
	if (SDL_MUSTLOCK(mScreen)) SDL_LockSurface(mScreen);
#endif
	mDirect = 1;
}

void sal_VideoSetPAL(u32 fullscreenOption, u32 pal)
{
	if (fullscreenOption == 3) /* hardware scaling */
	{
		sal_VideoEnterGame(fullscreenOption, pal, mRefreshRate);
	}
}

void sal_VideoExitGame()
{
#ifdef GCW_ZERO
	if (SDL_MUSTLOCK(mScreen)) SDL_UnlockSurface(mScreen);
	mScreen = SDL_SetVideoMode(SAL_SCREEN_WIDTH, SAL_SCREEN_HEIGHT, mBpp, SDL_HWSURFACE |
#ifdef SDL_TRIPLEBUF
		SDL_TRIPLEBUF
#else
		SDL_DOUBLEBUF
#endif
		);
	if (SDL_MUSTLOCK(mScreen)) SDL_LockSurface(mScreen);
#endif
	mDirect = 0;
	ShadowUpdate();
}

void sal_VideoBitmapDim(u16* img, u32 pixelCount)
{
	u32 i;
	for (i = 0; i < pixelCount; i += 2)
		*(u32 *) &img[i] = (*(u32 *) &img[i] & 0xF7DEF7DE) >> 1;
	if (pixelCount & 1)
		img[i - 1] = (img[i - 1] & 0xF7DE) >> 1;
}

void sal_VideoFlip(s32 vsync)
{
	if (!mDirect && mShadow)
		memcpy(mScreen->pixels, mShadow, mScreen->pitch * mScreen->h);
	if (SDL_MUSTLOCK(mScreen)) SDL_UnlockSurface(mScreen);
	SDL_Flip(mScreen);
	if (SDL_MUSTLOCK(mScreen)) SDL_LockSurface(mScreen);
}

void *sal_VideoGetBuffer()
{
	if (!mDirect && mShadow)
		return (void*)mShadow;
	return (void*)mScreen->pixels;
}

void sal_VideoPaletteSync() 
{ 	
	
} 

void sal_VideoPaletteSet(u32 index, u32 color)
{
	*mPaletteCurr++=index;
	*mPaletteCurr++=color;
	if(mPaletteCurr>mPaletteEnd) mPaletteCurr=&mPaletteBuffer[0];
}

void sal_Reset(void)
{
	sal_AudioClose();
	SDL_Quit();
}

int mainEntry(int argc, char *argv[]);

// Prove entry point wrapper
int main(int argc, char *argv[])
{	
	return mainEntry(argc,argv);
//	return mainEntry(argc-1,&argv[1]);
}
