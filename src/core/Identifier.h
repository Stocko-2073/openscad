#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>

/*
 * An interned variable, function or module name.
 *
 * Every distinct spelling is stored exactly once, so comparing two Identifiers
 * is a pointer comparison. That is what makes scanning a ContextFrame cheap:
 * instantiating a BOSL2-heavy model performs ~150M frame probes, and with
 * std::string keys the memcmp behind each one was the single largest cost in
 * script evaluation.
 *
 * Constructing an Identifier from text interns it, which takes a lock and
 * hashes the string. Build them while parsing and pass them along; never
 * construct one inside an evaluation loop.
 */
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

  /*
   * True for the dynamically scoped names: '$' followed by anything except
   * "$children", which is an ordinary lexical variable.
   */
  bool isConfigVariable() const { return entry->is_config; }

  /*
   * One set bit, distinct for the first 64 names interned and cycling after
   * that. Context frames OR together the bits of the names they can supply as
   * functions, so a frame that cannot possibly answer a lookup is rejected by
   * a single AND instead of a virtual call and a table probe.
   */
  uint64_t bit() const { return entry->bit; }

  /*
   * Position of this name in the intern table, counting from zero. Names are
   * numbered as they are first seen, so a bitmap indexed by index() is dense
   * and collision-free up to its size. LocalScope uses one to answer "no such
   * function here" without probing its table.
   */
  size_t index() const { return entry->index; }

  /*
   * True once anything, anywhere in the process, has bound this name to a
   * function *value* -- a variable holding a function, as opposed to a
   * `function name() = ...` definition. Function lookup can then no longer be
   * decided from the immutable scope structure alone, so the call-site cache
   * in FunctionCall stops trusting itself for this name.
   *
   * Set from ContextFrame::set_variable and never cleared, which keeps it
   * conservative: the worst a stale reading can do is walk the context chain
   * that the cache would have skipped. Sharing it across evaluations is
   * likewise only ever conservative.
   */
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
    // See hasFunctionValue(). Written from script evaluation, which the GUI's
    // animation prefetch runs on more than one thread at once.
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
