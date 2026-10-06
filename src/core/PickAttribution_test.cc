#include "core/PickAttribution.h"

#include <algorithm>
#include <catch2/catch_all.hpp>
#include <cmath>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Feature.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/SourceFile.h"
#include "core/Tree.h"
#include "core/node.h"
#include "geometry/GeometryCache.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySet.h"
#ifdef ENABLE_CGAL
#include "geometry/cgal/CGALCache.h"
#endif
#include "glview/RenderSettings.h"
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

// Entries from one backend must not be served to the other.
void clearCaches()
{
  GeometryCache::instance()->clear();
#ifdef ENABLE_CGAL
  CGALCache::instance()->clear();
#endif
}

struct Backend {
  explicit Backend(RenderBackend3D backend) : previous(RenderSettings::inst()->backend3D)
  {
    RenderSettings::inst()->backend3D = backend;
    clearCaches();
  }
  ~Backend()
  {
    RenderSettings::inst()->backend3D = previous;
    clearCaches();
  }
  RenderBackend3D previous;
};

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
  const std::string mainFile = (fs::temp_directory_path() / "pick-test.scad").generic_string();
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

// What the picker has to work with.
struct View {
  std::vector<pick::PlacedMesh> surface;
  std::vector<pick::Leaf> leaves;
};

// The F6 view: the rendered result and every primitive in the tree.
View renderView(const Scene& scene)
{
  GeometryEvaluator evaluator(*scene.tree);
  View view;
  view.surface = pick::surfaceOf(evaluator.evaluateGeometry(*scene.root, true));
  view.leaves = pick::collectLeaves(*scene.tree, *scene.root, Transform3d::Identity());
  return view;
}

pick::Ray ray(const Vector3d& from, const Vector3d& to)
{
  return {from, to - from};
}

// A ray straight down through (x, y), from z = 50 to z = -50.
pick::Ray down(double x, double y)
{
  return ray({x, y, 50}, {x, y, -50});
}

std::shared_ptr<const AbstractNode> node(const Scene& scene, int index)
{
  std::deque<std::shared_ptr<const AbstractNode>> path;
  return scene.root->getNodeByID(index, path);
}

// Source lines of the attributed primitives, best first.
std::vector<int> lines(const Scene& scene, const std::vector<int>& indices)
{
  std::vector<int> result;
  for (const int index : indices) {
    const auto found = node(scene, index);
    REQUIRE(found);
    REQUIRE(found->modinst);
    result.push_back(found->modinst->location().firstLine());
  }
  return result;
}

std::vector<int> attribute(const View& view, const pick::Ray& r)
{
  return pick::attribute(view.surface, view.leaves, r);
}

using Lines = std::vector<int>;

}  // namespace

TEST_CASE("A ray meets a triangle of either winding", "[pick]")
{
  const Vector3d a(0, 0, 0), b(1, 0, 0), c(0, 1, 0);
  const Vector3d origin(0.25, 0.25, 1), dir(0, 0, -2);
  CHECK(pick::intersectTriangle(origin, dir, a, b, c).value() == Catch::Approx(0.5));
  CHECK(pick::intersectTriangle(origin, dir, a, c, b).value() == Catch::Approx(0.5));
  // Missing it, parallel to it, and through an edge.
  CHECK_FALSE(pick::intersectTriangle({1, 1, 1}, dir, a, b, c));
  CHECK_FALSE(pick::intersectTriangle({-1, 0.25, 0}, {2, 0, 0}, a, b, c));
  CHECK(pick::intersectTriangle({0.5, 0, 1}, dir, a, b, c).value() == Catch::Approx(0.5));
}

TEST_CASE("The closest point of a triangle is on its face, an edge, or a corner", "[pick]")
{
  const Vector3d a(0, 0, 0), b(1, 0, 0), c(0, 1, 0);
  CHECK(pick::closestPointOnTriangle({0.25, 0.25, 1}, a, b, c).isApprox(Vector3d(0.25, 0.25, 0)));
  CHECK(pick::closestPointOnTriangle({0.5, -1, 0}, a, b, c).isApprox(Vector3d(0.5, 0, 0)));
  CHECK(pick::closestPointOnTriangle({-1, -1, 0}, a, b, c).isApprox(a));
  CHECK(pick::closestPointOnTriangle({1, 1, 0}, a, b, c).isApprox(Vector3d(0.5, 0.5, 0)));
}

TEST_CASE("A ray meets the surface where it first crosses it", "[pick]")
{
  // Two squares, at z = 10 and z = 0, as convex quads.
  auto ps = std::make_shared<PolySet>(3, /*convex*/ true);
  ps->vertices = {{0, 0, 10}, {1, 0, 10}, {1, 1, 10}, {0, 1, 10},
                  {0, 0, 0},  {1, 0, 0},  {1, 1, 0},  {0, 1, 0}};
  ps->indices = {{0, 1, 2, 3}, {4, 5, 6, 7}};
  const std::vector<pick::PlacedMesh> surface{{ps, Transform3d::Identity()}};
  const auto r = ray({0.5, 0.5, 20}, {0.5, 0.5, -20});

  CHECK(pick::castRay(surface, r)->t == Catch::Approx(0.25));
  CHECK(pick::castRay(surface, r)->point.isApprox(Vector3d(0.5, 0.5, 10)));
  CHECK_FALSE(pick::castRay(surface, ray({2, 2, 20}, {2, 2, -20})));
  // Only t in [0, 1] counts: past the far plane is not drawn.
  CHECK_FALSE(pick::castRay(surface, ray({0.5, 0.5, 20}, {0.5, 0.5, 15})));
}

#ifdef ENABLE_MANIFOLD

TEST_CASE("A face cut by difference() belongs to the primitive that cut it", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  const auto scene = instantiate(
    "difference() {\n"
    "  cube([20, 20, 10]);\n"
    "  translate([10, 10, -1]) cylinder(r = 4, h = 12, $fn = 48);\n"
    "}");
  const View view = renderView(*scene);
  CHECK(lines(*scene, attribute(view, down(2, 2))) == Lines{2});
  // From inside the hole, slightly off the x axis so the ray misses the facet edges.
  CHECK(lines(*scene, attribute(view, ray({10, 10, 5}, {40, 10.9, 5}))) == Lines{3});
  CHECK(attribute(view, down(50, 50)).empty());
}

TEST_CASE("A face that is both added and cut away belongs to the primitive adding it", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  const auto scene = instantiate(
    "union() {\n"
    "  difference() {\n"
    "    cube([10, 10, 20]);\n"
    "    translate([-1, -1, 10]) cube([12, 12, 20]);\n"
    "  }\n"
    "  cube(10);\n"
    "}");
  CHECK(lines(*scene, attribute(renderView(*scene), down(5, 5))) == Lines{6, 4});
}

TEST_CASE("A face just below the surface is not part of it", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  const auto scene = instantiate(
    "union() {\n"
    "  translate([200, 200, 0]) cube([100, 100, 99.999]);\n"
    "  cube([1000, 1000, 100]);\n"
    "}");
  // From above the model, which is 100 tall.
  CHECK(lines(*scene, attribute(renderView(*scene), ray({250, 250, 500}, {250, 250, -500}))) ==
        Lines{3});
}

TEST_CASE("Highlighted primitives count, background ones and 2D ones do not", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  {
    const auto scene = instantiate(
      "union() {\n"
      "  #cube(10);\n"
      "  translate([20, 0, 0]) cube(5);\n"
      "}");
    CHECK(lines(*scene, attribute(renderView(*scene), down(5, 5))) == Lines{2});
  }
  {
    const auto scene = instantiate(
      "union() {\n"
      "  cube(10);\n"
      "  %translate([0, 0, 10]) cube(10);\n"
      "}");
    CHECK(lines(*scene, attribute(renderView(*scene), down(5, 5))) == Lines{2});
  }
  {
    const auto scene = instantiate(
      "union() {\n"
      "  cube(10);\n"
      "  translate([0, 0, 10.5]) square(5);\n"
      "}");
    CHECK(lines(*scene, attribute(renderView(*scene), down(2, 2))) == Lines{2});
  }
}

// Source line of each leaf of the whole tree, and whether it cuts material away.
std::vector<std::pair<int, bool>> subtractedByLine(const Scene& scene)
{
  std::vector<std::pair<int, bool>> result;
  for (const auto& leaf : pick::collectLeaves(*scene.tree, *scene.root, Transform3d::Identity())) {
    result.emplace_back(lines(scene, {leaf.index}).front(), leaf.subtracted);
  }
  return result;
}

using Cuts = std::vector<std::pair<int, bool>>;

TEST_CASE("A leaf cuts material when an odd number of difference()s subtract it", "[pick]")
{
  const auto scene = instantiate(
    "difference() {\n"
    "  cube(10);\n"
    "  difference() {\n"
    "    translate([2, 2, -1]) cube(6);\n"
    "    translate([4, 4, -2]) cube(2);\n"
    "  }\n"
    "  translate([0, 0, 9]) cube(1);\n"
    "}\n"
    "intersection() { cube(1); sphere(1); }");
  CHECK(subtractedByLine(*scene) ==
        Cuts{{2, false}, {4, true}, {5, false}, {7, true}, {9, false}, {9, false}});
}

TEST_CASE("The first operand of difference() that isn't % is the one cut", "[pick]")
{
  const auto scene = instantiate(
    "difference() {\n"
    "  %cube(1);\n"
    "  cube(5);\n"
    "  translate([1, 1, 1]) cube(1);\n"
    "}");
  CHECK(subtractedByLine(*scene) == Cuts{{3, false}, {4, true}});
}

TEST_CASE("A loop in difference() is one operand, or several with lazy union", "[pick]")
{
  const std::string text =
    "difference() {\n"
    "  for (i = [0:2]) translate([3 * i, 0, 0]) cube(2);\n"
    "}";
  SECTION("group")
  {
    const auto scene = instantiate(text);
    CHECK(subtractedByLine(*scene) == Cuts{{2, false}, {2, false}, {2, false}});
  }
  SECTION("lazy union")
  {
    struct LazyUnion {
      LazyUnion() { Feature::enable_feature("lazy-union", true); }
      ~LazyUnion() { Feature::enable_feature("lazy-union", false); }
    } lazyUnion;
    const auto scene = instantiate(text);
    CHECK(subtractedByLine(*scene) == Cuts{{2, false}, {2, true}, {2, true}});
  }
}

TEST_CASE("hull() and minkowski() are named as a whole", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  {
    const auto scene = instantiate(
      "render() hull() {\n"
      "  cube(5);\n"
      "  translate([10, 0, 0]) sphere(2, $fn = 16);\n"
      "}");
    const auto picked = attribute(renderView(*scene), down(2, 2));
    REQUIRE(picked.size() == 1);
    CHECK(node(*scene, picked.front())->name() == "hull");
  }
  {
    const auto scene = instantiate(
      "minkowski() {\n"
      "  cube(10);\n"
      "  sphere(1, $fn = 8);\n"
      "}");
    const auto picked = attribute(renderView(*scene), down(5, 5));
    REQUIRE(picked.size() == 1);
    CHECK(node(*scene, picked.front())->name() == "minkowski");
  }
}

TEST_CASE("Primitives inside a placed render() of nested modules are named", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  const auto scene = instantiate(
    "$fn = 12;\n"
    "module post(h) { cylinder(r = 2, h = h); }\n"
    "module plate() {\n"
    "  difference() {\n"
    "    cube([40, 20, 4]);\n"
    "    translate([10, 10, -1]) cylinder(r = 3, h = 6);\n"
    "  }\n"
    "}\n"
    "translate([100, 0, 0]) rotate([0, 0, 90])\n"
    "  render() { plate(); translate([30, 10, 4]) post(10); }");
  // translate([100, 0, 0]) rotate([0, 0, 90]) takes local (x, y, z) to (100 - y, x, z).
  const auto world = [](double x, double y, double z) { return Vector3d(100 - y, x, z); };
  const auto plateTop = down(world(5.3, 4.7, 0).x(), world(5.3, 4.7, 0).y());
  const auto postTop = down(world(30.3, 10.2, 0).x(), world(30.3, 10.2, 0).y());
  const auto holeWall =
    ray(world(10, 10, 2), world(10 + 20 * std::cos(0.26), 10 + 20 * std::sin(0.26), 2));

  const View view = renderView(*scene);
  CHECK(lines(*scene, attribute(view, plateTop)) == Lines{5});
  const auto post = attribute(view, postTop);
  REQUIRE(lines(*scene, post) == Lines{2});
  // The chain runs through the module call and the render().
  std::deque<std::shared_ptr<const AbstractNode>> path;
  scene->root->getNodeByID(post.front(), path);
  CHECK(std::any_of(path.begin(), path.end(),
                    [](const auto& step) { return step->verbose_name() == "module post"; }));
  CHECK(
    std::any_of(path.begin(), path.end(), [](const auto& step) { return step->name() == "render"; }));
  CHECK(lines(*scene, attribute(view, holeWall)) == Lines{6});
  CHECK(attribute(view, down(0, 0)).empty());
}

TEST_CASE("Mirrored and far-off geometry is named the same", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  const std::string part =
    "difference() {\n"
    "  cube([20, 20, 10]);\n"
    "  translate([10, 10, -1]) cylinder(r = 4, h = 12, $fn = 48);\n"
    "}";
  {
    const auto scene = instantiate("mirror([1, 0, 0])\n" + part);
    const View view = renderView(*scene);
    CHECK(lines(*scene, attribute(view, down(-2, 2))) == Lines{3});
    CHECK(lines(*scene, attribute(view, ray({-10, 10, 5}, {-40, 10.9, 5}))) == Lines{4});
  }
  {
    const auto scene = instantiate("translate([1e5, 1e5, 0])\n" + part);
    const View view = renderView(*scene);
    CHECK(lines(*scene, attribute(view, down(1e5 + 2, 1e5 + 2))) == Lines{3});
    CHECK(lines(*scene, attribute(view, ray({1e5 + 10, 1e5 + 10, 5}, {1e5 + 40, 1e5 + 10.9, 5}))) ==
          Lines{4});
  }
}

TEST_CASE("A concave face of the result is not hit where it has a notch", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  // An L-shaped prism: the square [0, 10]^2 without [4, 10]^2.
  const auto scene = instantiate(
    "polyhedron(\n"
    "  points = [[10, 4, 0], [4, 4, 0], [4, 10, 0], [0, 10, 0], [0, 0, 0], [10, 0, 0],\n"
    "            [10, 4, 10], [4, 4, 10], [4, 10, 10], [0, 10, 10], [0, 0, 10], [10, 0, 10]],\n"
    "  faces = [[0, 1, 2, 3, 4, 5], [11, 10, 9, 8, 7, 6],\n"
    "           [6, 7, 1, 0], [7, 8, 2, 1], [8, 9, 3, 2], [9, 10, 4, 3], [10, 11, 5, 4],\n"
    "           [11, 6, 0, 5]]);");
  const View view = renderView(*scene);
  CHECK(attribute(view, down(6, 8.5)).empty());
  CHECK(lines(*scene, attribute(view, down(2, 2))) == Lines{1});
}

TEST_CASE("A 2D result names nothing", "[pick]")
{
  const Backend manifold(RenderBackend3D::ManifoldBackend);
  const auto scene = instantiate("square(10);");
  const View view = renderView(*scene);
  CHECK(view.surface.empty());
  CHECK(attribute(view, down(5, 5)).empty());
}

#endif  // ENABLE_MANIFOLD

#ifdef ENABLE_CGAL

TEST_CASE("CGAL's rounded result is attributed the same", "[pick][cgal]")
{
  const Backend cgal(RenderBackend3D::CGALBackend);
  const std::string part =
    "difference() {\n"
    "  cube([20, 20, 10]);\n"
    "  translate([10, 10, -1]) cylinder(r = 4, h = 12, $fn = 48);\n"
    "}";
  {
    const auto scene = instantiate(part);
    const View view = renderView(*scene);
    CHECK(lines(*scene, attribute(view, down(2, 2))) == Lines{2});
    CHECK(lines(*scene, attribute(view, ray({10, 10, 5}, {40, 10.9, 5}))) == Lines{3});
  }
  {
    const auto scene = instantiate("translate([1e4, 0, 0])\n" + part);
    const View view = renderView(*scene);
    CHECK(lines(*scene, attribute(view, down(1e4 + 2, 2))) == Lines{3});
    CHECK(lines(*scene, attribute(view, ray({1e4 + 10, 10, 5}, {1e4 + 40, 10.9, 5}))) == Lines{4});
  }
}

#endif  // ENABLE_CGAL
