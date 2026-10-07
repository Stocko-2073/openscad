#pragma once

#include <QMutex>
#include <QObject>
#include <QThreadPool>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "core/ModifierOverlays.h"

class SourceFile;
class Geometry;
class Camera;
namespace memo {
class MemoTable;
}

namespace OpenScad::Animate {

// One frame rendered as F6 renders, ready for the GLView to draw. Without the node tree, which can
// take several times the memory of the geometry (a GeometryList holds the nodes it refers to).
struct FrameResult
{
  std::shared_ptr<const Geometry> geometry;  // null when the frame has no geometry
  std::vector<overlay::Mesh> overlays;
  // The module calls of the user's files that the evaluation made, and how many of them a memo
  // table supplied (memo::Stats::userCalls and userCallsReused); zero without a table.
  size_t userCalls = 0;
  size_t userCallsReused = 0;
};

enum class FrameState : int { Pending = 0, Ready = 1, Failed = 2, Cancelled = 3 };

// Per-step entry in the cache. State transitions are atomic; the result pointer
// is only published once state == Ready.
struct CachedFrame
{
  int step = 0;
  double t = 0.0;
  std::atomic<FrameState> state{FrameState::Pending};
  std::shared_ptr<FrameResult> result;
};

// The frame tasks' memo tables (core/EvalMemo.h). A table serves one evaluation at a time, so a
// task takes one for its frame and gives it back once the frame is evaluated: the next frame
// evaluated with it reuses the module calls that do not depend on $t. Each such table holds a
// tree of its own, so the cache trims them once no frame is pending, keeping only the seed they
// are forked from. Shared with the tasks, which give their tables back to it even after the cache
// dropped it.
class MemoTablePool
{
public:
  MemoTablePool();
  ~MemoTablePool();
  MemoTablePool(const MemoTablePool&) = delete;
  MemoTablePool& operator=(const MemoTablePool&) = delete;

  // An idle table, else a fork of the seed, else an empty table.
  std::unique_ptr<memo::MemoTable> take();
  void give(std::unique_ptr<memo::MemoTable> table);

  // The tables made from now on start as forks of `seed`, itself a fork of the document's table,
  // whose nodes it shares: it costs little beside them, and nothing changes it.
  void setSeed(std::shared_ptr<const memo::MemoTable> seed);
  // Gives up the idle tables, for the caller to free; the seed stays.
  [[nodiscard]] std::vector<std::unique_ptr<memo::MemoTable>> trim();
  [[nodiscard]] size_t idle() const;

private:
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<memo::MemoTable>> idle_;
  std::shared_ptr<const memo::MemoTable> seed_;
};

// Pre-fetches the geometry of animation frames on a worker pool so the GUI
// thread only has to do the GL draw when a frame is due. All public methods
// are safe to call from the GUI thread; the cache internally serialises access
// to its frame map and signals on the GUI thread when a frame is ready.
//
// Caching policy: Ready frames are kept indefinitely (bounded by num_steps_),
// so on a stable script the second loop iteration plays at full FPS even when
// per-frame compute time exceeds 1/fps. setSource()/invalidateAll() drop the
// cached frames; cycle wrap does not.
class FrameCache : public QObject
{
  Q_OBJECT

public:
  explicit FrameCache(QObject *parent = nullptr);
  ~FrameCache() override;

  // Reset the cache to use the given source. invalidates all pending/ready
  // frames. Pass numSteps==0 to disable. The frames' memo tables are forked from `seed`, the
  // document's, if given, until the next seed: it must not be in use meanwhile.
  void setSource(std::shared_ptr<SourceFile> sourceFile, std::string documentPath,
                 int numSteps, const Camera &camera, const memo::MemoTable *seed = nullptr);

  // Schedule pre-computation for [currentStep, currentStep+lookahead-1] modulo numSteps,
  // cancelling tasks for steps outside that window. Safe to call from the GUI thread.
  void prefetchWindow(int currentStep, int lookahead);

  // Non-blocking lookup. Returns nullptr if there's no entry for that step.
  std::shared_ptr<CachedFrame> tryGet(int step);

  // Non-blocking lookup for the most-recently-ready frame in the cache, if
  // any. Used by the tick fallback when the exact requested step hasn't
  // finished yet — better to show a slightly-stale frame than nothing.
  std::shared_ptr<CachedFrame> latestReady();

  // Drop everything, the memo tables too: no frames are wanted. Tasks already running will check
  // their cancel flag and exit early.
  void invalidateAll();

  // Drops the frame tasks' memo tables and their seed, as Flush Caches and turning off the reuse
  // of module results drop the documents'. The next frames evaluate with new ones, if reuse is on.
  void dropMemoTables();
  // The memo tables the frame tasks are done with, kept until no frame is pending.
  [[nodiscard]] size_t idleMemoTables() const;

  // Number of worker threads, defaults to hardware_concurrency capped at 16.
  // Returns the actual thread count in use.
  int workerCount() const;

signals:
  // Emitted on the GUI thread when a previously-pending frame becomes Ready.
  void frameReady(int step);

private:
  friend class FrameTask;

  // Called from worker on completion via QMetaObject::invokeMethod.
  Q_INVOKABLE void onFrameComplete(int step, int generation);

  void enqueueStep_unlocked(int step);
  // The frame tasks' memo tables, made or dropped as the preference says.
  std::shared_ptr<MemoTablePool> memoTables_unlocked();
  // Frees what `garbage` holds on a worker: a table holds a tree, which takes a while to free.
  template <class T>
  void freeOnWorker(T garbage);

  mutable QMutex mutex_;

  // Generation counter — incremented by every invalidateAll/setSource. Tasks
  // carry the generation they were submitted under; stale completions are dropped.
  int generation_ = 0;

  std::shared_ptr<SourceFile> source_file_;
  std::string document_path_;
  int num_steps_ = 0;

  // Camera snapshot at enqueue time. Not refreshed on user orbit; documented limitation.
  std::shared_ptr<Camera> camera_;

  // Shared cancel flag for the current generation; set when invalidating.
  std::shared_ptr<std::atomic<bool>> cancel_flag_;

  std::map<int, std::shared_ptr<CachedFrame>> frames_;

  // Kept across sources: after an edit, the frames reuse what it did not change. Made by the
  // first frame enqueued, or source set, while the reuse of module results is on (Preferences).
  std::shared_ptr<MemoTablePool> memo_tables_;

  std::unique_ptr<QThreadPool> pool_;
};

} // namespace OpenScad::Animate
