#include "core/ModifierOverlays.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/ModuleInstantiation.h"
#include "core/NodeVisitor.h"
#include "core/State.h"
#include "core/TransformNode.h"
#include "core/Tree.h"
#include "core/node.h"
#ifdef ENABLE_PHYSICS
#include "core/PhysicsNode.h"
#include "geometry/physics/PhysicsSimulator.h"
#endif
#include "geometry/Geometry.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySet.h"
#include "geometry/PolySetUtils.h"
#include "geometry/Polygon2d.h"
#include "geometry/linalg.h"

namespace overlay {

namespace {

bool isBackground(const AbstractNode& node)
{
  return node.modinst && node.modinst->isBackground();
}
bool isHighlight(const AbstractNode& node)
{
  return node.modinst && node.modinst->isHighlight();
}

[[maybe_unused]] bool hasModifierBelow(const AbstractNode& node)
{
  for (const auto& child : node.getChildren()) {
    if (isBackground(*child) || isHighlight(*child) || hasModifierBelow(*child)) return true;
  }
  return false;
}

// A subtree to draw. `matrix` places the node's parent: evaluateGeometry() applies the node's own
// transform.
struct Target {
  Kind kind;
  std::shared_ptr<const AbstractNode> node;
  Transform3d matrix;
};

// Walks the tree the way GeometryEvaluator places it, collecting the `#` and `%` subtrees.
class TargetCollector : public NodeVisitor
{
public:
  TargetCollector(const Tree& tree, const AbstractNode& root, GeometryEvaluator& evaluator)
    : tree(tree), root(root), evaluator(evaluator)
  {
  }

  Response visit(State& state, const AbstractNode& node) override { return enter(state, node, true); }
  Response visit(State& state, const AbstractPolyNode& node) override
  {
    return enter(state, node, false);
  }

  Response visit(State& state, const TransformNode& node) override
  {
    // Removed from the geometry, as in GeometryEvaluator.
    if (state.isPrefix() &&
        (matrix_contains_infinity(node.matrix) || matrix_contains_nan(node.matrix))) {
      return Response::PruneTraversal;
    }
    const Response response = enter(state, node, true);
    if (state.isPrefix()) state.setMatrix(state.matrix() * node.matrix);
    return response;
  }

#ifdef ENABLE_PHYSICS
  // The simulated pose is baked into the node's geometry, so the node is placed by its parent's
  // matrix and its children follow the pose.
  Response visit(State& state, const PhysicsNode& node) override
  {
    const Response response = enter(state, node, true);
    if (state.isPrefix() && hasModifierBelow(node)) {
      const std::string key = this->tree.getIdString(node);
      Transform3d pose;
      // Simulating publishes the pose. F6 has normally done that already.
      if (!physicsTransformCacheLookup(key, pose)) this->evaluator.evaluateGeometry(node, false);
      if (physicsTransformCacheLookup(key, pose)) state.setMatrix(state.matrix() * pose);
    }
    return response;
  }
#endif

  std::vector<Target> targets;

private:
  Response enter(const State& state, const AbstractNode& node, bool lookInside)
  {
    if (state.isPostfix()) {
      if (!this->open.empty() && this->open.back() == &node) this->open.pop_back();
      return Response::ContinueTraversal;
    }
    if (isBackground(node) && &node != &this->root) {
      record(Kind::Background, node, state.matrix());
    } else if (isHighlight(node) && this->open.empty()) {
      record(Kind::Highlight, node, state.matrix());
    }
    return lookInside ? Response::ContinueTraversal : Response::PruneTraversal;
  }

  void record(Kind kind, const AbstractNode& node, const Transform3d& matrix)
  {
    this->targets.push_back({kind, node.shared_from_this(), matrix});
    this->open.push_back(&node);
  }

  const Tree& tree;
  const AbstractNode& root;
  GeometryEvaluator& evaluator;
  std::vector<const AbstractNode *> open;  // targets being traversed, innermost last
};

void appendMeshes(const std::shared_ptr<const Geometry>& geom, Kind kind, const Transform3d& matrix,
                  std::vector<Mesh>& out)
{
  if (!geom || geom->isEmpty()) return;
  if (const auto list = std::dynamic_pointer_cast<const GeometryList>(geom)) {
    for (const auto& item : list->getChildren()) appendMeshes(item.second, kind, matrix, out);
    return;
  }
  std::unique_ptr<PolySet> ps;
  if (const auto polygon = std::dynamic_pointer_cast<const Polygon2d>(geom)) {
    ps = polygon->tessellate();
  } else if (const auto mesh = PolySetUtils::getGeometryAsPolySet(geom)) {
    ps = mesh->isTriangular() ? std::make_unique<PolySet>(*mesh) : PolySetUtils::tessellate_faces(*mesh);
  }
  if (!ps || ps->isEmpty()) return;
  ps->transform(matrix);
  out.push_back({kind, std::move(ps)});
}

void appendTarget(GeometryEvaluator& evaluator, const AbstractNode& node, Kind kind,
                  const Transform3d& matrix, std::vector<Mesh>& out)
{
  // GeometryEvaluator skips a list given as the root when it is `%`, so evaluate its children one by
  // one. Those that are `%` themselves were collected on their own.
  if (dynamic_cast<const ListNode *>(&node)) {
    for (const auto& child : node.getChildren()) {
      if (!isBackground(*child)) appendTarget(evaluator, *child, kind, matrix, out);
    }
    return;
  }
  appendMeshes(evaluator.evaluateGeometry(node, false), kind, matrix, out);
}

}  // namespace

std::vector<Mesh> collect(const Tree& tree, const AbstractNode& root)
{
  GeometryEvaluator evaluator(tree);
  TargetCollector collector(tree, root, evaluator);
  collector.traverse(root);

  std::vector<Mesh> meshes;
  for (const auto& target : collector.targets) {
    appendTarget(evaluator, *target.node, target.kind, target.matrix, meshes);
  }
  return meshes;
}

}  // namespace overlay
