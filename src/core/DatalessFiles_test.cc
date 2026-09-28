#include "core/DatalessFiles.h"

#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "openscad.h"

namespace fs = std::filesystem;

namespace {

// A folder holding main.scad's neighbours: lib.scad, which the tests treat as
// still being in iCloud, and ok.scad, which they do not.
fs::path makeDir(const std::string& name)
{
  const fs::path dir = fs::temp_directory_path() / ("openscad-dataless-test-" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "lib.scad") << "module from_lib() {}\n";
  std::ofstream(dir / "ok.scad") << "module from_ok() {}\n";
  return fs::canonical(dir);  // as the parser resolves includes
}

bool libIsDataless(const fs::path& path)
{
  return path.filename() == "lib.scad";
}

// Undoes the GUI-only settings the tests turn on.
struct Restore {
  ~Restore()
  {
    DatalessFiles::setDeferReads(false);
    DatalessFiles::setProbeForTesting(nullptr);
    DatalessFiles::takeDeferred();
  }
};

std::string parseToText(const fs::path& dir, const std::string& text)
{
  const std::string mainFile = (dir / "main.scad").generic_string();
  SourceFile *file = nullptr;
  REQUIRE(parse(file, text + "\n\x03\n", mainFile, mainFile, false));
  std::ostringstream out;
  file->print(out, "");
  delete file;
  return out.str();
}

}  // namespace

TEST_CASE("Only the GUI skips files still in iCloud", "[dataless]")
{
  const Restore restore;
  const fs::path dir = makeDir("cli");
  DatalessFiles::setProbeForTesting(libIsDataless);

  // The command line reads them, letting the system download them first.
  CHECK_FALSE(DatalessFiles::shouldDefer(dir / "lib.scad"));
  const std::string text = parseToText(dir, "include <lib.scad>");
  CHECK(text.find("from_lib") != std::string::npos);
  CHECK(DatalessFiles::takeDeferred().empty());
}

TEST_CASE("The GUI skips an include<> still in iCloud and reports it once", "[dataless]")
{
  const Restore restore;
  const fs::path dir = makeDir("include");
  DatalessFiles::setProbeForTesting(libIsDataless);
  DatalessFiles::setDeferReads(true);

  const std::string text =
    parseToText(dir, "include <lib.scad>\ninclude <lib.scad>\ninclude <ok.scad>\nmodule from_main() {}");
  CHECK(text.find("from_lib") == std::string::npos);
  CHECK(text.find("from_ok") != std::string::npos);
  CHECK(text.find("from_main") != std::string::npos);
  CHECK(DatalessFiles::takeDeferred() == std::vector<std::string>{(dir / "lib.scad").generic_string()});
  CHECK(DatalessFiles::takeDeferred().empty());

  // Once downloaded, the next parse reads it.
  DatalessFiles::setProbeForTesting([](const fs::path&) { return false; });
  CHECK(parseToText(dir, "include <lib.scad>").find("from_lib") != std::string::npos);
  CHECK(DatalessFiles::takeDeferred().empty());
}

TEST_CASE("The GUI skips a use<> still in iCloud without caching the miss", "[dataless]")
{
  const Restore restore;
  const fs::path dir = makeDir("use");
  DatalessFiles::setProbeForTesting(libIsDataless);
  DatalessFiles::setDeferReads(true);

  const std::string mainFile = (dir / "main.scad").generic_string();
  const std::string lib = (dir / "lib.scad").generic_string();
  auto *cache = SourceFileCache::instance();
  cache->clear();

  SourceFile *used = nullptr;
  CHECK(cache->process(mainFile, lib, used) == 0);
  CHECK(used == nullptr);
  CHECK(cache->lookup(lib) == nullptr);
  CHECK(DatalessFiles::takeDeferred() == std::vector<std::string>{lib});

  // Once downloaded, the next pass parses it.
  DatalessFiles::setProbeForTesting([](const fs::path&) { return false; });
  CHECK(cache->process(mainFile, lib, used) > 0);
  CHECK(used != nullptr);
  CHECK(DatalessFiles::takeDeferred().empty());
  cache->clear();
}

TEST_CASE("Ordinary files are not dataless; downloads report what is still missing", "[dataless]")
{
  const fs::path dir = makeDir("ordinary");
  CHECK_FALSE(DatalessFiles::isDataless(dir / "ok.scad"));
  CHECK(DatalessFiles::materialize(dir / "ok.scad"));

  const Restore restore;
  DatalessFiles::setProbeForTesting(libIsDataless);
  std::vector<std::string> files;
  for (int i = 0; i < 20; ++i)
    files.push_back((dir / (i == 7 ? "lib.scad" : "ok.scad")).generic_string());
  CHECK(DatalessFiles::materializeAll(files) ==
        std::vector<std::string>{(dir / "lib.scad").generic_string()});
  CHECK(DatalessFiles::materializeAll({}).empty());
}
