#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

#include "core/AST.h"
#include "core/Identifier.h"

/*
 * Per-site counts for --profile. The registry points at counts inside the AST, so only ScopedRun
 * clears it. Registration locks, as animation prefetch evaluates the AST on worker threads; the
 * increments don't, so concurrent evaluations may lose counts.
 */
class ScriptProfile
{
public:
  enum class Kind : uint8_t { Call, Module, LoopIteration };

  static ScriptProfile& instance();

  // Checked on every script-level event; keep it a plain global.
  static bool enabled;
  // Where a run's full report goes. Empty means console output only.
  static std::string reportFile;
  static std::string documentRoot;

  class ScopedRun
  {
  public:
    ScopedRun() = default;
    ~ScopedRun()
    {
      if (!ScriptProfile::instance().empty()) {
        ScriptProfile::instance().clear();
      }
    }
    ScopedRun(const ScopedRun&) = delete;
    ScopedRun& operator=(const ScopedRun&) = delete;
  };

  void add(Kind kind, const Identifier& name, const Location& location, uint64_t *count);

  bool empty() const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return sites.empty();
  }
  void report(std::ostream& stream, size_t limit) const;
  void writeTsv(const std::string& path) const;
  void clear();

private:
  struct Site {
    Kind kind;
    Identifier name;
    const Location *location;
    uint64_t *count;
  };

  std::vector<Site> sites;
  mutable std::mutex mutex;
};

struct ProfileSite {
  Identifier name;
  uint64_t *count = nullptr;
};

inline void profileEvent(ScriptProfile::Kind kind, const Identifier& name, const Location& location,
                         uint64_t& count)
{
  if (!ScriptProfile::enabled) {
    return;
  }
  if (count++ == 0) {
    ScriptProfile::instance().add(kind, name, location, &count);
  }
}

inline void profileEvent(ScriptProfile::Kind kind, const ProfileSite& site, const Location& location)
{
  if (site.count) {
    profileEvent(kind, site.name, location, *site.count);
  }
}
