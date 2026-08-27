#pragma once

#include <cstddef>
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

  size_t hash() const { return std::hash<const void *>{}(entry); }

private:
  struct Entry {
    std::string name;
    bool is_config;
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
