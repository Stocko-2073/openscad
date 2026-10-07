#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>

// An interned name: equality is a pointer compare, but constructing one from text takes a
// global lock and a hash, so build them while parsing, never inside an evaluation loop.
class Identifier
{
public:
  Identifier() : entry(emptyEntry()) {}
  Identifier(const char *name) : entry(intern(name)) {}
  Identifier(const std::string& name) : entry(intern(name)) {}

  const std::string& str() const { return entry->name; }
  const char *c_str() const { return entry->name.c_str(); }
  bool empty() const { return entry->name.empty(); }
  operator const std::string&() const { return entry->name; }

  bool operator==(const Identifier& other) const { return entry == other.entry; }
  bool operator!=(const Identifier& other) const { return entry != other.entry; }
  bool operator==(const std::string& other) const { return entry->name == other; }
  bool operator!=(const std::string& other) const { return entry->name != other; }
  bool operator==(const char *other) const { return entry->name == other; }
  bool operator!=(const char *other) const { return entry->name != other; }

  // Dynamically scoped: '$' names other than "$children".
  bool isConfigVariable() const { return entry->is_config; }

  // One set bit, cycling every 64 interned names: a key for the frames' Bloom filters.
  uint64_t bit() const { return entry->bit; }

  // Numbered from zero in interning order, so dense and collision-free.
  size_t index() const { return entry->index; }

  // Set for good once any variable of this name, in any evaluation, holds a function value;
  // calls to the name then walk the context chain instead of trusting the call-site cache.
  bool hasFunctionValue() const
  {
    return entry->function_value.load(std::memory_order_relaxed);
  }
  void markFunctionValue() const
  {
    entry->function_value.store(true, std::memory_order_relaxed);
  }

  size_t hash() const { return std::hash<const void *>{}(entry); }

private:
  struct Entry {
    std::string name;
    bool is_config = false;
    uint64_t bit = 0;
    size_t index = 0;
    // Atomic: animation prefetch evaluates scripts on several threads at once.
    mutable std::atomic<bool> function_value{false};
  };
  static const Entry *intern(const std::string& name);
  static const Entry *emptyEntry();

  const Entry *entry;
};

inline std::ostream& operator<<(std::ostream& stream, const Identifier& identifier)
{
  return stream << identifier.str();
}

template <>
struct std::hash<Identifier> {
  size_t operator()(const Identifier& identifier) const { return identifier.hash(); }
};
