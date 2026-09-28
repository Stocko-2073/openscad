#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/**
 * Files that iCloud Drive (or another File Provider) has evicted to free up
 * space are "dataless": they still stat() normally, but the first read blocks
 * inside the kernel until the system has downloaded them, which can take
 * minutes. A libraries folder in an iCloud-synced ~/Documents is a common
 * source of them.
 *
 * The GUI parses on its main thread, so it turns on deferReads: the parser
 * then skips an include<> or use<> file that is still dataless instead of
 * reading it, and records it for the GUI to download off the main thread and
 * recompile once it has arrived. The command line reads such files as usual.
 */
namespace DatalessFiles {

// Whether reading the file's contents would first have to download it.
// Always false off macOS.
bool isDataless(const fs::path& path);

void setDeferReads(bool defer);

// True, and records the file for takeDeferred(), when deferReads is on and
// the file is dataless. The parser calls this just before it would read it.
bool shouldDefer(const fs::path& path);

// The files shouldDefer() skipped since the last call, each once.
std::vector<std::string> takeDeferred();

// Reads the file through so the system downloads it, blocking until it has.
// For a worker thread. Returns whether the file is no longer dataless.
bool materialize(const fs::path& path);

// materialize() for several files at once, since each download can take
// seconds. Returns the files that are still dataless.
std::vector<std::string> materializeAll(const std::vector<std::string>& files);

// Replaces isDataless() for tests, which cannot make a dataless file.
// nullptr restores it.
void setProbeForTesting(bool (*probe)(const fs::path&));

}  // namespace DatalessFiles
