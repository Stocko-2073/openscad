#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/**
 * Files that iCloud Drive (or another File Provider) has evicted are "dataless": they stat()
 * normally, but the first read blocks in the kernel until they are downloaded, which can take
 * minutes. The GUI parses on its main thread, so it has the parser skip them (deferReads).
 */
namespace DatalessFiles {

// Always false off macOS.
bool isDataless(const fs::path& path);

void setDeferReads(bool defer);

// True when deferReads is on and the file is dataless; it is then recorded for takeDeferred().
bool shouldDefer(const fs::path& path);

// The files shouldDefer() recorded since the last call, each once.
std::vector<std::string> takeDeferred();

// Blocks until the file has downloaded; for a worker thread. False if it is still dataless.
bool materialize(const fs::path& path);

// materialize() in parallel. Returns the files that are still dataless.
std::vector<std::string> materializeAll(const std::vector<std::string>& files);

}  // namespace DatalessFiles
