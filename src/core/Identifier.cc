#include "core/Identifier.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Entries are never erased, and std::unordered_map keeps references to its elements
// valid, so an Entry's address identifies its name for the life of the process.
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
  // Entry holds an atomic, so it is not movable and must be built in place.
  auto [it, inserted] = table->try_emplace(name);
  if (inserted) {
    Entry& entry = it->second;
    const size_t index = next_index++;
    entry.name = name;
    entry.is_config = isConfigName(name);
    entry.bit = uint64_t(1) << (index & 63);
    entry.index = index;
  }
  return &it->second;
}
