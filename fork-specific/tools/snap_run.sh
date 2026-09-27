#!/bin/bash
# Usage: snap_run.sh <romset> <tag> <first_frame> <last_frame> <step> [K=V ...]
# Cold-boots <romset> via profile_run.py and saves MAME's native-resolution snapshot at every
# <step>th emulated frame in [first, last] (frame-exact, unlike wall-clock UDP screenshots).
# Snapshots end up in fork-specific/out/<tag>/snaps/. K=V pairs are passed as RetroArch env vars.
TOOLS=$(cd "$(dirname "$0")" && pwd)
OUT=$(dirname "$TOOLS")/out
ROM=$1 TAG=$2 FIRST=$3 LAST=$4 STEP=$5; shift 5
SNAPDIR=$HOME/.config/retroarch/saves/MAME/mame/snaps/$ROM
ENVS=(); for kv in "$@"; do ENVS+=(--env "$kv"); done
mkdir -p $SNAPDIR; BEFORE=$(ls $SNAPDIR)   # never touch snapshots that were already there
FPS=58
DUR=$(( LAST / FPS + 15 ))
L="fs={} for f=$FIRST,$LAST,$STEP do fs[f]=true end sub=emu.add_machine_frame_notifier(function() local n=manager.machine.screens[':screen']:frame_number() if fs[n] then manager.machine.video:snapshot() end end)"
python3 $TOOLS/profile_run.py $ROM $TAG --warmup $DUR --duration 0 --shot-every $DUR --no-perf "${ENVS[@]}" --lua "3:$L" >/dev/null 2>&1
mkdir -p $OUT/$TAG/snaps; rm -f $OUT/$TAG/snaps/*.png
for f in $(ls $SNAPDIR); do
    grep -qxF "$f" <<< "$BEFORE" || mv "$SNAPDIR/$f" "$OUT/$TAG/snaps/"
done
echo "$TAG: $(ls $OUT/$TAG/snaps | wc -l) snapshots in $OUT/$TAG/snaps"
