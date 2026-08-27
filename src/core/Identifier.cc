#include "core/Identifier.h"

#include <mutex>
#include <string>
#include <unordered_map>

/*
 * std::unordered_map never invalidates references to its elements, so the
 * address of a mapped Entry is stable for the lifetime of the process and can
 * serve as the identity of the name.
 *
 * The table is never emptied. Names come from source text, so it stays
 * proportional to the size of the scripts parsed, and the interned strings
 * outlive any AST that refers to them.
 */
namespace {

std::mutex& internMutex()
{
  static std::mutex mutex;
  return mutex;
}

bool isConfigName(const std::string& name)
{
  return !name.empty() && name[0] == '$' && name != "$children";
}

}  // namespace

const Identifier::Entry *Identifier::emptyEntry()
{
  static const Entry *entry = intern(std::string());
  return entry;
}

const Identifier::Entry *Identifier::intern(const std::string& name)
{
  using Table = std::unordered_map<std::string, Identifier::Entry>;
  static auto *table = new Table();

  const std::lock_guard<std::mutex> lock(internMutex());
  auto it = table->find(name);
  if (it == table->end()) {
    it = table->emplace(name, Entry{name, isConfigName(name)}).first;
  }
  return &it->second;
}
