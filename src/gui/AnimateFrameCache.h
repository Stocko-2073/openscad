#pragma once

#include <QMutex>
#include <QObject>
#include <QThreadPool>
#include <atomic>
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

// One frame as F6 renders it. No node tree: it can take several times the geometry's memory.
struct FrameResult
{
  std::shared_ptr<const Geometry> geometry;  // null when the frame has no geometry
  std::vector<overlay::Mesh> overlays;
};

enum class FrameState : int { Pending = 0, Ready = 1, Failed = 2, Cancelled = 3 };

// The result pointer is only published once state == Ready.
struct CachedFrame
{
  int step = 0;
  double t = 0.0;
  std::atomic<FrameState> state{FrameState::Pending};
  std::shared_ptr<FrameResult> result;
};

// A table serves one evaluation at a time; given back, it lets the next frame reuse the calls that
// do not depend on $t. Shared with the tasks, which give tables back even after the cache drops it.
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

  void setSeed(std::shared_ptr<const memo::MemoTable> seed);
  // Gives up the idle tables, for the caller to free; the seed stays.
  [[nodiscard]] std::vector<std::unique_ptr<memo::MemoTable>> trim();

private:
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<memo::MemoTable>> idle_;
  std::shared_ptr<const memo::MemoTable> seed_;
};

// Pre-fetches the geometry of animation frames on a worker pool, so the GUI thread only draws them.
class FrameCache : public QObject
{
  Q_OBJECT

public:
  explicit FrameCache(QObject *parent = nullptr);
  ~FrameCache() override;

  // Drops all frames; numSteps == 0 disables the cache. The frames' memo tables are forked from
  // `seed`, if given, until the next seed; it must not be in use during the call.
  void setSource(std::shared_ptr<SourceFile> sourceFile, std::string documentPath,
                 int numSteps, const Camera &camera, const memo::MemoTable *seed = nullptr);

  // Schedules [currentStep, currentStep+lookahead-1] modulo numSteps.
  void prefetchWindow(int currentStep, int lookahead);

  std::shared_ptr<CachedFrame> tryGet(int step);

  // The Ready frame of the highest step, if any.
  std::shared_ptr<CachedFrame> latestReady();

  // Drops everything, the memo tables too: no frames are wanted. Running tasks stop at their next
  // cancel check.
  void invalidateAll();

  void dropMemoTables();

  int workerCount() const;

signals:
  // Emitted on the GUI thread when a previously-pending frame becomes Ready.
  void frameReady(int step);

private:
  friend class FrameTask;

  // Queued by a finished FrameTask, so it runs on the GUI thread.
  Q_INVOKABLE void onFrameComplete(int step, int generation);

  void enqueueStep_unlocked(int step);
  // Made or dropped as the preference says.
  std::shared_ptr<MemoTablePool> memoTables_unlocked();
  // A table holds a tree, which takes a while to free.
  template <class T>
  void freeOnWorker(T garbage);

  mutable QMutex mutex_;

  // Bumped by setSource() and invalidateAll(); completions from older generations are dropped.
  int generation_ = 0;

  std::shared_ptr<SourceFile> source_file_;
  std::string document_path_;
  int num_steps_ = 0;

  // Camera snapshot at setSource(). Not refreshed on user orbit.
  std::shared_ptr<Camera> camera_;

  std::shared_ptr<std::atomic<bool>> cancel_flag_;

  std::map<int, std::shared_ptr<CachedFrame>> frames_;

  // Kept across sources: after an edit, the frames reuse what it did not change.
  std::shared_ptr<MemoTablePool> memo_tables_;

  std::unique_ptr<QThreadPool> pool_;
};

} // namespace OpenScad::Animate
