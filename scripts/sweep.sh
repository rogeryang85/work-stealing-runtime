#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
TAG=${1:-mac}
THREADS=${THREADS:-"1 2 4 6 8 10 12"}
REPS=${REPS:-5}
export OMP_STACKSIZE=64M OMP_WAIT_POLICY=passive
HDR=bench,impl,threads,n,cutoff,median_s,min_s,result,tasks,steals,steal_attempts
B=./build/bench

out=results/${TAG}_scaling.csv
echo $HDR > $out
for b in fib qsort nqueens uts; do
  $B --bench $b --impl serial --reps $REPS >> $out
  for i in ws cq omp; do
    for t in $THREADS; do
      r=$REPS
      if [ $b = fib ] && [ $i != ws ] && [ $t -gt 1 ]; then r=3; fi
      $B --bench $b --impl $i --threads $t --reps $r --check >> $out
      echo "$b $i $t" >&2
    done
  done
done

out=results/${TAG}_granularity.csv
echo $HDR > $out
GT=${GT:-6}
for c in 1 4 8 12 16 20 24; do
  $B --bench fib --impl serial --cutoff $c --reps $REPS >> $out
  for i in ws cq omp; do
    $B --bench fib --impl $i --threads $GT --cutoff $c --reps 3 --check >> $out
  done
  echo "granularity cutoff=$c" >&2
done
echo done >&2
