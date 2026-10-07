#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include "Cache.h"
#include "geometry/Geometry.h"
#include "utils/Hash128.h"

// Keyed by geometry digests (core/NodeDigest.h). Locked: animation pre-fetch workers share it.
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

  bool contains(const Hash128& id) const;
  std::shared_ptr<const Geometry> get(const Hash128& id) const;
  // Whether id is cached and, if so, its geometry, in one step: another thread's insertion can
  // evict the entry between a contains() and a get().
  bool find(const Hash128& id, std::shared_ptr<const Geometry>& geom) const;
  bool insert(const Hash128& id, const std::shared_ptr<const Geometry>& N);
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
  Cache<Hash128, cache_entry, Hash128Hash> cache;
};
