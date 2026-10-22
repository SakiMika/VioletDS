# Ragnarok Violet / VioletDS - BGM patch

## Root cause

- The 9 long background tracks `nitrofs/res/sound/0.mmf` through `8.mmf`
  use Yamaha SMAF Handy Phone Standard (`MTR`, format `0x00`). The original
  decoder only handled Mobile Standard (`0x02`) and ADPCM (`ATR`). The old
  BGM-specific fallback also searched for non-existent `sound/1xx.mmf` files.
- The special `6.mmf` contains BOTH score and audio tracks. Its score must
  take priority for the main melody rather than rendering only the ATR.

## Changed

- `source/rv_smaf_hps.h/.cpp`: checked Format 0 event parser with note,
  timing, pitch, octave, volume and expression, per-channel and per-track.
- `source/wipi_shim.cpp`: mixes concurrent score tracks into DS PCM using
  the existing low-cost synthesizer; fixes resource paths, BGM/SFX classification
  and BGM handoff; loops theme IDs 0-8; clears ownership on Stop.
- `tests/rv_smaf_hps_native_test.cpp`: verifies all 26 original MMF assets,
  14 score files, and the 9 main multitrack BGM themes.
- `README.md`: documents format handling and verification.

## Verification

Host build and format parsing test **passed**:

```
g++ -std=c++17 -O2 -Wall -Wextra -Werror source/rv_smaf_hps.cpp tests/rv_smaf_hps_native_test.cpp -o check_hps
./check_hps nitrofs
PASS: all 9 multitrack themes + 5 short HPS cues parse and emit valid notes
```

**Not verified:** complete .nds build or audio output on hardware/emulator.
This environment does not include the required devkitARM/libnds/ndstool
build toolchain. Run `build.bat` on an appropriately configured Windows
machine or the included GitHub Actions workflow, then test BGM transitions.
The music voices are approximate (general MIDI-like shapes), not bit-exact
Yamaha FM synthesis.
