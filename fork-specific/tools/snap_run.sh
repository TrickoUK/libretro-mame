#!/bin/bash
# Usage: [FF=<secs>|all] snap_run.sh <romset> <tag> <first_frame> <last_frame> <step> [K=V ...]
# Cold-boots <romset> via profile_run.py and saves MAME's native-resolution snapshot at every
# <step>th emulated frame in [first, last] (frame-exact, unlike wall-clock UDP screenshots).
# FF=<secs> holds RetroArch fast-forward that long (long inits); FF=all holds it for the whole run and
# stops as soon as the last snapshot is saved. Frames are pixel-identical to a 1x run (checked on gticlub).
# Snapshots end up in fork-specific/out/<tag>/snaps/. K=V pairs are passed as RetroArch env vars.
TOOLS=$(cd "$(dirname "$0")" && pwd)
OUT=$(dirname "$TOOLS")/out
ROM=$1 TAG=$2 FIRST=$3 LAST=$4 STEP=$5; shift 5
SNAPDIR=$HOME/.config/retroarch/saves/MAME/mame/snaps/$ROM
ENVS=(); for kv in "$@"; do ENVS+=(--env "$kv"); done
mkdir -p $SNAPDIR; BEFORE=$(ls $SNAPDIR)   # never touch snapshots that were already there
FPS=58
DUR=$(( LAST / FPS + 15 ))
L="fs={} for f=$FIRST,$LAST,$STEP do fs[f]=true end sub=emu.add_machine_frame_notifier(function() local scr=nil for _,s in pairs(manager.machine.screens) do scr=s break end local n=scr:frame_number() if fs[n] then manager.machine.video:snapshot() end end)"
FFARG=(); [ -n "$FF" ] && FFARG=(--ff $([ "$FF" = all ] && echo -1 || echo $FF))
python3 $TOOLS/profile_run.py $ROM $TAG --warmup $DUR --duration 0 --shot-every $DUR --no-perf "${FFARG[@]}" "${ENVS[@]}" --lua "3:$L" >/dev/null 2>&1 &
PR=$!
if [ "$FF" = all ]; then
    WANT=$(( (LAST - FIRST) / STEP + 1 ))
    while kill -0 $PR 2>/dev/null; do
        [ $(comm -13 <(echo "$BEFORE") <(ls $SNAPDIR) | wc -l) -ge $WANT ] && { sleep 1; kill -TERM $PR; break; }
        sleep 1
    done
fi
wait $PR
mkdir -p $OUT/$TAG/snaps; rm -f $OUT/$TAG/snaps/*.png
for f in $(ls $SNAPDIR); do
    grep -qxF "$f" <<< "$BEFORE" || mv "$SNAPDIR/$f" "$OUT/$TAG/snaps/"
done
echo "$TAG: $(ls $OUT/$TAG/snaps | wc -l) snapshots in $OUT/$TAG/snaps"
