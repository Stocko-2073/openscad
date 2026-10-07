#include "gui/AnimateFrameTask.h"

#include <QMetaObject>
#include <QPointer>
#include <Qt>
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/BuiltinContext.h"
#include "core/Context.h"
#include "core/EvalMemo.h"
#include "core/EvaluationSession.h"
#include "core/ModifierOverlays.h"
#include "core/RenderVariables.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/Tree.h"
#include "core/node.h"
#include "core/progress.h"
#include "geometry/GeometryEvaluator.h"
#ifdef ENABLE_MANIFOLD
#include "geometry/manifold/ManifoldGeometry.h"
#endif
#include "gui/AnimateFrameCache.h"
#include "utils/exceptions.h"
#include "utils/printutils.h"
#include "utils/scope_guard.hpp"

#ifdef ENABLE_PYTHON
#include "python/python_public.h"
#endif

namespace OpenScad::Animate {

namespace {

bool is_cancelled(const std::shared_ptr<std::atomic<bool>>& flag)
{
  return flag && flag->load(std::memory_order_relaxed);
}

} // namespace

FrameTask::FrameTask(QPointer<FrameCache> owner,
                     int generation,
                     std::shared_ptr<CachedFrame> frame,
                     std::shared_ptr<SourceFile> sourceFile,
                     std::string documentPath,
                     Camera camera,
                     std::shared_ptr<std::atomic<bool>> cancelFlag,
                     std::shared_ptr<MemoTablePool> memoTables)
  : owner_(std::move(owner)),
    generation_(generation),
    frame_(std::move(frame)),
    source_file_(std::move(sourceFile)),
    document_path_(std::move(documentPath)),
    camera_(std::move(camera)),
    cancel_flag_(std::move(cancelFlag)),
    memo_tables_(std::move(memoTables))
{
  setAutoDelete(true);
}

void FrameTask::run()
{
  // Worker thread: never touch GUI / GL state. We only use the immutable
  // SourceFile and produce geometry.
  //
  // Silence PRINT/LOG entirely on this thread. The output handler reaches into
  // Qt widgets (main-thread only) and the print machinery has shared global
  // buffers (lastmessages/print_messages_stack). Workers are speculative
  // anyway — if a frame fails, the user will see the real error when they
  // scrub to that t value and the synchronous render runs on the GUI thread.
  PrintSuppressGuard print_suppress;
  // Likewise progress, which goes to the GUI's F6 render when one is running.
  ProgressSuppressGuard progress_suppress;

  if (is_cancelled(cancel_flag_)) {
    frame_->state.store(FrameState::Cancelled, std::memory_order_release);
    return;
  }

#ifdef ENABLE_PYTHON
  python_lock();
#endif

  auto result = std::make_shared<FrameResult>();
  bool ok = false;
  // This frame's memo table (core/EvalMemo.h), from the pool the cache shares with its tasks: a
  // table serves one evaluation at a time, and the GUI thread's renders use the document's.
  std::unique_ptr<memo::MemoTable> memo_table;

  try {
    EvaluationSession session{document_path_};
    ContextHandle<BuiltinContext> builtin_context{Context::create<BuiltinContext>(&session)};

    const RenderVariables r = {
      .time = frame_->t,
      .camera = camera_,
    };
    r.applyToContext(builtin_context);

    if (is_cancelled(cancel_flag_)) throw ProgressCancelException();

    AbstractNode::resetIndexCounter();  // numbers this frame's nodes from 1, as the GUI does its trees
    printedDeprecations.clear();        // and records its deprecations as a render does
    std::shared_ptr<const FileContext> file_context;
    std::shared_ptr<AbstractNode> absolute_root;
    std::vector<std::shared_ptr<AbstractNode>> replaced;
    if (memo_tables_) memo_table = memo_tables_->take();
    {
      // Detached and destroyed before the session, whose values it holds, also when the
      // evaluation throws; the table stays consistent then.
      std::optional<memo::EvalMemoSession> memo;
      if (memo_table) {
        memo.emplace(*memo_table, *source_file_);
        session.setMemo(&*memo);
      }
      const auto detach = sg::make_scope_guard([&session]() noexcept { session.setMemo(nullptr); });
      absolute_root = source_file_->instantiate(*builtin_context, &file_context);
      if (memo) {
        result->userCalls = memo->stats().userCalls;
        result->userCallsReused = memo->stats().userCallsReused;
        replaced = memo->takeReplaced();
      }
    }
    if (memo_table) {
      // What this frame did not use was evaluated at other times, and only the calls that do not
      // depend on $t carry over to the next frame. Given back now, the table can serve the next
      // frame while this one's geometry is made.
      memo_table->evict(0);
      memo_tables_->give(std::move(memo_table));
    }
    replaced.clear();

    if (!absolute_root) {
      throw EvaluationException("instantiation produced no root node");
    }

    // Honour the root modifier (!) just like MainWindow::instantiateRoot does.
    std::shared_ptr<AbstractNode> root_node = find_root_tag(absolute_root);
    if (!root_node) root_node = absolute_root;

    auto tree = std::make_shared<Tree>(root_node, document_path_);

    if (is_cancelled(cancel_flag_)) throw ProgressCancelException();

    GeometryEvaluator geomevaluator(*tree);
    result->geometry = geomevaluator.evaluateGeometry(*root_node, true);
#ifdef ENABLE_MANIFOLD
    // Manifold evaluates lazily; finish here rather than on the GUI thread.
    if (auto manifold = std::dynamic_pointer_cast<const ManifoldGeometry>(result->geometry)) {
      (void)manifold->getManifold().Status();
    }
#endif

    if (is_cancelled(cancel_flag_)) throw ProgressCancelException();

    result->overlays = overlay::collect(*tree, *root_node);

    // Don't retain file_context — its ContextMemoryManager (owned by the
    // session) is destructed below and asserts that all managed contexts have
    // been released. The GUI's render path doesn't retain it either; geometry
    // owns its meshes outright (no raw pointers back into the FileContext).
    // Nor the tree, which the frame no longer needs, and which is freed here
    // rather than when the GUI thread drops the frame.

    ok = true;
  } catch (const ProgressCancelException&) {
    // Cancelled — leave ok=false, frame goes to Cancelled below.
  } catch (const HardWarningException&) {
    LOG(message_group::Warning, "Animation pre-fetch frame %1$d cancelled on warning.", frame_->step);
  } catch (const std::exception& e) {
    LOG(message_group::Warning, "Animation pre-fetch frame %1$d failed: %2$s", frame_->step, e.what());
  } catch (...) {
    LOG(message_group::Warning, "Animation pre-fetch frame %1$d failed (unknown exception).",
        frame_->step);
  }

  // A table whose evaluation threw is consistent, and keeps what it held.
  if (memo_table) memo_tables_->give(std::move(memo_table));

#ifdef ENABLE_PYTHON
  python_unlock();
#endif

  if (is_cancelled(cancel_flag_)) {
    frame_->state.store(FrameState::Cancelled, std::memory_order_release);
    return;
  }

  if (ok) {
    frame_->result = std::move(result);
    frame_->state.store(FrameState::Ready, std::memory_order_release);
  } else {
    frame_->state.store(FrameState::Failed, std::memory_order_release);
  }

  if (owner_) {
    const int step = frame_->step;
    const int gen = generation_;
    QPointer<FrameCache> owner = owner_;
    QMetaObject::invokeMethod(
      owner.data(), "onFrameComplete", Qt::QueuedConnection,
      Q_ARG(int, step), Q_ARG(int, gen));
  }
}

} // namespace OpenScad::Animate
