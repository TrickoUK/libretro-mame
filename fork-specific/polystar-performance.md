# Polystars: toward a solid 60 fps in gameplay

Written 2026-09-25, on branch `ppc-upstream-sync` (upstream PPC vTLB/603 TLB picks on top of
`arcade-focused` 575419b607d). Not started as a project. This records where the time goes today,
so a later session can pick it up. The goal is steady full speed in busy stage-1 scenes, even if
the original hardware slowed down there.

## What was measured

Symptom (from play): polystar starts and plays correctly, but feels sluggish once you are properly
into a level.

Method: cold boot, then identical inputs from `tools/polystar_stage1.lua`. The script is keyed on
emulated time, so both runs reach the same frames at the same moments. Profiled 20 s of stage 1
(about 60-80 s after launch) with `tools/profile_run.py` (flat perf, 1999 Hz). The screenshots from
the two cores match frame for frame.

| Core | stage-1 speed_percent | main emu thread | each TE worker |
|---|---|---|---|
| `ppc-upstream-sync` (new vTLB) | 0.962 0.983 0.962 | 78.0% of a core | ~12% |
| `arcade-focused` before the picks | 0.959 0.984 0.974 | 78.8% of a core | ~12% |

So the upstream PPC changes are not the cause. Both builds are about 2-4% short of full speed in
busy stage-1 play. Attract mode and the calmer start of the stage run at 1.02-1.08.

Save states don't work for this comparison: the vTLB grew from 128 to 4096 entries and its tables
are part of the state. States saved before the change don't load after it, and vice versa. They
fall back to a cold boot into attract mode, which is easy to mistake for a real run. The
`savestates/polystar*.state` files (and `m2_profile_run.py`, which uses `savestates/polystar.state`)
are from before the change.

## Where the time goes

perf, whole process, stage 1:

- `m2_te_device::walk_span` (the M2 triangle engine's software pixel fill, `3dom2_te.cpp`): about
  47% of all samples. It is spread over ~12 worker threads at ~3-4% each, plus ~3% on the main
  thread. Each worker sits at only ~12% of a core, so the host has lots of spare parallel
  capacity.
- `osd_work_queue_wait` **on the main thread: ~5.6%**. The main thread blocks while it waits for
  the TE workers to finish a batch.
- The rest of the main thread: memory handlers (`handler_entry_read/write_memory`), the DSPP audio
  DSP (`dspp_device::execute_run`/`read_next_operand`/`exec_arithmetic`), TE setup
  (`setup_triangle`), `nr_invert`, VDU scanout, and the two PPC602s' JIT code.
- PPC DRC compile/TLB work (`code_compile_block`, asmjit, `vtlb_*`, `icache_flash_invalidate`,
  `ppc_check_translation`): under 0.5% in total. The recompile storms fixed earlier (see
  `m2-fix-investigation.md`) are gone.

The limit is the single main emulation thread, not the total CPU. That thread runs both PPC602s,
the DSPP, TE command/setup and the memory system. At stage-1 load it tops out at ~78% busy plus
waits, and slips to 0.96-0.98x.

## Ideas, roughly cheapest first

1. **CPU governor.** This host runs the `powersave` governor system-wide (~2.2 GHz typical vs
   ~4.8 GHz max; see CLAUDE.md, Phase 2 "Performance: final state"). A 2-4% shortfall on one
   thread may disappear under `performance`. Re-measure with the same script before changing any
   code.
2. **Stop the main thread waiting on TE workers** (the ~5.6% `osd_work_queue_wait`). Find what
   forces the sync: the game polling TE status/IRQ, a framebuffer read-back, or the end of a
   command list. See whether TE rendering can run further behind emulation (deferred until a real
   dependency such as a CPU read of the framebuffer or a status register), so it overlaps with
   PPC/DSPP work instead of stalling it. `m2-fix-investigation.md` has the TE command-list and
   IRQ timing notes.
3. **Cheaper main-thread work**: the DSPP interpreter (a noticeable slice of the main thread), and
   the memory handler overhead from the PPCs going through `handler_entry_*` rather than direct
   RAM pointers.
4. **Game-side slowdown vs emulator slowdown.** Not measured yet. Check how often the game actually
   updates its frame in busy scenes (e.g. count identical consecutive framebuffers, or watch the
   game's frame counter in RAM). If the game itself drops to 30 Hz there, that is game/hardware
   behaviour, and fixing it would need a game-specific hack rather than emulator speed.

## Reproducing

```sh
python3 fork-specific/tools/profile_run.py polystar <tag> --warmup 60 --duration 20 --shot-every 10 \
    --lua "3:$(grep -v '^--' fork-specific/tools/polystar_stage1.lua)" [--core <other .so>]
# per-thread breakdown:
distrobox enter mame-dev -- perf report -i $PWD/fork-specific/out/<tag>/perf.data --no-children \
    --sort pid,symbol --stdio | head -30
```

Check `fork-specific/out/<tag>/shots/` shows the control-select screen and then stage 1. If the
timing drifts after a code change, the fixed emulated-time inputs can leave you on a different
screen.
