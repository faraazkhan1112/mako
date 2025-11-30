#pragma once

#include "masstree.hh"
#include "kvthread.hh"
#include "masstree_tcursor.hh"
#include "masstree_insert.hh"
#include "masstree_print.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "string.hh"

#include "StringWrapper.hh"
#include "versioned_value.hh"
#include "stuffed_str.hh"
#include "SundialConfig.hh"

typedef stuffed_str<uint64_t> versioned_str;

struct versioned_str_struct : public versioned_str {
  typedef Masstree::Str value_type;
  typedef versioned_str::stuff_type version_type;

  bool needsResize(const value_type& v) {
    return needs_resize(v.length());
  }
  bool needsResize(const std::string& v) {
    return needs_resize(v.length());
  }

  versioned_str_struct* resizeIfNeeded(const value_type& potential_new_value) {
    // TODO: this cast is only safe because we have no ivars or virtual methods
    return (versioned_str_struct*)this->reserve(versioned_str::size_for(potential_new_value.length()));
  }
  versioned_str_struct* resizeIfNeeded(const std::string& potential_new_value) {
    // TODO: this cast is only safe because we have no ivars or virtual methods
    return (versioned_str_struct*)this->reserve(versioned_str::size_for(potential_new_value.length()));
  }

  template <typename StringType>
  inline void set_value(const StringType& v) {
    auto *ret = this->replace(v.data(), v.length());
    // we should already be the proper size at this point
    (void)ret;
    assert(ret == this);
  }
  
  // responsibility is on the caller of this method to make sure this read is atomic
  value_type read_value() {
    return Masstree::Str(this->data(), this->length());
  }
  
  inline version_type& version() {
    return stuff();
  }

  inline void deallocate_rcu(threadinfo& ti) {
    ti.deallocate_rcu(this, this->capacity() + sizeof(versioned_str_struct), memtag_value);
  }
  
  // ============================================================================
  // Sundial Lease Methods (inherited from versioned_str/stuffed_str)
  // These are already available via inheritance, but we add explicit wrappers
  // for clarity and to match versioned_value_struct interface
  // ============================================================================
  
  // Note: get_wts(), set_wts(), get_rts(), set_rts(), extend_rts(),
  // get_lock_owner(), try_sundial_lock(), sundial_unlock(),
  // is_locked_by_other(), is_locked_by() are all inherited from stuffed_str
};