# Sundial Implementation in Mako

## 1. Logical Leases: The Core Concept

### 1.1 What Are Logical Leases?

A **logical lease** is a pair of timestamps attached to each tuple that defines the range of logical time during which the tuple's value is valid:

```
Tuple = { wts, rts, data }
```

| Field | Name | Meaning |
|-------|------|---------|
| `wts` | Write Timestamp | The logical time when this tuple was last written |
| `rts` | Read Timestamp | The upper bound of the lease (how long the value is valid) |

A transaction with `commit_ts` can access a tuple if: **wts ≤ commit_ts ≤ rts**

### 1.2 How Leases Enable Dynamic Ordering

The key insight from the Sundial paper:

> "The DBMS dynamically computes a transaction's commit timestamp, which must overlap with the leases of all tuples accessed by the transaction."

Instead of assigning timestamps at transaction start (like traditional T/O protocols), Sundial **computes the commit timestamp at the end** based on what tuples were accessed. This allows transactions to find a valid commit point even if their accesses would conflict under static ordering.

### 1.3 How Leases Are Updated

From the paper:
> "The wts of a lease changes only when the tuple is updated during the commit phase; the rts of a lease changes only in the prepare or commit phases. Both wts and rts can only increase but never decrease."

**Two scenarios where rts changes:**

| Scenario | When | How rts Changes |
|----------|------|-----------------|
| **Lease Extension** | Read validation (prepare phase) | If commit_ts > current_rts, extend rts to commit_ts |
| **New Lease** | Write commit | Set rts = commit_ts + lease_duration |

**Our Implementation:**

We simplified lease management by using a **fixed lease duration**:

```cpp
// SundialConfig.hh
#define SUNDIAL_LEASE_DURATION 1000

// MassTrans.hh (on write commit)
e->set_wts(commit_ts);
sundial::timestamp_t new_rts = commit_ts + SUNDIAL_LEASE_DURATION;
if (new_rts > current_rts) {
    e->set_rts(new_rts);
}
```

- **SUNDIAL_LEASE_DURATION = 1000** - 1000 logical timestamp units (not wall-clock time)
- **rts only increases** - We only update rts if `new_rts > current_rts`
- **No dynamic read-time extension** - We rely on the initial lease being long enough

The paper's approach allows more dynamic extension (extending rts on-demand during validation), but our fixed-duration approach is simpler and works for our evaluation.

---

## 2. Motivation

We wanted to **evaluate how Sundial's logical leases affect Mako's performance**. The Sundial paper reports significant improvements (57% in YCSB, 34% in TPC-C) using logical leases alone (without caching). 

Our goal was to:
1. Implement logical leases in Mako's STO layer
2. Measure the impact on throughput, latency, and abort rates
3. Understand how Sundial's approach interacts with Mako's architecture (Paxos replication, speculative execution)

The paper also explores a **caching optimization** that helps for skewed read-heavy workloads. We focused on implementing logical leases first, and if they show reasonable improvements, we look to explore caching as a next step.

---

## 3. Implementation Details

We implemented Sundial's logical leases in Mako's STO (Software Transactional Objects) layer.

### 3.1 Logical Leases (wts/rts Tracking)

#### Tuple-Level Changes (versioned_value.hh)

Each tuple stores lease information:

```cpp
std::atomic<sundial::timestamp_t> wts_;  // Write timestamp
std::atomic<sundial::timestamp_t> rts_;  // Read timestamp (lease end)
```

**Verification against paper:** The paper specifies `DB[key] = {wts, rts, owner, waitlist, data}`. We store wts and rts on each tuple. ✓

#### Transaction-Level Tracking (Transaction.hh)

Each transaction tracks:

```cpp
sundial::timestamp_t sundial_start_ts_;      // When transaction started
sundial::timestamp_t sundial_max_read_wts_;  // Max wts of all reads
sundial::timestamp_t sundial_min_read_rts_;  // Min rts of all reads (for fast path)
sundial::timestamp_t sundial_max_write_rts_; // Max rts of all writes
sundial::timestamp_t sundial_commit_ts_;     // Computed commit timestamp
```

#### Read Instrumentation (MassTrans.hh)

On each read:
```cpp
sundial::timestamp_t observed_wts = e->get_wts();
sundial::timestamp_t observed_rts = e->get_rts();
TThread::txn->sundial_update_max_read_wts(observed_wts);
TThread::txn->sundial_update_min_read_rts(observed_rts);
```

**Verification against paper:** The paper says `T.commit_ts = Max(T.commit_ts, T.RS[key].wts)` on reads. We track max_read_wts for this. ✓

#### Commit Timestamp Computation (SundialConfig.hh)

```cpp
inline timestamp_t compute_commit_ts(timestamp_t max_wts_read, timestamp_t max_rts_write) {
    return std::max(max_wts_read + 1, max_rts_write + 1);
}
```

**Verification against paper:** commit_ts must be ≥ all read wts and > all write rts (since we're creating new leases). ✓

#### Lease Update on Commit (MassTrans.hh)

```cpp
e->set_wts(commit_ts);
e->set_rts(commit_ts + SUNDIAL_LEASE_DURATION);
```

**Verification against paper:** On commit, wts is set to commit_ts, rts is extended. ✓

---

### 3.2 Wait-Die (Deadlock Prevention)

The paper uses Wait-Die for **write-write conflicts**:

> "Sundial handles write-write conflicts using the 2PL Wait-Die algorithm. A transaction waits for a lock only if its priority is higher than the current lock owner; otherwise the DBMS will abort it."

#### Our Implementation (MassTrans.hh)

```cpp
if (!sundial::should_wait(my_ts, holder_ts)) {
    // Younger transaction (higher ts) - abort immediately (DIE)
    return false;
}
// Older transaction (lower ts) - spin wait (WAIT)
```

#### Wait-Die Decision Logic (SundialConfig.hh)

```cpp
inline bool should_wait(timestamp_t requester_ts, timestamp_t holder_ts) {
    // Older (lower timestamp) waits, younger (higher timestamp) dies
    return requester_ts < holder_ts;
}
```

**Verification against paper:** Older transactions (lower ts) wait, younger transactions (higher ts) abort. ✓

#### Additional Tuple Fields for Wait-Die

```cpp
std::atomic<sundial::thread_id_t> lock_owner_;     // Who holds the lock
std::atomic<sundial::timestamp_t> lock_holder_ts_; // Lock holder's start time
```

**Verification against paper:** The paper specifies `DB[key] = {wts, rts, owner, waitlist, data}`. We track owner and holder timestamp. ✓

---

### 3.3 Read-Only Fast Path

For read-only transactions, if all leases are still valid, we can skip OCC validation entirely.

#### Condition (Transaction.cc)

```cpp
bool sundial_can_use_read_only_fast_path() const {
    return sundial_read_only_                         // No writes
        && (sundial_commit_ts_ <= sundial_min_read_rts_);  // All leases valid
}
```

#### Usage in Commit

```cpp
if (sundial_can_use_read_only_fast_path()) {
    // Skip phase 2 validation - commit directly
    stop(true, nullptr, 0);
    return true;
}
```

**Verification against paper:** The paper states that a transaction can commit if its commit_ts falls within the leases of all accessed tuples. For read-only transactions with commit_ts ≤ min_rts, all leases are valid. ✓

---

### 3.4 Configuration Flags (SundialConfig.hh)

| Flag | Description |
|------|-------------|
| `SUNDIAL_ENABLED` | Enable/disable Sundial protocol entirely |
| `SUNDIAL_WAIT_DIE` | Enable Wait-Die deadlock prevention for write-write conflicts |
| `SUNDIAL_READ_ONLY_OPT` | Enable read-only fast path optimization |
| `SUNDIAL_LEASE_DURATION` | Lease duration in logical timestamp units |
| `SUNDIAL_DEBUG` | Enable verbose debug logging |
| `SUNDIAL_STATS` | Enable statistics collection (printed at exit) |

**Viewing Sundial Metrics**: When `SUNDIAL_STATS=1`, detailed statistics are automatically printed to stderr at program exit. These include:
- `Reads/Writes tracked` - Number of operations instrumented with lease tracking
- `WTS validations/conflicts` - How often wts changed between read and commit (indicates write conflicts)
- `Lock conflicts` - Write-write conflicts where younger transaction aborted (Wait-Die "die" events)
- `Wait-Die waits/successes/timeouts` - Older transactions that waited for locks
- `RO fast path/slow path` - Read-only transactions that skipped vs. required OCC validation

To run different configurations, modify these flags in `SundialConfig.hh` and rebuild.

---

## 4. How Our Changes Work with Replication

Our Sundial changes are in the **STO layer** (local transaction processing). This layer sits **above** Paxos replication:

```
┌─────────────────────────────────────┐
│  Transaction Layer (STO)            │  ← Our Sundial changes
│  - wts/rts on each tuple            │
│  - Wait-Die for write locks         │
│  - Read-only fast path              │
├─────────────────────────────────────┤
│  Paxos Replication Layer            │  ← Unchanged
│  - Replicates committed data        │
│  - Includes tuple fields (wts/rts)  │
└─────────────────────────────────────┘
```

**Key points:**
1. **wts/rts are part of the tuple** - When Paxos replicates data, it includes these fields automatically
2. **No changes to Paxos** - Our changes only affect local transaction processing
3. **Each shard runs independently** - In multi-shard, each shard has its own Sundial optimizations

---

## 5. Evaluation

We evaluated our Sundial implementation with several benchmarks. See [SUNDIAL_EVALUATION.md](SUNDIAL_EVALUATION.md) for detailed results.

### Key Observations

1. **Logical leases work correctly** - wts/rts tracking and validation function as expected
2. **Wait-Die trades throughput for fewer aborts** (Section 2: -30.4% aborts, but -14.5% throughput)
3. **Read-only fast path provides measurable benefit** (+9.6% throughput in read-heavy workloads)
4. **Multi-shard has low contention** - Wait-Die/WTS validation rarely trigger (verified via Sundial stats)

---

## 6. Caching Optimization

### 6.1 What the Paper Describes

Caching stores remote data locally to reduce network latency. A transaction can use cached data as long as its `commit_ts` falls within the cached tuple's lease (`commit_ts ≤ cached_rts`). This avoids cross-shard RPCs for frequently accessed data.

### 6.2 Integration Points Identified

To integrate caching in Mako, we identified four changes needed:

| Integration Point | File | Change Required |
|-------------------|------|-----------------|
| **Remote Read Path** | `shardClient.cc` | Check cache before RPC; cache responses |
| **RPC Handler** | `shardServer.cc` | Return `(data, wts, rts)` instead of just `data` |
| **Commit Validation** | `Transaction.cc` | Validate cached reads against current lease |
| **Cache Invalidation** | New infrastructure | Notify other shards when data is updated |

### 6.3 Challenges

**Challenge 1: No Cache Infrastructure**

Mako has no caching layer. Every cross-shard read goes directly to RPC. We would need to build cache management, thread-safe lookups, and eviction policies from scratch.

**Challenge 2: Speculative Execution Conflict**

Sundial assumes the transaction knows its `commit_ts` **before** reading (to check if lease is valid). Mako computes `commit_ts` **after** all reads (speculative execution). This creates a circular dependency:
- To check lease validity: need `commit_ts`
- To compute `commit_ts`: need all reads complete
- Cannot decide if cache is valid until after using it

**Challenge 3: No Cross-Shard Invalidation**

When Shard B updates data, Shard A's cache becomes stale. Mako's Paxos replication is intra-shard only—there's no mechanism to notify other shards. Building invalidation would require new cross-shard communication infrastructure.

### 6.4 Our Prototype

We created a cache prototype in [SundialCache.hh](src/mako/benchmarks/sto/SundialCache.hh) with the data structures needed. It is **not integrated** yet due to the challenges above.

### 6.5 Conclusion

Caching integration is deferred to future work. The architectural changes required (speculation-aware validation, cross-shard invalidation) are significant and warrant dedicated effort. Our current focus was proving logical leases work first—a necessary foundation before investing in distributed caching.


---

## 7. References

**Papers:**
- Sundial: "Sundial: Harmonizing Concurrency Control and Caching in a Distributed OLTP Database Management System" (VLDB 2018)
- Mako: "Mako: Speculative Distributed Transactions with Geo-Replication" (OSDI 2025)
