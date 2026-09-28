#include "core/DatalessFiles.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <sys/resource.h>
#endif

namespace DatalessFiles {

namespace {

std::atomic<bool> deferReads{false};
bool (*probeForTesting)(const fs::path&) = nullptr;

std::mutex deferredMutex;
std::vector<std::string> deferred;

}  // namespace

bool isDataless(const fs::path& path)
{
  if (probeForTesting) return probeForTesting(path);
#if defined(__APPLE__) && defined(SF_DATALESS)
  struct stat st;
  return stat(path.c_str(), &st) == 0 && (st.st_flags & SF_DATALESS) != 0;
#else
  return false;
#endif
}

void setDeferReads(bool defer)
{
  deferReads = defer;
}

bool shouldDefer(const fs::path& path)
{
  if (!deferReads || !isDataless(path)) return false;
  const std::string name = path.generic_string();
  const std::lock_guard<std::mutex> lock(deferredMutex);
  if (std::find(deferred.begin(), deferred.end(), name) == deferred.end()) deferred.push_back(name);
  return true;
}

std::vector<std::string> takeDeferred()
{
  const std::lock_guard<std::mutex> lock(deferredMutex);
  std::vector<std::string> taken;
  taken.swap(deferred);
  return taken;
}

bool materialize(const fs::path& path)
{
#if defined(__APPLE__) && defined(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES)
  // The system default is not to download on access; make sure this thread does.
  setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD,
                 IOPOL_MATERIALIZE_DATALESS_FILES_ON);
#endif
  std::ifstream in(path, std::ios::binary);
  in.ignore(std::numeric_limits<std::streamsize>::max());
  return !isDataless(path);
}

std::vector<std::string> materializeAll(const std::vector<std::string>& files)
{
  constexpr size_t kMaxDownloads = 8;
  std::vector<char> ok(files.size(), false);
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  for (size_t i = 0; i < std::min(files.size(), kMaxDownloads); ++i) {
    workers.emplace_back([&]() {
      for (size_t f; (f = next++) < files.size();) ok[f] = materialize(files[f]);
    });
  }
  for (auto& worker : workers) worker.join();

  std::vector<std::string> failed;
  for (size_t f = 0; f < files.size(); ++f) {
    if (!ok[f]) failed.push_back(files[f]);
  }
  return failed;
}

void setProbeForTesting(bool (*probe)(const fs::path&))
{
  probeForTesting = probe;
}

}  // namespace DatalessFiles
