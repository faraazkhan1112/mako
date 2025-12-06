/**
 * Sundial Protocol Unit Tests - Phase 1
 * 
 * Tests the Sundial logical lease implementation.
 * 
 * Coverage:
 * 1. SundialConfig utility functions (timestamps, lease operations)
 * 2. Atomic lease field operations (wts, rts, lock_owner)
 * 3. Sundial locking operations (try_lock, unlock, is_locked_by)
 * 4. Lease extension (extend_rts)
 * 5. Wait-Die policy
 * 
 * Note: This test uses a standalone SundialTuple mock to avoid STO framework
 * dependencies while still validating the core Sundial mechanisms.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <chrono>
#include <set>

// Include only SundialConfig - it's self-contained
#include "mako/benchmarks/sto/SundialConfig.hh"

// ============================================================================
// Mock Tuple for Testing Sundial Lease Operations
// This mirrors the Sundial fields in stuffed_str without STO dependencies
// ============================================================================

struct SundialTuple {
    std::atomic<sundial::timestamp_t> wts_{0};
    std::atomic<sundial::timestamp_t> rts_{0};
    std::atomic<sundial::thread_id_t> lock_owner_{sundial::NO_LOCK_OWNER};
    std::atomic<sundial::timestamp_t> lock_holder_ts_{0};  // Phase 2a: Lock holder's timestamp
    
    // Lease getters/setters
    sundial::timestamp_t get_wts() const {
        return wts_.load(std::memory_order_acquire);
    }
    
    void set_wts(sundial::timestamp_t ts) {
        wts_.store(ts, std::memory_order_release);
    }
    
    sundial::timestamp_t get_rts() const {
        return rts_.load(std::memory_order_acquire);
    }
    
    void set_rts(sundial::timestamp_t ts) {
        rts_.store(ts, std::memory_order_release);
    }
    
    sundial::timestamp_t extend_rts(sundial::timestamp_t new_rts) {
        sundial::timestamp_t current_rts = rts_.load(std::memory_order_acquire);
        while (new_rts > current_rts) {
            if (rts_.compare_exchange_weak(current_rts, new_rts,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                return new_rts;
            }
        }
        return current_rts;
    }
    
    // Lock operations (Phase 2a: with timestamp support)
    sundial::thread_id_t get_lock_owner() const {
        return lock_owner_.load(std::memory_order_acquire);
    }
    
    sundial::timestamp_t get_lock_holder_ts() const {
        return lock_holder_ts_.load(std::memory_order_acquire);
    }
    
    bool try_sundial_lock(sundial::thread_id_t thread_id, 
                          sundial::timestamp_t txn_start_ts = 0) {
        sundial::thread_id_t expected = sundial::NO_LOCK_OWNER;
        if (lock_owner_.compare_exchange_strong(expected, thread_id,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            lock_holder_ts_.store(txn_start_ts, std::memory_order_release);
            return true;
        }
        return false;
    }
    
    void sundial_unlock(sundial::thread_id_t thread_id) {
        sundial::thread_id_t expected = thread_id;
        if (lock_owner_.compare_exchange_strong(expected, sundial::NO_LOCK_OWNER,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
            lock_holder_ts_.store(0, std::memory_order_release);
        }
    }
    
    bool is_locked_by_other(sundial::thread_id_t my_thread_id) const {
        sundial::thread_id_t owner = lock_owner_.load(std::memory_order_acquire);
        return owner != sundial::NO_LOCK_OWNER && owner != my_thread_id;
    }
    
    bool is_locked_by(sundial::thread_id_t thread_id) const {
        return lock_owner_.load(std::memory_order_acquire) == thread_id;
    }
    
    // Get lock info atomically (for Wait-Die decisions)
    bool get_lock_info(sundial::thread_id_t& out_owner,
                       sundial::timestamp_t& out_holder_ts) const {
        out_owner = lock_owner_.load(std::memory_order_acquire);
        if (out_owner == sundial::NO_LOCK_OWNER) {
            out_holder_ts = 0;
            return false;
        }
        out_holder_ts = lock_holder_ts_.load(std::memory_order_acquire);
        return true;
    }
};

// ============================================================================
// SundialConfig Tests
// ============================================================================

class SundialConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
    }
};

TEST_F(SundialConfigTest, GetCurrentTimestampIncrementsMonotonically) {
    sundial::timestamp_t ts1 = sundial::get_current_timestamp();
    sundial::timestamp_t ts2 = sundial::get_current_timestamp();
    sundial::timestamp_t ts3 = sundial::get_current_timestamp();
    
    EXPECT_GT(ts2, ts1) << "Second timestamp should be greater than first";
    EXPECT_GT(ts3, ts2) << "Third timestamp should be greater than second";
}

TEST_F(SundialConfigTest, GetCurrentTimestampConcurrent) {
    constexpr int kNumThreads = 4;
    constexpr int kTimestampsPerThread = 1000;
    
    std::vector<std::vector<sundial::timestamp_t>> per_thread_timestamps(kNumThreads);
    std::vector<std::thread> threads;
    
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < kTimestampsPerThread; ++j) {
                per_thread_timestamps[i].push_back(sundial::get_current_timestamp());
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Collect all timestamps and verify uniqueness
    std::set<sundial::timestamp_t> all_timestamps;
    for (const auto& vec : per_thread_timestamps) {
        for (auto ts : vec) {
            EXPECT_TRUE(all_timestamps.insert(ts).second) 
                << "Duplicate timestamp found: " << ts;
        }
    }
    
    EXPECT_EQ(all_timestamps.size(), static_cast<size_t>(kNumThreads * kTimestampsPerThread));
}

TEST_F(SundialConfigTest, ComputeCommitTsBasic) {
    // commit_ts = max(max_wts_read + 1, max_rts_write + 1)
    
    // Case 1: max_wts_read dominates
    sundial::timestamp_t ts1 = sundial::compute_commit_ts(100, 50);
    EXPECT_EQ(ts1, 101);
    
    // Case 2: max_rts_write dominates  
    sundial::timestamp_t ts2 = sundial::compute_commit_ts(50, 100);
    EXPECT_EQ(ts2, 101);
    
    // Case 3: Equal values
    sundial::timestamp_t ts3 = sundial::compute_commit_ts(100, 100);
    EXPECT_EQ(ts3, 101);
    
    // Case 4: Zero values
    sundial::timestamp_t ts4 = sundial::compute_commit_ts(0, 0);
    EXPECT_EQ(ts4, 1);
}

TEST_F(SundialConfigTest, CanExtendLease) {
    // Lease can be extended only if current_wts == observed_wts
    EXPECT_TRUE(sundial::can_extend_lease(100, 100));
    EXPECT_FALSE(sundial::can_extend_lease(101, 100)); // wts changed
    EXPECT_FALSE(sundial::can_extend_lease(99, 100));  // also mismatch
}

TEST_F(SundialConfigTest, ShouldWaitWaitDieLogic) {
    // Wait-Die: older transaction (smaller ts) waits, younger (larger ts) aborts
    
    // Older requester should wait
    EXPECT_TRUE(sundial::should_wait(100, 200));
    
    // Younger requester should abort (not wait)
    EXPECT_FALSE(sundial::should_wait(200, 100));
    
    // Same timestamp - should not wait (abort to be safe)
    EXPECT_FALSE(sundial::should_wait(100, 100));
}

TEST_F(SundialConfigTest, SpecialTimestampValues) {
    EXPECT_EQ(sundial::TIMESTAMP_ZERO, 0ULL);
    EXPECT_EQ(sundial::TIMESTAMP_INFINITY, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(sundial::NO_LOCK_OWNER, -1);
}

// ============================================================================
// SundialTuple Lease Tests (mirrors stuffed_str behavior)
// ============================================================================

class SundialTupleTest : public ::testing::Test {
protected:
    SundialTuple tuple_;
    
    void SetUp() override {
    }
};

TEST_F(SundialTupleTest, InitialLeaseValues) {
    // After construction, wts and rts should be 0, lock_owner should be NO_LOCK_OWNER
    EXPECT_EQ(tuple_.get_wts(), 0ULL);
    EXPECT_EQ(tuple_.get_rts(), 0ULL);
    EXPECT_EQ(tuple_.get_lock_owner(), sundial::NO_LOCK_OWNER);
}

TEST_F(SundialTupleTest, SetGetWts) {
    tuple_.set_wts(100);
    EXPECT_EQ(tuple_.get_wts(), 100ULL);
    
    tuple_.set_wts(200);
    EXPECT_EQ(tuple_.get_wts(), 200ULL);
}

TEST_F(SundialTupleTest, SetGetRts) {
    tuple_.set_rts(100);
    EXPECT_EQ(tuple_.get_rts(), 100ULL);
    
    tuple_.set_rts(200);
    EXPECT_EQ(tuple_.get_rts(), 200ULL);
}

TEST_F(SundialTupleTest, ExtendRtsBasic) {
    tuple_.set_rts(100);
    
    // Extending to higher value should succeed
    sundial::timestamp_t result = tuple_.extend_rts(200);
    EXPECT_EQ(result, 200ULL);
    EXPECT_EQ(tuple_.get_rts(), 200ULL);
    
    // Extending to lower value should be no-op
    result = tuple_.extend_rts(150);
    EXPECT_EQ(result, 200ULL);
    EXPECT_EQ(tuple_.get_rts(), 200ULL);
}

TEST_F(SundialTupleTest, ExtendRtsConcurrent) {
    tuple_.set_rts(0);
    
    constexpr int kNumThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<sundial::timestamp_t> max_extended{0};
    
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&, i]() {
            sundial::timestamp_t my_rts = (i + 1) * 100;
            sundial::timestamp_t result = tuple_.extend_rts(my_rts);
            
            // Track the maximum successfully extended value
            sundial::timestamp_t current_max = max_extended.load();
            while (result > current_max) {
                if (max_extended.compare_exchange_weak(current_max, result)) {
                    break;
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Final rts should be the maximum value (800 = 8 * 100)
    EXPECT_EQ(tuple_.get_rts(), 800ULL);
}

// ============================================================================
// Sundial Locking Tests
// ============================================================================

class SundialLockingTest : public ::testing::Test {
protected:
    SundialTuple tuple_;
    
    void SetUp() override {
    }
};

TEST_F(SundialLockingTest, TryLockUnlockBasic) {
    sundial::thread_id_t thread1 = 1;
    
    // Lock should succeed on unlocked tuple
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1));
    EXPECT_EQ(tuple_.get_lock_owner(), thread1);
    
    // Unlock should release the lock
    tuple_.sundial_unlock(thread1);
    EXPECT_EQ(tuple_.get_lock_owner(), sundial::NO_LOCK_OWNER);
}

TEST_F(SundialLockingTest, DoubleLockFails) {
    sundial::thread_id_t thread1 = 1;
    sundial::thread_id_t thread2 = 2;
    
    // First lock succeeds
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1));
    
    // Second lock by different thread fails
    EXPECT_FALSE(tuple_.try_sundial_lock(thread2));
    
    // Lock owner should still be thread1
    EXPECT_EQ(tuple_.get_lock_owner(), thread1);
}

TEST_F(SundialLockingTest, SameThreadDoubleLock) {
    sundial::thread_id_t thread1 = 1;
    
    // First lock succeeds
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1));
    
    // Same thread trying to lock again fails (we don't support reentrant locks)
    // This is intentional - transactions shouldn't try to lock the same tuple twice
    EXPECT_FALSE(tuple_.try_sundial_lock(thread1));
}

TEST_F(SundialLockingTest, IsLockedByOther) {
    sundial::thread_id_t thread1 = 1;
    sundial::thread_id_t thread2 = 2;
    
    // Not locked - should not be locked by other
    EXPECT_FALSE(tuple_.is_locked_by_other(thread1));
    EXPECT_FALSE(tuple_.is_locked_by_other(thread2));
    
    // Lock by thread1
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1));
    
    // thread1 sees it as NOT locked by other
    EXPECT_FALSE(tuple_.is_locked_by_other(thread1));
    
    // thread2 sees it as locked by other
    EXPECT_TRUE(tuple_.is_locked_by_other(thread2));
}

TEST_F(SundialLockingTest, IsLockedBy) {
    sundial::thread_id_t thread1 = 1;
    sundial::thread_id_t thread2 = 2;
    
    // Not locked
    EXPECT_FALSE(tuple_.is_locked_by(thread1));
    EXPECT_FALSE(tuple_.is_locked_by(thread2));
    
    // Lock by thread1
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1));
    
    EXPECT_TRUE(tuple_.is_locked_by(thread1));
    EXPECT_FALSE(tuple_.is_locked_by(thread2));
}

TEST_F(SundialLockingTest, WrongThreadUnlock) {
    sundial::thread_id_t thread1 = 1;
    sundial::thread_id_t thread2 = 2;
    
    // Lock by thread1
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1));
    
    // Unlock by thread2 should fail (lock stays held)
    tuple_.sundial_unlock(thread2);
    EXPECT_EQ(tuple_.get_lock_owner(), thread1);
    
    // Proper unlock by thread1 should work
    tuple_.sundial_unlock(thread1);
    EXPECT_EQ(tuple_.get_lock_owner(), sundial::NO_LOCK_OWNER);
}

TEST_F(SundialLockingTest, ConcurrentLocking) {
    constexpr int kNumThreads = 8;
    std::atomic<int> successful_locks{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&, i]() {
            if (tuple_.try_sundial_lock(i)) {
                successful_locks.fetch_add(1);
                // Hold lock briefly
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                tuple_.sundial_unlock(i);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Only one thread should have successfully acquired the lock first
    // But subsequent threads may also acquire after unlock
    EXPECT_GE(successful_locks.load(), 1);
    
    // Lock should be released at the end
    EXPECT_EQ(tuple_.get_lock_owner(), sundial::NO_LOCK_OWNER);
}

// ============================================================================
// Lease Coherence Tests (Simulated Sundial Operations)
// ============================================================================

class SundialLeaseCoherenceTest : public ::testing::Test {
protected:
    SundialTuple tuple_;
    
    void SetUp() override {
        // Initialize lease with some values
        tuple_.set_wts(100);
        tuple_.set_rts(100);
    }
};

TEST_F(SundialLeaseCoherenceTest, ReadDoesNotChangeWts) {
    sundial::timestamp_t initial_wts = tuple_.get_wts();
    
    // Simulate a read: observe wts and rts
    sundial::timestamp_t observed_wts = tuple_.get_wts();
    sundial::timestamp_t observed_rts = tuple_.get_rts();
    
    EXPECT_EQ(observed_wts, 100ULL);
    EXPECT_EQ(observed_rts, 100ULL);
    
    // wts should not change after read
    EXPECT_EQ(tuple_.get_wts(), initial_wts);
}

TEST_F(SundialLeaseCoherenceTest, WriteUpdatesWtsAndRts) {
    sundial::thread_id_t writer = 1;
    sundial::timestamp_t commit_ts = 200;
    
    // Simulate write: acquire lock, update wts/rts
    EXPECT_TRUE(tuple_.try_sundial_lock(writer));
    
    tuple_.set_wts(commit_ts);
    
    // rts should be at least commit_ts after write
    if (commit_ts > tuple_.get_rts()) {
        tuple_.set_rts(commit_ts);
    }
    
    tuple_.sundial_unlock(writer);
    
    EXPECT_EQ(tuple_.get_wts(), 200ULL);
    EXPECT_EQ(tuple_.get_rts(), 200ULL);
}

TEST_F(SundialLeaseCoherenceTest, LeaseExtensionSucceedsIfWtsUnchanged) {
    // Transaction reads at wts=100, rts=100
    sundial::timestamp_t observed_wts = tuple_.get_wts();
    
    // Transaction wants to commit at ts=150, needs to extend rts
    sundial::timestamp_t commit_ts = 150;
    
    // Check if we can extend (wts hasn't changed)
    EXPECT_TRUE(sundial::can_extend_lease(tuple_.get_wts(), observed_wts));
    
    // Extend the lease
    tuple_.extend_rts(commit_ts);
    
    EXPECT_EQ(tuple_.get_rts(), 150ULL);
    EXPECT_EQ(tuple_.get_wts(), 100ULL); // wts unchanged
}

TEST_F(SundialLeaseCoherenceTest, LeaseExtensionFailsIfWtsChanged) {
    // Transaction reads at wts=100
    sundial::timestamp_t observed_wts = tuple_.get_wts();
    
    // Meanwhile, another transaction writes (changes wts)
    tuple_.set_wts(105);
    
    // Original transaction tries to extend lease at commit time
    // Should fail because wts changed
    EXPECT_FALSE(sundial::can_extend_lease(tuple_.get_wts(), observed_wts));
}

TEST_F(SundialLeaseCoherenceTest, WriteWriteConflictDetection) {
    sundial::thread_id_t txn1 = 1;
    sundial::thread_id_t txn2 = 2;
    
    // Txn1 acquires lock
    EXPECT_TRUE(tuple_.try_sundial_lock(txn1));
    
    // Txn2 tries to write - should see conflict
    EXPECT_TRUE(tuple_.is_locked_by_other(txn2));
    EXPECT_FALSE(tuple_.try_sundial_lock(txn2));
    
    // Txn1 commits
    tuple_.set_wts(200);
    tuple_.sundial_unlock(txn1);
    
    // Now Txn2 can proceed
    EXPECT_FALSE(tuple_.is_locked_by_other(txn2));
    EXPECT_TRUE(tuple_.try_sundial_lock(txn2));
    tuple_.sundial_unlock(txn2);
}

TEST_F(SundialLeaseCoherenceTest, ReadWriteConflictDetection) {
    // This tests the core Sundial read-write conflict scenario:
    // T1 reads a tuple, T2 writes to it, T1 should abort at commit time
    
    // T1 reads the tuple - captures observed_wts
    sundial::timestamp_t t1_observed_wts = tuple_.get_wts();
    sundial::timestamp_t t1_observed_rts = tuple_.get_rts();
    EXPECT_EQ(t1_observed_wts, 100ULL);
    EXPECT_EQ(t1_observed_rts, 100ULL);
    
    // T2 comes along and writes to the same tuple (completes first)
    sundial::thread_id_t t2_id = 2;
    EXPECT_TRUE(tuple_.try_sundial_lock(t2_id));
    tuple_.set_wts(150);  // T2 commits at ts=150
    tuple_.set_rts(150);
    tuple_.sundial_unlock(t2_id);
    
    // T1 tries to commit - must validate that wts hasn't changed
    // This is the Sundial OCC validation for read-write conflicts
    bool can_commit = sundial::can_extend_lease(tuple_.get_wts(), t1_observed_wts);
    
    // T1 should FAIL validation because wts changed (100 -> 150)
    EXPECT_FALSE(can_commit) << "T1 should abort: wts changed from 100 to 150";
}

TEST_F(SundialLeaseCoherenceTest, ReadWriteNoConflict) {
    // Test that concurrent reads with no intervening writes succeed
    
    // T1 and T2 both read the tuple
    sundial::timestamp_t t1_observed_wts = tuple_.get_wts();
    sundial::timestamp_t t2_observed_wts = tuple_.get_wts();
    
    EXPECT_EQ(t1_observed_wts, 100ULL);
    EXPECT_EQ(t2_observed_wts, 100ULL);
    
    // T1 commits (read-only, just extends lease if needed)
    sundial::timestamp_t t1_commit_ts = 105;
    bool t1_can_commit = sundial::can_extend_lease(tuple_.get_wts(), t1_observed_wts);
    EXPECT_TRUE(t1_can_commit);
    if (t1_can_commit && t1_commit_ts > tuple_.get_rts()) {
        tuple_.extend_rts(t1_commit_ts);
    }
    
    // T2 commits (read-only) - should still succeed
    sundial::timestamp_t t2_commit_ts = 110;
    bool t2_can_commit = sundial::can_extend_lease(tuple_.get_wts(), t2_observed_wts);
    EXPECT_TRUE(t2_can_commit) << "T2 should succeed: wts unchanged, just lease extended";
    if (t2_can_commit && t2_commit_ts > tuple_.get_rts()) {
        tuple_.extend_rts(t2_commit_ts);
    }
    
    // rts should be extended to 110 (max of both)
    EXPECT_EQ(tuple_.get_rts(), 110ULL);
    // wts should remain unchanged
    EXPECT_EQ(tuple_.get_wts(), 100ULL);
}

// ============================================================================
// Multi-Tuple Transaction Tests
// ============================================================================

TEST_F(SundialLeaseCoherenceTest, CommitTsFromMultipleReadsAndWrites) {
    // Simulate a transaction that reads from multiple tuples and writes to one
    // commit_ts = max(max_wts_in_read_set + 1, max_rts_in_write_set + 1)
    
    // Create additional tuples for multi-access scenario
    SundialTuple tuple2, tuple3;
    tuple2.set_wts(50);   tuple2.set_rts(60);
    tuple3.set_wts(200);  tuple3.set_rts(250);  // This has the highest wts
    
    // Transaction reads from tuple_ (wts=100), tuple2 (wts=50), tuple3 (wts=200)
    sundial::timestamp_t max_wts_read = 0;
    
    // Read tuple_
    sundial::timestamp_t wts1 = tuple_.get_wts();  // 100
    max_wts_read = std::max(max_wts_read, wts1);
    
    // Read tuple2
    sundial::timestamp_t wts2 = tuple2.get_wts();  // 50
    max_wts_read = std::max(max_wts_read, wts2);
    
    // Read tuple3
    sundial::timestamp_t wts3 = tuple3.get_wts();  // 200
    max_wts_read = std::max(max_wts_read, wts3);
    
    EXPECT_EQ(max_wts_read, 200ULL);
    
    // Transaction writes to tuple_ (rts=100)
    sundial::timestamp_t max_rts_write = tuple_.get_rts();  // 100
    
    // Compute commit_ts: max(200+1, 100+1) = 201
    sundial::timestamp_t commit_ts = sundial::compute_commit_ts(max_wts_read, max_rts_write);
    EXPECT_EQ(commit_ts, 201ULL);
}

TEST_F(SundialLeaseCoherenceTest, CommitTsFromWriteSetDominates) {
    // Test case where write set's rts dominates the commit_ts calculation
    
    SundialTuple tuple2;
    tuple2.set_wts(50);
    tuple2.set_rts(300);  // High rts
    
    // Transaction reads tuple_ (wts=100) and writes to tuple2 (rts=300)
    sundial::timestamp_t max_wts_read = tuple_.get_wts();  // 100
    sundial::timestamp_t max_rts_write = tuple2.get_rts(); // 300
    
    // Compute commit_ts: max(100+1, 300+1) = 301
    sundial::timestamp_t commit_ts = sundial::compute_commit_ts(max_wts_read, max_rts_write);
    EXPECT_EQ(commit_ts, 301ULL);
}

// ============================================================================
// Integration Test: Simulated Transaction Flow
// ============================================================================

TEST_F(SundialLeaseCoherenceTest, SimulatedReadWriteTransaction) {
    // Simulate a read-write transaction using Sundial protocol
    
    // Transaction start
    sundial::timestamp_t start_ts = sundial::get_current_timestamp();
    sundial::thread_id_t my_thread_id = 42;
    (void)start_ts; // suppress unused warning
    
    // Read phase: observe wts and rts
    sundial::timestamp_t observed_wts = tuple_.get_wts();
    sundial::timestamp_t observed_rts = tuple_.get_rts();
    
    // Track max values for commit_ts calculation
    sundial::timestamp_t max_wts_read = observed_wts;
    sundial::timestamp_t max_rts_write = 0;
    
    // Write phase: acquire lock
    EXPECT_TRUE(tuple_.try_sundial_lock(my_thread_id));
    max_rts_write = std::max(max_rts_write, tuple_.get_rts());
    
    // Compute commit_ts
    sundial::timestamp_t commit_ts = sundial::compute_commit_ts(max_wts_read, max_rts_write);
    
    // Validation phase: check that reads are still valid
    bool valid = sundial::can_extend_lease(tuple_.get_wts(), observed_wts);
    EXPECT_TRUE(valid);
    
    if (valid) {
        // If commit_ts > observed_rts, extend the lease
        if (commit_ts > observed_rts) {
            tuple_.extend_rts(commit_ts);
        }
        
        // Commit: update wts and rts
        tuple_.set_wts(commit_ts);
        if (commit_ts > tuple_.get_rts()) {
            tuple_.set_rts(commit_ts);
        }
    }
    
    // Release lock
    tuple_.sundial_unlock(my_thread_id);
    
    // Verify final state
    EXPECT_EQ(tuple_.get_wts(), commit_ts);
    EXPECT_GE(tuple_.get_rts(), commit_ts);
    EXPECT_EQ(tuple_.get_lock_owner(), sundial::NO_LOCK_OWNER);
}

// ============================================================================
// Wait-Die Policy Tests
// ============================================================================

class WaitDiePolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
    }
};

TEST_F(WaitDiePolicyTest, OlderTransactionShouldWait) {
    // Older transaction (ts=100) trying to acquire lock held by younger (ts=200)
    EXPECT_TRUE(sundial::should_wait(100, 200));
}

TEST_F(WaitDiePolicyTest, YoungerTransactionShouldDie) {
    // Younger transaction (ts=200) trying to acquire lock held by older (ts=100)
    EXPECT_FALSE(sundial::should_wait(200, 100));
}

TEST_F(WaitDiePolicyTest, SameAgeTransactionShouldDie) {
    // Same timestamp - to avoid deadlock, should die
    EXPECT_FALSE(sundial::should_wait(100, 100));
}

// ============================================================================
// Phase 2a: Wait-Die with Timestamp Support Tests
// ============================================================================

class WaitDieTimestampTest : public ::testing::Test {
protected:
    SundialTuple tuple_;
    
    void SetUp() override {
    }
};

TEST_F(WaitDieTimestampTest, LockStoresTimestamp) {
    sundial::thread_id_t thread1 = 1;
    sundial::timestamp_t ts1 = 100;
    
    // Acquire lock with timestamp
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1, ts1));
    
    // Verify timestamp is stored
    EXPECT_EQ(tuple_.get_lock_owner(), thread1);
    EXPECT_EQ(tuple_.get_lock_holder_ts(), ts1);
}

TEST_F(WaitDieTimestampTest, UnlockClearsTimestamp) {
    sundial::thread_id_t thread1 = 1;
    sundial::timestamp_t ts1 = 100;
    
    // Acquire and release lock
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1, ts1));
    tuple_.sundial_unlock(thread1);
    
    // Verify timestamp is cleared
    EXPECT_EQ(tuple_.get_lock_owner(), sundial::NO_LOCK_OWNER);
    EXPECT_EQ(tuple_.get_lock_holder_ts(), 0ULL);
}

TEST_F(WaitDieTimestampTest, GetLockInfoReturnsCorrectValues) {
    sundial::thread_id_t thread1 = 1;
    sundial::timestamp_t ts1 = 100;
    
    // Initially not locked
    sundial::thread_id_t owner;
    sundial::timestamp_t holder_ts;
    EXPECT_FALSE(tuple_.get_lock_info(owner, holder_ts));
    
    // Acquire lock
    EXPECT_TRUE(tuple_.try_sundial_lock(thread1, ts1));
    
    // Now should return lock info
    EXPECT_TRUE(tuple_.get_lock_info(owner, holder_ts));
    EXPECT_EQ(owner, thread1);
    EXPECT_EQ(holder_ts, ts1);
}

TEST_F(WaitDieTimestampTest, OlderTransactionWaitsAndAcquires) {
    sundial::thread_id_t younger_thread = 1;
    sundial::thread_id_t older_thread = 2;
    sundial::timestamp_t younger_ts = 200;  // Younger = higher timestamp
    sundial::timestamp_t older_ts = 100;    // Older = lower timestamp
    
    // Younger transaction acquires lock first
    EXPECT_TRUE(tuple_.try_sundial_lock(younger_thread, younger_ts));
    
    // Older transaction should wait (according to Wait-Die policy)
    EXPECT_TRUE(sundial::should_wait(older_ts, younger_ts));
    
    // Simulate waiting: younger releases, older acquires
    tuple_.sundial_unlock(younger_thread);
    EXPECT_TRUE(tuple_.try_sundial_lock(older_thread, older_ts));
    
    EXPECT_EQ(tuple_.get_lock_owner(), older_thread);
    EXPECT_EQ(tuple_.get_lock_holder_ts(), older_ts);
}

TEST_F(WaitDieTimestampTest, YoungerTransactionDies) {
    sundial::thread_id_t older_thread = 1;
    sundial::thread_id_t younger_thread = 2;
    sundial::timestamp_t older_ts = 100;    // Older = lower timestamp
    sundial::timestamp_t younger_ts = 200;  // Younger = higher timestamp
    
    // Older transaction acquires lock first
    EXPECT_TRUE(tuple_.try_sundial_lock(older_thread, older_ts));
    
    // Younger transaction should die (not wait)
    EXPECT_FALSE(sundial::should_wait(younger_ts, older_ts));
    
    // Younger cannot acquire lock
    EXPECT_FALSE(tuple_.try_sundial_lock(younger_thread, younger_ts));
    
    // Lock still held by older
    EXPECT_EQ(tuple_.get_lock_owner(), older_thread);
}

TEST_F(WaitDieTimestampTest, ConcurrentWaitDieSimulation) {
    // Simulate multiple transactions with different timestamps
    constexpr int kNumTransactions = 4;
    std::vector<sundial::timestamp_t> timestamps = {300, 100, 200, 400};  // Unsorted
    std::atomic<int> wait_count{0};
    std::atomic<int> die_count{0};
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < kNumTransactions; ++i) {
        threads.emplace_back([&, i]() {
            sundial::thread_id_t my_id = i;
            sundial::timestamp_t my_ts = timestamps[i];
            
            // Try to acquire lock
            if (tuple_.try_sundial_lock(my_id, my_ts)) {
                success_count.fetch_add(1);
                // Hold lock briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                tuple_.sundial_unlock(my_id);
            } else {
                // Lock held by someone else - check Wait-Die
                sundial::thread_id_t holder;
                sundial::timestamp_t holder_ts;
                if (tuple_.get_lock_info(holder, holder_ts)) {
                    if (sundial::should_wait(my_ts, holder_ts)) {
                        wait_count.fetch_add(1);
                        // Simulate waiting with retry
                        for (int retry = 0; retry < 10; retry++) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            if (tuple_.try_sundial_lock(my_id, my_ts)) {
                                success_count.fetch_add(1);
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                tuple_.sundial_unlock(my_id);
                                break;
                            }
                        }
                    } else {
                        die_count.fetch_add(1);  // Younger dies
                    }
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // At least one transaction should succeed initially
    EXPECT_GE(success_count.load(), 1);
    
    // Lock should be released at the end
    EXPECT_EQ(tuple_.get_lock_owner(), sundial::NO_LOCK_OWNER);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
