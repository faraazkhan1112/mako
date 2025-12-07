#!/bin/bash

# TPC-C Evaluation Script for Sundial Integration
# Usage: ./run_tpcc_eval.sh <config>
# Where <config> is one of: baseline, sundial

CONFIG=$1
NUM_RUNS=5
THREADS=6
TEMP_FILE="/tmp/tpcc_run_output.txt"

# Validate argument
if [ -z "$CONFIG" ]; then
    echo "Usage: $0 <config>"
    echo "  config: baseline | sundial"
    exit 1
fi

# Set output file and description based on config
case $CONFIG in
    baseline)
        OUTPUT_FILE="baseline_tpccresults.txt"
        CONFIG_DESC="Baseline (SUNDIAL_ENABLED=0)"
        ;;
    sundial)
        OUTPUT_FILE="sundial_tpccresults.txt"
        CONFIG_DESC="Sundial (SUNDIAL_ENABLED=1)"
        ;;
    *)
        echo "Error: Unknown config '$CONFIG'"
        echo "  Valid options: baseline | sundial"
        exit 1
        ;;
esac

echo "Running: $CONFIG_DESC"

# Kill any lingering processes
echo "Killing any lingering processes..."
pkill -9 -f "test_rpc|dbtest|simpleTransaction" 2>/dev/null
sleep 2

# Create output file with header
echo "========================================" > $OUTPUT_FILE
echo "TPC-C Evaluation Results" >> $OUTPUT_FILE
echo "Configuration: $CONFIG_DESC" >> $OUTPUT_FILE
echo "Threads: $THREADS" >> $OUTPUT_FILE
echo "Date: $(date '+%a %b %d %Y')" >> $OUTPUT_FILE
echo "========================================" >> $OUTPUT_FILE
echo "" >> $OUTPUT_FILE

# Arrays to store results for summary
declare -a THROUGHPUTS
declare -a LATENCIES
declare -a ABORT_RATES
declare -a RO_FAST_PCTS

# Function to extract stats from output
extract_stats() {
    local run_num=$1
    local temp_file=$2
    
    # Extract benchmark stats
    local runtime=$(grep "^runtime:" $temp_file | tail -1 | awk '{print $2}')
    local n_commits=$(grep "^n_commits:" $temp_file | tail -1 | awk '{print $2}')
    local throughput=$(grep "^agg_throughput:" $temp_file | tail -1 | awk '{print $2}')
    local latency=$(grep "^avg_latency:" $temp_file | tail -1 | awk '{print $2}')
    local abort_rate=$(grep "^agg_abort_rate:" $temp_file | tail -1 | awk '{print $2}')
    
    # Store for summary (remove commas for arithmetic)
    THROUGHPUTS+=("$throughput")
    LATENCIES+=("$latency")
    ABORT_RATES+=("$abort_rate")
    
    # Extract Sundial stats (if present)
    local reads_tracked=$(grep "Reads tracked:" $temp_file | tail -1 | awk '{print $3}')
    local writes_tracked=$(grep "Writes tracked:" $temp_file | tail -1 | awk '{print $3}')
    local wts_validations=$(grep "WTS validations:" $temp_file | tail -1 | awk '{print $3}')
    local wts_conflicts=$(grep "WTS conflicts:" $temp_file | tail -1 | awk '{print $3}')
    local lock_conflicts=$(grep "Lock conflicts:" $temp_file | tail -1 | awk '{print $3}')
    local commits=$(grep "Commits:" $temp_file | tail -1 | awk '{print $2}')
    local ro_fast=$(grep "RO fast path:" $temp_file | tail -1 | awk '{print $4}')
    local ro_slow=$(grep "RO slow path:" $temp_file | tail -1 | awk '{print $4}')
    
    # Calculate percentages if Sundial stats exist
    local wts_conflict_pct="N/A"
    local ro_fast_pct="N/A"
    
    if [ -n "$wts_validations" ] && [ "$wts_validations" != "0" ]; then
        wts_conflict_pct=$(echo "scale=4; $wts_conflicts / $wts_validations * 100" | bc 2>/dev/null || echo "N/A")
    fi
    
    if [ -n "$ro_fast" ] && [ -n "$ro_slow" ]; then
        local ro_total=$((ro_fast + ro_slow))
        if [ "$ro_total" != "0" ]; then
            ro_fast_pct=$(echo "scale=2; $ro_fast / $ro_total * 100" | bc 2>/dev/null || echo "N/A")
            RO_FAST_PCTS+=("$ro_fast_pct")
        fi
    fi
    
    # Write to output file
    echo "================================================================================" >> $OUTPUT_FILE
    echo "RUN $run_num" >> $OUTPUT_FILE
    echo "================================================================================" >> $OUTPUT_FILE
    echo "" >> $OUTPUT_FILE
    echo "--- Benchmark Statistics ---" >> $OUTPUT_FILE
    printf "%-18s %s sec\n" "runtime:" "$runtime" >> $OUTPUT_FILE
    printf "%-18s %s\n" "n_commits:" "$n_commits" >> $OUTPUT_FILE
    printf "%-18s %s ops/sec\n" "agg_throughput:" "$throughput" >> $OUTPUT_FILE
    printf "%-18s %s ms\n" "avg_latency:" "$latency" >> $OUTPUT_FILE
    printf "%-18s %s aborts/sec\n" "agg_abort_rate:" "$abort_rate" >> $OUTPUT_FILE
    echo "" >> $OUTPUT_FILE
    
    # Only write Sundial stats if they exist
    if [ -n "$reads_tracked" ]; then
        echo "--- Sundial Statistics ---" >> $OUTPUT_FILE
        printf "%-18s %s\n" "Reads tracked:" "$reads_tracked" >> $OUTPUT_FILE
        printf "%-18s %s\n" "Writes tracked:" "$writes_tracked" >> $OUTPUT_FILE
        printf "%-18s %s\n" "WTS validations:" "$wts_validations" >> $OUTPUT_FILE
        printf "%-18s %s (%s%%)\n" "WTS conflicts:" "$wts_conflicts" "$wts_conflict_pct" >> $OUTPUT_FILE
        printf "%-18s %s\n" "Lock conflicts:" "$lock_conflicts" >> $OUTPUT_FILE
        printf "%-18s %s\n" "Commits:" "$commits" >> $OUTPUT_FILE
        printf "%-18s %s (%s%%)\n" "RO fast path:" "$ro_fast" "$ro_fast_pct" >> $OUTPUT_FILE
        printf "%-18s %s\n" "RO slow path:" "$ro_slow" >> $OUTPUT_FILE
        echo "" >> $OUTPUT_FILE
    fi
}

# Run benchmark NUM_RUNS times
for i in $(seq 1 $NUM_RUNS); do
    echo ""
    echo "========================================"
    echo "Run $i of $NUM_RUNS"
    echo "========================================"
    
    # Run the benchmark and capture full output to temp file
    ./build/dbtest --num-threads $THREADS --shard-index 0 \
        --shard-config src/mako/config/local-shards1-warehouses${THREADS}.yml \
        -F config/1leader_2followers/paxos6_shardidx0.yml \
        -F config/occ_paxos.yml -P localhost > $TEMP_FILE 2>&1
    
    # Extract and format stats
    extract_stats $i $TEMP_FILE
    
    # Kill processes between runs
    pkill -9 -f "test_rpc|dbtest|simpleTransaction" 2>/dev/null
    sleep 2
    
    echo "Run $i completed."
done

# Calculate and write summary
echo "================================================================================" >> $OUTPUT_FILE
echo "SUMMARY ($NUM_RUNS Runs)" >> $OUTPUT_FILE
echo "================================================================================" >> $OUTPUT_FILE
echo "" >> $OUTPUT_FILE

# Calculate averages
if [ ${#THROUGHPUTS[@]} -gt 0 ]; then
    sum_tp=0
    sum_lat=0
    sum_abort=0
    
    for tp in "${THROUGHPUTS[@]}"; do
        sum_tp=$(echo "$sum_tp + $tp" | bc 2>/dev/null || echo "0")
    done
    for lat in "${LATENCIES[@]}"; do
        sum_lat=$(echo "$sum_lat + $lat" | bc 2>/dev/null || echo "0")
    done
    for ab in "${ABORT_RATES[@]}"; do
        sum_abort=$(echo "$sum_abort + $ab" | bc 2>/dev/null || echo "0")
    done
    
    avg_tp=$(echo "scale=1; $sum_tp / $NUM_RUNS" | bc 2>/dev/null || echo "N/A")
    avg_lat=$(echo "scale=4; $sum_lat / $NUM_RUNS" | bc 2>/dev/null || echo "N/A")
    avg_abort=$(echo "scale=2; $sum_abort / $NUM_RUNS" | bc 2>/dev/null || echo "N/A")
    
    echo "Average Throughput:  $avg_tp ops/sec" >> $OUTPUT_FILE
    echo "Average Latency:     $avg_lat ms" >> $OUTPUT_FILE
    echo "Average Abort Rate:  $avg_abort aborts/sec" >> $OUTPUT_FILE
    
    if [ ${#RO_FAST_PCTS[@]} -gt 0 ]; then
        sum_ro=0
        for ro in "${RO_FAST_PCTS[@]}"; do
            sum_ro=$(echo "$sum_ro + $ro" | bc 2>/dev/null || echo "0")
        done
        avg_ro=$(echo "scale=2; $sum_ro / ${#RO_FAST_PCTS[@]}" | bc 2>/dev/null || echo "N/A")
        echo "Average RO Fast Path: $avg_ro%" >> $OUTPUT_FILE
    fi
fi

echo "" >> $OUTPUT_FILE
echo "================================================================================" >> $OUTPUT_FILE

# Cleanup
rm -f $TEMP_FILE

echo ""
echo "========================================"
echo "All $NUM_RUNS runs completed!"
echo "Results saved to: $OUTPUT_FILE"
echo "========================================"

# Print summary to console
echo ""
echo "Quick Summary:"
cat $OUTPUT_FILE | grep -A 10 "SUMMARY"
