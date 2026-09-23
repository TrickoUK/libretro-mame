#!/bin/bash
# Usage: fbcompare.sh <ref-tag> <tag> - compare per-frame hashes from two fbcheck.sh runs
# frame by frame and print each run's PERF (raster CPU) line
S=$(dirname "$(cd "$(dirname "$0")" && pwd)")/out
for g in polystar evilngt totlvice; do
  r=$(join <(grep -v PERF $S/fb/$1/$g.txt | awk '{print $1"_"$2, $3}' | sort) <(grep -v PERF $S/fb/$2/$g.txt | awk '{print $1"_"$2, $3}' | sort) | awk '{c++} $2!=$3{m++} END{print "frames_compared="c+0, "MISMATCHES="m+0}')
  echo "$2 vs $1  $g: $r  $(grep PERF $S/fb/$2/$g.txt)"
done
