// Tests of geometry/GeometryEvaluator.cc (the unit tests are collected from src/core, src/utils and
// src/gui).

#include "geometry/GeometryEvaluator.h"

#include <catch2/catch_all.hpp>
#include <filesystem>
#include <memory>
#include <string>

#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/NodeVisitor.h"
#include "core/SourceFile.h"
#include "core/State.h"
#include "core/TransformNode.h"
#include "core/Tree.h"
#include "core/node.h"
#include "geometry/Geometry.h"
#include "geometry/GeometryCache.h"
#include "geometry/cgal/CGALCache.h"
#include "openscad.h"
#include "platform/PlatformUtils.h"

namespace fs = std::filesystem;

namespace {

// Meshes from Manifold need the color schemes, which live at the top of the source tree.
void setUpOnce()
{
  static const bool done = [] {
    const fs::path source = fs::path(__FILE__).parent_path();  // <root>/src/core
    PlatformUtils::registerApplicationPath(source.parent_path().generic_string());
    Builtins::instance()->initialize();
    return true;
  }();
  (void)done;
}

struct Scene {
  std::unique_ptr<SourceFile> file;  // the nodes' modinst point into its AST
  std::shared_ptr<AbstractNode> root;
  std::unique_ptr<Tree> tree;
};

std::unique_ptr<Scene> instantiate(const std::string& text)
{
  setUpOnce();
  auto scene = std::make_unique<Scene>();
  const std::string dir = fs::temp_directory_path().generic_string();
  const std::string mainFile = (fs::temp_directory_path() / "evaluator-test.scad").generic_string();
  SourceFile *file = nullptr;
  REQUIRE(parse(file, text + "\n\x03\n", mainFile, mainFile, false));
  scene->file.reset(file);

  EvaluationSession session{dir};
  ContextHandle<BuiltinContext> builtinContext{Context::create<BuiltinContext>(&session)};
  std::shared_ptr<const FileContext> fileContext;
  scene->root = scene->file->instantiate(*builtinContext, &fileContext);
  REQUIRE(scene->root);
  scene->tree = std::make_unique<Tree>(scene->root, dir);
  return scene;
}

// Empties the caches right after a transform's prefix visit finds it cached, as another thread's
// insertions can: its children are pruned, so only what that visit found can make it.
class EvictingEvaluator : public GeometryEvaluator
{
public:
  using GeometryEvaluator::GeometryEvaluator;
  using GeometryEvaluator::visit;

  Response visit(State& state, const TransformNode& node) override
  {
    const Response response = GeometryEvaluator::visit(state, node);
    if (state.isPrefix() && response == Response::PruneTraversal) {
      GeometryCache::instance()->clear();
      CGALCache::instance()->clear();
      this->evicted = true;
    }
    return response;
  }

  bool evicted = false;
};

}  // namespace

TEST_CASE("A node found cached keeps its geometry when another thread evicts it", "[geometry]")
{
  // The part's geometry is cached, as rendering another frame of the design leaves it.
  const auto part = instantiate("translate([5, 0, 0]) cube(2);");
  GeometryEvaluator(*part->tree).evaluateGeometry(*part->root, true);

  const auto scene = instantiate("cube(1);\ntranslate([5, 0, 0]) cube(2);");
  EvictingEvaluator evaluator(*scene->tree);
  const auto geom = evaluator.evaluateGeometry(*scene->root, true);
  REQUIRE(evaluator.evicted);
  REQUIRE(geom);
  const BoundingBox box = geom->getBoundingBox();
  CHECK(box.min().x() == Catch::Approx(0));
  CHECK(box.max().x() == Catch::Approx(7));
  CHECK(box.max().z() == Catch::Approx(2));
}
