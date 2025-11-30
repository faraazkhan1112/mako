#pragma once
#include <iostream>
#include <atomic>
#include "Interface.hh"
#include "SundialConfig.hh"

// TODO(nate): ugh. really we should have a MassTrans subclass of this with the
// deallocate_rcu functions so we 1) don't have to include Masstree headers in
// nearly everything STO-related and 2) don't accidentally call a Masstree
// function (deallocate_rcu) in some other context.
#include "kvthread.hh"

template <typename T, typename=void>
struct versioned_value_struct /*: public threadinfo::rcu_callback*/ {
  typedef T value_type;
  typedef TransactionTid::type version_type;

  versioned_value_struct() : version_(), value_(), wts_(0), rts_(0), lock_owner_(sundial::NO_LOCK_OWNER) {}
  // XXX Yihe: I made it public; is there any reason why it should be private?
  versioned_value_struct(const value_type& val, version_type v) 
    : version_(v), value_(val), wts_(0), rts_(0), lock_owner_(sundial::NO_LOCK_OWNER) {}
  
  static versioned_value_struct* make(const value_type& val, version_type version) {
    return new versioned_value_struct<T>(val, version);
  }
  
  bool needsResize(const value_type&) {
    return false;
  }
  
  versioned_value_struct* resizeIfNeeded(const value_type&) {
    return NULL;
  }
  
  inline void set_value(const value_type& v) {
    value_ = v;
  }
  
  inline const value_type& read_value() const {
    return value_;
  }

  inline value_type& writeable_value() {
    return value_;
  }
  
  inline const version_type& version() const {
    return version_;
  }
  inline version_type& version() {
    return version_;
  }

  // ============================================================================
  // Sundial Lease Methods
  // ============================================================================
  
  /**
   * Get the write timestamp (wts) - timestamp of last committed write
   */
  inline sundial::timestamp_t get_wts() const {
    return wts_.load(std::memory_order_acquire);
  }
  
  /**
   * Set the write timestamp (wts) - called when a write commits
   */
  inline void set_wts(sundial::timestamp_t ts) {
    wts_.store(ts, std::memory_order_release);
  }
  
  /**
   * Get the read timestamp (rts) - upper bound of the lease
   */
  inline sundial::timestamp_t get_rts() const {
    return rts_.load(std::memory_order_acquire);
  }
  
  /**
   * Set the read timestamp (rts) directly
   */
  inline void set_rts(sundial::timestamp_t ts) {
    rts_.store(ts, std::memory_order_release);
  }
  
  /**
   * Atomically extend the read timestamp (rts) if the new value is greater
   * This is used during reads to extend the lease
   * Returns the actual rts after the operation
   */
  inline sundial::timestamp_t extend_rts(sundial::timestamp_t new_rts) {
    sundial::timestamp_t current_rts = rts_.load(std::memory_order_acquire);
    while (new_rts > current_rts) {
      if (rts_.compare_exchange_weak(current_rts, new_rts, 
          std::memory_order_acq_rel, std::memory_order_acquire)) {
        return new_rts;
      }
      // current_rts is updated by compare_exchange_weak on failure
    }
    return current_rts;
  }
  
  /**
   * Get the lock owner thread ID
   * Returns sundial::NO_LOCK_OWNER if not locked
   */
  inline sundial::thread_id_t get_lock_owner() const {
    return lock_owner_.load(std::memory_order_acquire);
  }
  
  /**
   * Try to acquire the Sundial write lock
   * Uses Wait-Die: returns true if lock acquired, false if should retry or abort
   * 
   * @param thread_id The requesting thread's ID
   * @return true if lock acquired, false otherwise
   */
  inline bool try_sundial_lock(sundial::thread_id_t thread_id) {
    sundial::thread_id_t expected = sundial::NO_LOCK_OWNER;
    return lock_owner_.compare_exchange_strong(expected, thread_id,
        std::memory_order_acq_rel, std::memory_order_acquire);
  }
  
  /**
   * Release the Sundial write lock
   * Only the lock holder should call this
   */
  inline void sundial_unlock(sundial::thread_id_t thread_id) {
    sundial::thread_id_t expected = thread_id;
    lock_owner_.compare_exchange_strong(expected, sundial::NO_LOCK_OWNER,
        std::memory_order_acq_rel, std::memory_order_acquire);
  }
  
  /**
   * Check if this tuple is locked by someone else
   */
  inline bool is_locked_by_other(sundial::thread_id_t my_thread_id) const {
    sundial::thread_id_t owner = lock_owner_.load(std::memory_order_acquire);
    return owner != sundial::NO_LOCK_OWNER && owner != my_thread_id;
  }
  
  /**
   * Check if this tuple is locked by the specified thread
   */
  inline bool is_locked_by(sundial::thread_id_t thread_id) const {
    return lock_owner_.load(std::memory_order_acquire) == thread_id;
  }

  inline void deallocate_rcu(threadinfo& ti) {
    ti.deallocate_rcu(this, sizeof(versioned_value_struct), memtag_value);
  }

#if 0
  // rcu_callback method to self-destruct ourself
  void operator()(threadinfo& ti) {
    // this will call value's destructor
    this->versioned_value_struct::~versioned_value_struct();
    // and free our memory too
    ti.deallocate(this, sizeof(versioned_value_struct), memtag_value);
  }
#endif
  
private: 
  version_type version_;
  value_type value_;
  
  // Sundial lease fields
  std::atomic<sundial::timestamp_t> wts_;     // Write timestamp - last committed write
  std::atomic<sundial::timestamp_t> rts_;     // Read timestamp - lease upper bound
  std::atomic<sundial::thread_id_t> lock_owner_;  // Lock owner for Wait-Die 2PL
};

// double box for non trivially copyable types!
template<typename T>
struct versioned_value_struct<T, typename std::enable_if<!__has_trivial_copy(T)>::type> {
public:
  typedef T value_type;
  typedef TransactionTid::type version_type;

  static versioned_value_struct* make(const value_type& val, version_type version) {
    return new versioned_value_struct(val, version);
  }

  versioned_value_struct() : version_(), valueptr_(), wts_(0), rts_(0), lock_owner_(sundial::NO_LOCK_OWNER) {}

  bool needsResize(const value_type&) {
    return false;
  }
  versioned_value_struct* resizeIfNeeded(const value_type&) {
    return this;
  }

  void set_value(const value_type& v) {
    //auto *old = valueptr_;
    valueptr_ = new value_type(std::move(v));
    // rcu free old (HOW without threadinfo access??)
  }

  const value_type& read_value() const {
    return *valueptr_;
  }

  inline const version_type& version() const {
    return version_;
  }
  version_type& version() {
    return version_;
  }

  // ============================================================================
  // Sundial Lease Methods (same as trivially copyable version)
  // ============================================================================
  
  inline sundial::timestamp_t get_wts() const {
    return wts_.load(std::memory_order_acquire);
  }
  
  inline void set_wts(sundial::timestamp_t ts) {
    wts_.store(ts, std::memory_order_release);
  }
  
  inline sundial::timestamp_t get_rts() const {
    return rts_.load(std::memory_order_acquire);
  }
  
  inline void set_rts(sundial::timestamp_t ts) {
    rts_.store(ts, std::memory_order_release);
  }
  
  inline sundial::timestamp_t extend_rts(sundial::timestamp_t new_rts) {
    sundial::timestamp_t current_rts = rts_.load(std::memory_order_acquire);
    while (new_rts > current_rts) {
      if (rts_.compare_exchange_weak(current_rts, new_rts, 
          std::memory_order_acq_rel, std::memory_order_acquire)) {
        return new_rts;
      }
    }
    return current_rts;
  }
  
  inline sundial::thread_id_t get_lock_owner() const {
    return lock_owner_.load(std::memory_order_acquire);
  }
  
  inline bool try_sundial_lock(sundial::thread_id_t thread_id) {
    sundial::thread_id_t expected = sundial::NO_LOCK_OWNER;
    return lock_owner_.compare_exchange_strong(expected, thread_id,
        std::memory_order_acq_rel, std::memory_order_acquire);
  }
  
  inline void sundial_unlock(sundial::thread_id_t thread_id) {
    sundial::thread_id_t expected = thread_id;
    lock_owner_.compare_exchange_strong(expected, sundial::NO_LOCK_OWNER,
        std::memory_order_acq_rel, std::memory_order_acquire);
  }
  
  inline bool is_locked_by_other(sundial::thread_id_t my_thread_id) const {
    sundial::thread_id_t owner = lock_owner_.load(std::memory_order_acquire);
    return owner != sundial::NO_LOCK_OWNER && owner != my_thread_id;
  }
  
  inline bool is_locked_by(sundial::thread_id_t thread_id) const {
    return lock_owner_.load(std::memory_order_acquire) == thread_id;
  }

  inline void deallocate_rcu(threadinfo& ti) {
    // XXX: really this one needs to be a rcu_callback so we can call destructor
    ti.deallocate_rcu(this, sizeof(versioned_value_struct), memtag_value);
  }
  
private:
  versioned_value_struct(const value_type& val, version_type version) 
    : version_(version), valueptr_(new value_type(std::move(val))), 
      wts_(0), rts_(0), lock_owner_(sundial::NO_LOCK_OWNER) {}

  version_type version_;
  value_type* valueptr_;
  
  // Sundial lease fields
  std::atomic<sundial::timestamp_t> wts_;
  std::atomic<sundial::timestamp_t> rts_;
  std::atomic<sundial::thread_id_t> lock_owner_;
};
