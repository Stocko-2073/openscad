#include "core/ScriptProfile.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/parsersettings.h"
#include "io/fileutils.h"
#include "utils/printutils.h"

bool ScriptProfile::enabled = false;
std::string ScriptProfile::reportFile;

namespace {

const char *kindName(ScriptProfile::Kind kind)
{
  switch (kind) {
    case ScriptProfile::Kind::Call: return "call";
    case ScriptProfile::Kind::Module: return "module";
    case ScriptProfile::Kind::LoopIteration: return "loop";
  }
  return "?";
}

// Fixed width so the name column lines up between "call" and "module".
std::string paddedKind(ScriptProfile::Kind kind)
{
  std::string name = kindName(kind);
  name.resize(6, ' ');
  return name;
}

std::string grouped(uint64_t n)
{
  const std::string digits = std::to_string(n);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  size_t placed = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it, ++placed) {
    if (placed && placed % 3 == 0) out += ',';
    out += *it;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

/*
 * "BOSL2/shapes3d.scad:412:19". Shown relative to the script's own directory
 * where that is shorter, and otherwise relative to whichever library directory
 * the file came from -- a library sitting outside the project would otherwise
 * report as a long climb through "..".
 */
std::string where(const Location *location)
{
  if (!location || location->isNone()) return "<unknown>";
  const fs::path& path = location->filePath();

  std::string shortest = path.generic_string();
  const auto consider = [&shortest, &path](const fs::path& base) {
    if (base.empty()) return;
    const std::string candidate = fs_uncomplete(path, base).generic_string();
    if (candidate.size() < shortest.size()) shortest = candidate;
  };

  consider(fs::path(ScriptProfile::documentRoot));
  for (const std::string& dir : get_library_path()) {
    consider(fs::path(dir));
  }

  return shortest + ":" + std::to_string(location->firstLine()) + ":" +
         std::to_string(location->firstColumn());
}

}  // namespace

std::string ScriptProfile::documentRoot;

ScriptProfile& ScriptProfile::instance()
{
  static ScriptProfile profile;
  return profile;
}

void ScriptProfile::add(Kind kind, const Identifier& name, const Location& location, uint64_t *count)
{
  const std::lock_guard<std::mutex> lock(mutex);
  sites.push_back(Site{kind, name, &location, count});
}

void ScriptProfile::clear()
{
  const std::lock_guard<std::mutex> lock(mutex);
  for (const Site& site : sites) {
    *site.count = 0;
  }
  sites.clear();
}

void ScriptProfile::report(std::ostream& stream, size_t limit) const
{
  const std::lock_guard<std::mutex> lock(mutex);

  struct Total {
    uint64_t count = 0;
    size_t sites = 0;
  };
  Total totals[3];
  std::map<std::pair<int, std::string>, uint64_t> byName;

  for (const Site& site : sites) {
    Total& total = totals[static_cast<int>(site.kind)];
    total.count += *site.count;
    ++total.sites;
    byName[{static_cast<int>(site.kind), site.name.str()}] += *site.count;
  }

  stream << "Script evaluation profile\n";
  const Kind kinds[3] = {Kind::Call, Kind::Module, Kind::LoopIteration};
  const char *labels[3] = {"function calls", "module instantiations", "loop iterations"};
  for (int i = 0; i < 3; ++i) {
    const Total& total = totals[static_cast<int>(kinds[i])];
    const std::string count = grouped(total.count);
    stream << "  " << labels[i] << std::string(24 - std::string(labels[i]).size(), ' ')
           << std::string(count.size() < 12 ? 12 - count.size() : 0, ' ') << count << "  at "
           << grouped(total.sites) << (total.sites == 1 ? " site\n" : " sites\n");
  }

  std::vector<std::pair<uint64_t, std::string>> names;
  names.reserve(byName.size());
  for (const auto& entry : byName) {
    names.emplace_back(entry.second, paddedKind(static_cast<Kind>(entry.first.first)) + "  " +
                                       entry.first.second);
  }
  std::sort(names.begin(), names.end(), std::greater<>());

  stream << "\nBusiest names\n";
  for (size_t i = 0; i < names.size() && i < limit; ++i) {
    std::string count = grouped(names[i].first);
    stream << "  " << std::string(count.size() < 14 ? 14 - count.size() : 0, ' ') << count << "  "
           << names[i].second << "\n";
  }

  std::vector<const Site *> bySite;
  bySite.reserve(sites.size());
  for (const Site& site : sites) {
    bySite.push_back(&site);
  }
  std::sort(bySite.begin(), bySite.end(),
            [](const Site *a, const Site *b) { return *a->count > *b->count; });

  stream << "\nBusiest sites\n";
  for (size_t i = 0; i < bySite.size() && i < limit; ++i) {
    const Site *site = bySite[i];
    std::string count = grouped(*site->count);
    std::string name = paddedKind(site->kind) + "  " + site->name.str();
    if (name.size() < 34) name += std::string(34 - name.size(), ' ');
    stream << "  " << std::string(count.size() < 14 ? 14 - count.size() : 0, ' ') << count << "  "
           << name << "  " << where(site->location) << "\n";
  }

  if (bySite.size() > limit) {
    stream << "  ... " << grouped(bySite.size() - limit) << " further sites";
    if (!reportFile.empty()) {
      stream << "; all of them in " << reportFile;
    } else {
      stream << "; --profile-file writes them all";
    }
    stream << "\n";
  }
}

void ScriptProfile::writeTsv(const std::string& path) const
{
  const std::lock_guard<std::mutex> lock(mutex);

  std::ofstream out(path);
  if (!out.is_open()) {
    LOG(message_group::Error, "Can't open profile file '%1$s'", path);
    return;
  }
  out << "count\tkind\tname\tfile\tline\tcolumn\n";
  std::vector<const Site *> ordered;
  ordered.reserve(sites.size());
  for (const Site& site : sites) {
    ordered.push_back(&site);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const Site *a, const Site *b) { return *a->count > *b->count; });
  for (const Site *site : ordered) {
    out << *site->count << '\t' << kindName(site->kind) << '\t' << site->name.str() << '\t'
        << (site->location ? site->location->filePath().generic_string() : "") << '\t'
        << (site->location ? site->location->firstLine() : 0) << '\t'
        << (site->location ? site->location->firstColumn() : 0) << '\n';
  }
}
