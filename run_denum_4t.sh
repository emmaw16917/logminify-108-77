#!/bin/bash
# Denum, 4 worker threads, block size 100000, mode 1.
# Per-block compression is `tar --use-compress-program='lzma -6'` (baseline version).
# Run from the directory that contains Logs/<name>/<name>.log.
set -u
cd "$(dirname "$0")"

LIST="Android Apache BGL Hadoop HDFS HealthApp HPC Linux Mac OpenSSH OpenStack Proxifier Spark Thunderbird Windows Zookeeper"
mkdir -p results output

echo "build start $(date -Is)" | tee results/run.log
if ! g++ -O3 -std=c++17 -o denum_compress denum_compress.cpp \
        -lboost_iostreams -lpthread -lpcre2-8 2> results/build.err; then
    echo "BUILD FAILED $(date -Is)" | tee -a results/run.log
    exit 1
fi
echo "build done $(date -Is)" | tee -a results/run.log

for name in $LIST; do
    echo "=== $name start $(date -Is) ===" | tee -a results/run.log
    ./denum_compress "$name" 100000 1 4 > "results/$name.out" 2> "results/$name.err"
    rc=$?
    echo "=== $name rc=$rc end $(date -Is) ===" | tee -a results/run.log
done
echo "ALL DONE $(date -Is)" | tee -a results/run.log
