#pragma once
#include <boost/container/small_vector.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "core/Identifier.h"
#include "core/IdentifierMap.h"
#include "core/Value.h"

/*
 * Variable storage for a single ContextFrame.
 *
 * Frames are overwhelmingly tiny. Instantiating a BOSL2-heavy model builds
 * ~39M of them, of which 54% hold nothing at all and 92% hold at most one
 * variable, while the interpreter probes them ~150M times looking names up.
 * A flat array scanned linearly therefore beats a hash map twice over: no
 * name hash per probe, and no heap allocation at all for the sizes that
 * dominate.
 *
 * Names are interned, so the scan compares pointers rather than characters.
 *
 * A few frames are not tiny at all, and they sit at the bottom of every
 * lookup chain: `include` merges the whole of BOSL2 into one file scope, so
 * that frame holds hundreds of variables and is scanned on the way to any
 * name it does not hold. Frames past index_threshold entries therefore carry
 * a side index. Measured over u-bot.scad, 92% of all scan steps were spent in
 * the frames that qualify.
 *
 * Unlike std::unordered_map, an insert invalidates iterators and references
 * to entries already held. Callers must not keep a Value reference obtained
 * from a frame across a set_variable() on that same frame.
 */
class ValueMap
{
  using entry_t = std::pair<Identifier, Value>;
  // Covers the 95% of frames that hold two variables or fewer without
  // touching the heap.
  using map_t = boost::container::small_vector<entry_t, 2>;
  map_t map;

  // Positions in `map`, keyed by name. Null until the frame outgrows the
  // linear scan, which almost none do.
  std::unique_ptr<IdentifierMap<uint32_t>> index;
  static constexpr size_t index_threshold = 16;

public:
  using iterator = map_t::iterator;
  using const_iterator = map_t::const_iterator;

  const_iterator find(const Identifier& name) const
  {
    if (index) {
      const uint32_t *position = index->find(name);
      return position ? map.begin() + *position : map.end();
    }
    for (auto it = map.begin(); it != map.end(); ++it) {
      if (it->first == name) return it;
    }
    return map.end();
  }
  bool contains(const Identifier& name) const { return find(name) != map.end(); }

  const_iterator begin() const { return map.begin(); }
  const_iterator end() const { return map.end(); }
  iterator begin() { return map.begin(); }
  iterator end() { return map.end(); }
  void clear()
  {
    map.clear();
    index.reset();
  }
  size_t size() const { return map.size(); }

  // Returns false in the second position if the name was already present,
  // matching std::unordered_map::insert_or_assign.
  std::pair<iterator, bool> insert_or_assign(const Identifier& name, Value&& value)
  {
    if (index) {
      if (const uint32_t *position = index->find(name)) {
        iterator it = map.begin() + *position;
        it->second = std::move(value);
        return {it, false};
      }
      map.emplace_back(name, std::move(value));
      index->insert_or_assign(name, uint32_t(map.size() - 1));
      return {map.end() - 1, true};
    }
    for (auto it = map.begin(); it != map.end(); ++it) {
      if (it->first == name) {
        it->second = std::move(value);
        return {it, false};
      }
    }
    map.emplace_back(name, std::move(value));
    if (map.size() > index_threshold) {
      buildIndex();
    }
    return {map.end() - 1, true};
  }

  // Get value by name, without possibility of default-constructing a missing name
  //   return Value::undefined if key missing
  const Value& get(const Identifier& name) const
  {
    auto result = find(name);
    return result == map.end() ? Value::undefined : result->second;
  }

private:
  void buildIndex()
  {
    index = std::make_unique<IdentifierMap<uint32_t>>();
    for (size_t i = 0; i < map.size(); ++i) {
      index->insert_or_assign(map[i].first, uint32_t(i));
    }
  }
};
