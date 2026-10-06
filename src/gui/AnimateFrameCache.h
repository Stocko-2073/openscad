#pragma once

#include <QMutex>
#include <QObject>
#include <QThreadPool>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include "core/ModifierOverlays.h"

class SourceFile;
class AbstractNode;
class Geometry;
class Tree;
class Camera;

namespace OpenScad::Animate {

// One frame rendered as F6 renders, ready for the GLView to draw.
struct FrameResult
{
  std::shared_ptr<const Geometry> geometry;  // null when the frame has no geometry
  std::vector<overlay::Mesh> overlays;
  // Kept alive with the geometry, which may refer to its nodes.
  std::shared_ptr<AbstractNode> root_node;
  std::shared_ptr<Tree> tree;
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
  // frames. Pass numSteps==0 to disable.
  void setSource(std::shared_ptr<SourceFile> sourceFile, std::string documentPath,
                 int numSteps, const Camera &camera);

  // Schedule pre-computation for [currentStep, currentStep+lookahead-1] modulo numSteps,
  // cancelling tasks for steps outside that window. Safe to call from the GUI thread.
  void prefetchWindow(int currentStep, int lookahead);

  // Non-blocking lookup. Returns nullptr if there's no entry for that step.
  std::shared_ptr<CachedFrame> tryGet(int step);

  // Non-blocking lookup for the most-recently-ready frame in the cache, if
  // any. Used by the tick fallback when the exact requested step hasn't
  // finished yet — better to show a slightly-stale frame than nothing.
  std::shared_ptr<CachedFrame> latestReady();

  // Drop everything (e.g. on script edit, FPS change). Tasks already running
  // will check their cancel flag and exit early.
  void invalidateAll();

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

  std::unique_ptr<QThreadPool> pool_;
};

} // namespace OpenScad::Animate
