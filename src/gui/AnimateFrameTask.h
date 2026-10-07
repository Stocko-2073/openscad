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
class MemoTablePool;
struct CachedFrame;

// Pre-computes one animation frame's geometry and overlays on a worker thread, as
// F6 renders them. Touches no Qt GUI / GL state. Reads the shared SourceFile
// (immutable post-parse) and the per-frame inputs from the FrameCache. Evaluates
// with a memo table from `memoTables`, unless that is null.
class FrameTask : public QRunnable
{
public:
  FrameTask(QPointer<FrameCache> owner,
            int generation,
            std::shared_ptr<CachedFrame> frame,
            std::shared_ptr<SourceFile> sourceFile,
            std::string documentPath,
            Camera camera,
            std::shared_ptr<std::atomic<bool>> cancelFlag,
            std::shared_ptr<MemoTablePool> memoTables = nullptr);

  void run() override;

private:
  QPointer<FrameCache> owner_;
  int generation_;
  std::shared_ptr<CachedFrame> frame_;
  std::shared_ptr<SourceFile> source_file_;
  std::string document_path_;
  Camera camera_;
  std::shared_ptr<std::atomic<bool>> cancel_flag_;
  std::shared_ptr<MemoTablePool> memo_tables_;
};

} // namespace OpenScad::Animate
