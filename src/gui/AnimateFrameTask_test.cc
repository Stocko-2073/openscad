#include "gui/AnimateFrameTask.h"

#include <QPointer>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/Context.h"
#include "core/EvalMemo.h"
#include "core/EvaluationSession.h"
#include "core/ModifierOverlays.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/node.h"
#include "core/parsersettings.h"
#include "core/progress.h"
#include "geometry/Geometry.h"
#include "geometry/GeometryCache.h"
#include "geometry/PolySet.h"
#include "glview/Camera.h"
#include "gui/AnimateFrameCache.h"
#include "openscad.h"
#include "platform/PlatformUtils.h"
#ifdef ENABLE_CGAL
#include "geometry/cgal/CGALCache.h"
#endif

namespace fs = std::filesystem;
using namespace OpenScad::Animate;

namespace {

// Meshes from Manifold need the color schemes, which live at the top of the source tree.
void setUpOnce()
{
  static const bool done = [] {
    const fs::path source = fs::path(__FILE__).parent_path();  // <root>/src/gui
    PlatformUtils::registerApplicationPath(source.parent_path().generic_string());
    Builtins::instance()->initialize();
    return true;
  }();
  (void)done;
}

std::shared_ptr<SourceFile> parseText(const std::string& text)
{
  setUpOnce();
  const std::string mainFile = (fs::temp_directory_path() / "frame-test.scad").generic_string();
  SourceFile *file = nullptr;
  REQUIRE(parse(file, text + "\n\x03\n", mainFile, mainFile, false));
  return std::shared_ptr<SourceFile>(file);
}

// Runs the frame task for time t on this thread, as a pool worker would, with memo tables from
// `tables` if given.
std::shared_ptr<CachedFrame> renderFrame(const std::shared_ptr<SourceFile>& file, double t,
                                         const std::shared_ptr<MemoTablePool>& tables = nullptr)
{
  auto frame = std::make_shared<CachedFrame>();
  frame->step = 1;
  frame->t = t;
  FrameTask task(QPointer<FrameCache>(), 0, frame, file, fs::temp_directory_path().generic_string(),
                 Camera(), std::make_shared<std::atomic<bool>>(false), tables);
  task.run();
  return frame;
}

BoundingBox boxOf(const std::shared_ptr<CachedFrame>& frame)
{
  REQUIRE(frame->state.load() == FrameState::Ready);
  REQUIRE(frame->result);
  REQUIRE(frame->result->geometry);
  return frame->result->geometry->getBoundingBox();
}

// The tables a pool holds idle, which it gives back to it.
std::vector<std::unique_ptr<memo::MemoTable>> idleTables(MemoTablePool& pool, size_t expected)
{
  std::vector<std::unique_ptr<memo::MemoTable>> tables;
  for (size_t i = 0; i < expected; ++i) tables.push_back(pool.take());
  return tables;
}

const char *const kStillAndSpinning =
  "module still() translate([3, 0, 0]) cube(1);\n"
  "module spinning() rotate($t * 360) translate([1, 0, 0]) cube(0.5);\n"
  "still();\nspinning();";

}  // namespace

TEST_CASE("An animation frame is rendered at its time, as F6 renders", "[animate]")
{
  const auto file = parseText(
    "translate([10 * $t, 0, 0]) cube(1);\n"
    "if ($preview) translate([0, 0, 50]) cube(1);\n"
    "%translate([0, 5, 0]) cube(1);");
  const auto frame = renderFrame(file, 0.5);
  REQUIRE(frame->state.load() == FrameState::Ready);
  REQUIRE(frame->result);
  REQUIRE(frame->result->geometry);

  // $preview is false, and the % cube is not part of the geometry.
  const BoundingBox box = frame->result->geometry->getBoundingBox();
  CHECK(box.min().x() == Catch::Approx(5));
  CHECK(box.max().x() == Catch::Approx(6));
  CHECK(box.max().y() == Catch::Approx(1));
  CHECK(box.max().z() == Catch::Approx(1));

  REQUIRE(frame->result->overlays.size() == 1);
  CHECK(frame->result->overlays[0].kind == overlay::Kind::Background);
  CHECK(frame->result->overlays[0].polyset->getBoundingBox().min().y() == Catch::Approx(5));
}

TEST_CASE("An animation frame without geometry is still ready", "[animate]")
{
  const auto frame = renderFrame(parseText("if ($t > 0.75) cube(1);"), 0.25);
  REQUIRE(frame->state.load() == FrameState::Ready);
  REQUIRE(frame->result);
  CHECK((!frame->result->geometry || frame->result->geometry->isEmpty()));
}

TEST_CASE("A frame takes no part in the progress of the render beside it", "[animate]")
{
  // The GUI's F6 render reports its progress through a function that throws once its Cancel is
  // pressed. A frame evaluated meanwhile, on a worker, neither reports to it nor is cancelled.
  static int reports;
  reports = 0;
  progress_report_f = [](const std::shared_ptr<const AbstractNode>&, void *, int) {
    ++reports;
    throw ProgressCancelException();
  };
  const auto frame = renderFrame(parseText("rotate($t * 90) cube(1);"), 0.5);
  progress_report_fin();

  CHECK(reports == 0);
  REQUIRE(frame->state.load() == FrameState::Ready);
  REQUIRE(frame->result);
  CHECK(frame->result->geometry);
}

TEST_CASE("Frames rendered with a memo table reuse what does not depend on $t", "[animate]")
{
  const auto file = parseText(kStillAndSpinning);
  const auto tables = std::make_shared<MemoTablePool>();

  const auto first = renderFrame(file, 0.25, tables);
  CHECK(boxOf(first).max().x() == Catch::Approx(4));
  CHECK(first->result->userCalls == 2);
  CHECK(first->result->userCallsReused == 0);

  // The table came back to the pool, and serves the next frame: still() did not read $t.
  const auto second = renderFrame(file, 0.5, tables);
  CHECK(second->result->userCalls == 2);
  CHECK(second->result->userCallsReused == 1);
  const BoundingBox fresh = boxOf(renderFrame(file, 0.5));
  CHECK(boxOf(second).min().x() == Catch::Approx(fresh.min().x()));
  CHECK(boxOf(second).max().x() == Catch::Approx(fresh.max().x()));
  CHECK(boxOf(second).min().y() == Catch::Approx(fresh.min().y()));

  // One table served both, and kept only what the last frame used.
  auto table = tables->take();
  CHECK(table->generation() == 2);
  CHECK(table->size() == 2);
}

TEST_CASE("Frames start from a copy of the document's table", "[animate]")
{
  // As the GUI's render left the document's table, at the time on screen.
  const auto file = parseText(kStillAndSpinning);
  memo::MemoTable document;
  {
    const PrintSuppressGuard quiet;
    EvaluationSession session{fs::temp_directory_path().generic_string()};
    ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(&session)};
    AbstractNode::resetIndexCounter();
    std::optional<memo::EvalMemoSession> memo;
    memo.emplace(document, *file);
    session.setMemo(&*memo);
    std::shared_ptr<const FileContext> file_context;
    REQUIRE(file->instantiate(*builtin, &file_context));
    session.setMemo(nullptr);
  }
  const size_t entries = document.size();
  const auto tables = std::make_shared<MemoTablePool>();
  tables->setSeed(document.fork());

  // The first frame reuses still() from a fork of the seed, and gives that back.
  const auto first = renderFrame(file, 0.5, tables);
  CHECK(first->result->userCallsReused == 1);
  CHECK(tables->idle() == 1);

  // Trimmed of it, the pool forks the seed again for the next frame.
  CHECK(tables->trim().size() == 1);
  CHECK(tables->idle() == 0);
  const auto second = renderFrame(file, 0.75, tables);
  CHECK(second->result->userCallsReused == 1);
  CHECK(tables->take()->generation() == document.generation() + 1);
  CHECK(document.size() == entries);
}

TEST_CASE("A frame whose evaluation stops early gives its table back", "[animate]")
{
  const auto file = parseText(std::string(kStillAndSpinning) + "\nassert($t < 0.75);\nspinning();");
  const auto tables = std::make_shared<MemoTablePool>();

  // As F6 does, the frame shows what was made before the assertion failed.
  const auto stopped = renderFrame(file, 0.8, tables);
  REQUIRE(stopped->state.load() == FrameState::Ready);
  CHECK(stopped->result->userCalls == 2);

  // The next frame reuses still() from it, evaluates spinning() at its own time, and then reuses
  // that for the second call.
  const auto next = renderFrame(file, 0.25, tables);
  REQUIRE(next->state.load() == FrameState::Ready);
  CHECK(next->result->userCalls == 3);
  CHECK(next->result->userCallsReused == 2);
  CHECK(tables->take()->generation() == 2);
}

TEST_CASE("Frames rendered at once each take a table of their own", "[animate]")
{
  const auto file = parseText(kStillAndSpinning);
  const auto tables = std::make_shared<MemoTablePool>();
  constexpr int kFrames = 16;
  constexpr int kThreads = 4;
  std::vector<std::shared_ptr<CachedFrame>> frames(kFrames);
  std::vector<std::thread> threads;
  for (int k = 0; k < kThreads; ++k) {
    threads.emplace_back([&, k]() {
      for (int i = k; i < kFrames; i += kThreads) frames[i] = renderFrame(file, double(i) / kFrames, tables);
    });
  }
  for (auto& thread : threads) thread.join();

  for (int i = 0; i < kFrames; ++i) {
    INFO("frame " << i);
    const BoundingBox box = boxOf(frames[i]);
    const BoundingBox fresh = boxOf(renderFrame(file, double(i) / kFrames));
    CHECK(box.min().x() == Catch::Approx(fresh.min().x()));
    CHECK(box.min().y() == Catch::Approx(fresh.min().y()));
    CHECK(box.max().y() == Catch::Approx(fresh.max().y()));
  }
  // No more tables than frames evaluated at once, and every one back in the pool.
  const auto idle = idleTables(*tables, kThreads);
  uint64_t evaluations = 0;
  for (const auto& table : idle) evaluations += table->generation();
  CHECK(evaluations <= kFrames);
  CHECK(tables->take()->generation() == 0);
}

TEST_CASE("A frame's deprecations leave the GUI thread its own to print", "[animate]")
{
  // rotate_extrude() with an odd $fn and no angle warns that it is deprecated, printed once. A
  // frame rendered on a worker after a render began prints nothing, and must not count as having
  // printed it for that render.
  const auto file = parseText("rotate_extrude($fn = 3) translate([2, 0]) square(1);");
  resetSuppressedMessages();
  std::thread worker([&]() { renderFrame(file, 0); });
  worker.join();

  std::vector<Message> messages;
  g_message_capture.push_back(&messages);
  {
    const PrintSuppressGuard quiet;
    EvaluationSession session{fs::temp_directory_path().generic_string()};
    ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(&session)};
    std::shared_ptr<const FileContext> file_context;
    REQUIRE(file->instantiate(*builtin, &file_context));
  }
  g_message_capture.pop_back();
  REQUIRE(messages.size() == 1);
  CHECK(messages[0].group == message_group::Deprecated);
  CHECK(!messages[0].repeat);
}

/*
 * Not a test: times the first pass over an animation's frames as the Animate dock's workers make
 * it, every frame at once on as many threads, after an F6 render at t = 0 as playing follows: with
 * no memo tables (reuse turned off), with tables made empty, and with tables made from the
 * document's. OPENSCAD_ANIMATE_BENCH names the design, OPENSCAD_ANIMATE_BENCH_FRAMES the number of
 * frames (20 by default), OPENSCAD_ANIMATE_BENCH_THREADS the threads (as FrameCache: the cores, at
 * most 16), and OPENSCAD_ANIMATE_BENCH_ONLY, if set to none, empty or seeded, the one pass to run.
 */
TEST_CASE("Time the first pass over an animation's frames", "[.animate-bench]")
{
  const char *design = std::getenv("OPENSCAD_ANIMATE_BENCH");
  if (!design) SKIP("Set OPENSCAD_ANIMATE_BENCH to a design.");
  const int frames = std::getenv("OPENSCAD_ANIMATE_BENCH_FRAMES")
                       ? std::atoi(std::getenv("OPENSCAD_ANIMATE_BENCH_FRAMES"))
                       : 20;
  const int threads = std::getenv("OPENSCAD_ANIMATE_BENCH_THREADS")
                        ? std::atoi(std::getenv("OPENSCAD_ANIMATE_BENCH_THREADS"))
                        : std::min<int>(std::max(1u, std::thread::hardware_concurrency()), 16);
  setUpOnce();
  parser_init();
  const fs::path path = fs::absolute(design);
  std::ifstream in(path);
  REQUIRE(in.is_open());
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const fs::path original = fs::current_path();
  fs::current_path(path.parent_path());
  SourceFile *parsed = nullptr;
  REQUIRE(parse(parsed, text + "\n\x03\n", path.generic_string(), path.generic_string(), false));
  std::shared_ptr<SourceFile> file(parsed);
  file->handleDependencies();
  const auto seconds = [](std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
  };

  // As large as the GUI's preferences allow; emptied before each pass.
  GeometryCache::instance()->setMaxSizeMB(5000);
#ifdef ENABLE_CGAL
  CGALCache::instance()->setMaxSizeMB(5000);
#endif
  enum class Tables { None, Empty, Seeded };
  std::vector<Tables> kinds{Tables::None, Tables::Empty, Tables::Seeded};
  if (const char *only = std::getenv("OPENSCAD_ANIMATE_BENCH_ONLY")) {
    const std::string name(only);
    kinds = {name == "none" ? Tables::None : name == "empty" ? Tables::Empty : Tables::Seeded};
  }
  for (const Tables kind : kinds) {
    GeometryCache::instance()->clear();
#ifdef ENABLE_CGAL
    CGALCache::instance()->clear();
#endif
    // The F6 render that the frames follow, with the document's table if reuse is on.
    auto document = std::make_shared<MemoTablePool>();
    if (kind != Tables::None) document->give(std::make_unique<memo::MemoTable>());
    const auto f6 = std::chrono::steady_clock::now();
    REQUIRE(renderFrame(file, 0, kind != Tables::None ? document : nullptr)->state.load() ==
            FrameState::Ready);
    const double f6Seconds = seconds(f6);

    const auto tables = kind != Tables::None ? std::make_shared<MemoTablePool>() : nullptr;
    double forkSeconds = 0;
    if (kind == Tables::Seeded) {
      const auto table = document->take();
      const auto fork = std::chrono::steady_clock::now();
      tables->setSeed(table->fork());
      forkSeconds = seconds(fork);
    }

    std::vector<std::shared_ptr<CachedFrame>> made;
    std::vector<std::pair<double, double>> times(frames);
    QThreadPool pool;
    pool.setMaxThreadCount(threads);
    pool.setStackSize(static_cast<uint>(
      std::min<unsigned long>(PlatformUtils::stackLimit() + STACK_BUFFER_SIZE, UINT_MAX)));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
      auto frame = std::make_shared<CachedFrame>();
      frame->step = i;
      frame->t = double(i) / frames;
      made.push_back(frame);
      pool.start([&, i, frame]() {
        times[i].first = seconds(start);
        FrameTask task(QPointer<FrameCache>(), 0, frame, file, path.parent_path().generic_string(), Camera(),
                       std::make_shared<std::atomic<bool>>(false), tables);
        task.run();
        times[i].second = seconds(start);
      });
    }
    pool.waitForDone();
    const double total = seconds(start);
    for (int i = 0; i < frames; ++i) {
      const auto& result = made[i]->result;
      std::cout << "  frame " << i << ": " << times[i].first << " - " << times[i].second << " s";
      if (result) std::cout << ", reused " << result->userCallsReused << " of " << result->userCalls;
      std::cout << "\n";
    }
    size_t calls = 0, reused = 0;
    int ready = 0;
    for (const auto& frame : made) {
      if (frame->state.load() != FrameState::Ready) continue;
      ++ready;
      calls += frame->result->userCalls;
      reused += frame->result->userCallsReused;
    }
    std::cout << (kind == Tables::None ? "no tables" : kind == Tables::Empty ? "empty tables" : "seeded tables")
              << ": F6 " << f6Seconds << " s, fork " << forkSeconds << " s, then " << frames << " frames on "
              << threads << " threads in " << total << " s, " << ready << " ready, reused " << reused
              << " of " << calls << " module calls\n";
    CHECK(ready == frames);
  }
  fs::current_path(original);
}
