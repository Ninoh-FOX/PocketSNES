/* PocketSNES <-> snes9x2005 (libretro API) bridge.
 * The core is linked statically and driven only through libretro.h, so it
 * can be updated without touching the frontend. */
#ifndef POCKETSNES_CORE_H
#define POCKETSNES_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CORE_WIDTH            256
#define CORE_HEIGHT           224
#define CORE_HEIGHT_EXTENDED  239
#define CORE_MAX_CHEATS       800
#define CORE_CHEAT_NAME       48

/* What the frontend has to provide */
uint32_t CoreFrontendInput(void);          /* SAL_INPUT_* bits, polled once per frame */
void CoreFrontendVideo(const uint16_t *pixels, int pal, unsigned pitch); /* 256 x (224|239), pitch in pixels */
void CoreFrontendAudio(const int16_t *stereo, unsigned frames); /* at CORE_AUDIO_RATE */

int  CoreInit(void);
void CoreDeinit(void);
int  CoreLoadRom(const char *path);        /* .smc/.sfc/.fig/.swc or .zip */
void CoreUnloadRom(void);
void CoreReset(void);
/* render: 0 = skip the video of this frame (frameskip) */
void CoreRunFrame(int render);
unsigned CoreFps(void);                    /* 60 (NTSC) or 50 (PAL) */
double CoreFpsExact(void);
unsigned CoreAudioRate(void);              /* 32040 */
int  CoreIsPal(void);

/* Battery RAM */
int  CoreSramLoad(const char *path);
int  CoreSramSave(const char *path);       /* writes only if it changed; returns 1 if written */
int  CoreSramChanged(void);

/* Save states */
int  CoreStateSave(const char *path);
int  CoreStateLoad(const char *path);

/* Cheats (PocketSNES .cht text format: "7e1f8a-1e-03 #name") */
int  CoreCheatsLoad(const char *path);
unsigned CoreCheatCount(void);
const char *CoreCheatName(unsigned i);
int  CoreCheatEnabled(unsigned i);
void CoreCheatToggle(unsigned i);

#ifdef __cplusplus
}
#endif

#endif
