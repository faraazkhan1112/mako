#!/bin/bash

# Sundial Replicated Benchmark Script
# Usage: ./benchmark_sundial_replicated.sh [sundial|baseline|sundial_readonly|baseline_readonly]
#
# This script runs a 1-shard replicated TPC-C benchmark with full Paxos consensus.
# Results are saved to corresponding results file based on mode.

set -e

# Check command line argument
if [ -z "$1" ]; then
    echo "Usage: $0 [sundial|baseline|sundial_readonly|baseline_readonly]"
    echo "  sundial           - Run benchmark with SUNDIAL_ENABLED=1 (standard workload)"
    echo "  baseline          - Run benchmark with SUNDIAL_ENABLED=0 (requires rebuild)"
    echo "  sundial_readonly  - Run benchmark with SUNDIAL_READ_ONLY_OPT=1 (read-heavy workload)"
    echo "  baseline_readonly - Run benchmark with SUNDIAL_ENABLED=0 (read-heavy workload)"
    exit 1
fi

MODE="$1"
if [ "$MODE" != "sundial" ] && [ "$MODE" != "baseline" ] && [ "$MODE" != "sundial_readonly" ] && [ "$MODE" != "baseline_readonly" ]; then
    echo "Error: Invalid mode '$MODE'. Use 'sundial', 'baseline', 'sundial_readonly', or 'baseline_readonly'."
    exit 1
fi

# Set output file based on mode
OUTPUT_FILE="${MODE}_replication_results.txt"

# Get script directory and project root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

echo "========================================="
echo "Sundial Replicated Benchmark"
echo "========================================="
echo "Mode: $MODE"
echo "Output: $OUTPUT_FILE"
echo "Threads: 2"
echo "Topology: 1 Shard, 3 Replicas + 1 Learner"
echo "========================================="

# Clean up any existing processes
echo "Cleaning up old processes..."
pkill -9 dbtest 2>/dev/null || true
pkill -9 -f "bash.*shard.sh" 2>/dev/null || true
sleep 2

# Clean up old files
rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Configuration
TRD=2
SHARD_IDX=0
NSHARD=1
CONFIG_PATH="src/mako/config"
PAXOS_CONFIG_PATH="config/1leader_2followers"

# Build the command
CMD="./build/dbtest --num-threads $TRD --shard-index $SHARD_IDX \
  --shard-config $CONFIG_PATH/local-shards${NSHARD}-warehouses1.yml \
  -F $PAXOS_CONFIG_PATH/paxos${TRD}_shardidx${SHARD_IDX}.yml \
  -F config/occ_paxos.yml \
  --is-replicated"

echo ""
echo "Starting replicated benchmark..."
echo "Command: $CMD -P <role>"
echo ""

# Log file prefix
LOG_PREFIX="${MODE}_replicated"

# Start followers and learner in background
echo "[1/4] Starting learner..."
nohup $CMD -P learner > ${LOG_PREFIX}_learner.log 2>&1 &
LEARNER_PID=$!

echo "[2/4] Starting follower p2..."
nohup $CMD -P p2 > ${LOG_PREFIX}_p2.log 2>&1 &
P2_PID=$!

echo "[3/4] Starting follower p1..."
nohup $CMD -P p1 > ${LOG_PREFIX}_p1.log 2>&1 &
P1_PID=$!

# Wait for followers to initialize and establish connections
echo "Waiting for followers to initialize (5 seconds)..."
sleep 5

# Start leader - this runs the benchmark
echo "[4/4] Starting leader (localhost)..."
echo "Benchmark running... (this may take 1-2 minutes)"
$CMD -P localhost > ${LOG_PREFIX}_localhost.log 2>&1 &
LOCALHOST_PID=$!

# Wait for benchmark to complete (benchmark runs ~30s + loading time)
# Give it extra time for slow VMs (read-heavy workloads can be 4x slower)
echo "Waiting for benchmark to complete (180 seconds)..."
sleep 180

# Graceful shutdown
echo ""
echo "Stopping processes..."
kill -TERM $LOCALHOST_PID $P1_PID $P2_PID $LEARNER_PID 2>/dev/null || true
sleep 3
kill -9 $LOCALHOST_PID $P1_PID $P2_PID $LEARNER_PID 2>/dev/null || true
pkill -9 dbtest 2>/dev/null || true
sleep 2

# Extract and save results
echo ""
echo "========================================="
echo "Extracting results..."
echo "========================================="

LEADER_LOG="${LOG_PREFIX}_localhost.log"

if [ ! -f "$LEADER_LOG" ]; then
    echo "Error: Leader log file not found!"
    exit 1
fi

# Check if benchmark completed
if ! grep -q "agg_throughput" "$LEADER_LOG"; then
    echo "Error: Benchmark did not complete. Check logs for errors."
    echo ""
    echo "Last 20 lines of leader log:"
    tail -20 "$LEADER_LOG"
    exit 1
fi

# Create results file with header
{
    echo "========================================="
    echo "Sundial Replicated Benchmark Results"
    echo "========================================="
    echo "Mode: $MODE"
    echo "Date: $(date)"
    echo "Threads: $TRD"
    echo "Shards: $NSHARD"
    echo "Topology: 1 Leader + 2 Followers + 1 Learner"
    echo "Replication: Paxos (Multi-Paxos)"
    echo "========================================="
    echo ""
    echo "--- Benchmark Statistics ---"
    grep -E "^runtime:|^n_commits:|^agg_throughput:|^avg_per_core_throughput:|^agg_persist_throughput:|^avg_latency:|^agg_abort_rate:" "$LEADER_LOG"
    echo ""
    echo "--- Transaction Latencies ---"
    grep -E "_local_commit_latency:" "$LEADER_LOG"
    echo ""
    echo "--- Abort Ratios ---"
    grep -E "_local_abort_ratio:" "$LEADER_LOG"
    echo ""
    echo "--- Remote Transaction Stats ---"
    grep -E "_remote_ratio:|_remote_abort_ratio:" "$LEADER_LOG"
    echo ""
    echo "========================================="
    echo "Full benchmark output saved in: $LEADER_LOG"
    echo "========================================="
} > "$OUTPUT_FILE"

# Display results
echo ""
cat "$OUTPUT_FILE"

echo ""
echo "========================================="
echo "Results saved to: $OUTPUT_FILE"
echo "Log files: ${LOG_PREFIX}_*.log"
echo "========================================="

# Summary for quick comparison
echo ""
echo "Quick Summary:"
THROUGHPUT=$(grep "agg_throughput:" "$LEADER_LOG" | awk '{print $2}')
LATENCY=$(grep "avg_latency:" "$LEADER_LOG" | awk '{print $2}')
ORDERSTATUS_LAT=$(grep "OrderStatus_local_commit_latency:" "$LEADER_LOG" | awk '{print $2}')
echo "  Throughput: $THROUGHPUT ops/sec"
echo "  Avg Latency: $LATENCY ms"
echo "  OrderStatus Latency: $ORDERSTATUS_LAT ms"

