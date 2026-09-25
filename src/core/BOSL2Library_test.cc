#include "core/BOSL2Library.h"

#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using BOSL2Library::Version;

namespace {

// A throwaway BOSL2 folder holding only a version.scad with the given text.
fs::path makeBOSL2Dir(const std::string& name, const std::string& versionScad)
{
  const fs::path dir = fs::temp_directory_path() / ("openscad-bosl2-test-" + name) / "BOSL2";
  fs::remove_all(dir.parent_path());
  fs::create_directories(dir);
  std::ofstream(dir / "version.scad") << versionScad;
  return dir;
}

}  // namespace

TEST_CASE("BOSL2 release tags parse into versions", "[BOSL2]")
{
  CHECK(BOSL2Library::parseTag("v2.0.757") == Version{2, 0, 757});
  CHECK(BOSL2Library::parseTag("2.0.757") == Version{2, 0, 757});
  CHECK_FALSE(BOSL2Library::parseTag("v2.0").has_value());
  CHECK_FALSE(BOSL2Library::parseTag("v2.0.757-beta").has_value());
  CHECK_FALSE(BOSL2Library::parseTag("").has_value());
}

TEST_CASE("BOSL2 versions order numerically, not as text", "[BOSL2]")
{
  CHECK(*BOSL2Library::parseTag("v2.0.1000") > *BOSL2Library::parseTag("v2.0.999"));
  CHECK(*BOSL2Library::parseTag("v2.1.0") > *BOSL2Library::parseTag("v2.0.757"));
  CHECK(BOSL2Library::toString(Version{2, 0, 757}) == "2.0.757");
}

TEST_CASE("BOSL2 version is read from version.scad", "[BOSL2]")
{
  SECTION("the release layout")
  {
    const fs::path dir = makeBOSL2Dir("release",
                                      "_BOSL2_VERSION = true;\n\n"
                                      "BOSL_VERSION = [2,0,757];\n\n"
                                      "function bosl_version() = BOSL_VERSION;\n");
    CHECK(BOSL2Library::readVersion(dir) == Version{2, 0, 757});
    fs::remove_all(dir.parent_path());
  }
  SECTION("spaces inside the list")
  {
    const fs::path dir = makeBOSL2Dir("spaced", "BOSL_VERSION = [ 2, 1 , 3 ];\n");
    CHECK(BOSL2Library::readVersion(dir) == Version{2, 1, 3});
    fs::remove_all(dir.parent_path());
  }
  SECTION("no BOSL_VERSION")
  {
    const fs::path dir = makeBOSL2Dir("missing", "_BOSL2_VERSION = true;\n");
    CHECK_FALSE(BOSL2Library::readVersion(dir).has_value());
    fs::remove_all(dir.parent_path());
  }
  SECTION("no version.scad")
  {
    CHECK_FALSE(BOSL2Library::readVersion(fs::temp_directory_path() / "openscad-bosl2-test-none")
                  .has_value());
  }
}
