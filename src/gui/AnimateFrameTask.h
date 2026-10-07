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

// Pre-computes one animation frame on a worker thread, as F6 renders it. Touches no Qt GUI / GL
// state; the SourceFile it shares is immutable once parsed.
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
