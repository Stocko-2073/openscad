#include "core/Identifier.h"

#include <cstdint>
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

  static size_t next_index = 0;

  const std::lock_guard<std::mutex> lock(internMutex());
  // Built in place rather than moved in: Entry holds an atomic flag, so it is
  // not movable. See Entry::function_value.
  auto [it, inserted] = table->try_emplace(name);
  if (inserted) {
    Entry& entry = it->second;
    const size_t index = next_index++;
    entry.name = name;
    entry.is_config = isConfigName(name);
    // Round-robin so the first 64 distinct names get a bit each; see bit().
    entry.bit = uint64_t(1) << (index & 63);
    entry.index = index;
  }
  return &it->second;
}
