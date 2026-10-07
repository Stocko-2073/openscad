#include "core/BOSL2Library.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <string>
#include <system_error>

#include "core/parsersettings.h"
#include "platform/PlatformUtils.h"

namespace BOSL2Library {

namespace {

constexpr const char *kUpdateFolder = "builtin-libraries";

bool sameDir(const fs::path& a, const fs::path& b)
{
  std::error_code ec;
  return !b.empty() && fs::equivalent(a, b, ec);
}

}  // namespace

std::string toString(const Version& version)
{
  return std::to_string(version[0]) + "." + std::to_string(version[1]) + "." +
         std::to_string(version[2]);
}

std::optional<Version> parseTag(const std::string& tag)
{
  static const std::regex re{R"(^v?(\d+)\.(\d+)\.(\d+)$)"};
  std::smatch m;
  if (!std::regex_match(tag, m, re)) return {};
  try {
    return Version{std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3])};
  } catch (const std::exception&) {
    return {};
  }
}

std::optional<Version> readVersion(const fs::path& bosl2Dir)
{
  std::ifstream in(bosl2Dir / "version.scad");
  if (!in) return {};
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  // BOSL_VERSION = [2,0,757];
  static const std::regex re{R"(\bBOSL_VERSION\s*=\s*\[\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\])"};
  std::smatch m;
  if (!std::regex_search(text, m, re)) return {};
  try {
    return Version{std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3])};
  } catch (const std::exception&) {
    return {};
  }
}

fs::path builtinRoot()
{
  return PlatformUtils::resourcePath("builtin-libraries");
}

fs::path updateRoot()
{
#if defined(__APPLE__) || defined(_WIN32)
  // ~/Library/Application Support/OpenSCAD, or %LOCALAPPDATA%/OpenSCAD
  const std::string base = PlatformUtils::userConfigPath();
  if (base.empty()) return {};
  return fs::path(base) / kUpdateFolder;
#else
  // ~/.local/share/OpenSCAD, next to the user's libraries folder
  const std::string path = PlatformUtils::userPath(kUpdateFolder);
  return path.empty() ? fs::path{} : fs::path(path);
#endif
}

fs::path activeRoot()
{
  fs::path builtin = builtinRoot();
  const auto builtinVersion = builtin.empty() ? std::nullopt : readVersion(builtin / "BOSL2");
  fs::path updated = updateRoot();
  const auto updatedVersion = updated.empty() ? std::nullopt : readVersion(updated / "BOSL2");

  if (updatedVersion && (!builtinVersion || *updatedVersion > *builtinVersion)) return updated;
  if (builtinVersion || (!builtin.empty() && fs::exists(builtin / "BOSL2" / "std.scad"))) {
    return builtin;
  }
  return {};
}

std::optional<Version> activeVersion()
{
  const fs::path root = activeRoot();
  return root.empty() ? std::nullopt : readVersion(root / "BOSL2");
}

std::string describe()
{
  const fs::path active = activeRoot();
  const auto versionText = [](const std::optional<Version>& version) {
    return version ? "v" + toString(*version) : std::string("(unknown version)");
  };

  const fs::path found = search_libs("BOSL2/std.scad");
  if (found.empty()) {
    const fs::path base = PlatformUtils::resourceBasePath();
    return "BOSL2 not found: the built-in copy is missing from " +
           (base / "builtin-libraries" / "BOSL2").generic_string();
  }

  const fs::path dir = found.parent_path();
  const fs::path root = dir.parent_path();
  const std::string usage = ": include <BOSL2/std.scad>";
  if (sameDir(root, active)) {
    const bool updated = sameDir(root, updateRoot());
    return "BOSL2 " + versionText(readVersion(dir)) + " built in" +
           (updated ? " (updated from GitHub)" : "") + usage;
  }
  return "BOSL2 " + versionText(readVersion(dir)) + " from " + dir.generic_string() +
         (active.empty() ? " (no built-in copy found)"
                         : ", overriding the built-in " + versionText(activeVersion())) +
         usage;
}

}  // namespace BOSL2Library
