#!/bin/zsh
# Usage: bench.sh [runs] [label]
# Reports min / median / mean of the "Script evaluation" phase over N runs.
RUNS=${1:-7}
LABEL=${2:-run}
BIN=${BIN:-$(git rev-parse --show-toplevel)/build-release/OpenSCAD.app/Contents/MacOS/OpenSCAD}
MODEL=${MODEL:-$HOME/prj/Make/stocko/u-bot/u-bot.scad}
cd "$(dirname "$MODEL")" || exit 1
# Discard warm-up runs: this machine needs ~4 passes to settle, and an
# unsettled run reads ~4% slow, which is the size of the effects being measured.
typeset -a evals totals
for i in 1 2 3 4; do
  $BIN -o /tmp/warm_$$.stl $MODEL >/dev/null 2>&1
done
rm -f /tmp/warm_$$.stl
for i in $(seq $RUNS); do
  line=$($BIN --summary time --summary-file - -o /tmp/bench_$$.stl $MODEL 2>/dev/null | tail -1)
  e=$(print -r -- "$line" | sed 's/.*"Script evaluation","total":\([0-9]*\).*/\1/')
  t=$(print -r -- "$line" | sed 's/.*"total":\([0-9]*\)}}$/\1/')
  evals+=$e; totals+=$t
  print -n "  $e"
done
print ""
md5=$(md5 -q /tmp/bench_$$.stl); rm -f /tmp/bench_$$.stl
print -r -- "$evals" | tr ' ' '\n' | sort -n | awk -v L="$LABEL" -v N=$RUNS '
  {v[NR]=$1; s+=$1}
  END { printf "%-22s runs=%d  min=%d  median=%d  mean=%.0f  max=%d\n", L, NR, v[1], v[int((NR+1)/2)], s/NR, v[NR] }'
print "  stl md5: $md5"
