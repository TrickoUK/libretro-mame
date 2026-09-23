#!/bin/bash
# Usage: fbcheck.sh <tag>  - per-frame image hashes for polystar (save state), evilngt,
# totlvice (from boot) into fork-specific/out/fb/<tag>/.  Needs the temporary M2FBHASH
# instrumentation in m2_vdu_device::screen_update() - see m2-fix-investigation.md, step 5.
TOOLS=$(cd "$(dirname "$0")" && pwd)
OUT=$(dirname "$TOOLS")/out
T=$1; mkdir -p $OUT/fb/$T
M2_FBHASH=$OUT/fb/$T/polystar.txt M2_FBHASH_FRAMES=1500 M2PROF_OUT=$OUT/fb/$T python3 $TOOLS/m2_profile_run.py prof 30 >/dev/null 2>&1
M2_FBHASH=$OUT/fb/$T/evilngt.txt M2_FBHASH_FRAMES=3000 M2PROF_OUT=$OUT TAG=fb/$T/smoke_e python3 $TOOLS/smoke.py 60 evilngt >/dev/null 2>&1
M2_FBHASH=$OUT/fb/$T/totlvice.txt M2_FBHASH_FRAMES=3000 M2PROF_OUT=$OUT TAG=fb/$T/smoke_t python3 $TOOLS/smoke.py 60 totlvice >/dev/null 2>&1
for g in polystar evilngt totlvice; do echo "$T $g frames=$(grep -vc PERF $OUT/fb/$T/$g.txt) distinct_hashes=$(grep -v PERF $OUT/fb/$T/$g.txt | awk '{print $3}' | sort -u | wc -l)"; done
