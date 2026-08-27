#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "core/Identifier.h"

/*
 * Open-addressed map from interned name to T, for the tables the interpreter
 * probes on every call.
 *
 * Identifiers carry a dense index assigned when the name was interned, which
 * doubles as a perfect hash while the table has room, so a lookup is one
 * cache line and a pointer compare. That matters most for the misses: a
 * builtin call walks past the file scope on its way to the builtin table, and
 * instantiating a BOSL2-heavy model does that 14M times.
 *
 * A default-constructed Identifier -- the empty name, which nothing can be
 * called -- marks an unused slot.
 */
template <typename T>
class IdentifierMap
{
public:
  IdentifierMap() = default;

  const T *find(const Identifier& name) const
  {
    if (table.empty()) return nullptr;
    const size_t mask = table.size() - 1;
    for (size_t i = name.index() & mask;; i = (i + 1) & mask) {
      const auto& slot = table[i];
      if (slot.first.empty()) return nullptr;
      if (slot.first == name) return &slot.second;
    }
  }

  T *find(const Identifier& name)
  {
    return const_cast<T *>(const_cast<const IdentifierMap *>(this)->find(name));
  }

  void insert_or_assign(const Identifier& name, T value)
  {
    if ((count + 1) * 2 > table.size()) {
      grow();
    }

    const size_t mask = table.size() - 1;
    for (size_t i = name.index() & mask;; i = (i + 1) & mask) {
      auto& slot = table[i];
      if (slot.first == name) {
        slot.second = std::move(value);
        return;
      }
      if (slot.first.empty()) {
        slot.first = name;
        slot.second = std::move(value);
        ++count;
        return;
      }
    }
  }

  // Keeps the existing entry if the name is already present, matching
  // std::unordered_map::emplace.
  void emplace(const Identifier& name, T value)
  {
    if (find(name)) return;
    insert_or_assign(name, std::move(value));
  }

  size_t size() const { return count; }

private:
  static constexpr size_t initial_capacity = 8;

  void grow()
  {
    std::vector<std::pair<Identifier, T>> old(table.empty() ? initial_capacity : table.size() * 2);
    table.swap(old);
    const size_t mask = table.size() - 1;
    for (auto& entry : old) {
      if (entry.first.empty()) continue;
      for (size_t i = entry.first.index() & mask;; i = (i + 1) & mask) {
        if (table[i].first.empty()) {
          table[i] = std::move(entry);
          break;
        }
      }
    }
  }

  std::vector<std::pair<Identifier, T>> table;
  size_t count{0};
};
