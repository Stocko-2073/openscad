#include "gui/AnimateFrameCache.h"

#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "core/SourceFile.h"
#include "glview/Camera.h"
#include "gui/AnimateFrameTask.h"

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

FrameCache::FrameCache(QObject *parent) : QObject(parent), pool_(std::make_unique<QThreadPool>())
{
  pool_->setMaxThreadCount(compute_worker_count());
  // Don't kill idle threads aggressively — animation playback uses them repeatedly.
  pool_->setExpiryTimeout(60 * 1000);
  cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
}

FrameCache::~FrameCache()
{
  // Cancel any in-flight tasks before tearing down. Workers check the flag at
  // each pipeline boundary.
  if (cancel_flag_) cancel_flag_->store(true, std::memory_order_release);
  if (pool_) {
    pool_->clear();          // drop queued tasks
    pool_->waitForDone();    // wait for in-flight tasks to observe the cancel flag
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
                           bool isPreview)
{
  // Cancel any in-flight tasks before swapping state.
  std::shared_ptr<std::atomic<bool>> old_flag;
  {
    const QMutexLocker locker(&mutex_);
    old_flag = cancel_flag_;
    if (old_flag) old_flag->store(true, std::memory_order_release);

    ++generation_;
    source_file_ = std::move(sourceFile);
    document_path_ = std::move(documentPath);
    num_steps_ = numSteps;
    is_preview_ = isPreview;
    camera_ = std::make_shared<Camera>(camera);
    cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
    frames_.clear();
  }
  // Drop queued tasks; in-flight ones will exit early via the (old) cancel flag.
  if (pool_) pool_->clear();
}

void FrameCache::invalidateAll()
{
  std::shared_ptr<std::atomic<bool>> old_flag;
  {
    const QMutexLocker locker(&mutex_);
    old_flag = cancel_flag_;
    if (old_flag) old_flag->store(true, std::memory_order_release);

    ++generation_;
    cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
    frames_.clear();
  }
  if (pool_) pool_->clear();
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
  // No ordering metadata, so just scan and return any Ready frame. With the
  // current prefetch window this is a small set (lookahead*2 entries max).
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

  // Build set of steps we want in the window.
  std::vector<int> wanted;
  wanted.reserve(lookahead);
  for (int i = 0; i < lookahead && i < n; ++i) {
    wanted.push_back((start + i) % n);
  }

  // Keep Ready frames forever (until setSource/invalidateAll clears them).
  // Animation is cyclic and frame N is the same on every loop iteration as
  // long as the source is unchanged, so caching them across cycles means
  // cycle 2+ plays at full FPS even on scenes whose compute time exceeds
  // 1/fps. Drop only Failed/Cancelled leftovers outside the prefetch window
  // — those are cheap and just clutter the map.
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

  // Enqueue any wanted step that isn't already known.
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
                             is_preview_,
                             cancel_flag_);
  pool_->start(task);
}

void FrameCache::onFrameComplete(int step, int generation)
{
  // Runs on GUI thread (QueuedConnection from FrameTask). Confirm generation,
  // then notify listeners.
  {
    const QMutexLocker locker(&mutex_);
    if (generation != generation_) return; // stale
    auto it = frames_.find(step);
    if (it == frames_.end()) return;       // step was dropped from window
    const auto state = it->second->state.load(std::memory_order_acquire);
    if (state != FrameState::Ready) return; // failed or cancelled
  }
  emit frameReady(step);
}

} // namespace OpenScad::Animate
