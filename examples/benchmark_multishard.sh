#!/bin/bash

# Multi-Shard Benchmark Script (2 Shards with Replication)
# 
# Based on the working test_2shard_replication.sh
# 
# Usage: ./examples/benchmark_multishard.sh [output_prefix]
#
# Before running:
# 1. Edit src/mako/benchmarks/sto/SundialConfig.hh to set SUNDIAL_ENABLED, etc.
# 2. Rebuild: cd build && make -j$(nproc) benchmarks && cd ..
# 3. Run this script with appropriate output prefix (e.g., "sundial" or "baseline")

# Output prefix (default: multishard)
OUTPUT_PREFIX="${1:-multishard}"
RESULTS_FILE="${OUTPUT_PREFIX}_results.txt"

echo "========================================="
echo "Multi-Shard Benchmark (2 Shards + Replication)"
echo "========================================="
echo "Output prefix: $OUTPUT_PREFIX"
echo "Results file: $RESULTS_FILE"
echo "Threads per shard: 2"
echo "Configuration: 2 shards, each with 3 replicas + 1 learner"
echo "========================================="

# Clean up - kill all existing dbtest and shard scripts
ps aux | grep -i dbtest | awk '{print $2}' | xargs kill -9 2>/dev/null
pkill -9 -f "bash/shard.sh" 2>/dev/null || true
sleep 1

rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Configuration
TRD=2  # 2 threads per shard (safe for VM)

echo ""
echo "Starting shard 0..."
# Shard 0: localhost (leader), learner, p2, p1
nohup bash bash/shard.sh 2 0 $TRD localhost 0 1 > ${OUTPUT_PREFIX}_shard0_localhost.log 2>&1 &
SHARD0_LOCALHOST_PID=$!
nohup bash bash/shard.sh 2 0 $TRD learner 0 1 > ${OUTPUT_PREFIX}_shard0_learner.log 2>&1 &
SHARD0_LEARNER_PID=$!
nohup bash bash/shard.sh 2 0 $TRD p2 0 1 > ${OUTPUT_PREFIX}_shard0_p2.log 2>&1 &
SHARD0_P2_PID=$!
sleep 1
nohup bash bash/shard.sh 2 0 $TRD p1 0 1 > ${OUTPUT_PREFIX}_shard0_p1.log 2>&1 &
SHARD0_P1_PID=$!

sleep 5

echo "Starting shard 1..."
# Shard 1: localhost (leader), learner, p2, p1
nohup bash bash/shard.sh 2 1 $TRD localhost 0 1 > ${OUTPUT_PREFIX}_shard1_localhost.log 2>&1 &
SHARD1_LOCALHOST_PID=$!
nohup bash bash/shard.sh 2 1 $TRD learner 0 1 > ${OUTPUT_PREFIX}_shard1_learner.log 2>&1 &
SHARD1_LEARNER_PID=$!
nohup bash bash/shard.sh 2 1 $TRD p2 0 1 > ${OUTPUT_PREFIX}_shard1_p2.log 2>&1 &
SHARD1_P2_PID=$!
sleep 1
nohup bash bash/shard.sh 2 1 $TRD p1 0 1 > ${OUTPUT_PREFIX}_shard1_p1.log 2>&1 &
SHARD1_P1_PID=$!

# Wait for experiments to run
# Need 180s to handle worst case: 30s runtime + 30s runtime_plus extension + 30s buffer + shutdown time
# One shard may get runtime_plus:30 while the other gets runtime_plus:0, causing timing asymmetry
echo ""
echo "Running experiments for 180 seconds..."
echo "(Allowing extra time for runtime_plus extension and cross-shard coordination)"
sleep 180

# Give extra time for graceful completion and shutdown
echo ""
echo "Benchmark time elapsed. Waiting 30s for graceful shutdown..."
sleep 30

# Kill the processes - FORCE KILL ALL (matching test_2shard_replication.sh)
echo ""
echo "Stopping any remaining processes..."

# First, kill the parent bash scripts to prevent them from respawning dbtest
pkill -9 -f "bash/shard.sh" 2>/dev/null || true

# Kill all dbtest processes immediately with SIGKILL
pkill -9 dbtest 2>/dev/null || true
killall -9 dbtest 2>/dev/null || true

# Wait for OS to clean up
sleep 2

# Reap zombie processes by explicitly waiting on child PIDs
for pid in $SHARD0_LOCALHOST_PID $SHARD0_LEARNER_PID $SHARD0_P2_PID $SHARD0_P1_PID \
           $SHARD1_LOCALHOST_PID $SHARD1_LEARNER_PID $SHARD1_P2_PID $SHARD1_P1_PID; do
    wait $pid 2>/dev/null || true
done

# Collect and display results
echo ""
{
    echo "========================================="
    echo "MULTI-SHARD BENCHMARK RESULTS"
    echo "========================================="
    echo "Output prefix: $OUTPUT_PREFIX"
    echo "Timestamp: $(date)"
    echo "Threads per shard: $TRD"
    echo "Configuration: 2 shards, each with 3 replicas + 1 learner"
    echo "========================================="
    echo ""

    # Extract and display results from both shards
    for shard in 0 1; do
        log="${OUTPUT_PREFIX}_shard${shard}_localhost.log"
        echo "=== Shard $shard Results ==="
        
        if [ -f "$log" ]; then
            # Check if benchmark completed
            if grep -q "agg_throughput" "$log"; then
                echo "Status: COMPLETED"
                echo ""
                echo "Throughput:"
                grep -E "agg_throughput|agg_persist_throughput" "$log" | tail -2
                echo ""
                echo "Abort Rate:"
                grep -E "agg_abort_rate|NewOrder_remote_abort_ratio" "$log" | tail -2
                echo ""
                echo "Latency:"
                grep -E "avg_latency" "$log" | tail -1
            else
                echo "Status: DID NOT COMPLETE"
                echo "(Log exists but no results found - check ${log} for errors)"
            fi
        else
            echo "Status: LOG NOT FOUND"
            echo "Log file not found: $log"
        fi
        echo ""
    done

    # Check for Sundial stats if enabled
    echo "=== Sundial Statistics (if enabled) ==="
    sundial_found=0
    for shard in 0 1; do
        log="${OUTPUT_PREFIX}_shard${shard}_localhost.log"
        if [ -f "$log" ] && grep -q "Sundial Statistics" "$log"; then
            sundial_found=1
            echo "Shard $shard:"
            grep -A 12 "=== Sundial Statistics ===" "$log" | tail -13
            echo ""
        fi
    done
    if [ $sundial_found -eq 0 ]; then
        echo "(Sundial statistics not found - SUNDIAL_STATS may be disabled)"
    fi
    echo ""

    echo "========================================="
    echo "Benchmark complete!"
    echo "========================================="
} | tee "$RESULTS_FILE"

echo ""
echo "Results saved to: $RESULTS_FILE"
echo "Log files: ${OUTPUT_PREFIX}_shard*_*.log"

# Final cleanup
ps aux | grep -i dbtest | awk '{print $2}' | xargs kill -9 2>/dev/null || true
