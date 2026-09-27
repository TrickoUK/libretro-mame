# jpark3 at 60 fps - experiment write-up (2026-09-27)

## Goal

Jurassic Park III (`jpark3`, Konami Viper, Voodoo 3) and GTI Club 2 (`gticlub2`) run at 30 fps.
The question: can they be made to run at a real 60 fps (twice as many frames, same game speed)?

## Outcome

**Not practical.** The 30 fps cap is a single comparison in the game's code, and a one-word
runtime patch removes it: the game then renders ~60 frames a second. But its logic advances a
fixed amount per game frame, so it also runs at **double speed**. Real 60 fps would mean finding
and halving every per-frame quantity in the game (float time steps, integer timers, animation,
attract/script timing, physics, reload/invulnerability timers, sound sync, light-gun timing):
weeks of reverse engineering per game with a long tail of subtle bugs. Stopped there. No code
from the experiment was kept; only this write-up.

## What was done, step by step

### 1. Is the cap in the hardware or the game?

Temporary trace in `voodoo_1_device::reg_swapbuffer_w()` (`src/devices/video/voodoo.cpp`):
histogram of the swap command's "sync to vblank" bit, its swap interval, and how many vblanks had
passed when the game issued it. Ran both games through attract mode with
`fork-specific/tools/profile_run.py`.

- Every swap is sent **unsynced** (bit 0 = 0, interval 0): the Voodoo swaps immediately.
- The command arrives **exactly 2 vblanks** after the previous one, >3000 frames in a row in both
  games (a handful of 3s). A CPU-bound game would vary.

So the game's main loop deliberately waits 2 refreshes per frame; nothing in the Voodoo emulation
enforces it.

### 2. Find the game's timing variables

A Lua frame notifier (sent to MAME's console via `profile_run.py --lua`) dumped all 16 MB of main
RAM on 6 consecutive frames: `space:read_range(0, 0xffffff, 8)` written to files. A numpy script
compared the dumps for 32-bit values that go +1 every frame, +1 every 2 frames, or alternate:

| Address | Behaviour | Meaning |
|---|---|---|
| `0x0E9CDC` | +1 every frame | vblank counter |
| `0x0E9CE0` | +1 every 2 frames | game frame counter |
| byte `0x0E9EDC` | 1, 2, 1, 2 | vblanks elapsed in the current game frame |
| byte `0x0E9D04` | 0, 1, 0, 1 | odd/even vblank |

### 3. Find the code that uses them

Static searches of the RAM dump with `capstone` (PPC 32-bit big-endian, available in the host
`python3`) found nothing conclusive: the variables are fields of a structure reached through a
pointer. Following the interrupt path did help:

- External interrupt vector `0x500` -> OS dispatcher -> `0xF1C8`, which reads the interrupt
  controller's acknowledge register and calls a handler from a table at `0x300 + vector`
  (16-byte entries: handler address, r2 value, ...).
- jpark3's vblank handler `0x4402C` (runs with r2 = `0x0E9264`) only calls an OS function
  (`0x90A0`, r3 = 4, "signal event 4") to wake the game task.

The accesses themselves were found **dynamically**, with a temporary diagnostic build:

1. `ppcdrc.cpp`: when `MAME_PPC_TRACKPC` was set, emit `UML_MOV(mem(&m_core->pc), desc->pc)`
   before each compiled instruction, so the PC is exact during memory accesses.
2. `viper.cpp`: when `MAME_VIPER_NOFASTRAM` was set, skip `ppcdrc_add_fastram()` so main RAM goes
   through MAME's memory system (the DRC's direct RAM access bypasses memory taps).
3. `viper.cpp`: when `MAME_VIPER_TAPTRACE` was set, install read/write taps **in C++** on the
   variables, counting accesses per `(address, byte mask, m_maincpu->pc())`, printed every 600
   vblanks. (Lua taps on the 64-bit Viper bus crashed the run: "integer value will be
   misrepresented in lua".)

### 4. Read the frame loop

Disassembling the reported PCs showed a timing structure at `0x0E9CD8` (loaded with
`lwz r31,4(r2)`): `+4` vblank counter, `+8` frame counter, `+0x204` vblanks-in-frame, `+0x2C`
frame-ready flag, `+0x2D` start-next-frame signal, `+2` "frame overran" flag.

- **Per-vblank function `0x42534`**: if a frame is ready **and vblanks-in-frame >= 2**
  (`0x42574: cmplwi cr1,r0,2`, instruction word `0x28800002`), present it and signal the next
  frame (reset the count, set `+0x2D`). It always increments the counters.
- **Frame function `0x44054`**: runs the game subsystems when `+0x2D` is set. At the end it sets
  `+2` if the frame overran (>= 2 vblanks).

That `2` is the 30 fps lock.

### 5. The experiment: unlock it

Prototype runtime patch in `viper_state::voodoo_vblank()`, gated by `MAME_JPARK3_60FPS` and only
for `jpark3`: once the game code is loaded (checked against the neighbouring words `0x881F0204`,
`0x28800002`, `0x4082000C`), write `0x28800001` to `0x42574` and flush the PPC DRC cache. That
needed a small public `ppc_device::ppcdrc_flush_cache()` setting `m_cache_dirty`, since
`code_flush_cache()` is private. The same pattern as the fork's runtime `opwolf` patch.

Measured with a Lua frame notifier reading the game's own counters every 300 vblanks:

| | Game frames per 300 vblanks |
|---|---|
| Unpatched | 150 (30 fps) |
| Patched | 271-300 (~58-60 fps; the odd miss is a frame that took >1 vblank) |

Game speed, checked with frame-exact snapshots (`fork-specific/tools/snap_run.sh`) at the same
emulated frames as an unpatched run: the patched game is far ahead (title screen at frame 2400 vs
about frame 3900 unpatched), i.e. **double speed**.

### 6. Is there a single time-step to halve?

Searched RAM for likely constants: 17 copies of 1/30 (float) and 7 of 1/60 in the constant pools
(`0xCF000-0xD8000`), so some systems use explicit per-frame float steps. But the attract sequence
and timers count integer frames, so halving those constants alone can't fix the speed. Stopped
here.

## If it's ever picked up again

- Start from the addresses above (jpark3 only; gticlub2 likely has the same engine code at other
  addresses, find it the same way).
- Real 60 fps needs every per-frame quantity halved: the float constants, integer frame timers
  (attract script, animation indices, spawn, reload, invulnerability), camera/physics paths,
  and a check of sound/music sync and light-gun hit timing. Expect weeks of work per game.
- Everything experimental was reverted. Re-create the diagnostic pieces from the descriptions in
  steps 3 and 5 (they're a few lines each).

## Reusable techniques from this experiment

- Frame-exact RAM dumps via Lua `read_range` in a frame notifier, analysed with numpy.
- PC-accurate memory taps under the PPC DRC: per-instruction PC store + fastram off + C++ taps.
- `capstone` in the host Python for PowerPC disassembly of RAM dumps.
- Measuring a game's real frame rate from its own counters instead of guessing from video.
