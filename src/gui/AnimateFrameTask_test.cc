#include "gui/AnimateFrameTask.h"

#include <QPointer>
#include <atomic>
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <memory>
#include <string>

#include "core/Builtins.h"
#include "core/ModifierOverlays.h"
#include "core/SourceFile.h"
#include "geometry/Geometry.h"
#include "geometry/PolySet.h"
#include "glview/Camera.h"
#include "gui/AnimateFrameCache.h"
#include "openscad.h"
#include "platform/PlatformUtils.h"

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

// Runs the frame task for time t on this thread, as a pool worker would.
std::shared_ptr<CachedFrame> renderFrame(const std::shared_ptr<SourceFile>& file, double t)
{
  auto frame = std::make_shared<CachedFrame>();
  frame->step = 1;
  frame->t = t;
  FrameTask task(QPointer<FrameCache>(), 0, frame, file, fs::temp_directory_path().generic_string(),
                 Camera(), std::make_shared<std::atomic<bool>>(false));
  task.run();
  return frame;
}

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
