#include "core/NodeDigest.h"

#include <catch2/catch_all.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Feature.h"
#include "core/BuiltinContext.h"
#include "core/Builtins.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/Tree.h"
#include "core/node.h"
#include "openscad.h"
#include "platform/PlatformUtils.h"
#include "utils/printutils.h"

namespace fs = std::filesystem;

namespace {

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

// A script's tree, with the parse its nodes point into.
struct Evaluated {
  std::unique_ptr<SourceFile> file;
  std::shared_ptr<AbstractNode> root;
};

Evaluated evaluate(const std::string& text)
{
  setUpOnce();
  const std::string mainFile = (fs::temp_directory_path() / "digest-test.scad").generic_string();
  SourceFile *parsed = nullptr;
  const bool ok = parse(parsed, text + "\n\x03\n", mainFile, mainFile, false);
  Evaluated result;
  result.file.reset(parsed);
  REQUIRE(ok);
  result.file->handleDependencies();
  const PrintSuppressGuard quiet;
  EvaluationSession session{fs::temp_directory_path().generic_string()};
  ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(&session)};
  std::shared_ptr<const FileContext> fileContext;
  result.root = result.file->instantiate(*builtin, &fileContext);
  REQUIRE(result.root);
  return result;
}

// The digest of what a script makes, and the text key it had before.
struct Key {
  Hash128 digest;
  std::string text;
};

Key keyOf(const std::string& script)
{
  const Evaluated evaluated = evaluate(script);
  const Tree tree(evaluated.root);
  return {tree.digest(*evaluated.root), tree.getIdString(*evaluated.root)};
}

// Each pair is two scripts.
using Pairs = std::vector<std::pair<std::string, std::string>>;

// Lazy union makes for, if and children() give lists instead of groups.
struct LazyUnion {
  LazyUnion() : was(Feature::ExperimentalLazyUnion.is_enabled())
  {
    Feature::enable_feature("lazy-union", true);
  }
  ~LazyUnion() { Feature::enable_feature("lazy-union", this->was); }
  bool was;
};

void forEachNode(const AbstractNode& node, const std::function<void(const AbstractNode&)>& f)
{
  f(node);
  for (const auto& child : node.children) forEachNode(*child, f);
}

}  // namespace

TEST_CASE("Digests are equal where the text keys were", "[digest]")
{
  const Pairs same = {
    // A group of one unmodified operand is that operand, however deep, and empty groups are no
    // operands of it.
    {"cube(1);", "group() group() { group() cube(1); }"},
    {"cube(1);", "module m() cube(1); m();"},
    {"cube(1);", "group() { group(); cube(1); group() group(); }"},
    // Modifiers on such a group are its operand's.
    {"union() { %cube(1); sphere(1); }", "union() { %group() cube(1); sphere(1); }"},
    {"union() { #cube(1); sphere(1); }", "union() { #group() cube(1); sphere(1); }"},
    // Empty groups are no operands of what unions its children.
    {"translate([1, 0, 0]) cube(1);", "translate([1, 0, 0]) { group(); cube(1); }"},
    {"union() { cube(1); sphere(1); }", "union() { group(); cube(1); group(); sphere(1); }"},
    // intersection_for() is an intersection().
    {"intersection_for(i = [1, 2]) cube(i);", "intersection() { cube(1); cube(2); }"},
  };
  for (const auto& [a, b] : same) {
    CAPTURE(a, b);
    const Key x = keyOf(a);
    const Key y = keyOf(b);
    CHECK(x.text == y.text);
    CHECK(x.digest == y.digest);
  }
}

TEST_CASE("Digests differ where the text keys mixed up different geometry", "[digest]")
{
  const Pairs differ = {
    // Numbers beyond six significant digits, which the text keys did not print.
    {"cube(1);", "cube(1.0000001);"},
    {"translate([0.1 + 0.2, 0, 0]) cube(1);", "translate([0.3, 0, 0]) cube(1);"},
    // A group of a modified operand has no geometry, so it is an empty operand, which a
    // difference() starting with it is left with...
    {"difference() { %cube(1); sphere(1); }", "difference() { group() %cube(1); sphere(1); }"},
    // ...as it is with a group that is empty, or with an intersection() with one.
    {"difference() { cube(1); sphere(1); }", "difference() { group(); cube(1); sphere(1); }"},
    {"intersection() { cube(1); sphere(1); }", "intersection() { cube(1); group(); sphere(1); }"},
    // The modifier of an empty group was written before the next operand.
    {"union() { %group(); cube(1); sphere(1); }", "union() { %cube(1); sphere(1); }"},
  };
  for (const auto& [a, b] : differ) {
    CAPTURE(a, b);
    const Key x = keyOf(a);
    const Key y = keyOf(b);
    CHECK(x.text == y.text);
    CHECK(x.digest != y.digest);
  }
}

TEST_CASE("Digests differ where the text keys did", "[digest]")
{
  const Pairs differ = {
    {"cube(1);", "cube(2);"},
    {"cube([1, 2, 3]);", "cube([1, 2, 4]);"},
    {"cube(1);", "cube(1, center = true);"},
    {"cube(1);", "square(1);"},
    {"sphere(1);", "sphere(2);"},
    {"sphere(1, $fn = 8);", "sphere(1, $fn = 9);"},
    {"sphere(1, $fa = 5);", "sphere(1, $fa = 6);"},
    {"sphere(1, $fs = 1);", "sphere(1, $fs = 2);"},
    {"cylinder(1, 1, 1);", "cylinder(2, 1, 1);"},
    {"cylinder(1, 1, 1);", "cylinder(1, 2, 1);"},
    {"cylinder(1, 1, 1);", "cylinder(1, 1, 2);"},
    {"cylinder(1, 1, 1);", "cylinder(1, 1, 1, center = true);"},
    {"polyhedron([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], [[0, 1, 2], [0, 1, 3], [0, 2, 3], [1, 2, 3]]);",
     "polyhedron([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 2]], [[0, 1, 2], [0, 1, 3], [0, 2, 3], [1, 2, 3]]);"},
    {"polyhedron([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], [[0, 1, 2], [0, 1, 3], [0, 2, 3], [1, 2, 3]]);",
     "polyhedron([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], [[0, 2, 1], [0, 1, 3], [0, 2, 3], [1, 2, 3]]);"},
    {"polyhedron([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], [[0, 1, 2], [0, 1, 3], [0, 2, 3], [1, 2, 3]]);",
     "polyhedron([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]], [[0, 1, 2], [0, 1, 3], [0, 2, 3], [1, 2, 3]], "
     "convexity = 2);"},
    {"square([1, 2]);", "square([1, 3]);"},
    {"square(1);", "square(1, center = true);"},
    {"circle(1);", "circle(1, $fn = 5);"},
    {"polygon([[0, 0], [1, 0], [0, 1]]);", "polygon([[0, 0], [1, 0], [0, 2]]);"},
    {"polygon([[0, 0], [1, 0], [0, 1], [1, 1]], [[0, 1, 2]]);",
     "polygon([[0, 0], [1, 0], [0, 1], [1, 1]], [[0, 1, 3]]);"},
    {"translate([1, 0, 0]) cube(1);", "translate([0, 1, 0]) cube(1);"},
    {"rotate(10) cube(1);", "rotate(11) cube(1);"},
    {"color([1, 0, 0]) cube(1);", "color([0, 1, 0]) cube(1);"},
    {"color([1, 0, 0, 0.5]) cube(1);", "color([1, 0, 0, 0.6]) cube(1);"},
    {"render(convexity = 2) cube(1);", "render(convexity = 3) cube(1);"},
    {"offset(r = 1) square(1);", "offset(r = 2) square(1);"},
    {"offset(r = 1) square(1);", "offset(delta = 1) square(1);"},
    {"offset(delta = 1) square(1);", "offset(delta = 1, chamfer = true) square(1);"},
    {"projection() cube(1);", "projection(cut = true) cube(1);"},
    {"linear_extrude(1) square(1);", "linear_extrude(2) square(1);"},
    {"linear_extrude(1) square(1);", "linear_extrude(1, twist = 10) square(1);"},
    {"linear_extrude(1, twist = 10) square(1);", "linear_extrude(1, twist = 10, slices = 3) square(1);"},
    {"linear_extrude(1) square(1);", "linear_extrude(1, scale = 2) square(1);"},
    {"linear_extrude(1) square(1);", "linear_extrude(1, center = true) square(1);"},
    {"linear_extrude(1) square(1);", "linear_extrude(1, convexity = 3) square(1);"},
    {"rotate_extrude() translate([2, 0]) circle(1);", "rotate_extrude(angle = 90) translate([2, 0]) circle(1);"},
    {"rotate_extrude() translate([2, 0]) circle(1);", "rotate_extrude($fn = 7) translate([2, 0]) circle(1);"},
    {"hull() { cube(1); sphere(1); }", "minkowski() { cube(1); sphere(1); }"},
    {"hull() { cube(1); sphere(1); }", "union() { cube(1); sphere(1); }"},
    {"union() { cube(1); sphere(1); }", "difference() { cube(1); sphere(1); }"},
    {"difference() { cube(1); sphere(1); }", "difference() { sphere(1); cube(1); }"},
    {"resize([2, 2, 2]) cube(1);", "resize([2, 2, 3]) cube(1);"},
    {"resize([2, 0, 0]) cube(1);", "resize([2, 0, 0], auto = true) cube(1);"},
    {"text(\"a\");", "text(\"b\");"},
    {"text(\"a\");", "text(\"a\", size = 11);"},
    {"text(\"a\");", "text(\"a\", halign = \"center\");"},
    {"physics() cube(1);", "physics(density = 2) cube(1);"},
    {"union() { cube(1); sphere(1); }", "union() { %cube(1); sphere(1); }"},
    {"union() { cube(1); sphere(1); }", "union() { #cube(1); sphere(1); }"},
    {"union() { cube(1); sphere(1); }", "union() { %#cube(1); sphere(1); }"},
    {"cube(1); sphere(1);", "union() { cube(1); sphere(1); }"},
  };
  for (const auto& [a, b] : differ) {
    CAPTURE(a, b);
    const Key x = keyOf(a);
    const Key y = keyOf(b);
    CHECK(x.text != y.text);
    CHECK(x.digest != y.digest);
  }
}

TEST_CASE("Lists stand for their children", "[digest]")
{
  const LazyUnion lazy;
  // Equal as text, and as digests.
  const Pairs same = {
    {"union() { for (i = [0, 1]) translate([i, 0, 0]) cube(1); }",
     "union() { translate([0, 0, 0]) cube(1); translate([1, 0, 0]) cube(1); }"},
    {"union() { for (i = [0, 1]) for (j = [0, 1]) translate([i, j, 0]) cube(1); }",
     "union() { translate([0, 0, 0]) cube(1); translate([0, 1, 0]) cube(1); "
     "translate([1, 0, 0]) cube(1); translate([1, 1, 0]) cube(1); }"},
    {"if (true) cube(1);", "cube(1);"},
  };
  for (const auto& [a, b] : same) {
    CAPTURE(a, b);
    const Key x = keyOf(a);
    const Key y = keyOf(b);
    CHECK(x.text == y.text);
    CHECK(x.digest == y.digest);
  }

  // Equal as text, not as geometry.
  const Pairs differ = {
    // A group of several operands is one operand, their union.
    {"difference() { group() for (i = [0, 1]) translate([i, 0, 0]) cube(1); sphere(1); }",
     "difference() { for (i = [0, 1]) translate([i, 0, 0]) cube(1); sphere(1); }"},
    // The root gives the list of its operands, a group their union.
    {"translate([0, 0, 0]) cube(1); translate([1, 0, 0]) cube(1);",
     "group() for (i = [0, 1]) translate([i, 0, 0]) cube(1);"},
  };
  for (const auto& [a, b] : differ) {
    CAPTURE(a, b);
    const Key x = keyOf(a);
    const Key y = keyOf(b);
    CHECK(x.text == y.text);
    CHECK(x.digest != y.digest);
  }

  // A list's modifiers go to its children. The text gave them to everything below, which changed
  // no geometry.
  for (const std::string modifier : {"%", "#"}) {
    CAPTURE(modifier);
    const Key x = keyOf("union() { " + modifier + "for (i = [0, 1]) translate([i, 0, 0]) cube(1); }");
    const Key y = keyOf("union() { " + modifier + "translate([0, 0, 0]) cube(1); " + modifier +
                        "translate([1, 0, 0]) cube(1); }");
    CHECK(x.text != y.text);
    CHECK(x.digest == y.digest);
  }
}

TEST_CASE("Every node class hashes its own data", "[digest]")
{
  const std::string script = R"(
group() { cube(1); sphere(1); cylinder(1, 1); polyhedron([[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]],
                                                         [[0, 1, 2], [0, 1, 3], [0, 2, 3], [1, 2, 3]]); }
translate([1, 0, 0]) rotate(10) scale(2) mirror([1, 0, 0]) multmatrix(1) color("red") cube();
union() { difference() { cube(); sphere(); } intersection() { cube(); sphere(); } }
render() hull() minkowski() { cube(); sphere(); }
resize([2, 2, 2]) cube();
linear_extrude(1) square(1);
rotate_extrude() translate([2, 0]) circle(1);
offset(1) polygon([[0, 0], [1, 0], [0, 1]]);
projection() cube();
text("a");
intersection_for(i = [0, 1]) cube(i + 1);
physics() cube(1);
)";
  // Two evaluations make the same digests, node by node: no class falls back on a digest of
  // its own.
  const Evaluated a = evaluate(script);
  const Evaluated b = evaluate(script);
  const Tree treeA(a.root);
  const Tree treeB(b.root);
  CHECK(treeA.digest(*a.root) == treeB.digest(*b.root));
  std::vector<Hash128> digestsA;
  std::vector<Hash128> digestsB;
  forEachNode(*a.root, [&](const AbstractNode& node) {
    CHECK(node.hasDigest());
    digestsA.push_back(treeA.digest(node));
  });
  forEachNode(*b.root, [&](const AbstractNode& node) { digestsB.push_back(treeB.digest(node)); });
  CHECK(digestsA == digestsB);
}

TEST_CASE("A node class without its own hashContent() keys like no other node", "[digest]")
{
  struct Labelled : public GroupNode {
    using GroupNode::GroupNode;
    int label = 7;
  };
  const auto labelled = std::make_shared<Labelled>(nullptr, "labelled");
  const auto other = std::make_shared<Labelled>(nullptr, "labelled");
  const auto group = std::make_shared<GroupNode>(nullptr, "group");
  const Tree tree(group);
  CHECK(tree.digest(*labelled) != tree.digest(*other));
  CHECK(tree.digest(*labelled) != tree.digest(*group));
  CHECK(tree.digest(*labelled) == tree.digest(*labelled));
}

TEST_CASE("A deep copy carries its digests", "[digest]")
{
  const Evaluated evaluated = evaluate("union() { cube(1); translate([1, 0, 0]) sphere(1); }");
  const Tree tree(evaluated.root);
  const Hash128 digest = tree.digest(*evaluated.root);

  const auto copy = evaluated.root->clone();
  REQUIRE(copy);
  forEachNode(*copy, [](const AbstractNode& node) { CHECK(node.hasDigest()); });
  CHECK(Tree(copy).digest(*copy) == digest);

  // A copy alone has no children yet, and so no digest.
  const auto shallow = evaluated.root->children.front()->copy();
  REQUIRE(shallow);
  CHECK(!shallow->hasDigest());
}

TEST_CASE("A digest that reads a file's time lasts as long as the tree's root", "[digest]")
{
  const fs::path dir = fs::temp_directory_path() / "digest-test-files";
  fs::create_directories(dir);
  const fs::path dat = dir / "surface.dat";
  {
    std::ofstream out(dat);
    out << "1 2\n3 4\n";
  }
  const auto time = fs::last_write_time(dat);

  const Evaluated evaluated =
    evaluate("surface(\"" + dat.generic_string() + "\"); translate([5, 0, 0]) cube(1);");
  Tree tree(evaluated.root);
  const Hash128 before = tree.digest(*evaluated.root);
  const AbstractNode& surface = *evaluated.root->children.at(0);
  const AbstractNode& cube = *evaluated.root->children.at(1);
  CHECK(!surface.hasDigest());
  CHECK(!evaluated.root->hasDigest());
  CHECK(cube.hasDigest());

  fs::last_write_time(dat, time + std::chrono::seconds(10));
  // Read once per root...
  CHECK(tree.digest(*evaluated.root) == before);
  // ...and again with the next.
  tree.setRoot(evaluated.root);
  const Hash128 after = tree.digest(*evaluated.root);
  CHECK(after != before);
  CHECK(tree.digest(cube) == Tree(evaluated.root).digest(cube));

  fs::last_write_time(dat, time);
  tree.setRoot(evaluated.root);
  CHECK(tree.digest(*evaluated.root) == before);
  fs::remove_all(dir);
}

TEST_CASE("Threads computing one tree's digests at once agree", "[digest]")
{
  std::string script;
  for (int i = 0; i < 200; ++i) {
    script += "translate([" + std::to_string(i) + ", 0, 0]) group() { cube(1); sphere(1, $fn = 6); }\n";
  }
  const Evaluated reference = evaluate(script);
  const Hash128 expected = Tree(reference.root).digest(*reference.root);
  for (int round = 0; round < 5; ++round) {
    const Evaluated evaluated = evaluate(script);
    const Tree tree(evaluated.root);
    std::vector<Hash128> results(4);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < results.size(); ++t) {
      threads.emplace_back([&, t] { results[t] = tree.digest(*evaluated.root); });
    }
    for (auto& thread : threads) thread.join();
    for (const auto& result : results) CHECK(result == expected);
  }
}

TEST_CASE("A node knows whether a modifier is below it", "[digest]")
{
  const Evaluated evaluated = evaluate("union() { cube(1); group() { #sphere(1); } } cylinder(1, 1);");
  const Tree tree(evaluated.root);
  const AbstractNode& root = *evaluated.root;
  const AbstractNode& u = *root.children.at(0);
  const AbstractNode& group = *u.children.at(1);
  CHECK(tree.hasModifierBelow(root));
  CHECK(tree.hasModifierBelow(u));
  CHECK(tree.hasModifierBelow(group));
  CHECK(!tree.hasModifierBelow(*group.children.at(0)));
  CHECK(!tree.hasModifierBelow(*u.children.at(0)));
  CHECK(!tree.hasModifierBelow(*root.children.at(1)));
}
