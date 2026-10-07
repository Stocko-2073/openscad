#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

/**
 * BOSL2 comes in two copies: the built-in one, copied from the builtin-libraries/BOSL2 submodule
 * at build time, and a newer GitHub release the GUI downloads into updateRoot() (gui/BOSL2Updater).
 */
namespace BOSL2Library {

using Version = std::array<int, 3>;

std::string toString(const Version& version);

// "v2.0.757" or "2.0.757" -> {2, 0, 757}
std::optional<Version> parseTag(const std::string& tag);

std::optional<Version> readVersion(const fs::path& bosl2Dir);

// Library path entries (folders holding a BOSL2 subfolder).
fs::path builtinRoot();
fs::path updateRoot();

// The entry holding the newer BOSL2, the built-in one on a tie. Empty if neither is present.
fs::path activeRoot();

std::optional<Version> activeVersion();

// Which BOSL2 include <BOSL2/std.scad> resolves to, in one line. Needs parser_init().
std::string describe();

}  // namespace BOSL2Library
