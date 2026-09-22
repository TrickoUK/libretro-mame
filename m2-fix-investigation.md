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
