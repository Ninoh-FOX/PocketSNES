/* PocketSNES <-> snes9x2005 bridge, see core.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "libretro.h"
#include "unzip.h"
#include "sal.h"
#include "core.h"

#define AUDIO_VIDEO_ENABLE_VIDEO 1
#define AUDIO_VIDEO_ENABLE_AUDIO 2

static int mRender = 1;
static int mPal = 0;
static int mLoaded = 0;
static double mFps = 60.0;
static unsigned mAudioRate = 32040;

static uint8_t *mRomData = NULL;
static size_t mRomSize = 0;

static uint8_t *mSramShadow = NULL;   /* last saved copy, to detect changes */
static size_t mSramShadowSize = 0;

/* Downscaled/cropped frame handed to the frontend (256 x up to 239) */
static uint16_t mFrame[CORE_WIDTH * CORE_HEIGHT_EXTENDED];

/* ---------------------------------------------------------------- */
/* libretro callbacks                                                */

static bool environ_cb(unsigned cmd, void *data)
{
	switch (cmd)
	{
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(const enum retro_pixel_format *) data == RETRO_PIXEL_FORMAT_RGB565;

	case RETRO_ENVIRONMENT_GET_VARIABLE:
	{
		struct retro_variable *var = (struct retro_variable *) data;
		var->value = NULL;
		/* Frameskip is done by the frontend through GET_AUDIO_VIDEO_ENABLE,
		 * the rest are the core defaults. */
		if (!strcmp(var->key, "snes9x_2005_region"))                 var->value = "auto";
		else if (!strcmp(var->key, "snes9x_2005_frameskip"))         var->value = "disabled";
		else if (!strcmp(var->key, "snes9x_2005_low_pass_filter"))   var->value = "disabled";
		else if (!strcmp(var->key, "snes9x_2005_overclock_cycles"))  var->value = "disabled";
		else if (!strcmp(var->key, "snes9x_2005_reduce_sprite_flicker")) var->value = "disabled";
		return var->value != NULL;
	}

	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		*(int *) data = (mRender ? AUDIO_VIDEO_ENABLE_VIDEO : 0) | AUDIO_VIDEO_ENABLE_AUDIO;
		return true;

	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *) data = false;
		return true;

	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;

	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
		return true;

	default:
		return false;
	}
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
	const uint8_t *src = (const uint8_t *) data;
	unsigned y, x, outh;
	int interlaced;

	if (!data)
		return; /* skipped frame */

	/* hi-res (512) and interlaced (448/478) frames are reduced to 256 x 224/239 */
	interlaced = height > CORE_HEIGHT_EXTENDED;
	outh = interlaced ? height / 2 : height;
	if (outh > CORE_HEIGHT_EXTENDED)
		outh = CORE_HEIGHT_EXTENDED;
	mPal = outh > CORE_HEIGHT;

	/* Normal frames go straight from the core buffer, no copy */
	if (width == CORE_WIDTH && !interlaced)
	{
		CoreFrontendVideo((const uint16_t *) data, mPal, (unsigned) (pitch / sizeof(uint16_t)));
		return;
	}

	for (y = 0; y < outh; y++)
	{
		const uint16_t *s = (const uint16_t *) (src + (interlaced ? y * 2 : y) * pitch);
		uint16_t *d = &mFrame[y * CORE_WIDTH];

		if (width == 512)
		{
			/* average each pixel pair (RGB565, no carry between fields) */
			for (x = 0; x < CORE_WIDTH; x++)
			{
				uint32_t a = s[x * 2], b = s[x * 2 + 1];
				d[x] = (uint16_t) (((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1) + (a & b & 0x0821));
			}
		}
		else
			memcpy(d, s, CORE_WIDTH * sizeof(uint16_t));
	}

	CoreFrontendVideo(mFrame, mPal, CORE_WIDTH);
}

static void audio_cb(int16_t left, int16_t right)
{
	int16_t s[2] = { left, right };
	CoreFrontendAudio(s, 1);
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	CoreFrontendAudio(data, (unsigned) frames);
	return frames;
}

static uint32_t mInput = 0;

static void input_poll_cb(void)
{
	mInput = CoreFrontendInput();
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	int16_t mask = 0;
	(void) index;

	if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD)
		return 0;

	if (mInput & SAL_INPUT_B)      mask |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
	if (mInput & SAL_INPUT_Y)      mask |= 1 << RETRO_DEVICE_ID_JOYPAD_Y;
	if (mInput & SAL_INPUT_SELECT) mask |= 1 << RETRO_DEVICE_ID_JOYPAD_SELECT;
	if (mInput & SAL_INPUT_START)  mask |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
	if (mInput & SAL_INPUT_UP)     mask |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
	if (mInput & SAL_INPUT_DOWN)   mask |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
	if (mInput & SAL_INPUT_LEFT)   mask |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
	if (mInput & SAL_INPUT_RIGHT)  mask |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	if (mInput & SAL_INPUT_A)      mask |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
	if (mInput & SAL_INPUT_X)      mask |= 1 << RETRO_DEVICE_ID_JOYPAD_X;
	if (mInput & SAL_INPUT_L)      mask |= 1 << RETRO_DEVICE_ID_JOYPAD_L;
	if (mInput & SAL_INPUT_R)      mask |= 1 << RETRO_DEVICE_ID_JOYPAD_R;

	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return mask;
	return (mask >> id) & 1;
}

/* ---------------------------------------------------------------- */

int CoreInit(void)
{
	retro_set_environment(environ_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);
	retro_init();
	return 1;
}

void CoreUnloadRom(void)
{
	if (mLoaded)
		retro_unload_game();
	mLoaded = 0;
	free(mRomData);
	mRomData = NULL;
	mRomSize = 0;
	free(mSramShadow);
	mSramShadow = NULL;
	mSramShadowSize = 0;
}

void CoreDeinit(void)
{
	CoreUnloadRom();
	retro_deinit();
}

static int HasExt(const char *name, const char *ext)
{
	size_t n = strlen(name), e = strlen(ext);
	return n > e && !strcasecmp(name + n - e, ext);
}

static int IsRomName(const char *name)
{
	return HasExt(name, ".smc") || HasExt(name, ".sfc") || HasExt(name, ".fig")
	    || HasExt(name, ".swc") || HasExt(name, ".bin");
}

static int ReadZip(const char *path)
{
	unzFile zip = unzOpen(path);
	unz_file_info info;
	char name[SAL_MAX_PATH];
	int ok = 0, r;

	if (!zip)
		return 0;
	for (r = unzGoToFirstFile(zip); r == UNZ_OK; r = unzGoToNextFile(zip))
	{
		if (unzGetCurrentFileInfo(zip, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
			break;
		if (!IsRomName(name) || info.uncompressed_size == 0)
			continue;
		mRomData = (uint8_t *) malloc(info.uncompressed_size);
		if (mRomData && unzOpenCurrentFile(zip) == UNZ_OK)
		{
			mRomSize = info.uncompressed_size;
			ok = unzReadCurrentFile(zip, mRomData, mRomSize) == (int) mRomSize;
			unzCloseCurrentFile(zip);
		}
		break;
	}
	unzClose(zip);
	return ok;
}

static int ReadFile(const char *path)
{
	FILE *f = fopen(path, "rb");
	long size;
	int ok = 0;

	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size > 0 && (mRomData = (uint8_t *) malloc(size)))
	{
		mRomSize = size;
		ok = fread(mRomData, 1, mRomSize, f) == mRomSize;
	}
	fclose(f);
	return ok;
}

int CoreLoadRom(const char *path)
{
	struct retro_game_info game;
	struct retro_system_av_info av;

	CoreUnloadRom();

	if (!(HasExt(path, ".zip") ? ReadZip(path) : ReadFile(path)))
	{
		free(mRomData);
		mRomData = NULL;
		return 0;
	}

	memset(&game, 0, sizeof(game));
	game.path = path;
	game.data = mRomData;
	game.size = mRomSize;
	if (!retro_load_game(&game))
	{
		free(mRomData);
		mRomData = NULL;
		return 0;
	}
	mLoaded = 1;

	retro_get_system_av_info(&av);
	mFps = av.timing.fps;
	mAudioRate = (unsigned) (av.timing.sample_rate + 0.5);
	mPal = mFps < 55.0;
	return 1;
}

void CoreReset(void)
{
	retro_reset();
}

void CoreRunFrame(int render)
{
	mRender = render;
	retro_run();
}

unsigned CoreFps(void)        { return mFps < 55.0 ? 50 : 60; }
double CoreFpsExact(void)     { return mFps; }
unsigned CoreAudioRate(void)  { return mAudioRate; }
int CoreIsPal(void)           { return mPal; }

/* ---------------------------------------------------------------- */
/* Battery RAM                                                       */

static void SramSnapshot(void)
{
	size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	uint8_t *data = (uint8_t *) retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!size || !data)
		return;
	if (mSramShadowSize != size)
	{
		free(mSramShadow);
		mSramShadow = (uint8_t *) malloc(size);
		mSramShadowSize = mSramShadow ? size : 0;
	}
	if (mSramShadow)
		memcpy(mSramShadow, data, size);
}

int CoreSramLoad(const char *path)
{
	size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	uint8_t *data = (uint8_t *) retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	FILE *f;
	int ok = 0;

	if (size && data && (f = fopen(path, "rb")))
	{
		ok = fread(data, 1, size, f) > 0;
		fclose(f);
	}
	SramSnapshot();
	return ok;
}

int CoreSramChanged(void)
{
	size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	uint8_t *data = (uint8_t *) retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!size || !data)
		return 0;
	return !mSramShadow || mSramShadowSize != size || memcmp(mSramShadow, data, size) != 0;
}

int CoreSramSave(const char *path)
{
	size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	uint8_t *data = (uint8_t *) retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	FILE *f;
	int ok;

	if (!mLoaded || !CoreSramChanged())
		return 0;
	if (!(f = fopen(path, "wb")))
		return -1;
	ok = fwrite(data, 1, size, f) == size;
	fclose(f);
	if (!ok)
		return -1;
	SramSnapshot();
	return 1;
}

/* ---------------------------------------------------------------- */
/* Save states                                                       */

int CoreStateSave(const char *path)
{
	size_t size = retro_serialize_size();
	void *buf = malloc(size);
	FILE *f;
	int ok = 0;

	if (!buf)
		return 0;
	if (retro_serialize(buf, size) && (f = fopen(path, "wb")))
	{
		ok = fwrite(buf, 1, size, f) == size;
		ok = (fclose(f) == 0) && ok;
	}
	free(buf);
	return ok;
}

int CoreStateLoad(const char *path)
{
	size_t size = retro_serialize_size();
	void *buf = malloc(size);
	FILE *f;
	int ok = 0;

	if (!buf)
		return 0;
	if ((f = fopen(path, "rb")))
	{
		ok = fread(buf, 1, size, f) == size;
		fclose(f);
	}
	if (ok)
		ok = retro_unserialize(buf, size);
	free(buf);
	return ok;
}

/* ---------------------------------------------------------------- */
/* Cheats                                                            */

struct CHEAT
{
	char code[9];     /* Pro Action Replay: AAAAAAVV */
	char name[CORE_CHEAT_NAME];
	int enabled;
};

static struct CHEAT mCheats[CORE_MAX_CHEATS];
static unsigned mCheatCount = 0;

static void CheatsApply(void)
{
	unsigned i;
	/* reset restores the original bytes of the enabled ones */
	retro_cheat_reset();
	for (i = 0; i < mCheatCount; i++)
		retro_cheat_set(i, mCheats[i].enabled ? true : false, mCheats[i].code);
}

int CoreCheatsLoad(const char *path)
{
	char line[256];
	FILE *f;

	mCheatCount = 0;
	retro_cheat_reset();
	if (!(f = fopen(path, "rb")))
		return 0;

	while (fgets(line, sizeof(line), f) && mCheatCount < CORE_MAX_CHEATS)
	{
		struct CHEAT *c = &mCheats[mCheatCount];
		char hex[16];
		char *p, *name;
		int n = 0;

		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || line[0] == 0)
			continue;

		name = strchr(line, '#');
		if (name)
			*name++ = 0;

		/* "7e1f8a-1e-03": address (6), value (2), original value (2, unused) */
		for (p = line; *p && n < 8; p++)
			if (isxdigit((unsigned char) *p))
				hex[n++] = (char) toupper((unsigned char) *p);
		if (n < 8)
			continue;
		hex[8] = 0;

		memcpy(c->code, hex, 9);
		while (name && *name == ' ')
			name++;
		snprintf(c->name, sizeof(c->name), "%s", name && *name ? name : c->code);
		c->enabled = 0;
		mCheatCount++;
	}
	fclose(f);
	CheatsApply();
	return mCheatCount > 0;
}

unsigned CoreCheatCount(void)          { return mCheatCount; }
const char *CoreCheatName(unsigned i)  { return i < mCheatCount ? mCheats[i].name : ""; }
int CoreCheatEnabled(unsigned i)       { return i < mCheatCount && mCheats[i].enabled; }

void CoreCheatToggle(unsigned i)
{
	if (i >= mCheatCount)
		return;
	mCheats[i].enabled = !mCheats[i].enabled;
	CheatsApply();
}
