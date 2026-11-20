# PocketSNES for Miyoo Mini / Mini Plus (Koriki) — branch `koriki`

PocketSNES frontend (menu, ROM browser, save states with preview, cheats, scalers, all options) on the
**snes9x2005** core (`core/snes9x2005`, branch `koriki` of Ninoh-FOX/snes9x2005), driven through the
libretro API (`menu/core.c`). The core is linked statically and can be updated on its own.

Build (toolchain in /opt/miyoomini-toolchain):

    git submodule update --init
    make -f Makefile.miyoomini        # -> dist/PocketSNES

Changes for the Miyoo Mini:
- Miyoo Mini buttons (L2/R2 also act as L/R, MENU opens the menu), stereo enabled by default.
- Frame pacing waits on a condition signalled by the audio callback instead of usleep(1000)
  (10 ms on the 100 Hz kernel); with sound off or no audio device the clock paces the frames.
- Audio buffer stays a power of 2: with audiofix ON the SDL 1.2 OSS driver (libpadsp) needs it.
- SMOOTH scaler rewritten with NEON (bit-exact, 3x faster, never reads the framebuffer).
- The menu is composed in RAM and written once per flip (the text no longer flickers).
- SuperFX: switch dispatch in the core (`-DFX_SWITCH_DISPATCH`), PGO profile in `pgo/` + LTO.
- Core 32040 Hz audio resampled to the menu rate (44100 by default: audiofix needs it).

Measured on a Miyoo Mini Plus at 1.2 GHz (emulator thread CPU time per frame) against the previous
Koriki binary: +17 % on average (Star Fox +21 %, Yoshi's Island +19 %, Stunt Race FX +17 %,
Doom +13 %, Super Mario RPG +18 %, Donkey Kong Country +19 %, Super Mario World +14 %).
Save states from the old binary (snes9x 1.43 format) cannot be loaded; .srm files are compatible.

---

# PocketSNES
RG-350 Port of PocketSNES

# Goldfinger instructions

### 1.Add the cheat file to the .pocketsnes directory and rename it to "ROM name.cht"
### 2.The content of the cheat file is :
> Address(6 bit)-fixdata(2)-rawdata(2) #name

> 7e1f8a-1e-03 #Infinite HP

### 3.Start the game, select cheats in the menu and press a to turn on / off gold finger.
