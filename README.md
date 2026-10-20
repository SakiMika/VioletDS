# Ragnarok Violet — Nintendo DS native WIPI runner

Current target: **one normal `.nds` build**. A DSi can run this NDS-mode ROM directly. The build now leaves exactly one ROM file: `Ragnarok Violet.nds`.

## Build

Windows:

```bat
build.bat
```

Linux / GitHub Actions:

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
make -j2 PYTHON=python3
```

Output:

```text
Ragnarok Violet.nds
```

The internal port ID remains **RVDS**, but the NDS header game code is **CDLG** for stock melonDS compatibility. melonDS 1.1 does not have an `RVDS` ROM-list entry; unknown retail codes fall back to an 8 KiB EEPROM, which breaks Ragnarok's 128 KiB save backend. `CDLG` is a built-in 128 KiB EEPROM profile, so melonDS creates the correct `.sav` geometry without a manual override. The ROM is padded to 32 MiB to match that profile.

## Display

The original 240×320 portrait framebuffer is shown 1:1 across the two NDS LCDs:

- TOP: guest y=0..159, bottom-aligned on the 256×192 LCD;
- BOTTOM: guest y=160..319, top-aligned;
- x is centered at 8..247;
- no scaling.

This removes the artificial 32-pixel seam that existed when both halves were vertically centered.

## Input

Mapping follows the original runner README:

| NDS | WIPI / Ragnarok |
|---|---|
| D-pad | directions |
| A | EZ / FIRE / confirm (`-5`) |
| B | CLR (`-16`) |
| X | `0` |
| Y | `*` |
| R | `#` |
| L+A | `1` |
| L+B | `3` |
| L+X | `7` |
| L+Y | `9` |
| START | Call (`-10`) |
| SELECT | show/hide lower half (DS-side only) |

The one-time handset restart gate in the WIPI game is bypassed at its validated guest opcode so startup follows the same path as a successfully rebooted original handset.

## Save layout

Ragnarok has **three character slots**, not 15. The persistent WIPI files are:

```text
save0
save1
save2
option
list
```

`list` is the title-menu slot summary. If it is missing, the game shows all three entries as `새로 하기` even when `save0` itself exists. The EEPROM backend therefore persists all five names.

## Save-slot date fix

The title menu formats its slot timestamp with:

```text
%d/%d/%d %d:%d
```

Ragnarok obtains those fields through LGT/WIPI `MC_uicGetTime` (`0x1fb:0x338`). The old port left that import unresolved, so the function returned success without writing the caller's six-word time buffer. The game then saved whatever happened to be on the stack, producing large integers in the date line.

The current shim implements the real six-word ABI:

```text
[0] second
[1] minute
[2] hour
[3] day
[4] month - 1
[5] year - 1900
```

The existing libc `time/localtime` compatibility wrappers are also pinned to a 32-bit LGT guest ABI instead of exposing devkitARM/newlib's host layout directly.

**Existing saves whose `list` timestamp was already written with garbage values may keep the old bad date until that slot is saved again.** The character data itself does not need to be deleted; resaving the slot should regenerate the five timestamp fields through the fixed API.

## PNG compatibility

Ragnarok edits PNG palettes itself before `CreateImage`, so original PNG bytes are preserved. The runner uses `nitrofs/_port/rv_png_indices.bin` as an offline decompressed-index cache and combines it with the current runtime `PLTE`/`tRNS` data. This avoids a zlib decoder on ARM9 while preserving dynamic palette changes.

Current asset acceptance numbers:

```text
1279 original game files
1390 PNG instances
956 embedded PNGs
1122 unique cached IDAT streams
```

## GitHub Actions

`.github/workflows/build-nds.yml` now builds and uploads **one** artifact:

```text
Ragnarok Violet.nds
```

It runs the static acceptance test before compiling and verifies the 32 MiB size, compatibility game code `CDLG`, and ARM9 ROM offset `0x8000`.
