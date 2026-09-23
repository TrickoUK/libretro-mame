# fork-specific

Investigation notes and tools for this fork's own work (not upstream MAME).
The directory is gitignored (blanket `/*/` rule), so new files need `git add -f`
to be tracked; `m2-fix-investigation.md` is tracked.

## Documents

- `m2-fix-investigation.md`: Konami M2 (`konamim2.cpp`/`3dom2*.cpp`)
  performance investigation, covering every session's findings, measurements
  and what was tried and reverted.

## Tools (`tools/`)

Results go to `fork-specific/out/<tag>/`, or wherever `M2PROF_OUT` points.
Run the tools one at a time: each one force-kills RetroArch when it finishes.

- `m2_profile_run.py <tag> <seconds> [cg]`: loads `polystar` from
  `savestates/polystar.state` (the standard M2 benchmark). It attaches `perf`
  (flat, or a DWARF call graph with `cg`), polls `speed_percent` through the Lua
  console, records CPU per thread, then kills RetroArch and restores `MAME.opt`.
- `smoke.py <seconds> <romset>...`: boots each romset, checks it's still
  running, takes a screenshot and logs any errors.
- `fbcheck.sh <tag>` / `fbcompare.sh <ref-tag> <tag>`: the bit-exact
  rendering check. It hashes every displayed frame of polystar (from the save state),
  evilngt and totlvice, then compares two runs frame by frame. It needs the temporary
  `M2FBHASH` instrumentation, which is not committed. See `m2-fix-investigation.md`
  step 5 for how to re-add it and for the negative-control procedure.
