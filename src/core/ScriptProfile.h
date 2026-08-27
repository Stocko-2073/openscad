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
 * Counts what the script does, per source location.
 *
 * This profiles the .scad code, not the interpreter: it answers "which of my
 * functions, modules and loops is the evaluator spending its calls on", which
 * a C++ sampling profiler cannot, because every script-level call looks like
 * the same handful of C++ frames.
 *
 * Counting rather than timing is deliberate. There are tens of millions of
 * script-level events in a large model, so reading a clock twice per event
 * would both cost more than the work being measured and distort it. Counts
 * are exact, have no sampling error, and in practice localise the problem:
 * a helper called three million times is the answer whatever it costs each
 * time.
 *
 * Sites are registered the first time they run and hold their count in the AST
 * node itself, so the steady-state cost is an increment. Collection is off
 * unless enabled, which reduces it to a load of a global and a branch.
 *
 * Lifetime: the registry holds pointers into the AST, so it must not outlive
 * the evaluation that filled it. ScopedRun enforces that; nothing else should
 * call clear().
 *
 * Concurrency: registration and clearing take a lock, because the GUI's
 * animation prefetch re-evaluates the same AST on a worker thread. The
 * per-event increments are deliberately unsynchronised -- they are the hot
 * path -- so two concurrent evaluations can lose counts against each other.
 * Neither can corrupt the registry.
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
  // Script being evaluated; locations are reported relative to its directory.
  static std::string documentRoot;

  /*
   * Clears the run's counts on scope exit, so no pointer into the AST outlives
   * the evaluation that registered it -- including when evaluation throws.
   */
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

/*
 * A loop's slot in the profile. The only place that can count body executions
 * is the iteration driver, which is several frames below the AST node that
 * owns the count, so the slot is passed down.
 */
struct ProfileSite {
  Identifier name;
  uint64_t *count = nullptr;
};

/*
 * Records one script-level event against $count, registering the site the
 * first time it fires. Inline and guarded so that a build with profiling off
 * pays a predictable branch.
 */
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
