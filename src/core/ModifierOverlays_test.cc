#include "core/ModifierOverlays.h"

#include <catch2/catch_all.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Feature.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/SourceFile.h"
#include "core/Tree.h"
#include "core/node.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySet.h"
#include "geometry/linalg.h"
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
  const std::string mainFile = (fs::temp_directory_path() / "overlay-test.scad").generic_string();
  SourceFile *file = nullptr;
  REQUIRE(parse(file, text + "\n\x03\n", mainFile, mainFile, false));
  scene->file.reset(file);

  EvaluationSession session{dir};
  ContextHandle<BuiltinContext> builtinContext{Context::create<BuiltinContext>(&session)};
  AbstractNode::resetIndexCounter();
  std::shared_ptr<const FileContext> fileContext;
  scene->root = scene->file->instantiate(*builtinContext, &fileContext);
  REQUIRE(scene->root);
  scene->tree = std::make_unique<Tree>(scene->root, dir);
  return scene;
}

// What F6 draws: the result is rendered first, so `#` subtrees come from the cache.
std::vector<overlay::Mesh> overlays(const Scene& scene)
{
  GeometryEvaluator evaluator(*scene.tree);
  evaluator.evaluateGeometry(*scene.root, true);
  return overlay::collect(*scene.tree, *scene.root);
}

void requireBox(const overlay::Mesh& mesh, const Vector3d& min, const Vector3d& max,
                double margin = 1e-9)
{
  const BoundingBox box = mesh.polyset->getBoundingBox();
  for (int i = 0; i < 3; ++i) {
    CHECK(box.min()[i] == Catch::Approx(min[i]).margin(margin));
    CHECK(box.max()[i] == Catch::Approx(max[i]).margin(margin));
  }
}

using overlay::Kind;

}  // namespace

TEST_CASE("Nothing is drawn over a design without modifiers", "[overlay]")
{
  const auto scene = instantiate("difference() { cube(10); translate([2, 2, -1]) cube(3); }");
  CHECK(overlays(*scene).empty());
}

TEST_CASE("A highlighted cutter is drawn where it cuts", "[overlay]")
{
  const auto scene = instantiate(
    "translate([10, 0, 0]) difference() { cube(10); #translate([2, 3, -1]) cube([4, 5, 12]); }");
  const auto meshes = overlays(*scene);
  REQUIRE(meshes.size() == 1);
  CHECK(meshes[0].kind == Kind::Highlight);
  requireBox(meshes[0], {12, 3, -1}, {16, 8, 11});
}

TEST_CASE("A background subtree is drawn under its transforms", "[overlay]")
{
  const auto scene = instantiate(
    "cube(1);\n"
    "rotate([0, 0, 90]) translate([5, 0, 0]) %scale([2, 1, 1]) cube(1);");
  const auto meshes = overlays(*scene);
  REQUIRE(meshes.size() == 1);
  CHECK(meshes[0].kind == Kind::Background);
  requireBox(meshes[0], {-1, 5, 0}, {0, 7, 1});
}

TEST_CASE("Only the branches with modifiers are walked, however deep", "[overlay]")
{
  // A call chain makes a chain of groups; modules without modifiers are passed over.
  const auto scene = instantiate(
    "module plain(n) if (n > 0) plain(n - 1); else cube(1);\n"
    "module marked(n) if (n > 0) translate([1, 0, 0]) marked(n - 1); else #cube(1);\n"
    "plain(50);\n"
    "marked(50);\n");
  const auto meshes = overlays(*scene);
  REQUIRE(meshes.size() == 1);
  CHECK(meshes[0].kind == Kind::Highlight);
  requireBox(meshes[0], {50, 0, 0}, {51, 1, 1});
  const AbstractNode& plain = *scene->root->children.at(0);
  CHECK(!scene->tree->hasModifierBelow(plain));
  CHECK(scene->tree->hasModifierBelow(*scene->root->children.at(1)));
}

TEST_CASE("Modifiers inside a highlighted subtree", "[overlay]")
{
  // The # union draws its cubes at 0 and 10 together. The % cube at 5 is not part of it.
  const auto scene = instantiate(
    "#union() {\n"
    "  cube(1);\n"
    "  %translate([5, 0, 0]) cube(1);\n"
    "  #translate([10, 0, 0]) cube(1);\n"
    "}");
  const auto meshes = overlays(*scene);
  REQUIRE(meshes.size() == 2);
  CHECK(meshes[0].kind == Kind::Highlight);
  requireBox(meshes[0], {0, 0, 0}, {11, 1, 1});
  CHECK(meshes[1].kind == Kind::Background);
  requireBox(meshes[1], {5, 0, 0}, {6, 1, 1});
}

TEST_CASE("A background subtree inside another is drawn on its own", "[overlay]")
{
  const auto scene =
    instantiate("%union() { cube(1); %translate([5, 0, 0]) cube(1); #translate([10, 0, 0]) cube(1); }");
  const auto meshes = overlays(*scene);
  REQUIRE(meshes.size() == 2);
  CHECK(meshes[0].kind == Kind::Background);
  requireBox(meshes[0], {0, 0, 0}, {11, 1, 1});
  CHECK(meshes[1].kind == Kind::Background);
  requireBox(meshes[1], {5, 0, 0}, {6, 1, 1});
}

TEST_CASE("A background loop draws all of its children", "[overlay]")
{
  const std::string text = "%for (i = [0:2]) translate([3 * i, 0, 0]) cube(1);";
  SECTION("as a group")
  {
    const auto scene = instantiate(text);
    const auto meshes = overlays(*scene);
    REQUIRE(meshes.size() == 1);
    CHECK(meshes[0].kind == Kind::Background);
    requireBox(meshes[0], {0, 0, 0}, {7, 1, 1});
  }
  SECTION("as a lazy-union list")
  {
    // F6 leaves out a % list given to it whole, so its children are evaluated one by one.
    struct LazyUnion {
      LazyUnion() { Feature::enable_feature("lazy-union", true); }
      ~LazyUnion() { Feature::enable_feature("lazy-union", false); }
    } lazyUnion;
    const auto scene = instantiate(text);
    const auto meshes = overlays(*scene);
    REQUIRE(meshes.size() == 3);
    for (int i = 0; i < 3; ++i) {
      CHECK(meshes[i].kind == Kind::Background);
      requireBox(meshes[i], {3.0 * i, 0, 0}, {3.0 * i + 1, 1, 1});
    }
  }
}

TEST_CASE("render() and hull() are looked into, extrusions are not", "[overlay]")
{
  SECTION("render")
  {
    const auto scene =
      instantiate("render() difference() { cube(4); #translate([1, 1, -1]) cube([1, 1, 6]); }");
    const auto meshes = overlays(*scene);
    REQUIRE(meshes.size() == 1);
    requireBox(meshes[0], {1, 1, -1}, {2, 2, 5});
  }
  SECTION("hull")
  {
    const auto scene = instantiate("hull() { cube(1); %translate([4, 0, 0]) cube(1); }");
    const auto meshes = overlays(*scene);
    REQUIRE(meshes.size() == 1);
    CHECK(meshes[0].kind == Kind::Background);
    requireBox(meshes[0], {4, 0, 0}, {5, 1, 1});
  }
  SECTION("linear_extrude")
  {
    const auto scene = instantiate("linear_extrude(2) #square(1);");
    CHECK(overlays(*scene).empty());
  }
  SECTION("highlighted linear_extrude")
  {
    const auto scene = instantiate("translate([0, 0, 3]) #linear_extrude(2) square(1);");
    const auto meshes = overlays(*scene);
    REQUIRE(meshes.size() == 1);
    requireBox(meshes[0], {0, 0, 3}, {1, 1, 5});
  }
}

TEST_CASE("A 2D shape is drawn flat where it is placed", "[overlay]")
{
  const auto scene = instantiate("difference() { square(10); #translate([2, 3]) square([1, 2]); }");
  const auto meshes = overlays(*scene);
  REQUIRE(meshes.size() == 1);
  CHECK(meshes[0].kind == Kind::Highlight);
  CHECK_FALSE(meshes[0].polyset->isEmpty());
  requireBox(meshes[0], {2, 3, 0}, {3, 5, 0});
}

TEST_CASE("A mirrored subtree keeps its faces outward", "[overlay]")
{
  const auto scene = instantiate("mirror([1, 0, 0]) %translate([2, 0, 0]) cube(1);");
  const auto meshes = overlays(*scene);
  REQUIRE(meshes.size() == 1);
  requireBox(meshes[0], {-3, 0, 0}, {-2, 1, 1});
  // Every face's normal points away from the cube's center.
  const PolySet& ps = *meshes[0].polyset;
  const Vector3d center(-2.5, 0.5, 0.5);
  for (const auto& face : ps.indices) {
    REQUIRE(face.size() == 3);
    const Vector3d &a = ps.vertices[face[0]], &b = ps.vertices[face[1]], &c = ps.vertices[face[2]];
    CHECK((b - a).cross(c - a).dot((a + b + c) / 3 - center) > 0);
  }
}

TEST_CASE("The root's own background modifier is ignored", "[overlay]")
{
  const auto scene = instantiate("%translate([1, 0, 0]) cube(1);");
  const auto& top = scene->root->getChildren().at(0);
  CHECK(overlay::collect(*scene->tree, *top).empty());
}

#ifdef ENABLE_PHYSICS
TEST_CASE("Modifiers inside physics() follow the simulated pose", "[overlay]")
{
  // The body (cube and # cylinder) drops onto the floor; the % sphere rides along. The cube was
  // centered at z = 30, the cylinder too, and the sphere 11 above.
  const auto scene = instantiate(
    "physics() {\n"
    "  translate([0, 0, 30]) cube(10, center = true);\n"
    "  %translate([0, 0, 41]) sphere(5, $fn = 32);\n"
    "  #translate([0, 0, 30]) rotate([0, 90, 0]) cylinder(h = 16, r = 2, center = true, $fn = 24);\n"
    "}");
  GeometryEvaluator evaluator(*scene->tree);
  const auto body = evaluator.evaluateGeometry(*scene->root, true)->getBoundingBox();
  const double center = body.center().z();
  REQUIRE(center < 10);  // it fell

  const auto meshes = overlay::collect(*scene->tree, *scene->root);
  REQUIRE(meshes.size() == 2);
  CHECK(meshes[0].kind == Kind::Background);
  CHECK(meshes[0].polyset->getBoundingBox().center().z() == Catch::Approx(center + 11).margin(1e-3));
  CHECK(meshes[1].kind == Kind::Highlight);
  requireBox(meshes[1], {-8, -2, center - 2}, {8, 2, center + 2}, 1e-3);
}
#endif
