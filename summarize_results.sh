#!/bin/bash
# Summarize results/<dataset>.out produced by run_denum_4t.sh.
set -u
cd "$(dirname "$0")"

DATASETS="Android Apache BGL Hadoop HDFS HealthApp HPC Linux Mac OpenSSH OpenStack Proxifier Spark Thunderbird Windows Zookeeper"
OUT=results/summary.tsv
mkdir -p results

{
    printf 'dataset\tinput_bytes\tachieved_bytes\tCR\tCS_MBps\treported_ms\n'
    for name in $DATASETS; do
        out="results/$name.out"
        log="Logs/$name/$name.log"
        [ -f "$out" ] && [ -f "$log" ] || continue
        input=$(stat -c %s "$log")
        achieved=$(awk '/Achieved size/ {v=$3} END {print v}' "$out")
        cr=$(awk '/Compression ratio/ {v=$3} END {print v}' "$out")
        cs=$(awk '/Compression speed/ {v=$3} END {print v}' "$out")
        ms=$(grep -o 'completed in [0-9]* milliseconds' "$out" | tail -1 | grep -o '[0-9]*')
        [ -n "$cr" ] && [ -n "$ms" ] || continue
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$input" "$achieved" "$cr" "$cs" "$ms"
    done
} > "$OUT.body"

awk -F'\t' 'NR>1 {ti+=$2; to+=$3; tm+=$6; n++}
END {if (n) printf "TOTAL\t%.0f\t%.0f\t%.3f\t%.3f\t%.0f\n", ti, to, ti/to, ti/1048576/(tm/1000), tm}' \
    "$OUT.body" > "$OUT.total"

cat "$OUT.body" "$OUT.total" > "$OUT"
cat "$OUT"
