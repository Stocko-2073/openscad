#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include "Cache.h"
#include "geometry/Geometry.h"

// Thread-safe singleton wrapper around an LRU Cache. The mutex guards concurrent
// access from animation pre-fetch workers; single-threaded callers pay one
// uncontended lock per op.
class CGALCache
{
public:
  CGALCache(size_t limit = 100ul * 1024ul * 1024ul);

  static CGALCache *instance()
  {
    if (!inst) inst = new CGALCache;
    return inst;
  }
  static bool acceptsGeometry(const std::shared_ptr<const Geometry>& geom);

  bool contains(const std::string& id) const;
  std::shared_ptr<const Geometry> get(const std::string& id) const;
  bool insert(const std::string& id, const std::shared_ptr<const Geometry>& N);
  size_t size() const;
  size_t totalCost() const;
  size_t maxSizeMB() const;
  void setMaxSizeMB(size_t limit);
  void clear();
  void print();

private:
  static CGALCache *inst;

  struct cache_entry {
    std::shared_ptr<const Geometry> N;
    std::string msg;
    cache_entry(const std::shared_ptr<const Geometry>& N);
  };

  mutable std::mutex mutex_;
  Cache<std::string, cache_entry> cache;
};
