#include "gui/AnimateFrameTask.h"

#include <QMetaObject>
#include <QPointer>
#include <Qt>
#include <atomic>
#include <exception>
#include <memory>
#include <utility>

#include "core/BuiltinContext.h"
#include "core/CSGNode.h"
#include "core/CSGTreeEvaluator.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/RenderVariables.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/Tree.h"
#include "core/node.h"
#include "core/progress.h"
#include "geometry/GeometryEvaluator.h"
#include "glview/preview/CSGTreeNormalizer.h"
#include "gui/AnimateFrameCache.h"
#include "utils/exceptions.h"
#include "utils/printutils.h"

#ifdef ENABLE_PYTHON
#include "python/python_public.h"
#endif

namespace OpenScad::Animate {

namespace {

// Matches the limit used in MainWindow::compileCSG. Hard-coded here so the
// worker doesn't depend on Qt preferences (those aren't thread-safe to read).
// The GUI side passes us a pre-computed limit if it wants something else.
constexpr size_t kDefaultOpenCSGLimit = 5000;

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
                     bool isPreview,
                     std::shared_ptr<std::atomic<bool>> cancelFlag)
  : owner_(std::move(owner)),
    generation_(generation),
    frame_(std::move(frame)),
    source_file_(std::move(sourceFile)),
    document_path_(std::move(documentPath)),
    camera_(std::move(camera)),
    is_preview_(isPreview),
    cancel_flag_(std::move(cancelFlag))
{
  setAutoDelete(true);
}

void FrameTask::run()
{
  // Worker thread: never touch GUI / GL state. We only use the immutable
  // SourceFile and produce CSG products.
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
      .preview = is_preview_,
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
    CSGTreeEvaluator csgrenderer(*tree, &geomevaluator);

    auto csg_root = csgrenderer.buildCSGTree(*root_node);

    if (is_cancelled(cancel_flag_)) throw ProgressCancelException();

    CSGTreeNormalizer normalizer(2 * kDefaultOpenCSGLimit);

    if (csg_root) {
      auto normalized = normalizer.normalize(csg_root);
      if (normalized) {
        result->root_products = std::make_shared<CSGProducts>();
        result->root_products->import(normalized);
      }
    }

    const auto& highlight_terms = csgrenderer.getHighlightNodes();
    if (!highlight_terms.empty()) {
      result->highlights_products = std::make_shared<CSGProducts>();
      for (const auto& term : highlight_terms) {
        auto nterm = normalizer.normalize(term);
        if (nterm) result->highlights_products->import(nterm);
      }
    }

    const auto& background_terms = csgrenderer.getBackgroundNodes();
    if (!background_terms.empty()) {
      result->background_products = std::make_shared<CSGProducts>();
      for (const auto& term : background_terms) {
        auto nterm = normalizer.normalize(term);
        if (nterm) result->background_products->import(nterm);
      }
    }

    result->root_node = root_node;
    result->tree = tree;
    // Don't retain file_context — its ContextMemoryManager (owned by the
    // session) is destructed below and asserts that all managed contexts have
    // been released. The GUI's compileCSG path doesn't retain it either; the
    // normalized CSGProducts only reference PolySets and transforms, which
    // are owned outright (no raw pointers back into the FileContext).

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
