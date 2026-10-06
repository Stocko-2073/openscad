#include "gui/AnimateFrameTask.h"

#include <QMetaObject>
#include <QPointer>
#include <Qt>
#include <atomic>
#include <exception>
#include <memory>
#include <utility>

#include "core/BuiltinContext.h"
#include "core/Context.h"
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
                     std::shared_ptr<std::atomic<bool>> cancelFlag)
  : owner_(std::move(owner)),
    generation_(generation),
    frame_(std::move(frame)),
    source_file_(std::move(sourceFile)),
    document_path_(std::move(documentPath)),
    camera_(std::move(camera)),
    cancel_flag_(std::move(cancelFlag))
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
  // buffers (lastmessages/print_messages_stack/printedDeprecations). Workers
  // are speculative anyway — if a frame fails, the user will see the real
  // error when they scrub to that t value and the synchronous render runs on
  // the GUI thread.
  PrintSuppressGuard print_suppress;

  if (is_cancelled(cancel_flag_)) {
    frame_->state.store(FrameState::Cancelled, std::memory_order_release);
    return;
  }

#ifdef ENABLE_PYTHON
  python_lock();
#endif

  auto result = std::make_shared<FrameResult>();
  bool ok = false;

  try {
    EvaluationSession session{document_path_};
    ContextHandle<BuiltinContext> builtin_context{Context::create<BuiltinContext>(&session)};

    const RenderVariables r = {
      .preview = false,
      .time = frame_->t,
      .camera = camera_,
    };
    r.applyToContext(builtin_context);

    if (is_cancelled(cancel_flag_)) throw ProgressCancelException();

    std::shared_ptr<const FileContext> file_context;
    auto absolute_root = source_file_->instantiate(*builtin_context, &file_context);

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

    result->root_node = root_node;
    result->tree = tree;
    // Don't retain file_context — its ContextMemoryManager (owned by the
    // session) is destructed below and asserts that all managed contexts have
    // been released. The GUI's render path doesn't retain it either; geometry
    // owns its meshes outright (no raw pointers back into the FileContext).

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
