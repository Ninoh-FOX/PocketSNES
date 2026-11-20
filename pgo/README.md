# PGO profile (Miyoo Mini, Koriki)

`miyoomini/normal/` holds the .gcda files used by `make -f Makefile.miyoomini` (PGO=1, PLUS=0).
They match the object paths `build/normalO3pgo/...`, so the generate and use builds must keep the
same BUILD directory (the Makefile does).

Recorded 2026-11-20 with the instrumented binary run headless under qemu-arm (counters do not depend
on timing), 3500 frames per game with START/A/B/direction presses, on games NOT used for the
benchmarks: Super Metroid, Contra III, Chrono Trigger, Secret of Mana, Super Castlevania IV,
Kirby's Dream Course, Kirby's Dream Land 3 (SA-1), Pilotwings (DSP-1), Dungeon Master (DSP-2),
SD Gundam GX (DSP-3), Top Gear 3000 (DSP-4), Starwing and Vortex (SuperFX), Street Fighter Alpha 2
(SDD-1), Mega Man X3 (C4), Tales of Phantasia, Daikaijuu Monogatari II (S-RTC).
.gcda files whose counters were all zero were deleted (code never run keeps its normal -O3 tuning).

New profile after changing the code:
    make -f Makefile.miyoomini PGO=generate PGO_DIR=/abs/dir     # instrumented binary
    (run the games; the .gcda are written under /abs/dir/build/normalO3pgo/...)
    delete all-zero .gcda, copy /abs/dir/build into pgo/miyoomini/normal/, then rebuild with PGO=1

Measured on a Miyoo Mini Plus (1.2 GHz, emulator thread CPU time per frame): PGO adds ~4 % on top
of the other optimizations.
