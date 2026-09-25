#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

/**
 * BOSL2 ships inside OpenSCAD and is used through the usual
 * include <BOSL2/std.scad>. There are two copies:
 *
 *  - the built-in one, the builtin-libraries/BOSL2 submodule copied into the
 *    resources folder at build time;
 *  - an updated one, a newer GitHub release that the GUI downloads at launch
 *    into a writable folder (see gui/BOSL2Updater).
 *
 * Whichever of the two is newer goes on the library path ahead of the user's
 * libraries folder, so a stale hand-installed BOSL2 there does not shadow it.
 * Only OPENSCADPATH comes first.
 */
namespace BOSL2Library {

using Version = std::array<int, 3>;

std::string toString(const Version& version);

// "v2.0.757" or "2.0.757" -> {2, 0, 757}
std::optional<Version> parseTag(const std::string& tag);

// Reads BOSL_VERSION from <bosl2Dir>/version.scad.
std::optional<Version> readVersion(const fs::path& bosl2Dir);

// Library path entries (folders holding a BOSL2 subfolder).
fs::path builtinRoot();
fs::path updateRoot();

// The library path entry holding the newer BOSL2: the updated copy only while
// it is strictly newer than the built-in one. Empty if neither is present.
fs::path activeRoot();

// Version of the copy activeRoot() points at.
std::optional<Version> activeVersion();

// One line for the launch banner and --info saying which BOSL2
// include <BOSL2/std.scad> resolves to. Needs parser_init().
std::string describe();

}  // namespace BOSL2Library
