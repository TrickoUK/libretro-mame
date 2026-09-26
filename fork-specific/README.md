# fork-specific

Investigation notes and tools for this fork's own work (not upstream MAME).
The directory is gitignored (blanket `/*/` rule), so new files need `git add -f`
to be tracked.

## Documents

- `m2-fix-investigation.md`: Konami M2 (`konamim2.cpp`/`3dom2*.cpp`)
  performance investigation, covering every session's findings, measurements
  and what was tried and reverted.

- `viper-investigation.md`: Konami Viper (`viper.cpp`, gticlub2/thrild2/jpark3). Covers the PPC
  DRC recompile storm, the Voodoo 3 TMU1/multibase texture and blit fixes, the I2C analog
  controls, save states, why gticlub2 cars roll at speed (a game rule) and the optional gamepad
  Steering Response/Smoothing settings, plus game RAM notes.

- `voodoo-gpu-idea.md`: feasibility notes for GPU-offloaded 3dfx Voodoo rendering (not started).

- `potential-upstream-sync.md`: upstream MAME commits since mame0289 that would help this fork
  (PPC 603 TLB/vTLB, DRC rounding, PS1 SPU overhaul, System 22/Zeus/Model 3 fixes), whether
  each applies cleanly, and where they conflict with our changes. The PPC vTLB/603 TLB set was
  pulled on 2026-09-25, and on 2026-09-26 the whole PowerPC core was synced to upstream
  (including `0ee5c47faa7` + `96016fbe55c`); see its status note.

- `polystar-performance.md`: where polystar's time goes in busy stage-1 play (~0.96-0.98x,
  limited by the main emulation thread waiting on the M2 triangle-engine workers), plus
  ideas toward a steady 60 fps. Not started.

## Tools (`tools/`)

Results go to `fork-specific/out/<tag>/`, or wherever `M2PROF_OUT` points.
Run the tools one at a time: each one force-kills RetroArch when it finishes.

- `m2_profile_run.py <tag> <seconds> [cg]`: loads `polystar` from
  `savestates/polystar.state` (the standard M2 benchmark). It attaches `perf`
  (flat, or a DWARF call graph with `cg`), polls `speed_percent` through the Lua
  console, records CPU per thread, then kills RetroArch and restores `MAME.opt`.
- `profile_run.py <romset> <tag> [--warmup S] [--duration S] [--shot-every S] [--state F] [--cg]
  [--no-perf] [--core PATH] [--lua T:CMD ...] [--env K=V ...]`: a generic version of `m2_profile_run.py`
  for any romset. It boots cold (or from a state), then records perf, speed_percent, per-thread
  CPU and periodic core-framebuffer screenshots (UDP `SCREENSHOT`, into `<tag>/shots/`). `--lua`
  sends a Lua console command at T seconds after launch, e.g. to insert coins and press start.
  `--env` sets an environment variable for RetroArch (used by the replay plugin below).
- `polystar_stage1.lua`: cold-boot polystar inputs keyed on emulated time (coin, start, pick
  control scheme, fire and sweep), for repeatable stage-1 profiling with `profile_run.py --lua`.
  Usage is in the file header and in `polystar-performance.md`.
- `replay_inputs.py OUT.lua [--tap START:LEN:VALUE ...] [--dump F --dump-range LO:HI]
  [--full-dump PREFIX --full-dump-frames N,...] [--steering-response N] [--steering-smoothing N]`
  and `replay-plugin/`: frame-exact input replays from a save state (gticlub2 port names), with
  per-frame memory dumps. Copy `replay-plugin/` to `<retroarch system>/mame/plugins/replay` and run
  `profile_run.py ... --state S --env MAME_EXTRA_PLUGIN=replay --env MAME_REPLAY_SCRIPT=OUT.lua`.
  See `viper-investigation.md`.
- `smoke.py <seconds> <romset>...`: boots each romset, checks it's still
  running, takes a screenshot and logs any errors.
- `fbcheck.sh <tag>` / `fbcompare.sh <ref-tag> <tag>`: the bit-exact
  rendering check. It hashes every displayed frame of polystar (from the save state),
  evilngt and totlvice, then compares two runs frame by frame. It needs the temporary
  `M2FBHASH` instrumentation, which is not committed. See `m2-fix-investigation.md`
  step 5 for how to re-add it and for the negative-control procedure.
