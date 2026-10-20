# Ragnarok Violet -> Nintendo DS/DSi (first-pass port)

This tree is a first-pass conversion of the supplied Zenonia 2 WIPI/DSi loader to the supplied **Ragnarok Violet** WIPI build (`AID 000256A7`). It is intentionally conservative: remove Zenonia-specific binary hooks first, boot the Ragnarok native WIPI ELF, preserve the requested key mapping, and show the original portrait framebuffer without scaling.

## Current display policy

Ragnarok assets and WIPI framebuffer usage indicate a **240x320 portrait** target. The temporary DS layout is therefore pixel-perfect 1:1:

- TOP LCD: guest rows `y=0..159`
- BOTTOM LCD: guest rows `y=160..319`
- Each 240x160 half stays 1:1 with 8 px side margins. TOP is bottom-aligned (y=32..191) and SUB is top-aligned (y=0..159), eliminating the artificial 32 px seam between halves.
- Touch/settings/minimap overlays are disabled in normal gameplay so the bottom LCD remains the lower half of the game.

## Key mapping — preserved from the original source README

The original `zenonia2-dsi-main/README.md` mapping takes precedence over
Ragnarok first-pass assumptions. Its WIPI **EZ(FIRE)/CLR** codes are `-5/-16`, not
numeric `5/0`. Unmodified mapping:

| DS key | WIPI event / action |
| --- | --- |
| D-pad | Direction (`-1/-2/-3/-4`) |
| `A` | EZ / confirm / WIPI FIRE (`-5`) |
| `B` | CLR (`-16`) |
| `X` | `0` |
| `Y` | `*` |
| `R` | `#` |
| `L + A` | `1` |
| `L + B` | `3` |
| `L + X` | `7` |
| `L + Y` | `9` |
| `START` | Call (`-10`) — original README says "Call / quicksave"; whether it quicksaves depends on Ragnarok's handling of that WIPI event |
| `SELECT` | Show/hide bottom LCD contents (**DS-side toggle**, not a WIPI key) |

The L combinations suppress the associated single-key actions: for instance,
`L + A` sends `1`, **not** both `1` and EZ/FIRE. Bare `L` sends no event. On this
first-pass portrait port the SELECT toggle hides/shows the bottom **160 rows**
of the game framebuffer (not a functioning Zenonia minimap); the lower half
starts visible. The `X+Y+START` chord still exits this *diagnostic* build.

## Ragnarok-specific changes

1. Guest framebuffer changed from Zenonia's patched 256x192 layout to native 240x320.
2. Zenonia-specific sound/layout/minimap/boot address hooks are no longer installed.
3. Guest clock is restored to 1.00x.
4. Ragnarok imports WIPI `CreateImage`; the old Zenonia shim left it unimplemented. The shipped NitroFS preserves all original PNG bytes because Ragnarok rewrites PNG palettes at runtime. A private `_port/rv_png_indices.bin` sidecar caches decompressed indexed-pixel planes so ARM9 does not need zlib; `gfx_create_image()` combines that cache with the current PNG `PLTE`/`tRNS` data.
5. LGT image/screen/offscreen handles now use the expected public framebuffer + graphics-view + backing-framebuffer layout. `GetFramebufferPointer/Width/Height/BPL/BPP` resolves the handle passed by the guest instead of always returning the screen surface.
6. NitroFS contains the supplied Ragnarok `binary.mod` and resources, not the Zenonia payload.
7. Banner/title/build filenames are renamed for Ragnarok Violet.

To rebuild NitroFS from the original Ragnarok JAR:

```bash
python tools/pack_ragnarok_assets.py 000256A7.jar nitrofs
```

That helper requires Pillow. The already-prepared `nitrofs/` included here does **not** require Pillow at runtime or during a normal `make`.

## Build

Requires devkitPro/devkitARM + libnds/NitroFS support, as with the original project.

Windows:

```bat
build.bat
```

POSIX shell with `DEVKITPRO`/`DEVKITARM` configured:

```bash
make clean
make -j2
```

Expected raw target: `Ragnarok_Violet_DSi.nds`. The Windows script renames it to `Ragnarok Violet.nds`.

Static acceptance check (does not require devkitARM):

```bash
python tests/ragnarok_firstpass_test.py
```

The original Zenonia test suite is retained under `legacy_zenonia_tests/` for reference, but it is not the acceptance suite for this port.

## Build fix: 2026-10-20 compiler log

Removed the stale `ZenDecodedPcm` / `zen_load_title_pcm` call from
`source/wipi_shim.cpp::media_decode()`. That code required
`zen_predecoded_audio.cpp`, which is intentionally excluded from this Ragnarok
build because its `title108` cache is specific to Zenonia 2. Generic WIPI
SMAF (MMMD) and RIFF/WAV decoding are unchanged. The DS key mapping and
240x320 split layout are also unchanged.

The compile errors around `wipi_shim.cpp:2169-2170` in the supplied
`last_build(20261020-083839).log` are addressed. This is a static fix;
the current environment does not provide devkitARM, so `.nds` compilation
and runtime behavior still require verification on the user's toolchain.


## GitHub Actions

The repository includes `.github/workflows/build-nds.yml`. On pushes to `main`/`master`, pull requests, or a manual `workflow_dispatch`, it:

1. runs `tests/ragnarok_firstpass_test.py`;
2. builds with the `devkitpro/devkitarm:latest` container;
3. verifies the NDS header/game code;
4. uploads `Ragnarok Violet.nds` and `build.log` as a workflow artifact.

Generated ROMs, maps, ELF files, Python caches, and local `last_build*.log` files are ignored by Git. Historical one-off root `.txt` fix notes were removed; the maintained change/context notes stay in this README.

## First-pass limitations

This source has been statically validated in the current environment, but **not compiled or hardware/emulator runtime-tested here** because devkitARM/devkitPro are not installed in this environment. Several Ragnarok WIPI imports still fall back to the project's generic diagnostic stubs; the next runtime trace will identify which of those are actually required. Save persistence also still uses the old Zenonia-specific backend naming/layout, so Ragnarok database writes should be treated as session-only until the save backend is generalized.

The image conversion is deliberate for bring-up: it avoids adding a full PNG/zlib decoder to ARM9 and keeps original resource paths stable. If Ragnarok performs its own PNG signature checks before calling WIPI `CreateImage`, that specific path will need a real PNG decoder instead.

## Diagnostic build: both LCDs black / LOG button missing

The previous first-pass hid the SUB log *before* executing Ragnarok `binary.mod`,
never called its existing gear painter, and did not dispatch touch events.
These are verified source-level bugs (independent of any remaining WIPI render
bugs), and this diagnostic build corrects all three.

- TOP: always-visible boot checkpoints from `01 VIDEO READY` through
  `10 GAME LOOP`. If the ARM9 locks up, the last checkpoint remains visible.
- BOTTOM: diagnostic LOG console stays **open by default** during boot and
  gameplay; tap **CLOSE** in the top-right of the bottom LCD to reveal the
  lower 160 rows of the game. Tap the newly restored gear at x=236..251,
  y=4..19 to open LOG again.
- Normal DS-to-WIPI mapping from the original README is unchanged.
  Touch never goes to the guest.

Checkpoint meanings:

| Last TOP stage | Execution likely stopped in |
| --- | --- |
| No diagnostic screen | Before or inside `wipi_init_video()`, or the NDS file did not start |
| `02 OPEN NITROFS` | NitroFS mounting / resource access |
| `03 LOAD ELF` | ELF allocation, loading or relocations |
| `04 PATCH IMPORT` | Native WIPI veneer prepatch |
| `05 CALL ENTRY` | Ragnarok ELF entry point |
| `06 CALL INIT` | Native initializer |
| `07 CHECK CLET` | Clet registration / callback inspection |
| `08 START CLET` | `startClet` callback |
| `09 PAINT CLET` | `paintClet` callback |
| `10 GAME LOOP` | Main loop reached; if game remains black, inspect LOG + pixel counters |

This is **not** a claim that the Ragnarok game itself is now rendering. This
build is designed to make the next failure observable without modifying guest
input behavior or its 240x320 framebuffer split.

## 2026-10-20 black-screen image fix

The visible boot diagnostic reached the game loop but printed repeated
`[IMG!] unsupported encoded image` messages.  The original first-pass packer
converted only the 434 standalone `.png` files.  Ragnarok Violet additionally
contains **956 palette PNG images embedded inside DAT/font resources**.  Each
embedded PNG is preceded by a little-endian 16-bit payload length.

`tools/pack_ragnarok_assets.py` now converts both forms offline.  Embedded PNG
payloads are replaced with RVP1 and their 16-bit length fields are rewritten,
while surrounding Ragnarok metadata is left untouched.  The prepared NitroFS
therefore contains **1390 RVP1 images total (434 standalone + 956 embedded)**
and no PNG signatures.  This keeps PNG/zlib decoding out of the ARM9 runtime.

If `CreateImage` encounters another format, the runtime diagnostic now prints
the first four signature bytes as `sig=xxxxxxxx` rather than only the length.


## 2026-10-20 timer ABI fix (black screen after image conversion)

The diagnostic build reached `10 GAME LOOP` with `create=2` but no actual draw yet,
and had one armed timer.  The inherited Zenonia shim declared `MC_knlSetTimer` as
`(timer, delay, param, owner)`.  LGT/WIPI-C actually passes
`(timer, timeout_low, timeout_high, param)`.  Ragnarok therefore lost the real r3
callback parameter on its first 20 ms tick.  This build restores the standard
four-word timer ABI and prints the first few `[TMR] def/set/fire` lifecycle lines.
The README key mapping and 240x320 split-display policy are unchanged.

## Timer callback trace diagnostic (2026-10-20)

The latest melonDS capture proves the first WIPI timer reaches its due time and enters the scheduler. This build does **not** change the timer ABI, key mapping, resource pack, or display layout. It only makes the next failure observable:

- `[T>] s=N cb=........ p=........` is printed immediately before the guest timer callback.
- `[T<] s=N ret ...` is printed only if that callback returns.
- unresolved imports called while the timer is active are tagged with `T`, for example `[CALL?] 1fb:xxx ... T`.
- one fresh graphics/timer dump is printed after the first callback returns.

Interpretation:

- `[T>]` with no `[T<]`: the guest is stuck/crashing inside the timer callback. The last `CALL? ... T` identifies the strongest API candidate.
- `[T>]` followed by `[T<]`: the timer path is healthy; use the fresh `after timer` GFX counters to determine whether repaint/draw/flush occurred.

## 2026-10-20 callback deep-trace build
The previous timertrace screenshot proves the first timer fires and enters callback
`0x0212b585`, but there is no `[T<]`, so the scheduler itself is no longer the
primary suspect. This build adds diagnostic-only tracing for **resolved** WIPI
imports as well as unresolved imports.

New lines:
- `[T>] s=N cb=........` — entered timer callback.
- `[TPC] old=........ p=........` — exact callback address mapped back to `binary.mod`.
- `[TC0] .... .... .... ....` — first four Thumb halfwords at the callback.
- `[K] ttt:iii` — a resolved WIPI import entered while the timer callback is active.
- `[CALL?] ttt:iii ... T` — unresolved import entered while the timer callback is active.
- `[T<] ...` — callback returned normally.

If `[T>]`/`[TPC]` appear and then no `[K]`/`[CALL?]`/`[T<]`, the fault is inside
Ragnarok guest code before its first platform call. If `[K]` lines appear, the
last one before the freeze identifies the resolved service to inspect next.

No gameplay mapping, split-display geometry, RVP1 resources, or timer ABI has
been changed in this build.

## 2026-10-20: black-screen root cause after embedded-RVP1 conversion

The callback deep trace narrowed the first timer tick to this resolved-import
sequence:

```
[K] 1fb:0ce   # MC_grpSetContext
[K] 001:411   # strlen
[K] 1fb:080   # MC_knlGetResourceID
[K] 1fb:075   # MC_knlAlloc
[K] 1fb:081   # MC_knlGetResource
[K] 1fb:075   # MC_knlAlloc
[K] 001:414   # memcpy
```

Static disassembly of `binary.mod` matches that sequence to the native font
loader around old guest address `0x00036dd8`.  It loads `res/font`, copies each
of its eight length-prefixed PNGs, scans the PNG bytes for the literal `PLTE`
chunk, rewrites palette RGB bytes and recalculates PNG CRCs.  The previous
first-pass packer had replaced those embedded PNG payloads with `RVP1`, so the
`PLTE` scan failed.  The game then derived negative chunk offsets and remained
inside guest code after the final `memcpy`.  In other words, the image
preconversion itself was corrupting a format that Ragnarok actively parses.

This revision changes the image strategy:

* All 1,279 original JAR payload files are restored byte-for-byte in NitroFS.
* All 1,390 PNG instances remain real PNGs (434 standalone + 956 embedded).
* `res/font` is back to its original 2,711 bytes and contains eight real `PLTE`
  chunks.
* ARM9 still does **not** run zlib. `tools/pack_ragnarok_assets.py` builds a
  private `_port/rv_png_indices.bin` sidecar containing the decompressed palette
  index plane for each unique PNG IDAT stream.
* There are 1,122 unique IDAT streams, occupying 1,724,201 bytes in the sidecar.
* `MC_grpCreateImage` parses the current PNG `PLTE`/`tRNS` data, hashes only the
  unchanged IDAT stream, loads its cached index plane and expands it to RGB565 +
  alpha. Palette edits made by Ragnarok immediately before `CreateImage` are
  therefore preserved.
* The old RVP1 decoder remains only as a compatibility fallback; the shipped
  Ragnarok NitroFS no longer rewrites game PNGs to RVP1.

This addresses the specific timer-callback hang shown by the deep-trace build,
without changing the README-derived key mapping, timer ABI or temporary
240x320 split-screen layout.

Static validation checks every one of the 1,390 PNG instances against the
1,122-entry cache and verifies that an in-memory PLTE mutation leaves the IDAT
lookup key unchanged.

## 2026-10-20: EZ mapping + flicker fix

- `A` is explicitly the LGT **EZ/confirm** action. On the WIPI side this is `FIRE = -5`.
- `MC_grpFlushLcd(kind, source, ...)` now honors the `source` framebuffer, which is required for Ragnarok's offscreen/double-buffer presentation.
- DS LCD frames are composed in RAM and copied to VRAM in one transfer. The previous path cleared live VRAM to black before redrawing it pixel-by-pixel, causing visible flashing.
- No VBlank wait was added; earlier bring-up showed that blocking on the guest's VBlank path is unsafe.

## 2026-10-20: startup EZ input + stable LOG console

The handset restart prompt shown after the PNG fix uses Ragnarok game state `0x16`.
Static disassembly shows the native UI key handler at old guest address `0x0000B110`
accepts both WIPI EZ/FIRE (`-5`) and numeric `5`; the canonical mapping remains
`A -> -5`. The CLET `handleCletEvent` path only calls that native UI handler in
selected internal input modes, which can leave this startup handset dialog waiting.

This revision adds a relocation-safe, narrowly-scoped bridge:

- `0x015009C0` is translated through `lgt_translate_ptr()` to read the current
  Ragnarok UI state.
- Only while that state is exactly `0x16`, a DS `A` press invokes the original
  Ragnarok UI handler (`0x0000B110`, Thumb) directly with key `-5`.
- The matching key release is consumed to prevent a duplicate CLET release event.
- Every other screen and every other key continues through the normal README/WIPI
  mapping path.

The diagnostic LOG console also no longer redraws for each `printf()` and no longer
clears the entire text screen before each refresh. While open, dirty log frames are
limited to 10 Hz. After successful `10 GAME LOOP` bring-up the LOG console closes
automatically so the lower 160 guest rows are visible; tap **LOG** to reopen it.


## 2026-10-20: handset restart gate bypass

Ragnarok Violet has a one-time handset restart gate during startup. Static Thumb
disassembly of the pristine `binary.mod` shows:

```text
0x27D4  ldrb r3,[r4]       ; restart-complete flag loaded from 0x015003A0
0x27D6  cmp  r3,#0
0x27D8  beq  0x27E2       ; zero -> enter state 0x16 restart prompt
0x27DA  ...                ; nonzero -> normal post-reboot initialization
```

The EZ handler for state `0x16` sets the flag to 1, persists settings, then calls
LGT WIPI `1fb:068`. The port intentionally has no real handset firmware reboot,
so returning success from that API leaves the running process in the restart
prompt.

This build patches the *loaded guest copy* at old address `0x000027D8` from
Thumb `0xD003` (`BEQ`) to `0x46C0` (`NOP`). The original opcode is validated
before patching. This makes startup take the exact fall-through path used after
a successful handset reboot; game assets, save loading, timer ABI, PNG cache,
and README key mapping are otherwise unchanged.


## 2026-10-20 seam/gap fix

The previous temporary split centered both 240x160 halves vertically in their
192-line LCDs. That created 16 black rows below TOP plus 16 black rows above SUB,
which appears as a 32-pixel gap (64 pixels at 2x emulator scaling).

This revision keeps the guest image pixel-perfect 1:1 but anchors the halves to
the inner LCD edges:

- TOP guest y=0..159 -> DS y=32..191.
- SUB guest y=160..319 -> DS y=0..159.
- Horizontal placement remains x=8..247.

No loader, PNG cache, restart bypass, timer, or input behavior changed.
