#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include "Cache.h"
#include "geometry/Geometry.h"
#include "utils/Hash128.h"

// Keyed by geometry digests (core/NodeDigest.h). Locked: animation pre-fetch workers share it.
class GeometryCache
{
public:
  GeometryCache(size_t memorylimit = 100ul * 1024ul * 1024ul) : cache(memorylimit) {}

  static GeometryCache *instance()
  {
    if (!inst) inst = new GeometryCache;
    return inst;
  }

  bool contains(const Hash128& id) const;
  std::shared_ptr<const class Geometry> get(const Hash128& id) const;
  // Whether id is cached and, if so, its geometry (which may be null), in one step: another
  // thread's insertion can evict the entry between a contains() and a get().
  bool find(const Hash128& id, std::shared_ptr<const Geometry>& geom) const;
  bool insert(const Hash128& id, const std::shared_ptr<const Geometry>& geom);
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
  Cache<Hash128, cache_entry, Hash128Hash> cache;
};
