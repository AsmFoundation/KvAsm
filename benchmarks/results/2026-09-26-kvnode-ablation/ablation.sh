#!/usr/bin/env bash
# Ablation: the four KvNode builds (s0 baseline, s1 +stream cache, s2 +pool
# allocator, s3 +batched prefetch), interleaved within each repetition so
# drift affects all variants alike. Standard GET workload: 1M keys, 100-byte
# values, 64 connections x 16 in flight, 16 client threads on vCPUs 16-31.
# Prints one line per run: cores variant rep ops/s.
set -u
KVB=/src/benchmarks/kvbench/build/kvbench
PORT=50171
REPS=${REPS:-5}
for cores in ${CORES:-1 4}; do
  cpus=$([ "$cores" = 1 ] && echo 0 || echo "0-$((cores - 1))")
  for rep in $(seq 1 "$REPS"); do
    # Latin-square rotation: rep r starts at variant r mod 4, so every variant
    # runs in every position equally often and position effects cancel.
    all=(s0-base s1-cache s2-pool s3-batch)
    order=()
    for i in 0 1 2 3; do order+=("${all[$(( (rep + i) % 4 ))]}"); done
    for v in "${order[@]}"; do
      pkill -x "kvnode-$v" 2>/dev/null; pkill -f "/tmp/kvnode-" 2>/dev/null; sleep 0.3
      KVNODE_PORT=$PORT KVNODE_WORKERS=$cores KVNODE_CAPACITY=1250000 \
        taskset -c "$cpus" "/tmp/kvnode-$v" 2>/dev/null &
      spid=$!
      out=$(taskset -c 16-31 $KVB --protocol grpc --port $PORT --keys 1000000 --load \
            --warmup 2 --duration 8 --seed "$rep" 2>/dev/null)
      ops=$(echo "$out" | grep -o '"ops_per_sec":[0-9]*' | tail -1 | cut -d: -f2)
      valid=$(echo "$out" | grep -o '"valid":[a-z]*' | tail -1 | cut -d: -f2)
      echo "cores=$cores variant=$v rep=$rep ops=$ops valid=$valid"
      kill $spid 2>/dev/null; wait $spid 2>/dev/null
    done
  done
done
