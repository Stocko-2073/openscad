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
class GeometryCache
{
public:
  GeometryCache(size_t memorylimit = 100ul * 1024ul * 1024ul) : cache(memorylimit) {}

  static GeometryCache *instance()
  {
    if (!inst) inst = new GeometryCache;
    return inst;
  }

  bool contains(const std::string& id) const;
  std::shared_ptr<const class Geometry> get(const std::string& id) const;
  bool insert(const std::string& id, const std::shared_ptr<const Geometry>& geom);
  size_t size() const;
  size_t totalCost() const;
  size_t maxSizeMB() const;
  void setMaxSizeMB(size_t limit);
  void clear();
  void print();

private:
  static GeometryCache *inst;

  struct cache_entry {
    std::shared_ptr<const class Geometry> geom;
    std::string msg;
    cache_entry(const std::shared_ptr<const Geometry>& geom);
  };

  mutable std::mutex mutex_;
  Cache<std::string, cache_entry> cache;
};
