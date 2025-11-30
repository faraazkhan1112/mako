#pragma once

/**
 * Sundial Protocol Configuration
 * 
 * This header defines the configuration constants and types for the Sundial
 * distributed concurrency control protocol (VLDB 2018).
 * 
 * Sundial uses logical leases [wts, rts] on each tuple to dynamically determine
 * transaction commit order and maintain cache coherence.
 * 
 * Key concepts:
 * - wts (Write Timestamp): The timestamp of the last committed write
 * - rts (Read Timestamp / Lease End): The upper bound of the lease, extended on reads
 * - Hybrid CC: Wait-Die 2PL for write-write conflicts, OCC for read-write conflicts
 */

#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <limits>

namespace sundial {

// ============================================================================
// Type Definitions
// ============================================================================

// Timestamp type - using 64-bit for high precision
using timestamp_t = uint64_t;

// Thread ID type for lock ownership
using thread_id_t = int;

// Special values
static constexpr timestamp_t TIMESTAMP_INFINITY = std::numeric_limits<timestamp_t>::max();
static constexpr timestamp_t TIMESTAMP_ZERO = 0;
static constexpr thread_id_t NO_LOCK_OWNER = -1;

// ============================================================================
// Protocol Configuration
// ============================================================================

// Enable/disable Sundial protocol (set to 1 to enable)
#ifndef SUNDIAL_ENABLED
#define SUNDIAL_ENABLED 1
#endif

// Enable Wait-Die deadlock prevention for write-write conflicts
#ifndef SUNDIAL_WAIT_DIE
#define SUNDIAL_WAIT_DIE 1
#endif

// Enable read-only transaction optimization (fast path)
#ifndef SUNDIAL_READ_ONLY_OPT
#define SUNDIAL_READ_ONLY_OPT 1
#endif

// Maximum number of retry attempts for Wait-Die before aborting
#ifndef SUNDIAL_MAX_WAIT_RETRIES
#define SUNDIAL_MAX_WAIT_RETRIES 100
#endif

// Enable debug logging for Sundial operations (set to 1 for verbose output)
#ifndef SUNDIAL_DEBUG
#define SUNDIAL_DEBUG 0
#endif

// Enable statistics collection (lightweight)
#ifndef SUNDIAL_STATS
#define SUNDIAL_STATS 1
#endif

// ============================================================================
// Lease Configuration
// ============================================================================

// Default lease duration (in logical time units)
// When a transaction reads a tuple, it extends rts by at least this amount
#ifndef SUNDIAL_DEFAULT_LEASE_DURATION
#define SUNDIAL_DEFAULT_LEASE_DURATION 10
#endif

// Maximum lease extension allowed
#ifndef SUNDIAL_MAX_LEASE_EXTENSION
#define SUNDIAL_MAX_LEASE_EXTENSION 1000
#endif

// ============================================================================
// Lock Bits in Version Field
// ============================================================================

// We use the existing lock mechanism from TransactionTid, but add Sundial-specific
// tracking for the lock owner to implement Wait-Die
static constexpr uint64_t SUNDIAL_LOCK_BIT = 1ULL << 63;
static constexpr uint64_t SUNDIAL_LOCK_OWNER_MASK = 0x7FFFFFFFULL;  // 31 bits for thread ID
static constexpr uint64_t SUNDIAL_LOCK_OWNER_SHIFT = 32;

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Get current logical timestamp
 * This should be called to get a transaction's start timestamp
 */
inline timestamp_t get_current_timestamp() {
    static std::atomic<timestamp_t> global_ts{1};
    return global_ts.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Atomically read the global timestamp without incrementing
 */
inline timestamp_t read_current_timestamp() {
    static std::atomic<timestamp_t> global_ts{1};
    return global_ts.load(std::memory_order_relaxed);
}

/**
 * Compute the commit timestamp for a transaction based on Sundial rules:
 * commit_ts = max(max_wts_in_read_set + 1, max_rts_in_write_set + 1)
 */
inline timestamp_t compute_commit_ts(timestamp_t max_wts_read, timestamp_t max_rts_write) {
    return std::max(max_wts_read + 1, max_rts_write + 1);
}

/**
 * Check if a transaction can extend a tuple's rts to the desired commit_ts
 * Returns true if the tuple's current wts <= transaction's observed wts
 * (meaning no intervening write occurred)
 */
inline bool can_extend_lease(timestamp_t current_wts, timestamp_t observed_wts) {
    return current_wts == observed_wts;
}

/**
 * Wait-Die decision: should the requesting transaction wait or die?
 * In Wait-Die: older transactions wait, younger transactions abort (die)
 * 
 * @param requester_ts Timestamp of the requesting transaction
 * @param holder_ts Timestamp of the lock holder
 * @return true if requester should wait, false if requester should abort
 */
inline bool should_wait(timestamp_t requester_ts, timestamp_t holder_ts) {
    // Older transaction (lower timestamp) waits; younger aborts
    return requester_ts < holder_ts;
}

// ============================================================================
// Debug Macros
// ============================================================================

#if SUNDIAL_DEBUG
#include <cstdio>
#define SUNDIAL_LOG(fmt, ...) \
    fprintf(stderr, "[SUNDIAL] " fmt "\n", ##__VA_ARGS__)
#else
#define SUNDIAL_LOG(fmt, ...) ((void)0)
#endif

// ============================================================================
// Statistics Counters (for debugging/validation)
// ============================================================================

struct SundialStats {
    std::atomic<uint64_t> reads{0};           // Total reads with Sundial tracking
    std::atomic<uint64_t> writes{0};          // Total writes with Sundial tracking
    std::atomic<uint64_t> wts_validations{0}; // wts validation checks at commit
    std::atomic<uint64_t> wts_conflicts{0};   // wts changed (abort due to write conflict)
    std::atomic<uint64_t> lock_conflicts{0};  // Write-write lock conflicts
    std::atomic<uint64_t> commits{0};         // Successful commits with Sundial
    
    void print() const {
        fprintf(stderr, "\n=== Sundial Statistics ===\n");
        fprintf(stderr, "Reads tracked:     %lu\n", reads.load());
        fprintf(stderr, "Writes tracked:    %lu\n", writes.load());
        fprintf(stderr, "WTS validations:   %lu\n", wts_validations.load());
        fprintf(stderr, "WTS conflicts:     %lu\n", wts_conflicts.load());
        fprintf(stderr, "Lock conflicts:    %lu\n", lock_conflicts.load());
        fprintf(stderr, "Commits:           %lu\n", commits.load());
        fprintf(stderr, "==========================\n\n");
    }
    
    void reset() {
        reads.store(0);
        writes.store(0);
        wts_validations.store(0);
        wts_conflicts.store(0);
        lock_conflicts.store(0);
        commits.store(0);
    }
};

// Global statistics instance
inline SundialStats& get_sundial_stats() {
    static SundialStats stats;
    static bool registered = false;
    if (!registered) {
        registered = true;
        // Register to print stats at exit
        std::atexit([]() { 
            get_sundial_stats().print(); 
        });
    }
    return stats;
}

#if SUNDIAL_STATS
#define SUNDIAL_STAT_INC(field) sundial::get_sundial_stats().field.fetch_add(1, std::memory_order_relaxed)
#else
#define SUNDIAL_STAT_INC(field) ((void)0)
#endif

} // namespace sundial

