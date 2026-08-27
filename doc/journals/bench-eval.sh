#!/bin/zsh
# Usage: bench-eval.sh [runs] [label]
#
# Script evaluation only, via the .echo output path: no geometry, no 158MB STL
# write. Much quieter than bench.sh -- the full render's 2.9s of geometry and
# its export I/O add run-to-run noise of the same size as the effects being
# measured -- and about half the wall time per run.
#
# NB: this path runs with $preview=true, so it takes different branches through
# a library than the STL path does. Fine for an A/B of the same model, but the
# absolute ms are not comparable with bench.sh, and a winner here should still
# be confirmed with bench.sh before it is believed.
RUNS=${1:-9}
LABEL=${2:-run}
BIN=${BIN:-$(git rev-parse --show-toplevel)/build-release/OpenSCAD.app/Contents/MacOS/OpenSCAD}
MODEL=${MODEL:-$HOME/prj/Make/stocko-bench/u-bot/u-bot.scad}
cd "$(dirname "$MODEL")" || exit 1
typeset -a evals
for i in 1 2 3 4; do
  $BIN -o /tmp/warm_$$.echo $MODEL >/dev/null 2>&1
done
for i in $(seq $RUNS); do
  line=$($BIN --summary time --summary-file - -o /tmp/bench_$$.echo $MODEL 2>/dev/null | tail -1)
  e=$(print -r -- "$line" | sed 's/.*"Script evaluation","total":\([0-9]*\).*/\1/')
  evals+=$e
  print -n "  $e"
done
print ""
md5=$(md5 -q /tmp/bench_$$.echo); rm -f /tmp/bench_$$.echo /tmp/warm_$$.echo
print -r -- "$evals" | tr ' ' '\n' | sort -n | awk -v L="$LABEL" '
  {v[NR]=$1; s+=$1}
  END { printf "%-22s runs=%d  min=%d  median=%d  mean=%.0f  max=%d  spread=%.1f%%\n", \
        L, NR, v[1], v[int((NR+1)/2)], s/NR, v[NR], 100*(v[NR]-v[1])/v[1] }'
print "  echo md5: $md5"
