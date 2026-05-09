#pragma once

#include <QPointer>
#include <QRunnable>
#include <atomic>
#include <memory>
#include <string>

#include "glview/Camera.h"

class SourceFile;

namespace OpenScad::Animate {

class FrameCache;
struct CachedFrame;

// Pre-computes one animation frame's CSG products on a worker thread. Touches
// no Qt GUI / GL state. Reads the shared SourceFile (immutable post-parse) and
// the per-frame inputs from the FrameCache.
class FrameTask : public QRunnable
{
public:
  FrameTask(QPointer<FrameCache> owner,
            int generation,
            std::shared_ptr<CachedFrame> frame,
            std::shared_ptr<SourceFile> sourceFile,
            std::string documentPath,
            Camera camera,
            bool isPreview,
            std::shared_ptr<std::atomic<bool>> cancelFlag);

  void run() override;

private:
  QPointer<FrameCache> owner_;
  int generation_;
  std::shared_ptr<CachedFrame> frame_;
  std::shared_ptr<SourceFile> source_file_;
  std::string document_path_;
  Camera camera_;
  bool is_preview_;
  std::shared_ptr<std::atomic<bool>> cancel_flag_;
};

} // namespace OpenScad::Animate
