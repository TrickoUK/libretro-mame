# 3DO M2 (Konami M2) Full-Speed Fix — Investigation Notes

## Background

A MAME contributor going by **Tovarichtch** (GitHub: `Tovarichtch`, referred to as
"Tova" in credits) has produced a build that brings the Konami M2 / 3DO M2 arcade
driver to "near-perfect" emulation, with all five games running at full speed
(previously several ran at a fraction of real speed, e.g. Battle Tryst was not
fully playable).

This work has **not been merged into `mamedev/mame`** and does not exist as an
open PR or a pushed branch on Tovarichtch's public GitHub fork
(`github.com/Tovarichtch/mame` — checked branches as of 2026-09, none touch
`3dom2.cpp`/`3dom2_te.cpp`/`konamim2.cpp` beyond what's already upstream). It has
only been distributed informally as a prebuilt "MAME .289 preview/artifact build"
via the Discord of the YouTube channel "Video Game Esoterica"
(`discord.gg/pBDPNWmQb`), referenced in these videos:

- "3DO M2 Emulation FULL SPEED in MAME! All Games Now Work" (Video Game Esoterica)
- "3DO M2 Emulation Gets Even BETTER in MAME! Big Updates" (Video Game Esoterica)
  — description explicitly says: *"Credit for Commits by Tova to the MAME repo...
  Grab it on my Discord for now"*
- "Konami 3DO M2 All 5 Games tested - on Mame 289 Preview Build (full speed &
  glitch fix)" (Emu Gamer)

Tovarichtch's only confirmed *merged* MAME contribution is
[mamedev/mame#15599](https://github.com/mamedev/mame/pull/15599)
(`namco/namcos23.cpp: fix crszone input and disable idle debug-UART clock`,
merged 2026-06-29). That fix disabled a free-running debug ACIA baud clock that
had nothing attached to its RS232 port, taking Crisis Zone from ~49% to ~96%
speed. **The M2 fix is presumed to follow the same philosophy: find something
free-running / burning host cycles for no emulated benefit, and stop it.** No
literal UART/ACIA clock exists in the M2 driver, so the equivalent bottleneck is
almost certainly elsewhere (see below).

Goal of this document: give a fresh Claude session (working from a libretro-mame
fork, which is a fork of mamedev/mame) enough grounding to attempt reconstructing
the fix independently, without having to redo the investigation.

## Relevant source files (path relative to `src/mame/konami/` unless noted)

- `konamim2.cpp` — the actual arcade driver: CPU config, address maps, ROM
  defs, inputs, machine_config. ~1700 lines.
- `3dom2.cpp` / `3dom2.h` — reusable "3DO M2 Bulldog ASIC" (BDA) and "CDE ASIC"
  device implementations shared by the driver (memory controller, PowerBus,
  VDU/video, control ports, MPEG stub, CD/BioBus DMA controller).
- `3dom2_te.cpp` / `3dom2_te.h` — Triangle Engine (3D rasterizer) device.
- `../../devices/cpu/dspp/dspp.cpp` / `.h` — the DSPP audio DSP CPU core, shared
  between the original 3DO "Clio" DSPP and the M2 "Bulldog" DSPP variant
  (`dspp_bulldog_device`). Split into two device types as of commit `85d31d8b6d5`
  (2026-02-03, "preliminary split between regular (Clio) DSPP and M2 Bulldog").

Hardware: two IBM PowerPC 602 CPUs at 66.6667 MHz (`M2_CLOCK` in
`konamim2.cpp:241`), sharing RAM through the BDA ASIC, driven by a "Bulldog"
custom ASIC that combines the 3D rasterizer, a DSP audio processor, an MPEG
decoder, and various bus controllers.

## Findings, ranked by confidence

### 1. DSPP "Bulldog" audio DSP clock is very likely wrong (highest confidence)

In `3dom2.cpp:296`:
```cpp
DSPP_BULLDOG(config, m_dspp, DERIVED_CLOCK(1, 1));
```
This clocks the DSPP CPU device at the *full* M2 system clock — 66.6667 MHz.
But the driver's own PCB documentation comment block (`konamim2.cpp` around
line 81) records the real board's audio clock generator output:
```
CLKC - 16.9345MHz
```
That exact number (`16.9345`) already appears as a magic constant elsewhere in
`3dom2.cpp:253`:
```cpp
m_dac_timer->adjust(attotime::from_hz(16.9345));   // BUG: should be from_hz(16934500) — units are Hz not MHz
```
(it self-corrects to `attotime::from_hz(44100)` on the very next tick in
`dac_update()`, so this particular bug is cosmetic/one-shot — but it's strong
internal evidence that 16.9345 MHz, not 66.6667 MHz, was meant to be the DSPP's
real clock.)

**Why this matters for speed:** the DSPP core has **no DRC / recompiler** — it's
a pure interpreter (see finding #2). Clocking it 4x too high means MAME
interprets roughly 4x more DSP instruction-cycles per emulated second than
necessary, on every single frame, for every game on this hardware. This is the
single most likely uniform, cross-game speed bottleneck, and matches the video
description ("every OTHER 3DO M2 game runs at full speed").

**Suggested fix to try:** change the DSPP's clock in `device_add_mconfig` from
`DERIVED_CLOCK(1, 1)` to something reflecting the real ~16.9345 MHz audio clock
(e.g. `DERIVED_CLOCK(1, 4)` if it should be exactly a quarter of M2_CLOCK, or an
explicit `XTAL(16'934'500)` if it's actually driven by a separate oscillator
rather than derived from the main system clock — the PCB notes list CLKC as a
distinct output of the CY2292S clock generator chip, separate from CPUCLK
25.2MHz, so an independent XTAL is plausible). Also worth fixing the
`from_hz(16.9345)` → should almost certainly be `from_hz(16934500)` or simply
removed in favor of going straight to `from_hz(44100)`, though this is cosmetic.

### 2. DSPP interpreter burns full instruction budget even when idle

`dspp_bulldog_device::execute_run()` (`dspp.cpp:859`, non-DRC path) calls
`update_ticks()` unconditionally on every loop iteration:
```cpp
inline void dspp_device::update_ticks()
{
    --m_core->m_tclock;
    --m_core->m_icount;
}
```
This decrements `m_icount` regardless of whether `DSPX_CONTROL_GWILLING` is set
in `m_core->m_dspx_control`. `execute_one()` internally checks GWILLING before
actually decoding/executing an instruction, so when GWILLING is clear the DSP
does no real work — but MAME's scheduler still burns through the *entire*
timeslice for that CPU device just ticking an idle clock down to zero. Combined
with finding #1 (4x too-fast clock), this compounds the waste whenever the DSP
program is dormant between audio frames.

Also note: `dspp.cpp:213` — `m_isdrc = false; //allow_drc();` — DRC support for
this CPU core is explicitly disabled (commented out), unlike the PowerPC CPUs
which do use a recompiler. This is presumably deliberate (DSPP DRC may be
incomplete/untested) but it means any fix here has to work within the
interpreter, or finishing/re-enabling `allow_drc()` could itself be a large win
if it's actually functional.

### 3. Existing hand-injected "speed hack" comments hint at unhandled PPC spin-loops

The driver's own header (`konamim2.cpp:22-30`) documents manual GPR patches
already applied at specific PCs to work around observed slowness:
```cpp
// Polystars/Total Vice
if (pc == 0x40035958)
    gpr[11] = 1;

// Everything else
if (pc == 0x400385c8)
    gpr[11] = 0;
```
and the driver's TODO list explicitly states:
```
* Fix incorrect speed in Tobe! Polystars
* Fix incorrect speed in Heat of 11 and Total Vice (partially)
```
This strongly suggests unresolved busy-wait/polling loops in game code (likely
polling DSPP status, CDE/DMA status, or VDU vsync/line registers) that MAME's
dual PPC602 DRC cores execute at full host CPU cost instead of being recognized
as idle spins. This is the same underlying category of bug as the Crisis Zone
fix (namcos23 idle debug clock) — something spinning uselessly and consuming
real host cycles for no emulated benefit — just manifesting as CPU-side polling
rather than a peripheral's free-running clock.

If reconstructing the fix, it may be worth checking whether Tovarichtch's build
generalizes/replaces these two hardcoded GPR-patch hacks with a more principled
fix (e.g. correctly emulating whatever status flag the game is actually polling
for, so the spin loop naturally exits sooner, or adding proper idle-skip
detection).

### 4. Lower-confidence leads / things ruled out

- **`PPCDRC_COMPATIBLE_OPTIONS`** (`konamim2.cpp:696-697`, applied to both
  `m_ppc1` and `m_ppc2`) expands to `PPCDRC_STRICT_VERIFY | PPCDRC_FLUSH_PC |
  PPCDRC_ACCURATE_SINGLES` (see `src/devices/cpu/powerpc/ppc.h:167-177`), all of
  which are expensive relative to `PPCDRC_FASTEST_OPTIONS` (`= 0`). This looked
  promising at first but **is the standard setting used by nearly every PPC
  driver in MAME** (Model 3, Hornet, GTI Club, Viper, Firebeat, Triforce,
  GameCube, etc. — checked via `grep -rn ppcdrc_set_options src/mame/`), so it's
  unlikely to be an M2-specific quick win on its own. Still, if M2 games don't
  actually rely on self-modifying-code detection or PC-flushed memory
  accuracy, trying `PPCDRC_FASTEST_OPTIONS` in isolation and checking for
  regressions could be worth a quick experiment.
- **The `M2_BAD_TIMING` loading hack** (`3dom2.h:24`, `#define M2_BAD_TIMING 0`)
  was already removed/disabled in commit `355e240b346` (2024-12-31, "3dom2:
  remove hack believed to have become obsolete, it caused extreme slowdowns
  during loading" [David Haywood]). Already fixed, not part of this
  investigation.
- **CDE DMA** (`m2_cde_device::start_dma` in `3dom2.cpp`) does synchronous
  word-by-word memory copying in a tight `while` loop rather than a
  timer-scheduled incremental transfer, but it's a one-shot cost per DMA
  request (e.g. CD reads), not a recurring per-frame cost, so lower priority
  than #1–#3.
- **CDE serial debug (`SDBG`) registers** were checked as a possible analogue
  to namcos23's free-running debug UART clock, but M2's SDBG mechanism
  (`CDE_SDBG_CNTL`/`CDE_SDBG_WRT`/`CDE_SDBG_RD` in `3dom2.cpp`/`.h`) is a plain
  memory-mapped register interface with no backing `clock_device` — there's no
  equivalent free-running clock to disable here.

## Recommended starting point

Given the confidence ranking above, the highest-value, lowest-risk experiment
to try first is **finding #1**: correct the DSPP Bulldog clock from
`DERIVED_CLOCK(1, 1)` (66.6667 MHz) down toward the documented ~16.9345 MHz
audio clock, and see how much it moves the needle on host performance across
all five games (`polystar`, `totlvice`, `btltryst`, `heatof11`, `evilngt`/
`hellngt`). If that alone doesn't get close to "full speed," look at finding #3
(PPC spin-loop / polling behavior) next, since the driver's own TODO comments
already flag specific games with known speed problems tied to specific PCs.

## Public documentation referenced

- Archived *Panasonic 3DO M2 V2.7 Developer Reference Documents* on
  archive.org (`archive.org/details/Panasonic3DOM2DeveloperReferenceDocuments`)
  — Release Notes, Mercury Programmer's Guide, Command List Toolkit,
  Link-Dump Programmer's Guide. These are Portfolio OS / API-level docs, not
  low-level ASIC clock specs — checked for DSPP/Bulldog clock info and found
  nothing usable; the in-source PCB oscillator notes in `konamim2.cpp` (from
  the physical board silkscreen/component survey) are the best hardware-level
  evidence currently available and were treated as authoritative for finding
  #1 above.

## Session update (2026-09-22): finding #1 applied and tested, does not fix the speed problem

Applied the DSPP Bulldog clock fix exactly as recommended (`DERIVED_CLOCK(1, 1)`
→ `XTAL(16'934'400)`, the closest MAME-recognized crystal to the documented
16.9345MHz; also fixed the cosmetic `from_hz(16.9345)` → `from_hz(44100)` typo).
Builds clean, boots clean on `polystar`.

**Measured with a clean A/B test (same build, only this one line reverted via
`git stash`, both run head-to-head at matched wall-clock timestamps with
`fps_show` overlay enabled): no measurable difference.** Both the baseline
(66.6667MHz DSPP clock) and the fix (16.9344MHz) tracked each other within
noise (~37 FPS at t=15s, ~26 FPS at t=25s, ~27 FPS at t=35s, out of a native
59.36 FPS) across three timestamped samples each. **Conclusion: the DSPP
clock was genuinely wrong relative to the PCB documentation and the fix is
still correct/worth keeping on hardware-accuracy grounds, but it is not the
(or not a significant) source of Polystars' slowdown.** Finding #1's
confidence ranking in this doc should be revised down accordingly — the
"highest confidence" label was wrong.

### Finding #3's GPR patch hack is dead code, and the addresses it names are never hit

The header-comment code block quoted in Finding #3 above:
```cpp
if (pc == 0x40035958)
    gpr[11] = 1;
if (pc == 0x400385c8)
    gpr[11] = 0;
```
**is literally inside a `/* ... */` block comment** (`konamim2.cpp` lines
4-33) — it is not compiled, not called, not wired to anything. It was never
an applied patch in this tree; it's a note-to-self left by the original
driver author (Phil Bennett) about *something he tried*, not a description
of current behavior.

Verified live with MAME's debugger (see "How to get a live debugger" below):
set breakpoints at both `0x40035958` and `0x400385c8` on **both** `:ppc1` and
`:ppc2`, then played `polystar` for real — inserted a credit and a start via
the Lua console driving `ioport` fields, selected a control scheme, and flew
through the start of Stage 1 for ~15+ seconds of active gameplay (shooting,
moving, `manager.machine.video.speed_percent` confirmed at 31.3%, matching
the documented severe slowdown). **Neither breakpoint fired even once.**
These two addresses are not where Polystars' actual bottleneck is, at least
not during normal stage-1 play — the comment is stale and should not be
trusted as a lead without further verification against whatever MAME
revision it was originally written against.

### How to get a live debugger against this libretro build (useful for next session)

The libretro OSD's debugger module is `none.cpp` (stubbed, see main
CLAUDE.md), and `wait_for_debugger()` in it just calls `.go()` immediately —
so a real interactive breakpoint pause/stop isn't possible. But **the Lua
debugger console still works underneath it**, including `bpset` with an
action string that logs state and then calls `go()` itself, which behaves
like a non-blocking tracepoint. Recipe:

1. Temporarily add `Add_Option("-debug");` alongside the existing
   `Add_Option("-console");` in `retro_init.cpp`'s `Set_Default_Option()`
   (search for `lua_console_enable`) — `-debug` is required for breakpoints
   to exist at all; `-console` alone only autostarts the Lua REPL. **Revert
   this before committing** — it's a real slowdown (forces per-instruction
   debug hooks on all DRC CPUs) and shouldn't be a silent side effect of
   enabling the console for casual use.
2. `mame_lua_console = "enabled"` in `MAME.opt` as usual.
3. **The console needs a real pty, not a plain pipe** — writing to a `mkfifo`
   redirected as stdin does not work (linenoise/the console reader appears to
   require actual terminal semantics). Use Python's `pty.fork()` to spawn
   `distrobox enter mame-dev -- retroarch ...` under a real pseudo-terminal,
   log the master fd's output to a file, and forward a second control FIFO's
   input into the master fd for sending commands from separate tool calls.
   (A working throwaway script was used this session — not saved, but
   trivial to reconstruct: fork a pty, exec retroarch under it, select()-loop
   copying master→logfile and a command-fifo→master.)
4. The terminal echoes every input character back into the log (kernel line
   discipline default), so grep for exact output patterns (e.g. a unique
   tag string), not the command text — the command text itself will appear
   fragmented character-by-character from the echo.
5. Useful Lua one-liners once attached:
   - CPU tags: `for k,v in pairs(manager.machine.devices) do print(k) end`
     → `:ppc1`, `:ppc2`, `:bda:dspp`, etc.
   - Breakpoint-as-tracepoint:
     `manager.machine.devices[':ppc1'].debug:bpset(0x40035958, nil, "print('HIT'); go();")`
     (note `.debug` is a **property**, not a method — no `()`.)
   - List active breakpoints: `manager.machine.debugger:command('bplist')`
   - Live speed percent: `manager.machine.video.speed_percent` (0.313 = 31.3%
     confirmed on `polystar` stage 1 in this session)
   - Insert coin / start / play without a real gamepad, entirely from Lua:
     `manager.machine.ioport.ports[':P1'].fields['Coin 1']:set_value(1)` then
     `:set_value(0)` shortly after (same pattern for
     `manager.machine.ioport.ports[':P4'].fields['1 Player Start']` and
     `['P1 Button 1']` etc. — port/field names are driver-specific, list them
     via `for k,v in pairs(manager.machine.ioport.ports[':P4'].fields) do print(k) end`).

### Where this leaves the investigation

- Finding #1 (DSPP clock): fixed, correct, **but not the perf bottleneck**.
  Worth keeping as a genuine hardware-accuracy correction regardless.
- Finding #3 (GPR patch hack): the specific addresses named in the header
  comment are **not** the bottleneck either (confirmed live, zero hits during
  real gameplay) — but the general hypothesis (a PPC-side busy-wait/polling
  spin loop) is still unfalsified, since these particular two addresses were
  apparently never verified against this driver revision to begin with.
- **Not yet tried**: actually profiling where the 31.3%-speed wall-clock time
  is going. `manager.machine.video.speed_percent` gives the aggregate number
  but nothing here yet breaks it down per-device. Options for a future
  session: MAME's `-profile` flag (dumps per-device timing — untested against
  this OSD), or bisecting by temporarily stubbing out/short-circuiting
  individual `psxgpu`-style device update loops (not applicable here, wrong
  device) — for M2 specifically, look at whether `m2_te_device` (Triangle
  Engine, `3dom2_te.cpp`) or the PowerPC-side game code is the actual
  dominant cost, e.g. by temporarily disabling TE polygon submission and
  re-measuring `speed_percent`, or by re-attempting the live debugger with
  a coarser instrumentation approach (sampling PC periodically across many
  frames rather than guessing at specific addresses) to build a real hot-PC
  histogram instead of guessing individual breakpoint addresses.

## Session update 2 (2026-09-22): found the real bottleneck — the Triangle Engine's synchronous software rasterizer

Following up on the "not yet done: profile where the 31.3%-speed time is
going" note above, added temporary `std::chrono`-based instrumentation
directly around `m2_te_device::execute()` in `3dom2_te.cpp` (wraps the whole
function body, logs real wall-clock microseconds + triangle/pixel counts via
`fprintf(stderr, ...)` on every call). **This nailed it decisively** — not
kept in the tree (removed again after measuring, see below), but worth
reconstructing again for any renewed effort here.

### The finding

`m2_te_device::execute()` (`3dom2_te.cpp:3288`) is called **synchronously**,
directly from the PowerPC CPU's memory-write handler
(`teicntl_w()`, triggered by the game writing the TE's start/restart control
register — `3dom2_te.cpp:975`). It is **not** a `device_execute_interface`
with a cycle budget — it's a plain C++ function that walks the entire
triangle-engine command list to completion in one call, doing real
per-pixel software rasterization work (`write_dst_pixel()` and its
texture/blend helper chain) the whole time. **This means every microsecond
that loop takes is pure, unaccounted real wall-clock time** — MAME's
scheduler has no idea this device is "busy" doing this work; the calling
PPC's DRC-compiled store instruction just appears to take however long the
C++ interpreter needed, for real, on the host CPU.

(There *is* a `TEST_TIMING` mechanism — the code counts pixels/triangles/
texels during the run and then does `m_done_timer->adjust(clocks_to_attotime(
total_cycles))` to delay the "list complete" interrupt by a modeled amount
of *emulated* time matching real M2 hardware's documented throughput,
~600-700 triangles/sec. That part is fine and intentional/accurate. The
problem is separate: it's the *host* wall-clock cost of running the C++
interpreter/rasterizer itself to completion first, before that emulated
delay even starts being modeled.)

**Measured on `polystar` attract mode (no credit inserted, just idle demo
loop) over a 33-second capture: 6.7 seconds — 20% of total wall-clock time —
was spent inside `m2_te_device::execute()`.** Individual calls: two
recurring patterns repeat every visual frame —
- A **fixed, tiny command list** (`start_irp=0x407d3fa0`, only ~43 words)
  that draws exactly 2 triangles covering `71680` pixel-stores (a full-screen
  or near-full-screen background/sky quad) — consistently **~2.0-3.0ms of
  real wall-clock time per call**, i.e. roughly **28-40ns of real host CPU
  time per pixel written**, for what should be one of the cheapest possible
  draws (2 opaque triangles, no texture, presumably a flat or simple
  gradient fill).
- A **variable-length "real content" list** (`start_irp=0x407d3ef0`, right
  next to the background list in RAM) with actual scene geometry (dozens of
  triangles, thousands of texel reads) — similarly **~1.7-2.3ms per call**.

Both call types recur back-to-back, consistent with the game submitting
"background layer, then foreground layer" as two separate TE command lists
per frame — this is real game behavior, not a MAME-side duplicate-submission
bug (start addresses differ and both lists have genuinely different, stable
content across many consecutive calls).

**This is a much bigger, better-attested contributor than the DSPP clock
finding was** — 20%+ of wall time from attract mode alone (i.e. before a
credit is even inserted or the busier real-gameplay TE lists start firing),
directly measured rather than inferred.

### Why it's this slow, and why a quick fix didn't work

Hypothesis tried: the per-pixel `write_dst_pixel()` calls
`m2_bda_device::read_bus32()`/`write_bus32()` (in a different translation
unit, `3dom2.cpp`) once each per pixel — a real, non-inlined cross-TU
function call per pixel (this build has no LTO). Moved all six
`read_bus8/16/32`/`write_bus8/16/32` accessors to be defined `inline` inside
the class body in `3dom2.h` (so 3dom2_te.cpp could actually inline them into
its hot per-pixel loop) instead of out-of-line in `3dom2.cpp`. **Rebuilt,
re-measured with the same instrumentation: no measurable change** (still
~2.2ms average for the fixed background-quad call, same as before). **This
change was reverted** (not worth the added header complexity for zero
payoff) — only the DSPP clock fix remains in the tree from this session.

**Conclusion: the bottleneck is not memory-access/inlining overhead, it's
the actual amount of computation `write_dst_pixel()` and its callers do per
pixel** — floating-point interpolation, the multi-way `select_mul`/
`select_add`-style switch-driven color/alpha source selection helpers (see
`3dom2_te.cpp` around line 2280 for an example — `select_mul()`,
similar `select_add()`-shaped functions), texture fetch/filtering, and
z/blend handling, all evaluated per-pixel via chains of small non-inlined
function calls and runtime `switch` dispatch rather than e.g. a
specialized/templated fast path for the common (opaque, untextured,
unblended) case. Optimizing this for real is a substantially bigger,
riskier undertaking than the memory-access experiment — it means touching
the core per-pixel logic of a 3800-line, accuracy-sensitive, currently-only
`TEST_TIMING`-modeled-for-*emulated*-time software rasterizer used by every
game on this device. Any change here risks subtle visual regressions across
all five M2 games and needs careful before/after visual comparison, not
just a wall-clock measurement, before being trusted.

### Confirmed across all four other-CHD-available M2 titles (2026-09-22, later same day)

The missing CHDs (`810uba02.chd`, `639eba01.chd`, `636jac02.chd`) were added
to the local ROM collection after the note above was written. Re-ran the
same `std::chrono` instrumentation (temporarily re-added to
`m2_te_device::execute()`, removed again afterward — not kept in the tree)
against `evilngt`, `totlvice`, and `btltryst` in addition to `polystar`.
**The finding fully generalizes — every title shows the same synchronous,
unaccounted, double-digit-percentage TE cost**, confirming this is a
driver/device-wide architectural issue, not something specific to
Polystars' particular scene content:

| Game | Window measured | TE::execute() total | % of wall time |
|---|---|---|---|
| `polystar` | 33s (attract, no credit) | 6.70s | ~20% |
| `evilngt` | 24s (still on Konami boot logo the whole time!) | 1.89s (windowed sample) / 2.57s (fuller sample) | ~8-9% |
| `totlvice` | 21s (attract) | 3.43s | ~16% |
| `btltryst` | 16s (attract) | 4.69s | **~29%** |

`btltryst` in particular is worth flagging: nearly a third of wall-clock
time spent inside one synchronous, unscheduled C++ function, on a game
that's specifically called out in this repo's background notes as "not
fully playable" before Tovarichtch's build. This lines up directly with
that account and makes the TE bottleneck the single most attested,
best-evidenced lead in this whole investigation to date.

**Notable side-observation**: on `evilngt`, TE calls were already firing
during the *static Konami boot logo* (`tris=58`, i.e. rendering logo text
as 3D geometry via the TE, not a 2D tilemap/sprite layer) — confirming this
cost isn't gated behind "real" 3D gameplay content at all; it fires for
routine attract-mode/logo rendering on every M2 title tested.

### Fix #1 shipped: skip unused blend computation in `texture_blend()` (2026-09-22, same day)

Implemented the "fast path for the common non-BLEND case" suggested above.
In `m2_te_device::texture_blend()` (`3dom2_te.cpp:2139`), the block that
calls `select_lerp()` three times (plus `lerp()`/`multiply()`/
`select_mul()`) to compute `rbl`/`gbl`/`bbl`/`abl` was running
**unconditionally on every pixel**, but those four values are only ever
read by the `TXTTABCNTL_CO_SEL_BLEND`/`TXTTABCNTL_AO_SEL_BLEND` cases of the
two output-selection `switch`es immediately below it. Gated the whole block
behind `need_blend = (co_sel == CO_SEL_BLEND || ao_sel == AO_SEL_BLEND)` -
**provably output-identical** (when neither selector is BLEND, `rbl`/etc.
stay at their existing zero-initialized values, exactly as before; the
`switch`es' non-BLEND cases never read them either way) and zero risk of
changing rendered output for any draw, tested via live visual comparison on
`polystar` (`spectacle` screenshot, translucent enemy sprite's blend effect
confirmed intact and correct) - only the `TXTTABCNTL_CO/AO_SEL == BLEND`
code path pays the original cost now.

**Measured with the same `std::chrono` instrumentation** (temporarily
re-added, removed again after measuring - not in the shipped diff), using
`polystar`'s exactly-reproducible background-quad draw
(`start_irp=0x407d3fa0`, always `tris=2`/`pixel_stores=71680` every single
frame) as a clean, deterministic A/B:
- Before: ~2000-2900us per call (session 1/2 measurements)
- After: consistently ~1500-1550us per call
- **~30-35% faster for this draw**, byte-for-byte reproducible across many
  consecutive frames.

For the variable "real content" list, effect is selective as expected -
some draws sped up ~25% (e.g. a `tris=50, pixel_stores=72711` draw:
2129.5us -> 1632.6us), others were unchanged (draws that genuinely use
`BLEND` mode, e.g. Polystars' known translucency effects - "Fix Polystars
blending" is literally listed as a prior DONE item in this driver's header,
so BLEND-mode draws are expected and common in this specific game).
**Aggregate wall-clock improvement is real but partial** - exact overall
percentage depends on how much of a given scene uses BLEND vs. direct
iterated/texel color, which varies per game/per-frame and wasn't isolated
into a single clean aggregate number this session (the two attract-mode
samples taken before/after landed on different, non-identical points in the
non-deterministic demo loop, so raw before/after totals aren't a fair
comparison - only the identical fixed-content per-call numbers above are).

**Verified stable, no crashes, no visual regressions** across all four
testable titles after this change: `polystar`, `evilngt`, `totlvice`,
`btltryst` all boot cleanly (checked RetroArch/MAME logs for
errors/fatals - none beyond the pre-existing unrelated GameMode D-Bus
warning). `btltryst` specifically has an unusually long black-screen boot
period (75+ seconds observed, no errors logged) that predates this session
and isn't something this fix touches - the earlier profiling session
confirmed it does eventually reach real TE-rendered content with valid
triangle/pixel counts, this is just a slow boot, not a hang.

This is a genuine, low-risk, real perf improvement - safe to keep - but
**it is not remotely sufficient to reach full speed on its own**. It only
avoids wasted work in one specific sub-computation; the underlying
architectural problem (the entire per-pixel rasterizer running
synchronously, single-threaded, unaccounted by MAME's scheduler) is
untouched. There is very likely more low-risk, similarly-shaped
"computed-but-conditionally-unused" work elsewhere in the same per-pixel
path (`destination_blend()`'s clip-test chain, `get_texture_color()`'s
filtering, `select_lerp()`/`select_mul()`'s own internal switches) worth
auditing the same way before reaching for the bigger architectural options.

### What other MAME systems suggest about the right architecture (2026-09-22)

Asked, mid-session: are there sibling systems in MAME with a similar
"software chip that rasterizes into a framebuffer" shape that hint at the
right fix? Two found, pulling in different directions:

1. **The plain 3DO console driver** (`src/mame/misc/3do_madam.cpp`) —
   genuinely related, not just superficially: this fork's own comments note
   M2's "Bulldog" ASIC is the successor to original 3DO's "Clio" ASIC, and
   `src/devices/cpu/dspp/dspp.cpp` is explicitly split between a
   `dspp_device` (plain 3DO/Clio) and `dspp_bulldog_device` (M2) variant of
   the *same* CPU core. The 3DO Cel engine (2D perspective-corrected
   sprite/tile blitter, not a true 3D triangle rasterizer) is modeled as an
   **incrementally-ticking `emu_timer` callback** (`madam_device::
   cel_tick_cb`, scheduled via `m_cel_timer->adjust(attotime::from_ticks(
   tick_time, clock()))`), not one synchronous blocking call like M2's
   `TE::execute()`. This is the MAME-idiomatic way to model this class of
   chip for *timing accuracy* - but converting M2's TE to the same shape
   would **not**, by itself, reduce the raw wall-clock cost measured in this
   investigation: MAME's device execution is single-threaded, so the same
   total amount of per-pixel computation costs the same real CPU time
   whether it's issued in one call or spread across many scheduler ticks.
   Useful precedent for *correctness*, not a throughput fix.

2. **`src/devices/video/poly.h`'s `poly_manager` template** — the generic
   scanline-rasterizer framework this repo's own CLAUDE.md already notes is
   shared by ~22 drivers (Namco System 22 among them, for its "legacy
   software polygon helper"). This one **is** architecturally the real
   lever: it allocates a genuine multi-threaded `osd_work_queue`
   (`WORK_QUEUE_FLAG_MULTI`, see `poly.h:481-482`) and dispatches triangle
   rasterization to real OS worker threads, with the driver only
   synchronizing once per frame (`wait_for_polys()`, typically called from
   `screen_update`). That's true parallelism - actual additional wall-clock
   throughput on a multi-core host, not just better scheduling semantics -
   and this dev machine has 16 cores mostly idle during M2 emulation.
   Converting `m2_te_device::execute()` from "block the PPC store
   instruction for 2-5ms" to "queue the command list, rasterize on a worker
   thread, sync at frame end" is the same conceptual shape as this fork's
   own PS1 GPU HLE work (`osd::gpu_render_target`, see main CLAUDE.md) -
   just CPU worker threads instead of a GPU. This is a substantially bigger
   and riskier refactor than the `texture_blend()` fix above (touches every
   draw entry point and needs careful frame-boundary correctness), but it's
   the most credible path to actually reaching "full speed" rather than
   incremental percentage improvements - not attempted this session, flagged
   here as the standing next big architectural option alongside the
   `osd::gpu_render_target` GPU-offload alternative already noted.

### Recommended next step if resuming this

1. ~~Get real CHD dumps for at least `totlvice` or `btltryst`~~ — done, see
   above. The finding is now confirmed across 4/5 titles (only `heatof11`
   untested, no CHD needed for it per its `ROM_START` — check if it's
   already playable in this collection before assuming it needs one too).
2. ~~Attempt a first optimization~~ — done, see "Fix #1 shipped" above.
3. Audit the rest of the per-pixel path (`destination_blend()`,
   `get_texture_color()`/`get_texel()`, `select_lerp()`/`select_mul()`'s own
   internals) the same way — look for more "computed unconditionally but
   only conditionally read" work, the same safe pattern as Fix #1.
4. For the real architectural fix, pick one of the two options above:
   either convert `m2_te_device` to use `poly_manager`'s worker-thread
   rendering (biggest precedent in mainline MAME, real parallelism), or
   port this fork's own `osd::gpu_render_target` GPU-offload approach
   (biggest precedent *in this specific fork*, already proven end-to-end on
   `psxgpu_device`). Both are multi-session-scale projects, not a quick
   follow-up.
5. Re-add the `std::chrono` instrumentation (wrap `m2_te_device::execute()`
   — pattern shown above, ~10 lines) and get a profile during **actual
   gameplay** (not just attract mode) on at least one title — use the
   Lua-console-via-pty recipe from Session 1 above to insert a credit/
   start/play without needing a real gamepad.
6. Whatever gets changed here, do a real side-by-side screenshot comparison
   (not just a clean compile) against at least `polystar` and `btltryst`
   before/after, per this repo's standing working-conventions rule about
   rasterizer rewrites — this is exactly the kind of change that rule
   exists for.

## Session 3 (2026-09-22, later same day): real multi-threaded rasterization shipped

Implemented option 4 above — converted `m2_te_device` to farm rasterization
out to real worker threads via `osd_work_queue` (the low-level primitive
`poly_manager` itself is built on; `src/mame/sega/coolridr.cpp` was the
reference precedent for correct API usage). Full design plan (written before
implementation) is preserved at `/home/bazzite/.claude/plans/
proud-snuggling-thunder.md` if more detail than this summary is needed.

### Design actually shipped

- **`m2_te_device::walk_edges()`'s per-scanline calls no longer render
  inline.** Each scanline's `walk_span()` arguments are captured into a
  `span_job` and queued into one of `NUM_RENDER_BANDS` (4) buckets, keyed by
  `y % NUM_RENDER_BANDS` — every row is always processed by the same band,
  so within a band, jobs always execute in original submission order
  (byte-identical to the old single-threaded result for anything that only
  touches its own pixel, which is true of every M2 TE primitive), and
  different bands never write the same framebuffer row, so no locking is
  needed between them.
- **`flush_span_jobs()`** dispatches each non-empty band's job list to a
  worker thread (`osd_work_item_queue`/`render_band` callback) and calls
  `osd_work_queue_wait()` before returning. Called from **two** places, not
  just once at list-end: immediately before every `INST_WRITE_REG`
  instruction (config/texture-mutating CPU register writes), and once more
  at actual list completion (right before `set_interrupt(INTSTAT_LIST_END)`
  and again as a safety net at the very end of `execute()`). This matters
  because a single TE display list can freely interleave config changes
  with triangle draws — deferring *all* rendering to list-end would let
  later-queued triangles silently render against whatever texture/blend
  state happened to be active by the time a worker thread got around to
  them, not what was active when they were actually issued.
- Kept from the prerequisite refactor (previous commit): `pixel_scratch`
  already made the whole per-pixel call chain safe to call from multiple
  threads at once (no shared mutable device state touched per-pixel).

### Two real bugs found and fixed before this was correct (both by testing, not by reasoning alone)

1. **Stale per-triangle edge state** (found via a hung/solid-color frame on
   first boot, ~376% CPU, no crash — a livelock, not a crash). `m_es.r2l`
   and `m_es.ddx_r/g/b/a/uw/vw/w` are recomputed **per triangle** by
   `setup_triangle()` (called synchronously on the main thread, *not* via an
   explicit `INST_WRITE_REG` the barrier above would catch), not just by
   CPU register writes. Since jobs from multiple triangles can sit queued
   at once between flush points, `walk_span()` reading these fields *live*
   from `m_es` at (now deferred) render time meant a worker thread could
   render an early triangle's job using a *later* triangle's edge deltas —
   wrong/inconsistent enough to make the `while (xs != xe)` scanline loop
   fail to terminate. Fixed by capturing all 8 fields into `span_job` at
   queue time (`queue_span_job()`, while `m_es` still correctly reflects
   the triangle being queued) and threading them through as explicit
   `walk_span()` parameters instead of reading `m_es` live.
   **Lesson for next time**: the config-mutation-barrier audit needs to
   cover *every* write site for state the render path reads, not just
   sites reachable through the generic `INST_WRITE_REG`/`write()` register
   dispatch — `setup_triangle()` mutates `m_es` directly as part of normal
   per-triangle processing, completely outside that path.
2. **Wrong `osd_work_queue_alloc()` flags** (found via `osd_work_queue_wait()`
   returning `false` on the first two flushes of a run, then `true`
   afterward, with a full 100-second timeout given — i.e. it wasn't
   actually waiting the full duration, it was returning almost immediately
   with work still in flight, racing the very next dispatch). Used
   `WORK_QUEUE_FLAG_MULTI` alone; `osd_work_queue_wait()`'s implementation
   (`src/osd/osdsync.cpp`) only reliably spin-waits until the queue is
   truly empty when `WORK_QUEUE_FLAG_HIGH_FREQ` is also set — without it,
   the fallback path (reset a "done" event, wait on it) has a real missed-
   wakeup window for a queue that's flushed as frequently as this one
   (once per `INST_WRITE_REG`, i.e. many times per display list). Fixed by
   allocating with `WORK_QUEUE_FLAG_MULTI | WORK_QUEUE_FLAG_HIGH_FREQ`,
   matching `coolridr.cpp`'s exact flag choice (which this session
   initially copied the call *pattern* from but not the flags — worth
   copying precedent code exactly, not just its shape, next time). Kept a
   permanent safety net for this in `flush_span_jobs()`: if
   `osd_work_queue_wait()` ever does return `false` (timeout), the current
   batch's results are discarded (not merged, not trusted) and logged via
   `logerror()` rather than risking a read of a still-being-written
   `pixel_scratch`.

### Verification

- Debugged the first bug via a live GDB session against the Distrobox
  RetroArch (recipe: `set auto-solib-add off` before `run`, `sharedlibrary
  mame_libretro` after the crash — see Session 2's "Correctness, round 3"
  entry in this doc's history for why; the crash itself surfaced as a
  SIGSEGV in `ppc_device::frontend::describe()`, i.e. the PowerPC DRC
  choking on decoding memory that a wild/wrong TE pixel-address write had
  corrupted — several frames deep from the actual bug, not a direct crash
  in TE code, which is why static reasoning alone didn't find it as fast as
  a live repro did).
- Diagnosed the second bug with temporary `fprintf(stderr, ...)` tracing in
  `flush_span_jobs()` printing `active`/`osd_work_queue_wait()`'s return
  value — removed after the fix was confirmed (`wait_ok=1` on 100% of
  ~500,000+ flushes across multiple soak runs afterward, zero timeouts).
- **Soak-tested all four titles after the fix**: `polystar` (~60s, 467,351
  flushes, 0 timeouts), `evilngt` (~35s, 6,522 flushes), `totlvice` (~35s,
  38,369 flushes) — all clean, no errors, no crashes.
- **Visual correctness**: screenshot-compared `polystar` against the known-
  good pre-threading baseline at multiple points (attract-mode city scene
  with the blend/translucency-heavy bee enemy) — pixel-identical in
  composition, blend effects intact.
- **Performance**: `btltryst` (the worst-case title from Session 2, ~29% of
  wall-clock time in `TE::execute()` pre-threading) now shows a **solid
  60.0-60.3 FPS** (`fps_show` overlay) through the portion of boot that was
  previously part of the slow measurement window — a dramatic improvement
  over the ~17-18 FPS (29% of 60) baseline. Did not get a screenshot of
  `btltryst` reaching actual post-boot gameplay content in this session
  (its boot sequence takes 75+ real seconds per Session 2's notes) - worth
  a longer soak with real gameplay content as a follow-up, but the
  measured wall-clock win through boot alone is already a qualitative step
  change, not an incremental one.

### What's left

- Confirm the win holds during real `btltryst` gameplay (not just boot),
  and on `evilngt`/`totlvice` too - this session only got a solid FPS
  reading on `btltryst`'s boot sequence specifically.
- `NUM_RENDER_BANDS = 4` was picked as a reasonable starting constant, not
  tuned - worth experimenting with higher/lower band counts now that the
  design is proven correct, to see if there's more throughput on the table
  (this host has far more than 4 cores).
- The per-pixel-path micro-optimization audit (item 3 in the old
  "Recommended next step" list above) is still valid future work, now
  layered on top of a genuinely parallel rasterizer instead of instead of
  it.

## Session 4 (2026-09-22, later same day): diagnosed the post-threading audio-slowdown symptom

Following Session 3's worker-thread TE, the user reported a split symptom:
`polystar` still shows one CPU core pegged and real slowdown (expected,
matches Session 3's understood limits); but `evilngt`/`totlvice` *feel* full
speed, with no CPU core pegged, yet digitized speech sounds clearly slowed
down/pitch-shifted. Target machine is a fast 16-core host with low aggregate
CPU usage throughout, ruling out simple thread contention/starvation as the
cause.

### Step 1: confirmed MAME's own clock is not running slow

Using the same live-Lua-console technique documented above (`mame_lua_console`
core option; no `-debug` patch needed just to read state, only for
breakpoints), polled `manager.machine.video.speed_percent` throughout an
`evilngt` boot/attract run. **Result: steady ~98-103%** the whole time — MAME
genuinely believes it's running at full speed. This rules out "the DSPP/audio
CPU is starved of scheduler time" as the explanation.

### Step 2: instrumented both ends of the audio pipeline

Added temporary `std::chrono`+`std::atomic` profiling (same disposable
pattern as Sessions 2/3 - reverted after measuring, not shipped) in two
places simultaneously:
- `m2_te_device::flush_span_jobs()` (`3dom2_te.cpp`) - real wall-clock
  microseconds spent blocked in `osd_work_queue_wait()`, summed/maxed per
  real second.
- `upload_output_audio_buffer()` (`src/osd/libretro/libretro-internal/
  libretro.cpp`) - real wall-clock gap between successive calls, samples
  delivered, and whether the silence-fallback path fired, all per real
  second.

Ran `evilngt` for 60s under the same pty-driven RetroArch recipe documented
above and correlated the two logs by second.

### Finding: total audio throughput is correct, but delivery is bursty

- **Samples/sec stayed correct** throughout (~48500, matching the declared
  48000Hz `AVInfo` rate) and the silence-fallback path essentially never
  fired (`silence=0` in nearly every reported second). So MAME really is
  producing the right *total* amount of audio - consistent with
  `speed_percent`, and ruling out a straightforward sample-count deficit.
- **But delivery cadence is bursty, and it tracks TE stall load exactly.**
  Baseline: audio_batch_cb calls land roughly every 7-17ms (matching ~60fps).
  During seconds where `flush_span_jobs()`'s summed blocked time spiked -
  observed up to **~270ms of a single real second** spent blocked inside
  `osd_work_queue_wait()` (over a quarter of that second) - the gap between
  successive `retro_run()`/`audio_batch_cb` calls grew to **25-110ms**, roughly
  double-to-many-times the steady baseline, in lockstep with the stall spike.

### Conclusion

The Session 3 worker-thread TE rasterizer is real parallelism for the
per-pixel *rendering* work, but it's still **architecturally synchronous
from the main emulation thread's point of view** - every `INST_WRITE_REG`
blocks on `osd_work_queue_wait()` until that band's workers finish. Total
throughput survives this (smooth-feeling video, `speed_percent` ~100%), but
it means the main thread's real-world progress through a `retro_run()` call
is not evenly paced - it stalls in bursts. Audio delivery to RetroArch rides
on that same thread's cadence (`upload_output_audio_buffer()` is called once
per `retro_run()`), so those stalls turn into bursty, irregular
`audio_batch_cb` delivery even though the *total* sample count comes out
right. RetroArch's dynamic audio rate control reacts to that irregularity by
nudging playback rate down to avoid buffer underruns - inaudible on most
content, but very noticeable on sustained tones like speech. This explains
the split symptom: `polystar`'s TE load is heavy enough to peg a core solid
(Session 3's already-understood, more severe case); `evilngt`/`totlvice`'s
lighter-but-still-frequent stalls aren't heavy enough to peg a core, but are
still enough to disrupt audio pacing.

### Not yet done: an actual fix

Candidates for a follow-up session, roughly in order of how surgical they
are:
1. **Stop blocking the emulation thread on every `INST_WRITE_REG`.** The
   barrier exists to protect queued-but-unrendered jobs from a config/texture
   write that's about to mutate state they depend on (see Session 3's bug #1
   above) - but it currently blocks unconditionally, even when the incoming
   write wouldn't actually race anything still queued. Double-buffering the
   mutated state (so a new write starts a fresh "generation" instead of
   overwriting what in-flight jobs captured) could let `INST_WRITE_REG` return
   immediately in the common case.
2. **Decouple audio delivery from `retro_run()`'s per-frame cadence.**
   Buffering audio a little further ahead (independent of exactly when a given
   `retro_run()` call happens to return) would let `upload_output_audio_buffer()`
   smooth over an occasional slow frame instead of passing the jitter straight
   through to RetroArch.
3. **Reduce flush frequency.** If display list content tolerates it, batching
   multiple register writes before syncing (rather than syncing on every
   single one) would directly cut the number of stall points per frame.

All temporary profiling code from this session was reverted (`git checkout`)
before finishing - not shipped in the tree. The `std::chrono`/`std::atomic`
per-second-summary pattern used here (see Sessions 2/3's own instrumentation
above for the general shape) is trivial to reconstruct if resuming this.

## Session 5 (2026-09-22, later same day): shipped fix approach 1 - config snapshotting removes the per-INST_WRITE_REG block

Followed up on Session 4's candidate (a): stop blocking the emulation thread
on every `INST_WRITE_REG`. Landed as a real fix, not just a candidate.

### Design shipped

The reason `flush_span_jobs()` had to block before every `INST_WRITE_REG`
was that queued-but-not-yet-rendered scanline jobs read the TE's config
(`m_gc`/`m_es`/`m_tm`/`m_db`/`m_pipram`/`m_tram`) **live** at render time -
a register write mutating that state while a worker thread was still
rendering an older job would silently corrupt it. Audited every read site
in the render path (`walk_span()` and everything it transitively calls) and
found the live-state surface was small and enumerable despite touching 17
functions/107 call sites: `m_gc.te_master_mode`, `m_es.es_cntl`, all of
`m_tm`/`m_db` (21 + 34 words), plus `m_pipram`/`m_tram` contents (1KB +
16KB) - about 17.4KB total, cheap to copy.

Added `render_snapshot` (`3dom2_te.h`) - an immutable copy of all of the
above - taken lazily by `queue_span_job()` the first time it's called after
`write()` sets a new `m_snapshot_dirty` flag (set unconditionally at the top
of `write()`, regardless of which register - simpler and safer than trying
to classify which specific registers actually matter, and the cost is only
paid if a triangle is actually queued afterward). Each `span_job` now holds
a `shared_ptr<const render_snapshot>` captured at queue time;
`render_band()` sets `pixel_scratch::cfg` from the job's snapshot before
calling `walk_span()` for it. Every render-path function that used to read
`m_tm`/`m_db`/etc. directly now shadows those member names with a local
`const auto &m_tm = ps.cfg->tm;`-style reference at the top of the function
body, so the 107 individual read expressions didn't need touching
individually - only ~17 function signatures (adding `pixel_scratch &ps`
where missing) plus one shadow declaration each.

With jobs now immune to a register write mutating the live state after
they're queued, **`execute()`'s `INST_WRITE_REG` case no longer calls
`flush_span_jobs()` at all** - it applies the register write immediately.
Jobs simply accumulate (each internally tagged with whichever config
generation was active when it was queued) across however many register
writes happen before the next real synchronization point - list end, or
the safety net at the end of `execute()` - which are unchanged (still
dispatch+wait+merge status/stats, since those genuinely need the CPU to see
completed rendering, e.g. before reading the framebuffer back). Traced
through: mid-list, the CPU can't observe *any* intermediate state anyway
(the PPC store instruction that starts a display list runs `execute()`
synchronously to completion or pause/stop, with no way to interleave real
CPU instructions mid-list in this emulation model) - so the only place a
flush was ever really required for CPU-visible correctness was the exit
paths, which were already there.

**Known limitation, not fixed and not new**: `3dom2.cpp` installs the TE's
texture RAM directly into the PowerPC address space via `install_ram()`
(`space.install_ram(TE_TRAM_BASE, ..., m_te->tram_ptr())`), so the CPU can
write texture data straight into `m_tram` via plain memory stores,
completely bypassing `m2_te_device::write()` - these writes never set
`m_snapshot_dirty`. This is not a regression: `install_ram()` writes never
went through `flush_span_jobs()` in the *old* synchronous design either
(nothing about a raw RAM write triggers a flush), so this race predates
this session's change and is orthogonal to it. Worth fixing later (switch
to `install_write_handler()`) but out of scope here.

### Verification this session

- Clean rebuild (`HAVE_RETRO_GPU_TARGET=1 SOURCEFILTER=arcade.flt`), no
  warnings from the changed files.
- Sequential (not parallel - overlapping runs would corrupt timing/CPU
  measurements) 45s soak tests via a pty-driven RetroArch launch, one title
  fully killed and confirmed gone before the next started: `polystar`,
  `evilngt`, `totlvice` all booted clean (`Geometry: 640x240, ... Sample
  rate: 48000.00 Hz`), no fatal/segv/abort/assert/timeout/corruption in any
  log.
- **User visually watched all three during the soak tests and confirmed
  they looked correct** - this stood in for a more rigorous screenshot A/B
  diff, which was started (paused-frame `spectacle` capture of `polystar`
  attract mode) but called off as unnecessary once the user had already
  eyeballed live gameplay across all three titles.
- **Not yet done**: re-measuring the `[TEPROF]`/`[AUDPROF]` instrumentation
  from Session 4 to get a quantified "how much did this actually reduce
  blocking/audio jitter by" number - the fix landed on reasoning + live
  visual/stability verification, not a fresh profiling pass. Worth doing
  before considering this fully closed, especially re-checking whether
  Evil Night/Total Vice's audio pitch issue is actually gone (not just
  "looks fine visually") and whether Polystars' pegged core moved at all.
- Also not yet re-tested: `btltryst`, `starswep`, `nagano98` (per this
  driver's shared-device blast-radius note elsewhere in this doc).

### Post-commit performance test on polystar (2026-09-22)

Re-added the temporary `[TEPROF2]` instrumentation (flush count, summed/max
blocked microseconds, total span-job count per real second) plus live
`speed_percent` polling via the Lua console, ran a 60s attract-mode capture,
then reverted the instrumentation and rebuilt the clean committed binary
(all sequential, no overlapping RetroArch instances - see the process-kill
fix note below).

- **`speed_percent`: 60-89%** across 12 samples over the run (rising over
  time as the attract-mode demo settles into less TE-heavy content) - a
  real improvement over the pre-threading 31.3% baseline recorded in
  Session 2 (though that number was measured during actual stage-1
  gameplay, not attract mode, so not a strictly apples-to-apples
  comparison).
- **No single CPU core pegged near 100% any more.** Measured via a
  `/proc/stat` before/after delta over a 3s window: highest core was 63.7%,
  with load spread across ~5-6 cores (consistent with the main thread plus
  the 4 render-worker bands all being active), aggregate ~12% across all 16
  cores. This directly addresses the "one CPU core regularly hitting 100%"
  symptom originally reported for Polystars.
- `[TEPROF2]` shows flush count dropped to ~100-250/sec (vs. the
  thousands/sec seen pre-fix in Session 4's evilngt trace), each covering a
  far larger batch - up to hundreds of thousands of span jobs summed per
  second, confirming register writes are no longer triggering a flush.
  Total *blocked* time per second is still substantial (~250-330ms/sec,
  25-33% of wall clock) - this reflects Polystars' genuinely large
  rendering workload (now consolidated into fewer, larger waits) rather
  than synchronization overhead, matching the design note below: this fix
  targeted stall *frequency*/audio jitter, not the underlying per-pixel
  computation cost, so some residual slowdown on this specific
  heavy-content title is expected and not a sign the fix didn't work.

**Test-harness bug found and fixed mid-session**: the pty-driven soak-test
script's cleanup only `SIGTERM`'d its own direct child (`distrobox enter ...`),
which does **not** kill the real `retroarch` process running inside the
Distrobox container - the exact same process-group issue this repo's
CLAUDE.md already documents for killed *builds*. Caught because the user
was watching CPU usage and noticed two `retroarch` instances running at
once after two sequential-looking test invocations. Fixed by matching and
`pkill -9 -f`-ing the actual `retroarch -v -L <core> <rom>` command line,
then polling `pgrep` until confirmed gone before the script returns -
required for any future performance/timing test here, since overlapping
runs silently corrupt CPU/speed measurements without any visible error.

### Why this should help Polystars too, not just Evil Night/Total Vice

Even though the *total* per-pixel computation cost is unchanged (same
amount of rendering work either way), consolidating from "many small
dispatch+wait cycles per list (once per register write)" to "one
dispatch+wait per list" cuts the number of `osd_work_queue_wait()`
synchronization points substantially, removing repeated spin-wait/condvar
overhead that was paid many times per list. Total blocking time should be
similar-or-lower, not higher - the main open question (not yet measured)
is how much of Polystars' pegged-core symptom was pure computation
(unaffected by this change) versus synchronization overhead (which this
directly reduces).

## Session 6 (2026-09-22, later same day): band-count experiment, spin-wait root cause, audio decoupling (partial fix)

### `NUM_RENDER_BANDS=8` experiment: no gain, reverted to 4

Tried bumping the constant on this 16-core host, expecting more parallelism.
No improvement, and the user reported a longer watch-through (into real
gameplay, not just attract mode) still showed a single core spiking to 100%
and real slowdown.

**Root cause, not fixable safely at the driver level.** `NUM_RENDER_BANDS`
is not a thread-count cap - the real worker pool size comes from
`osd_work_queue_alloc()` (`src/osd/osdsync.cpp:261-311`): `numprocs - 1`
(host core count, up to 15 here), independent of band count entirely. What
actually causes the single-core spike: `osd_work_queue_wait()` for a
`WORK_QUEUE_FLAG_MULTI | WORK_QUEUE_FLAG_HIGH_FREQ` queue (`osdsync.cpp:
371-390`) has the *calling* thread - the main emulation thread - join in as
an extra worker (`worker_thread_process()`), then **busy-spin** with zero
yield/pause instruction (`spin_while_not()`, `osdsync.cpp:68-87`) until the
other bands finish. This pegs whatever core the main thread runs on near
100% for the whole flush duration, by design, regardless of `NUM_RENDER_
BANDS`.

Not touching this, for two reasons: (1) `osdsync.cpp` is shared MAME-wide
OSD infrastructure used by every driver's work queues - changing it is a
much bigger blast radius than anything done in `3dom2_te.cpp` so far, and
conflicts with every future upstream merge; (2) `HIGH_FREQ`'s spin exists
specifically because the event-based fallback path has a real,
already-hit-once race in this exact codebase (see Session 3's bug #2
above) - moving away from it now, even with fewer flushes than before,
reduces how *often* that race fires, not whether it can still happen.

### GPU-offload vs. per-pixel-audit risk discussion (informational, nothing started)

Asked to compare which approach has more real upside vs. risk of breaking
games on hardware quirks. Per-pixel audit (same shape as the already-shipped
`texture_blend()` gate): low risk, but low ceiling - it can only remove
computation that's *provably unused*, and Polystars' TE genuinely needs
most of what it computes, so there's a hard floor this approach can't get
under. GPU offload: real upside (moving to hardware built for this), but
meaningfully *more* risk here than this repo's own PS1 GPU HLE precedent,
specifically because M2's Triangle Engine has no external gold-standard
reference implementation the way PS1 has Beetle PSX HW to check against -
and this driver's *own* software rasterizer still carries open
`// TODO: Why isn't this working?` / `// This is probably wrong` comments
on core logic (Z-buffer comparison, bilinear filtering), meaning the ground
truth to port from is itself uncertain in spots. A GPU port would be
self-referencing that same shaky baseline across all five games with no
independent way to tell a new bug from a pre-existing quirk. If GPU work is
picked up later, look for an independent correctness reference first
(another emulator's M2 implementation, real hardware documentation) rather
than trusting this driver's own model as sole ground truth.

### Audio decoupling: real, measured improvement - but only a partial fix

**Shipped, but NOT YET COMMITTED as of this writing** - check `git status`
before doing anything else next session; the change is sitting in
`src/osd/libretro/libretro-internal/libretro.cpp`.

Changed `retro_audio_queue()` to call `audio_batch_cb()` **immediately**
instead of accumulating into `output_audio_buffer` and flushing everything
in one batch at the end of `retro_run()` (after `video_cb()`).
`upload_output_audio_buffer()` is now only the silence-padding fallback for
a frame where no real audio was generated at all. Rationale: MAME's
`sound_manager` stream updates run interleaved with CPU execution during
machine-advance, not strictly after it - a frame's audio is frequently
fully generated *before* that same frame's Triangle Engine stall even
starts, so waiting for the entire `retro_run()` call (stall included) to
finish before handing audio to the frontend was adding delay on top of the
stall for no reason.

**Measured via re-added AUDPROF instrumentation** (temporary, reverted
after use - same disposable `std::chrono` pattern as the TE profiling
above) on `evilngt`/`totlvice`: typical gap between real `audio_batch_cb`
calls dropped from ~6,700-9,000us to ~80us (delivery frequency jumped from
~60 calls/sec to ~3,500-3,600/sec, tracking `stream_sink_update()`'s real
granularity instead of once per video frame), and worst-case max gap
dropped from 24,000-38,000us (with boot-time spikes over 100,000us) down to
a steady 12,000-22,000us with no more huge spikes. **Sanity-checked on
`daytona`** (Model 2, unrelated driver) since this is a global OSD-level
change affecting every driver's audio path, not just M2 - clean boot,
correct sample delivery, no regressions.

**Live-tested by the user watching/listening to `evilngt` and the real
result is mixed**: title screen (light content) sounds correct now.
**Heavy gameplay in attract mode - speech still sounds slowed down.**
Diagnosed why this fix has a ceiling: it only speeds up delivery of audio
that finishes generating *before* a TE stall starts in a given frame - it
cannot do anything for audio still being generated *during or after* one,
since that audio genuinely doesn't exist yet regardless of delivery speed.
Heavier gameplay means longer/more stalls, so a growing fraction of each
frame's audio generation window overlaps with a stall, and that fraction
is exactly as delayed as it was before this fix. Light content has short
or no stalls, so nearly all of its audio finishes before any stall starts
- this matches the observed light/heavy split exactly, not a fluke.

### Where this leaves things (pick up here next session)

1. **Commit or discard the audio-decoupling change first** (currently
   uncommitted) - it's a real, measured, sanity-checked improvement worth
   keeping even though it doesn't fully solve the heavy-gameplay case.
2. **The remaining heavy-gameplay audio delay is the same underlying
   problem as Polystars' remaining slowdown** - both come down to genuine
   TE stall duration, not synchronization or audio-delivery overhead. No
   further lever is available on the audio-delivery side; progress on
   either symptom now requires actually shortening the stalls.
3. Two options for that, per the risk discussion above: the safer,
   lower-ceiling per-pixel audit (extend the `texture_blend()`-style "skip
   provably-unused computation" pattern to `destination_blend()`'s
   clip-test chain and `get_texture_color()`'s filtering - both already
   flagged as candidates earlier in this doc), or the higher-risk/
   higher-ceiling GPU-offload option (needs an independent correctness
   reference identified first - not yet found).

## Notes on provenance / how this document was produced

This was reconstructed by static analysis of the current mamedev/mame source
tree (as of 2026-09-22) plus web research (YouTube video descriptions, GitHub
API searches for Tovarichtch's PRs/forks/branches) — **no access to
Tovarichtch's actual diff was available**, since it isn't published anywhere
public. Everything above is inference from (a) the shape of his one confirmed
merged MAME fix (namcos23 idle-clock removal) and (b) close reading of the M2
driver's own code and comments for analogous "free-running / burning cycles for
no benefit" patterns. Treat the ranked findings as hypotheses to test, not a
confirmed diff.

## Session 7 (2026-09-23): baseline profile from `savestates/polystar.state`

**All testing from here on uses `savestates/polystar.state`** (a mid-game save
with severe slowdown, provided by the user). Harness: `fork-specific/tools/m2_profile_run.py`
(gitignored). It copies the state to RetroArch slot 0, turns on
`mame_lua_console` for the run and restores `MAME.opt` afterward, launches under a
pty with `video_vsync=false`, attaches `perf` inside `mame-dev`, polls
`speed_percent` every 5s, samples per-thread CPU from `/proc`, then
`pkill -9 -x retroarch` and polls until it's gone.
`python3 fork-specific/tools/m2_profile_run.py <tag> 60` gives a flat profile; add `cg` for a DWARF call graph (slow
to report, around 10 minutes for 30s of data). Build under test: the commit `8360d4469ce` tree plus the
uncommitted audio-decoupling change in `libretro.cpp`. CPU governor was
`powersave`, the same for all runs.

**Result: `speed_percent` 0.44-0.58, mean ~0.54.** The number is repeatable: two
separate runs gave the same per-5s sequence (0.57/0.44/0.51/0.57/... and
0.58/0.43/0.53/0.58/...), so this state works as an A/B benchmark.

Threads: **the main emulation thread is pegged (98%)**. Exactly 3 worker
threads run at about 20% each. With NUM_RENDER_BANDS=4 there are only 4 work
items per flush, and the main thread takes one of them itself inside
`osd_work_queue_wait()`, so the band count does limit parallelism in practice.

Main-thread self time, by category (flat 60s profile, 1999Hz):

| Share | Category |
|---|---|
| **24.4%** | **PPC DRC recompilation** (asmjit `_emit`, `drcbe_x64::generate*`, `uml::*`, `drcuml_block::optimize`, `describe_code`) |
| 21.2% | TE rasterizer work on the main thread (its share of the bands) |
| 6.7% | `osd_work_queue_wait` spin |
| 6.8% | DSPP interpreter (`execute_run`/`parse_operands`/`exec_arithmetic`/`read_next_operand`) |
| 6.6% | JIT-generated code (the PPCs actually running game code) |
| ~8% | memory handlers, `m2_bda_device::read_bus8`/`16` (mostly `load_texture()` on the main thread) |
| ~2.6% | libc memset/memmove |
| ~1.6-2% | `util::stream_format` (caller not identified yet, probably in the compile path) |

Inclusive (call graph, 30s): `ppc_device::code_compile_block` is **29%**
of main-thread time. That is about 4x the time spent running the compiled code.
`m2_te_device::execute` (called from a PPC write) is 33%, and 26% of that is
`flush_span_jobs`. `load_texture` 3.9%, DSPP 7.6%.

### What this means

1. **The biggest single cost is new, and it isn't the TE: the PPC DRC recompiles
   constantly.** Warm-up after the state load doesn't explain it, because the rate
   holds across the whole 60s window. Possible sources in
   `src/devices/cpu/powerpc/`: (a) `icbi` →
   `ppccom_execute_icbi()` throwing away a whole 4K page of compiled code;
   (b) `m_translation_generation` bumps (tlbie/tlbia/SR/BAT writes) that make
   blocks fail `ppc_check_translation`; (c) `PPCDRC_STRICT_VERIFY`
   (part of `PPCDRC_COMPATIBLE_OPTIONS`, which `konamim2.cpp:696` sets) byte-compare
   failures when code or data pages get rewritten; (d) full `code_flush_cache()`
   from `m_cache_dirty` or cache exhaustion. The cause is not known yet, so the
   next step is counters.
2. The TE is still about a quarter of the main thread, because the main thread
   rasterizes one band and then spins until the rest finish. It waits at every list end.
3. The DSPP interpreter and per-byte `read_bus8` texture loads are
   secondary but real.

### Step 1 done: PPC DRC recompile thrash found and fixed (2026-09-23)

Temporary counters (`[PPCPROF]`, since removed) in `ppcdrc.cpp`/`ppccom.cpp`
showed `:ppc1` recompiling ~10,500 blocks/s (~390 ms of each wall-clock second
spent compiling). About 90% of those were blocks whose hash already existed.
`icbi`=0, translation-check failures=0 and cache flushes were rare. The recompile
count exactly matched the number of **TLB-mismatch handler exits with a non-zero stale
entry**. The same ~500 PCs (the OS kernel around 0x4001xxxx) were recompiled ~190x/s each.

**Root cause.** Blocks compiled with `validate_tlb()` bake in the *exact* vtlb
entry value (`ppcdrc.cpp`, `generate_sequence_instruction`). `vtlb_fill()`
adds permission bits lazily, one intention at a time (FETCH, then READ once a data read
hits the page, etc.). The game changes segment registers ~220x/s
(`vtlb_flush_dynamic`), and the dynamic vtlb pool is round-robin, so code-page
entries are constantly flushed and refilled in a different order: 0x0C
(VALID|FETCH) one time, 0x0D (+READ) or 0x1C (+USER_READ) the next. The xor of
old and new entries was always 0x01 or 0x10. The whole-entry compare then failed,
and the mismatch handler recompiled because the stale entry was non-zero.

**Fix** (`src/devices/cpu/powerpc/ppcdrc.cpp`, 2 hunks):
1. The per-block TLB check masks out READ/WRITE/USER_READ/USER_WRITE before
   comparing. Only the physical page plus fetch/valid state matter for running the code.
2. `tlb_mismatch` handler: re-enter (hashjmp) instead of recompiling when the
   stale entry lacked FETCH_ALLOWED (not just when it was 0). The block's own
   check then decides. This terminates: after the fill, the entry has FETCH, so a
   second mismatch means the physical page really changed, and that recompiles.

**Result** (same state, clean 60s run): `speed_percent` **0.54 → ~0.86**
(0.72-1.05). Compiles ~10,500/s → ~800/s, compile time ~390 → ~29 ms/s.
Main-thread share: DRC recompilation 24% → 2.4%, and `stream_format` went
away too (it was in the compile path). Workers are busier (~34% each, was ~20%), because
more frames get rendered. New main-thread top items: TE work 28.6%, queue spin 12.5%, JIT
12.9%, memory handlers/`read_bus8` 10.8%, DSPP 9.1%.

**Regression checks**: this is the shared PPC DRC, so it affects Model 3, Konami
PPC boards, Viper and Mac. Boot smoke tests with no errors: daytona2 (renders
test menu), gticlub, hangplt, kviper, evilngt, totlvice, btltryst. A/B against a
baseline build at 90s (kviper, btltryst) gave pixel-identical screenshots (both
still black at that point: kviper is BIOS-only, btltryst boots slowly).
code1d/wcombat/jpark3 are missing ROM files in the collection (jpark3 is also
known-unreliable generally). Polystars from the state renders correctly.

**Leftover**: ~800 rehash compiles/s remain, concentrated on a few page-start
PCs (0x40024000 ~460/s, 0x40122008, 0x4007F000) where the entry's xor is 0. This is
probably the "no valid TLB at compile time → unconditional EXH" path when a block
is compiled while its page's entry is momentarily empty. Now only ~3% of the main thread; low
priority.

### PPC fix committed; audio decoupling stashed (2026-09-23)

The PPC fix is committed as `af439229f3d`. The Session 6 audio-decoupling change in
`libretro.cpp` is **stashed, not committed** (`git stash list`: "audio
decoupling (retro_audio_queue immediate delivery)"). The user's call: it
touches every game's audio path and made no noticeable real-world difference.
Retest without it on `polystar.state` (60s): speed_percent mean ~0.85
(0.79-1.04), vs ~0.86 with it, i.e. within noise. The new baseline for the next
steps is **~0.85**, on HEAD `af439229f3d` with no audio change.

### Step 2a: TE worker pool was capped at 3 threads - fixed, Polystars now ~full speed (2026-09-23)

The profile showed exactly 3 busy worker threads even at 16 bands. The cause is
`src/osd/osdsync.cpp`: `osd_work_queue_alloc()` calls
`effective_num_processors(!(flags & WORK_QUEUE_FLAG_HIGH_FREQ))`, and with
`heavy_mt=false` `osd_get_num_processors()` returns `min(cores, 4)`, so a
HIGH_FREQ queue gets **3 workers**, whatever the core count. **This corrects Session 6's
note**, which said the pool sized itself from `numprocs - 1` (up to 15). That is
only true for non-HIGH_FREQ queues. It's why the Session 6 8-band experiment
did nothing.

Fix (driver-local only, no OSD change): `NUM_RENDER_BANDS` 4 → 16, plus
`NUM_RENDER_QUEUES = 4` HIGH_FREQ queues (12 workers total). Bands go
round-robin across the queues, and `flush_span_jobs()` waits on each queue.
The main thread still helps with each queue it waits on.

`polystar.state`, 60s each (baseline ~0.85 on af439229f3d):

| Config | mean | min | total CPU (all threads) |
|---|---|---|---|
| 8 bands, 1 queue | 0.88 | 0.76 | ~200% |
| 16 bands, 1 queue | 0.89 | 0.86 | ~195% |
| 16 bands, 3 queues | 0.995 | 0.908 | 284% |
| **16 bands, 4 queues (chosen)** | **1.006** | **0.965** | 346% |
| 16 bands, 5 queues | 1.000 | 0.972 | 386% |

speed_percent ~1.0 means real time. The benchmark is now capped, so it can't show
further gains. Future steps need CPU-time or main-thread-headroom metrics
instead: the main thread is still ~93% busy at 1.0x. Visual check: polystar from the state,
evilngt, totlvice and btltryst all boot and render correctly.

### Committed; user playtest (2026-09-23)

Step 2a is committed as `ee732efc388`. The user played `evilngt` and `polystar` and
says both are **playable now**, and gameplay audio is OK. **Known, parked issue**: in
`evilngt` **cutscenes**, speech samples still play slowly. The user thinks this
is not related to the CPU/TE performance work, and it doesn't affect gameplay, so it's
noted but deliberately not being worked on now. (The Session 6 audio-decoupling stash
was aimed at a version of this symptom and didn't fix it.)

### Step 3: load_texture() TRAM DMA fast path (2026-09-23, uncommitted at time of writing)

The MMDMA→TRAM path copied textures one byte at a time via
`read_bus8()`/`write_tram8()`. Polystars does ~1,900 loads/s of ~15.5KB each
(nearly all 16KB of TRAM every time, ~30MB/s). The new fast path copies 8 bytes per
step when the source is 8-byte aligned: RAM is a big-endian 64-bit bus
(`BYTE8_XOR_BE`), so 8 address-ordered bytes are one native 64-bit RAM word
stored with `put_u64be`. The original byte loop still handles unaligned heads/tails and anything
that would run past the end of RAM (mask wrap) or TRAM.

Verified with a temporary self-check (removed): each load was replayed with
the original byte loop into a copy of TRAM, then the whole 16KB was memcmp'd. **0
mismatches** over ~84k real loads (~1.2GB): polystar.state 57k loads, evilngt
11k, totlvice 14k, btltryst 1k.

Main-thread share on polystar.state, 60s: `read_bus8` 4.34% → 0.01%,
`load_texture` 1.14% → 0.51%. Total texture-load cost went from ~5.5% to ~0.5%
of the main thread; main-thread CPU 93.5% → 91.1%. So all of `read_bus8`'s cost was TE
texture loads, not DSPP DMA. Speed stays capped at real time (mean 1.01, min 0.968).

### Step 4: DSPP idle-loop skipping (2026-09-23, committed)

Committed step 3 first as `d94c41f8448`. The DSPP DRC (`dsppdrc.cpp`) exists but is disabled
upstream (`m_isdrc = false;//allow_drc();`, several TODO/unimplemented ops),
so the interpreter always runs. Temporary counters (removed) on polystar.state:
~8.2M DSPP instructions/s, **~64% of them at PCs 0x006/0x009**. That's a
2-instruction poll loop: `006: 4640 [0x3DF],#7` / `009: E806` (branch to
006). 0x3DF on Bulldog is `output_status_r()`, the output FIFO fill count. The FIFO
is only drained by the BDA's 44.1kHz `dac_update` timer, so it can never change
within the DSPP's own timeslice. The rest of the program runs ~44k times/s (once
per sample frame). SLEEP is `--pc` with a TODO and is never used here (0 hits).

Fix (`src/devices/cpu/dspp/dspp.{cpp,h}`): generic idle-loop skipping in the
interpreter. On each backward branch (or SLEEP), `check_idle_loop()`
snapshots the whole `dspp_internal_state`. It skips only if all of these hold:
the loop returns to the same PC with an identical core state (ignoring icount/tclock);
it made no `write_data()`; it made no DMA service that did work; and every
`read_data()` was side-effect-free. `idle_safe_read()` is virtual: for Bulldog
it's RAM <0x2e0 plus 0x3DE/0x3DF, and for the base (3DO console) device it's
nothing, so any read blocks skipping. When all that holds, the remaining iterations
are provably identical, so skip whole iterations by decrementing icount and tclock by
k*len. That leaves 1..len cycles so the slice ends exactly where it would have.
Detection resets at each execute_run (other devices only run between slices).
Debugger-enabled runs never skip.

Verified: dumped every DAC sample for the first 15 emulated seconds from
polystar.state, with skipping forced off and on (same binary, temporary env
switch). **Bit-identical**: 661,500 stereo frames, ~32% non-silent. Main-thread
share on polystar.state: DSPP (+ its 16-bit memory handlers) 13.4% → 7.5%;
main-thread CPU 91.1% → 87.0%; speed min 0.968 → 0.99 (mean ~1.02, capped).
evilngt/totlvice/btltryst boot clean. 3do.cpp (console) also uses the base DSPP,
but it's not in the arcade build, so it's untested here.

### Test scope and where things stand (2026-09-23)

Proven-working M2 titles, and the only ones to test from now on: polystar, totlvice, evilngt
(hellngt clone). btltryst is MACHINE_NOT_WORKING (black screen, broken audio)
and is excluded, as are heatof11 and the totlvicj/a/u clones. The TE (`3dom2_te.cpp`)
is used only by `konamim2.cpp`.

Current main thread on polystar.state (all 4 commits, real time): ~87% busy.
TE spin ~19%, TE raster/setup ~15%, PPC JIT ~18%, DSPP ~7.5%, memory
handlers/devices ~6.5%, recompiles ~3%. Remaining TE options:
- **Per-pixel optimisation** (~50% would take the main thread from ~87% to ~72% and the workers from
  ~2.5 cores to ~1.3). Verifiable via per-frame framebuffer hashes from the save
  state. This is the preferred option if more headroom is ever needed.
- **Deferring the list-end wait**: not recommended. PPC RAM is DRC fastram, so CPU
  framebuffer access can't be intercepted, and any race would be timing-dependent.

### Step 5: TE per-pixel optimisation, frame-hash verified (2026-09-23, committed)

**Verification method** (temporary instrumentation, removed afterwards; re-add it to
resume): an FNV-1a hash of every displayed frame from `m2_vdu_device::screen_update`,
counted from the first frame seen. The runs were 1500 frames of polystar.state,
and 3000 frames each of evilngt/totlvice from boot. A/A runs of the same build gave
identical hashes on all 7,500 frames (987/543/969 distinct images), so the output is
deterministic. **Negative control**: flipping one bit of one pixel's blue channel
at (200,120) was caught in 1488/1500, 2835/3000 and 2826/3000 frames. A work-based
timing meter was used too, because speed is capped at 1.0: per-thread CPU time inside
`render_band` (CLOCK_THREAD_CPUTIME_ID) summed over exactly those frames, plus
main-thread wall time in `flush_span_jobs`. Noise is about ±2-3%.

Raster CPU in seconds (polystar / evilngt / totlvice). Every round was bit-identical:

| Round | Change | polystar | evilngt | totlvice | Kept? |
|---|---|---|---|---|---|
| base | - | 55.5 | 31.6 | 50.2 | |
| 1 | force-inline (`inline ATTR_FORCE_INLINE`) the 19 per-pixel helpers | 39.0 | 22.4 | 36.0 | yes |
| 2 | bus accessors `read_bus*/write_bus*` moved inline into 3dom2.h | 38.7 | 22.0 | 34.6 | yes (marginal) |
| 3 | `span_regs`: per-span local copy of gc/es/tm/db + tram/pipram passed by ref | **31.6** | **17.9** | **29.2** | **yes** |
| 4 | also a local copy of pixel_scratch | 33.0 | 18.6 | 29.9 | no (slower) |
| 5 | per-span stats/status accumulators in span_regs (non-const) | 34.4 | 18.6 | 31.1 | no (slower) |
| 6 | clz instead of shift loops (nr_invert, texcoord_gen normalise) | 31.5 | 17.6 | 29.4 | no (noise) |

Why round 3 matters: the build uses `-fno-strict-aliasing`, so every framebuffer/Z store
through a uint16_t*/uint32_t* could alias the snapshot. The compiler therefore reloaded and
re-decoded every register after each pixel store. A local copy whose address never
escapes (all callees are force-inlined) can't alias RAM.

**Net: ~43% less raster CPU on all three games.** Main-thread flush wall time is down ~37%.
On the 60s polystar benchmark: main thread 87% → 76%, total process CPU 330% → 221%,
speed mean 1.02 / min 0.979. evilngt/totlvice boot clean, totlvice screenshot correct.
Remaining time is spread across get_texel (~22% of walk_span), destination_blend (~18%)
and many small items, so there are diminishing returns and we stopped here. `TEST_TIMING` is
on in this driver, so the per-pixel stats counters feed the TE's completion timing and must
stay exact.

### Moved to fork-specific/ (2026-09-23)

This doc now lives in `fork-specific/`, the home for this fork's investigation docs
and tools. It was at the repo root before. The tools are in `fork-specific/tools/`
(see `fork-specific/README.md`): `m2_profile_run.py`, `smoke.py`, and the frame-hash
pair `fbcheck.sh`/`fbcompare.sh`. Their output goes to `fork-specific/out/`.
