#include "gui/AnimateFrameCache.h"

#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <climits>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "core/EvalMemo.h"
#include "core/Settings.h"
#include "core/SourceFile.h"
#include "glview/Camera.h"
#include "gui/AnimateFrameTask.h"
#include "platform/PlatformUtils.h"

namespace OpenScad::Animate {

namespace {

// Cap to bound peak memory across worker pool.
constexpr int kMaxWorkerCap = 16;

int compute_worker_count()
{
  unsigned int hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 4;
  return std::min<int>(static_cast<int>(hw), kMaxWorkerCap);
}

} // namespace

MemoTablePool::MemoTablePool() = default;

MemoTablePool::~MemoTablePool() = default;

std::unique_ptr<memo::MemoTable> MemoTablePool::take()
{
  std::shared_ptr<const memo::MemoTable> seed;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!idle_.empty()) {
      auto table = std::move(idle_.back());
      idle_.pop_back();
      return table;
    }
    seed = seed_;
  }
  // Nothing changes the seed, so tasks fork it at once, each on its own thread.
  return seed ? seed->fork() : std::make_unique<memo::MemoTable>();
}

void MemoTablePool::give(std::unique_ptr<memo::MemoTable> table)
{
  if (!table) return;
  const std::lock_guard<std::mutex> lock(mutex_);
  idle_.push_back(std::move(table));
}

void MemoTablePool::setSeed(std::shared_ptr<const memo::MemoTable> seed)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  seed_ = std::move(seed);
}

std::vector<std::unique_ptr<memo::MemoTable>> MemoTablePool::trim()
{
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::move(idle_);
}

FrameCache::FrameCache(QObject *parent) : QObject(parent), pool_(std::make_unique<QThreadPool>())
{
  pool_->setMaxThreadCount(compute_worker_count());
  // A frame recurses as deeply as an evaluation on the GUI thread, which StackCheck lets use up to
  // stackLimit(); other threads get far less by default (512 KB on macOS).
  pool_->setStackSize(static_cast<uint>(
    std::min<unsigned long>(PlatformUtils::stackLimit() + STACK_BUFFER_SIZE, UINT_MAX)));
  pool_->setExpiryTimeout(60 * 1000);
  cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
}

FrameCache::~FrameCache()
{
  if (cancel_flag_) cancel_flag_->store(true, std::memory_order_release);
  if (pool_) {
    pool_->clear();
    pool_->waitForDone();
  }
}

int FrameCache::workerCount() const
{
  return pool_ ? pool_->maxThreadCount() : 0;
}

void FrameCache::setSource(std::shared_ptr<SourceFile> sourceFile,
                           std::string documentPath,
                           int numSteps,
                           const Camera &camera,
                           const memo::MemoTable *seed)
{
  // Forked here, on the GUI thread, the document's table leaves the tasks a copy that nothing changes.
  std::shared_ptr<MemoTablePool> tables;
  {
    const QMutexLocker locker(&mutex_);
    tables = memoTables_unlocked();
  }
  if (tables && seed) tables->setSeed(seed->fork());

  std::shared_ptr<std::atomic<bool>> old_flag;
  {
    const QMutexLocker locker(&mutex_);
    old_flag = cancel_flag_;
    if (old_flag) old_flag->store(true, std::memory_order_release);

    ++generation_;
    source_file_ = std::move(sourceFile);
    document_path_ = std::move(documentPath);
    num_steps_ = numSteps;
    camera_ = std::make_shared<Camera>(camera);
    cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
    frames_.clear();
  }
  if (pool_) pool_->clear();
}

void FrameCache::invalidateAll()
{
  std::shared_ptr<std::atomic<bool>> old_flag;
  std::shared_ptr<MemoTablePool> old_tables;
  {
    const QMutexLocker locker(&mutex_);
    old_flag = cancel_flag_;
    if (old_flag) old_flag->store(true, std::memory_order_release);

    ++generation_;
    cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
    frames_.clear();
    old_tables = std::move(memo_tables_);
  }
  if (pool_) pool_->clear();
  if (old_tables) freeOnWorker(std::move(old_tables));
}

void FrameCache::dropMemoTables()
{
  std::shared_ptr<MemoTablePool> old_tables;
  {
    const QMutexLocker locker(&mutex_);
    old_tables = std::move(memo_tables_);
  }
  if (old_tables) freeOnWorker(std::move(old_tables));
}

template <class T>
void FrameCache::freeOnWorker(T garbage)
{
  if (!pool_) return;
  // Held by a copyable function, as QThreadPool takes one.
  auto held = std::make_shared<T>(std::move(garbage));
  pool_->start([held]() mutable { held.reset(); });
}

std::shared_ptr<MemoTablePool> FrameCache::memoTables_unlocked()
{
  if (!Settings::Settings::reuseModuleResults.value()) memo_tables_.reset();
  else if (!memo_tables_) memo_tables_ = std::make_shared<MemoTablePool>();
  return memo_tables_;
}

std::shared_ptr<CachedFrame> FrameCache::tryGet(int step)
{
  const QMutexLocker locker(&mutex_);
  if (num_steps_ <= 0) return nullptr;
  const int wrapped = ((step % num_steps_) + num_steps_) % num_steps_;
  auto it = frames_.find(wrapped);
  if (it == frames_.end()) return nullptr;
  return it->second;
}

std::shared_ptr<CachedFrame> FrameCache::latestReady()
{
  const QMutexLocker locker(&mutex_);
  for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
    if (it->second->state.load(std::memory_order_acquire) == FrameState::Ready
        && it->second->result) {
      return it->second;
    }
  }
  return nullptr;
}

void FrameCache::prefetchWindow(int currentStep, int lookahead)
{
  if (lookahead <= 0) return;

  const QMutexLocker locker(&mutex_);
  if (!source_file_ || num_steps_ <= 0) return;

  const int n = num_steps_;
  const int start = ((currentStep % n) + n) % n;

  std::vector<int> wanted;
  wanted.reserve(lookahead);
  for (int i = 0; i < lookahead && i < n; ++i) {
    wanted.push_back((start + i) % n);
  }

  // Ready frames stay until setSource() or invalidateAll(): every loop of the animation shows them.
  std::set<int> wanted_set(wanted.begin(), wanted.end());
  for (auto it = frames_.begin(); it != frames_.end(); ) {
    if (wanted_set.count(it->first) == 0) {
      const auto state = it->second->state.load(std::memory_order_acquire);
      if (state == FrameState::Failed || state == FrameState::Cancelled) {
        it = frames_.erase(it);
        continue;
      }
    }
    ++it;
  }

  for (int step : wanted) {
    if (frames_.find(step) == frames_.end()) {
      enqueueStep_unlocked(step);
    }
  }
}

void FrameCache::enqueueStep_unlocked(int step)
{
  if (num_steps_ <= 0 || !source_file_ || !pool_) return;

  auto frame = std::make_shared<CachedFrame>();
  frame->step = step;
  frame->t = static_cast<double>(step) / static_cast<double>(num_steps_);
  frame->state.store(FrameState::Pending, std::memory_order_release);
  frames_[step] = frame;

  const Camera cam = camera_ ? *camera_ : Camera{};
  auto *task = new FrameTask(QPointer<FrameCache>(this),
                             generation_,
                             frame,
                             source_file_,
                             document_path_,
                             cam,
                             cancel_flag_,
                             memoTables_unlocked());
  pool_->start(task);
}

void FrameCache::onFrameComplete(int step, int generation)
{
  std::vector<std::unique_ptr<memo::MemoTable>> idle_tables;
  bool ready = false;
  {
    const QMutexLocker locker(&mutex_);
    if (generation != generation_) return;
    auto it = frames_.find(step);
    if (it == frames_.end()) return;
    ready = it->second->state.load(std::memory_order_acquire) == FrameState::Ready;
    // With no frame pending, the tasks are done with their memo tables, each holding a tree of
    // its own. The next frames fork the seed again.
    const bool pending = std::any_of(frames_.begin(), frames_.end(), [](const auto& entry) {
      return entry.second->state.load(std::memory_order_acquire) == FrameState::Pending;
    });
    if (!pending && memo_tables_) idle_tables = memo_tables_->trim();
  }
  if (!idle_tables.empty()) freeOnWorker(std::move(idle_tables));
  if (ready) emit frameReady(step);
}

} // namespace OpenScad::Animate
