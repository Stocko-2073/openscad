#include "gui/CGALWorker.h"

#include <QThread>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>

#ifdef ENABLE_MANIFOLD
#include "geometry/InterferenceCheck.h"
#include "geometry/manifold/ManifoldGeometry.h"
#endif

#include "core/Tree.h"
#include "core/progress.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySet.h"
#include "utils/exceptions.h"
#include "utils/printutils.h"

#ifdef ENABLE_PYTHON
#include "python/python_public.h"
#endif

CGALWorker::CGALWorker()
{
  this->tree = nullptr;
  this->thread = new QThread();
  if (this->thread->stackSize() < 1024 * 1024) this->thread->setStackSize(1024 * 1024);
  connect(this->thread, &QThread::started, this, &CGALWorker::work);
  moveToThread(this->thread);
}

CGALWorker::~CGALWorker()
{
  this->thread->quit();
  this->thread->wait();
  delete this->thread;
}

void CGALWorker::start(const Tree& tree, bool checkInterference,
                       std::shared_ptr<const pick::WholeGeometry> wholeGeometry)
{
#ifdef ENABLE_PYTHON
  python_unlock();
#endif
  this->tree = &tree;
  this->checkInterference = checkInterference;
  this->wholeGeometry = std::move(wholeGeometry);
  this->thread->start();
}

void CGALWorker::work()
{
  // this is a worker thread: we don't want any exceptions escaping and crashing the app.
#ifdef ENABLE_PYTHON
  python_lock();
#endif
  auto result = std::make_shared<RenderResult>();
  const auto renderStart = std::chrono::steady_clock::now();
  try {
    GeometryEvaluator evaluator(*this->tree);
    result->geometry = evaluator.evaluateGeometry(*this->tree->root(), true);
    result->digest = this->tree->digest(*this->tree->root());

#ifdef ENABLE_MANIFOLD
    if (auto manifold = std::dynamic_pointer_cast<const ManifoldGeometry>(result->geometry)) {
      // calling status forces evaluation
      // we should complete evaluation within the worker thread, so computation
      // will not block the GUI.
      if (manifold->getManifold().Status() != manifold::Manifold::Error::NoError)
        LOG(message_group::Error, "Rendering cancelled due to unknown manifold error.");
    }
#endif

    // After the result, so the # subtrees come from the cache. Failing here keeps the result.
    result->overlays = overlay::collect(*this->tree, *this->tree->root());
    // On the worker, so the picker need not evaluate hull() and the like on a click.
    result->wholeGeometry = std::make_shared<const pick::WholeGeometry>(
      pick::holdWholeGeometry(evaluator, *this->tree->root(), this->wholeGeometry.get()));
    result->geometryTime = std::chrono::steady_clock::now() - renderStart;

#ifdef ENABLE_MANIFOLD
    if (this->checkInterference) {
      const auto checkStart = std::chrono::steady_clock::now();
      interference::Options opts;
      opts.primitives = false;  // the console names the parts; the overlaps show where
      auto report = std::make_shared<interference::Report>(interference::run(*this->tree, opts));
      for (const auto& collision : report->collisions) {
        if (auto ps = collision.overlap->toPolySet(); ps && !ps->isEmpty()) {
          result->overlays.push_back({overlay::Kind::Interference, std::move(ps)});
        }
      }
      result->interference = std::move(report);
      result->interferenceTime = std::chrono::steady_clock::now() - checkStart;
    }
#endif
  } catch (const ProgressCancelException& e) {
    LOG("Rendering cancelled.");
  } catch (const HardWarningException& e) {
    LOG("Rendering cancelled on first warning.");
  } catch (const std::exception& e) {
    LOG(message_group::Error, "Rendering cancelled by exception %1$s", e.what());
  } catch (...) {
    LOG(message_group::Error, "Rendering cancelled by unknown exception.");
  }
  if (result->geometryTime == std::chrono::steady_clock::duration::zero()) {
    result->geometryTime = std::chrono::steady_clock::now() - renderStart;
  }
  this->wholeGeometry.reset();
#ifdef ENABLE_PYTHON
  python_unlock();
#endif
  emit done(result);
  thread->quit();
}
