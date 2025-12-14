#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
#include "SundialConfig.hh"

namespace sundial {

// Cache entry for a remote tuple with Sundial lease metadata
struct CacheEntry {
    std::string data;           // Cached serialized value
    timestamp_t wts;            // Write timestamp when data was written
    timestamp_t rts;            // Read timestamp / lease end
    uint32_t source_shard;      // Shard that owns this data (authoritative copy)
    uint64_t table_id;          // Table this entry belongs to
    
    // Check if lease is still valid (current_ts <= rts)
    bool is_lease_valid(timestamp_t current_ts) const {
        return current_ts <= rts;
    }
    
    // Check if cached data matches expected version (for commit validation)
    bool version_matches(timestamp_t expected_wts) const {
        return wts == expected_wts;
    }
};

// Remote data cache with Sundial lease tracking (prototype, not integrated)
class SundialCache {
public:
    SundialCache() = default;
    ~SundialCache() = default;
    
    // Non-copyable
    SundialCache(const SundialCache&) = delete;
    SundialCache& operator=(const SundialCache&) = delete;
    
    // Lookup cached data; returns nullptr if not found or lease expired
    const CacheEntry* lookup(uint32_t shard_id, uint64_t table_id, 
                              const std::string& key, timestamp_t current_ts) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string cache_key = make_cache_key(shard_id, table_id, key);
        auto it = cache_.find(cache_key);
        
        if (it == cache_.end()) {
            stats_.misses++;
            return nullptr;
        }
        
        if (!it->second.is_lease_valid(current_ts)) {
            // Lease expired - treat as cache miss
            // Note: We don't evict here; entry might be refreshed soon
            stats_.lease_expired++;
            return nullptr;
        }
        
        stats_.hits++;
        return &it->second;
    }
    
    // Insert or update cached data with lease metadata
    void insert(uint32_t shard_id, uint64_t table_id, const std::string& key,
                const std::string& data, timestamp_t wts, timestamp_t rts) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string cache_key = make_cache_key(shard_id, table_id, key);
        cache_[cache_key] = CacheEntry{data, wts, rts, shard_id, table_id};
        stats_.inserts++;
        
        // Note: No eviction policy - cache grows unbounded in this prototype
    }
    
    // Invalidate a cached entry
    void invalidate(uint32_t shard_id, uint64_t table_id, const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string cache_key = make_cache_key(shard_id, table_id, key);
        if (cache_.erase(cache_key) > 0) {
            stats_.invalidations++;
        }
    }
    
    // Invalidate all entries from a specific shard (for failure recovery)
    void invalidate_shard(uint32_t shard_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Inefficient but simple - iterate and remove matching entries
        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (it->second.source_shard == shard_id) {
                it = cache_.erase(it);
                stats_.invalidations++;
            } else {
                ++it;
            }
        }
    }
    
    // Extend lease on cached entry (local only)
    bool extend_lease(uint32_t shard_id, uint64_t table_id, 
                      const std::string& key, timestamp_t new_rts) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string cache_key = make_cache_key(shard_id, table_id, key);
        auto it = cache_.find(cache_key);
        
        if (it == cache_.end()) {
            return false;
        }
        
        // Only extend, never shrink
        if (new_rts > it->second.rts) {
            it->second.rts = new_rts;
            stats_.lease_extensions++;
        }
        return true;
    }
    
    // Cache statistics
    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t lease_expired = 0;
        uint64_t inserts = 0;
        uint64_t invalidations = 0;
        uint64_t lease_extensions = 0;
        
        double hit_rate() const {
            uint64_t total = hits + misses + lease_expired;
            return total > 0 ? (double)hits / total : 0.0;
        }
    };
    
    Stats get_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        stats_ = Stats{};
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

private:
    static std::string make_cache_key(uint32_t shard_id, uint64_t table_id, 
                                       const std::string& key) {
        return std::to_string(shard_id) + ":" + 
               std::to_string(table_id) + ":" + key;
    }
    
    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::mutex mutex_;
    Stats stats_;
};

} // namespace sundial

