#include "core/PickAttribution.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Feature.h"
#include "core/CgalAdvNode.h"
#include "core/CsgOpNode.h"
#include "core/ModuleInstantiation.h"
#include "core/NodeVisitor.h"
#include "core/State.h"
#include "core/TransformNode.h"
#include "core/Tree.h"
#include "core/node.h"
#ifdef ENABLE_PHYSICS
#include "core/PhysicsNode.h"
#endif
#include "geometry/Geometry.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySet.h"
#include "geometry/PolySetUtils.h"
#include "geometry/linalg.h"
#include "utils/Hash128.h"
#include "utils/printutils.h"

namespace pick {

namespace {

// A triangle this flat (|ab × ac| against |ab||ac|) is skipped, and a ray this close to a triangle's
// plane misses it.
constexpr double kDegenerate = 1e-12;
// Barycentric slack, so a ray through a shared edge hits both triangles.
constexpr double kEdgeSlack = 1e-9;

struct Tolerance {
  double distance;
  double parallel;  // minimum |cos| between the face normals
};

// Manifold copies input vertices into its result unchanged and only collapses edges within
// 1e-12 of the coordinates, so faces of the result lie on their primitives' faces up to rounding.
Tolerance strictTolerance(double maxCoordinate, double diagonal)
{
  return {std::max(5e-8, 1e-9 * std::max(maxCoordinate, diagonal)), 1 - 1e-6};
}

// CGAL snaps its input to a 2^-20 grid and hands back float coordinates; Manifold's repair of a
// broken mesh snaps too. Tried only when the strict tolerance finds nothing.
Tolerance lossyTolerance(double maxCoordinate, const Tolerance& strict)
{
  return {std::max(strict.distance, 4e-6 + 2.5e-7 * maxCoordinate), std::cos(1e-2)};
}

// Faces that split into triangle fans: triangles, or the convex polygons of a convex PolySet.
std::shared_ptr<const PolySet> drawable(const std::shared_ptr<const PolySet>& ps)
{
  if (ps->isTriangular() || bool(ps->convexValue())) return ps;
  return PolySetUtils::tessellate_faces(*ps);
}

// A mirroring matrix turns the faces inside out, which `orientation` undoes for their normals.
class WorldVertices
{
public:
  explicit WorldVertices(const PlacedMesh& mesh)
    : orientation(mesh.matrix.linear().determinant() < 0 ? -1.0 : 1.0)
  {
    if (mesh.matrix.matrix().isIdentity(0)) {
      this->vertices = &mesh.polyset->vertices;
    } else {
      this->moved.reserve(mesh.polyset->vertices.size());
      for (const auto& v : mesh.polyset->vertices) this->moved.push_back(mesh.matrix * v);
      this->vertices = &this->moved;
    }
  }

  const Vector3d& operator[](size_t i) const { return (*this->vertices)[i]; }
  [[nodiscard]] size_t size() const { return this->vertices->size(); }

  const double orientation;

private:
  std::vector<Vector3d> moved;
  const std::vector<Vector3d> *vertices;
};

template <typename F>
void forEachTriangle(const PolySet& ps, F&& f)
{
  for (const auto& face : ps.indices) {
    for (size_t i = 1; i + 1 < face.size(); ++i) f(face[0], face[i], face[i + 1]);
  }
}

// A cheap pre-test for every triangle, on plain doubles for the sake of debug builds.
bool outsideBox(const double *a, const double *b, const double *c, const double *lo, const double *hi)
{
  for (int k = 0; k < 3; ++k) {
    if (a[k] > hi[k] && b[k] > hi[k] && c[k] > hi[k]) return true;
    if (a[k] < lo[k] && b[k] < lo[k] && c[k] < lo[k]) return true;
  }
  return false;
}

BoundingBox worldBox(const PlacedMesh& mesh)
{
  return mesh.matrix * mesh.polyset->getBoundingBox();
}

// Whether the ray, for t in [0, 1], passes within `pad` of `box`.
bool rayMeetsBox(const Ray& ray, const BoundingBox& box, double pad)
{
  if (box.isEmpty()) return false;
  double t0 = 0, t1 = 1;
  for (int i = 0; i < 3; ++i) {
    const double lo = box.min()[i] - pad, hi = box.max()[i] + pad;
    const double o = ray.origin[i], d = ray.direction[i];
    if (d == 0) {
      if (o < lo || o > hi) return false;
      continue;
    }
    double ta = (lo - o) / d, tb = (hi - o) / d;
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

double maxAbsCoordinate(const Vector3d& v)
{
  return v.cwiseAbs().maxCoeff();
}

// Strict and lossy, for a hit at `point` on meshes within `box`.
std::pair<Tolerance, Tolerance> tolerances(const Vector3d& point, const BoundingBox& box)
{
  const double maxCoordinate =
    std::max({maxAbsCoordinate(point), maxAbsCoordinate(box.min()), maxAbsCoordinate(box.max())});
  const Tolerance strict = strictTolerance(maxCoordinate, box.diagonal().norm());
  return {strict, lossyTolerance(maxCoordinate, strict)};
}

struct Match {
  const Leaf *leaf;
  bool parallel;     // has a face through the point, parallel to the hit face
  bool material;     // that face points the way the surface does (it adds material there)
  double alignment;  // best |cos| between a face through the point and the hit face
};

enum class Where : std::uint8_t { Outside, On, Inside, Unknown };

Where flip(Where where)
{
  if (where == Where::Inside) return Where::Outside;
  if (where == Where::Outside) return Where::Inside;
  return where;
}

// Van Oosterom and Strackee: the solid angle triangle abc subtends at the origin, signed by its
// winding, given the corners' distances from the origin.
double solidAngle(const double *a, const double *b, const double *c, double la, double lb, double lc)
{
  const double det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                     a[2] * (b[0] * c[1] - b[1] * c[0]);
  const double ab = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const double ac = a[0] * c[0] + a[1] * c[1] + a[2] * c[2];
  const double bc = b[0] * c[0] + b[1] * c[1] + b[2] * c[2];
  return 2 * std::atan2(det, la * lb * lc + ab * lc + ac * lb + bc * la);
}

// Where a point is against the solids of a LeafTree.
class Classifier
{
public:
  Classifier(const LeafTree& tree, const Vector3d& point, double tolerance)
    : tree(tree),
      point(point),
      tolerance(tolerance),
      probe(point - Vector3d::Constant(tolerance), point + Vector3d::Constant(tolerance))
  {
  }

  // Whether a face of `leaf` through the point is on the surface of the subtree collected, as
  // every node above the leaf still has a surface there.
  bool surfaces(const Leaf& leaf)
  {
    Where where = Where::On;
    for (int below = leaf.solid, i = this->tree.solids[below].parent; i >= 0;
         below = i, i = this->tree.solids[i].parent) {
      const Solid& solid = this->tree.solids[i];
      where = combine(solid, below, where);
      if (solid.op != Solid::Op::List && (where == Where::Inside || where == Where::Outside)) {
        return false;
      }
    }
    return true;
  }

private:
  Where of(int index)
  {
    const Solid& solid = this->tree.solids[index];
    if (!solid.bbox.intersects(this->probe)) return Where::Outside;
    if (solid.op == Solid::Op::Leaf) return of(this->tree.leaves[solid.leaf]);
    if (solid.op == Solid::Op::Unknown) return Where::Unknown;
    return combine(solid, -1, Where::Unknown);
  }

  // Where the point is against `solid`, given where it is against its operand `known`. A union is
  // outside where all its operands are, an intersection inside where all are, and a difference is
  // its first operand intersected with the others turned inside out.
  Where combine(const Solid& solid, int known, Where knownWhere)
  {
    if (solid.operands.empty()) return Where::Outside;
    const bool isUnion = solid.op == Solid::Op::Union || solid.op == Solid::Op::List;
    bool on = false, unknown = false;
    for (size_t i = 0; i < solid.operands.size(); ++i) {
      Where where = solid.operands[i] == known ? knownWhere : of(solid.operands[i]);
      if (isUnion || (solid.op == Solid::Op::Difference && i > 0)) where = flip(where);
      if (where == Where::Outside) return isUnion ? Where::Inside : Where::Outside;
      on = on || where == Where::On;
      unknown = unknown || where == Where::Unknown;
    }
    if (unknown) return Where::Unknown;
    if (on) return Where::On;
    return isUnion ? Where::Outside : Where::Inside;
  }

  // By the winding number, ±1 inside a closed mesh whichever way its faces turn. An affine map
  // keeps it, so it is counted in the mesh's own frame, placing only the faces near the point.
  Where of(const Leaf& leaf)
  {
    if (const auto it = this->classified.find(&leaf); it != this->classified.end()) return it->second;
    const Transform3d& matrix = leaf.mesh.matrix;
    // Squashed flat: no inside, and no faces in any result.
    if (!(std::abs(matrix.linear().determinant()) > 0)) {
      return this->classified[&leaf] = Where::Outside;
    }
    const Transform3d inverse = matrix.inverse();
    const Vector3d local = inverse * this->point;
    BoundingBox near;
    for (int corner = 0; corner < 8; ++corner) {
      near.extend(inverse * this->probe.corner(static_cast<BoundingBox::CornerType>(corner)));
    }

    const auto& vertices = leaf.mesh.polyset->vertices;
    std::vector<double> offsets(3 * vertices.size()), lengths(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
      double *offset = &offsets[3 * i];
      for (int k = 0; k < 3; ++k) offset[k] = vertices[i][k] - local[k];
      lengths[i] = std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
    }
    double angle = 0;
    bool on = false;
    forEachTriangle(*leaf.mesh.polyset, [&](int i, int j, int k) {
      if (on) return;
      if (!outsideBox(vertices[i].data(), vertices[j].data(), vertices[k].data(), near.min().data(),
                      near.max().data())) {
        const Vector3d a = matrix * vertices[i], b = matrix * vertices[j], c = matrix * vertices[k];
        const Vector3d ab = b - a, ac = c - a;
        if (ab.cross(ac).norm() > kDegenerate * ab.norm() * ac.norm() &&
            (closestPointOnTriangle(this->point, a, b, c) - this->point).norm() <= this->tolerance) {
          on = true;
          return;
        }
      }
      angle += solidAngle(&offsets[3 * i], &offsets[3 * j], &offsets[3 * k], lengths[i], lengths[j],
                          lengths[k]);
    });
    Where where = Where::On;
    if (!on) where = std::abs(angle) > 2 * M_PI ? Where::Inside : Where::Outside;
    return this->classified[&leaf] = where;
  }

  const LeafTree& tree;
  const Vector3d point;
  const double tolerance;
  const BoundingBox probe;  // around the point, by the tolerance
  std::unordered_map<const Leaf *, Where> classified;
};

std::vector<Match> matchesAt(const LeafTree& tree, const SurfaceHit& hit, const Tolerance& tolerance)
{
  const Vector3d pad = Vector3d::Constant(tolerance.distance);
  const Vector3d lo = hit.point - pad, hi = hit.point + pad;
  const BoundingBox probe(lo, hi);
  std::vector<Match> matches;
  for (const auto& leaf : tree.leaves) {
    if (!leaf.bbox.intersects(probe)) continue;
    bool near = false, parallel = false, material = false;
    double alignment = 0, nearestParallel = std::numeric_limits<double>::infinity();
    const WorldVertices world(leaf.mesh);
    forEachTriangle(*leaf.mesh.polyset, [&](int i, int j, int k) {
      const Vector3d &a = world[i], &b = world[j], &c = world[k];
      if (outsideBox(a.data(), b.data(), c.data(), lo.data(), hi.data())) return;
      const Vector3d ab = b - a, ac = c - a;
      const Vector3d n = ab.cross(ac);
      const double area = n.norm();
      if (!(area > kDegenerate * ab.norm() * ac.norm())) return;
      const double d = (closestPointOnTriangle(hit.point, a, b, c) - hit.point).norm();
      if (d > tolerance.distance) return;
      near = true;
      const double cosine = world.orientation * n.dot(hit.normal) / area;
      alignment = std::max(alignment, std::abs(cosine));
      if (std::abs(cosine) >= tolerance.parallel && d < nearestParallel) {
        nearestParallel = d;
        parallel = true;
        material = cosine > 0;
      }
    });
    if (near) matches.push_back({&leaf, parallel, material, alignment});
  }

  Classifier classifier(tree, hit.point, tolerance.distance);
  std::vector<Match> kept;
  std::copy_if(matches.begin(), matches.end(), std::back_inserter(kept),
               [&](const Match& m) { return classifier.surfaces(*m.leaf); });
  // None kept means the classifying erred, as on a mesh that isn't closed.
  if (!kept.empty()) matches = std::move(kept);

  // A face parallel to the hit face beats one that only touches it at an edge.
  const bool anyParallel =
    std::any_of(matches.begin(), matches.end(), [](const Match& m) { return m.parallel; });
  if (anyParallel) {
    matches.erase(
      std::remove_if(matches.begin(), matches.end(), [](const Match& m) { return !m.parallel; }),
      matches.end());
    // Where a face is both added and cut away, the one that adds it made the surface.
    std::stable_sort(matches.begin(), matches.end(), [](const Match& x, const Match& y) {
      if (x.material != y.material) return x.material;
      return x.leaf->index < y.leaf->index;
    });
  } else {
    std::stable_sort(matches.begin(), matches.end(), [](const Match& x, const Match& y) {
      if (x.alignment != y.alignment) return x.alignment > y.alignment;
      return x.leaf->index < y.leaf->index;
    });
  }
  return matches;
}

std::shared_ptr<const PolySet> meshOf(const std::shared_ptr<const Geometry>& geom)
{
  if (!geom || geom->getDimension() != 3) return nullptr;
  return PolySetUtils::getGeometryAsPolySet(geom);
}

// Walks a subtree the way GeometryEvaluator builds it.
class LeafCollector : public NodeVisitor
{
public:
  LeafCollector(const Tree& tree, const AbstractNode& start, bool evaluateWhole,
                const WholeGeometry *held)
    : tree(tree), start(start), evaluator(tree), evaluateWhole(evaluateWhole), held(held)
  {
  }

  Response visit(State& state, const AbstractNode& node) override
  {
    if (state.isPrefix()) {
      if (isBackground(node)) return Response::PruneTraversal;
      enter(state, node);
    }
    return Response::ContinueTraversal;
  }

  Response visit(State& state, const CsgOpNode& node) override
  {
    if (state.isPrefix() && !isBackground(node) && node.type == OpenSCADOperator::DIFFERENCE) {
      bool first = true;
      for (const auto *operand : operands(node)) {
        if (!first) this->cutters.insert(operand);
        first = false;
      }
    }
    return visit(state, static_cast<const AbstractNode&>(node));
  }

  Response visit(State& state, const TransformNode& node) override
  {
    if (state.isPrefix()) {
      if (isBackground(node)) return Response::PruneTraversal;
      // Removed from the geometry, as in GeometryEvaluator.
      if (matrix_contains_infinity(node.matrix) || matrix_contains_nan(node.matrix)) {
        return Response::PruneTraversal;
      }
      enter(state, node);
      state.setMatrix(state.matrix() * node.matrix);
    }
    return Response::ContinueTraversal;
  }

  Response visit(State& state, const AbstractPolyNode& node) override
  {
    return addLeaf(state, node, false);
  }
  // The classes isWhole() names, here and below.
  Response visit(State& state, const CgalAdvNode& node) override { return addLeaf(state, node, true); }
#ifdef ENABLE_PHYSICS
  // The simulated pose is baked into the geometry, so it is placed by its parent's matrix.
  Response visit(State& state, const PhysicsNode& node) override { return addLeaf(state, node, true); }
#endif

  LeafTree collected;

private:
  // The start node's own modifier is ignored, so a `%render()` can still be looked into.
  [[nodiscard]] bool isBackground(const AbstractNode& node) const
  {
    return &node != &this->start && node.modinst && node.modinst->isBackground();
  }

  std::vector<const AbstractNode *> operands(const AbstractNode& node) const
  {
    std::vector<const AbstractNode *> result;
    for (const auto& child : node.getChildren()) {
      if (isBackground(*child)) continue;
      if (dynamic_cast<const ListNode *>(child.get())) {
        const auto inner = operands(*child);
        result.insert(result.end(), inner.begin(), inner.end());
      } else {
        result.push_back(child.get());
      }
    }
    return result;
  }

  const std::shared_ptr<const Geometry> *heldGeometry(const AbstractNode& node) const
  {
    if (!this->held) return nullptr;
    const auto it = this->held->find(this->tree.digest(node));
    return it == this->held->end() ? nullptr : &it->second;
  }

  [[nodiscard]] Solid::Op opOf(const AbstractNode& node) const
  {
    if (const auto *op = dynamic_cast<const CsgOpNode *>(&node)) {
      if (op->type == OpenSCADOperator::INTERSECTION) return Solid::Op::Intersection;
      if (op->type == OpenSCADOperator::DIFFERENCE) return Solid::Op::Difference;
    }
    if (dynamic_cast<const AbstractIntersectionNode *>(&node)) return Solid::Op::Intersection;
    // GeometryEvaluator leaves these as a list.
    if (&node == &this->start && dynamic_cast<const ListNode *>(&node)) return Solid::Op::List;
    if (dynamic_cast<const RootNode *>(&node) && Feature::ExperimentalLazyUnion.is_enabled()) {
      return Solid::Op::List;
    }
    return Solid::Op::Union;
  }

  void enter(const State& state, const AbstractNode& node)
  {
    const auto parent = state.parent();
    bool subtracted = this->cutters.count(&node) > 0;
    if (parent) {
      if (const auto it = this->subtracted.find(parent.get()); it != this->subtracted.end()) {
        subtracted = subtracted != it->second;
      }
    }
    this->subtracted[&node] = subtracted;

    auto& solids = this->collected.solids;
    const int parentSolid = parent ? this->solidOf.at(parent.get()) : -1;
    // Its children are operands of its parent, as in GeometryEvaluator.
    if (&node != &this->start && dynamic_cast<const ListNode *>(&node)) {
      this->solidOf[&node] = parentSolid;
      return;
    }
    const int index = static_cast<int>(solids.size());
    solids.push_back({opOf(node), parentSolid});
    if (parentSolid >= 0) solids[parentSolid].operands.push_back(index);
    this->solidOf[&node] = index;
  }

  Response addLeaf(const State& state, const AbstractNode& node, bool whole)
  {
    if (!state.isPrefix()) return Response::ContinueTraversal;
    if (isBackground(node)) return Response::PruneTraversal;
    enter(state, node);
    Solid& solid = this->collected.solids[this->solidOf[&node]];
    std::shared_ptr<const PolySet> ps;
    if (!whole || this->evaluateWhole) {
      ps = meshOf(this->evaluator.evaluateGeometry(node, false));
    } else if (const auto *geom = heldGeometry(node)) {
      ps = meshOf(*geom);
    } else if (this->evaluator.isSmartCached(node)) {
      // Only a lookup: a right-click must not re-run a hull() or physics() the cache has dropped.
      ps = meshOf(this->evaluator.evaluateGeometry(node, false));
    } else {
      solid.op = Solid::Op::Unknown;
      // Anywhere, for all the picker knows.
      solid.bbox = BoundingBox(Vector3d::Constant(-std::numeric_limits<double>::infinity()),
                               Vector3d::Constant(std::numeric_limits<double>::infinity()));
    }
    if (ps && !ps->isEmpty()) {
      Leaf leaf;
      leaf.index = node.index();
      leaf.mesh = {drawable(ps), state.matrix()};
      leaf.bbox = worldBox(leaf.mesh);
      leaf.subtracted = this->subtracted[&node];
      leaf.solid = this->solidOf[&node];
      solid.op = Solid::Op::Leaf;
      solid.leaf = static_cast<int>(this->collected.leaves.size());
      solid.bbox = leaf.bbox;
      this->collected.leaves.push_back(std::move(leaf));
    }
    return Response::PruneTraversal;
  }

  const Tree& tree;
  const AbstractNode& start;
  GeometryEvaluator evaluator;
  bool evaluateWhole;
  const WholeGeometry *held;
  std::unordered_set<const AbstractNode *> cutters;  // operands after the first of a difference()
  std::unordered_map<const AbstractNode *, bool> subtracted;
  // The solid each node's children are operands of.
  std::unordered_map<const AbstractNode *, int> solidOf;
};

// The nodes below `root` that LeafCollector takes whole, by the same rules, `%` subtrees included,
// as the picker collects each of those for its overlay. A plain walk, as a visitor's per-node State
// costs far more.
std::vector<const AbstractNode *> wholeNodes(const Tree& tree, const AbstractNode& root)
{
  std::vector<const AbstractNode *> found;
  std::vector<const AbstractNode *> stack{&root};
  while (!stack.empty()) {
    const AbstractNode *node = stack.back();
    stack.pop_back();
    if (isWhole(*node)) {
      found.push_back(node);
      continue;
    }
    if (!tree.hasWholeBelow(*node) || dynamic_cast<const AbstractPolyNode *>(node)) continue;
    if (const auto *transform = dynamic_cast<const TransformNode *>(node)) {
      if (matrix_contains_infinity(transform->matrix) || matrix_contains_nan(transform->matrix)) {
        continue;
      }
    }
    for (const auto& child : node->children) stack.push_back(child.get());
  }
  return found;
}

void appendSurface(const std::shared_ptr<const Geometry>& geom, std::vector<PlacedMesh>& out)
{
  if (!geom || geom->isEmpty()) return;
  // getGeometryAsPolySet() would assert on a 2D item in a list, so walk lists here.
  if (const auto list = std::dynamic_pointer_cast<const GeometryList>(geom)) {
    for (const auto& item : list->getChildren()) appendSurface(item.second, out);
    return;
  }
  if (geom->getDimension() != 3) return;
  const auto ps = PolySetUtils::getGeometryAsPolySet(geom);
  if (ps && !ps->isEmpty()) out.push_back({drawable(ps), Transform3d::Identity()});
}

}  // namespace

bool isWhole(const AbstractNode& node)
{
  // By exact class, as neither has subclasses: the digests ask it of every node, and dynamic_cast
  // costs far more.
  const auto& type = typeid(node);
#ifdef ENABLE_PHYSICS
  if (type == typeid(PhysicsNode)) return true;
#endif
  return type == typeid(CgalAdvNode);
}

std::optional<double> intersectTriangle(const Vector3d& origin, const Vector3d& direction,
                                        const Vector3d& a, const Vector3d& b, const Vector3d& c)
{
  const Vector3d e1 = b - a, e2 = c - a;
  const Vector3d p = direction.cross(e2);
  const double det = e1.dot(p);
  if (!(std::abs(det) > kDegenerate * e1.norm() * e2.norm() * direction.norm())) return {};
  const double inv = 1.0 / det;
  const Vector3d s = origin - a;
  const double u = s.dot(p) * inv;
  if (u < -kEdgeSlack || u > 1 + kEdgeSlack) return {};
  const Vector3d q = s.cross(e1);
  const double v = direction.dot(q) * inv;
  if (v < -kEdgeSlack || u + v > 1 + kEdgeSlack) return {};
  return e2.dot(q) * inv;
}

// Ericson, Real-Time Collision Detection, 5.1.5.
Vector3d closestPointOnTriangle(const Vector3d& p, const Vector3d& a, const Vector3d& b,
                                const Vector3d& c)
{
  const Vector3d ab = b - a, ac = c - a, ap = p - a;
  const double d1 = ab.dot(ap), d2 = ac.dot(ap);
  if (d1 <= 0 && d2 <= 0) return a;

  const Vector3d bp = p - b;
  const double d3 = ab.dot(bp), d4 = ac.dot(bp);
  if (d3 >= 0 && d4 <= d3) return b;

  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));

  const Vector3d cp = p - c;
  const double d5 = ab.dot(cp), d6 = ac.dot(cp);
  if (d6 >= 0 && d5 <= d6) return c;

  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));

  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
    return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  }

  const double denom = 1 / (va + vb + vc);
  return a + ab * (vb * denom) + ac * (vc * denom);
}

LeafTree collectLeaves(const Tree& tree, const AbstractNode& node, const Transform3d& matrix,
                       bool evaluateWhole, const WholeGeometry *held)
{
  const PrintSuppressGuard quiet;
  LeafCollector collector(tree, node, evaluateWhole, held);
  State state(nullptr);
  state.setMatrix(matrix);
  collector.traverse(node, state);
  // A node's result lies within what its operands' do, and each solid comes after its parent.
  auto& solids = collector.collected.solids;
  for (size_t i = solids.size(); i-- > 0;) {
    if (solids[i].parent >= 0) solids[solids[i].parent].bbox.extend(solids[i].bbox);
  }
  return std::move(collector.collected);
}

WholeGeometry holdWholeGeometry(GeometryEvaluator& evaluator, const AbstractNode& root,
                                const WholeGeometry *previous)
{
  const PrintSuppressGuard quiet;
  const Tree& tree = evaluator.getTree();
  WholeGeometry held;
  for (const auto *node : wholeNodes(tree, root)) {
    const Hash128 key = tree.digest(*node);
    if (held.count(key)) continue;
    if (previous) {
      if (const auto it = previous->find(key); it != previous->end()) {
        held.emplace(key, it->second);
        continue;
      }
    }
    held.emplace(key, evaluator.evaluateGeometry(*node, true));
  }
  return held;
}

std::vector<PlacedMesh> surfaceOf(const std::shared_ptr<const Geometry>& geom)
{
  const PrintSuppressGuard quiet;
  std::vector<PlacedMesh> surface;
  appendSurface(geom, surface);
  return surface;
}

std::vector<SurfaceHit> castRay(const std::vector<PlacedMesh>& surface, const Ray& ray)
{
  std::vector<SurfaceHit> hits;
  if (!(ray.direction.squaredNorm() > 0)) return hits;
  // Two directions across the ray. A triangle the ray passes through has corners on both sides of
  // it along each, so most triangles are skipped after two dot products per vertex.
  const Vector3d along = ray.direction.normalized();
  const Vector3d across1 = along.unitOrthogonal(), across2 = along.cross(across1);
  const double *o = ray.origin.data(), *e1 = across1.data(), *e2 = across2.data();
  std::vector<double> u, v;
  for (const auto& mesh : surface) {
    const BoundingBox box = worldBox(mesh);
    const double pad = 1e-9 * (1 + box.diagonal().norm());
    if (!rayMeetsBox(ray, box, pad)) continue;
    const WorldVertices world(mesh);
    u.resize(world.size());
    v.resize(world.size());
    for (size_t i = 0; i < world.size(); ++i) {
      const double *p = world[i].data();
      const double x = p[0] - o[0], y = p[1] - o[1], z = p[2] - o[2];
      u[i] = x * e1[0] + y * e1[1] + z * e1[2];
      v[i] = x * e2[0] + y * e2[1] + z * e2[2];
    }
    forEachTriangle(*mesh.polyset, [&](int i, int j, int k) {
      if ((u[i] > pad && u[j] > pad && u[k] > pad) || (u[i] < -pad && u[j] < -pad && u[k] < -pad) ||
          (v[i] > pad && v[j] > pad && v[k] > pad) || (v[i] < -pad && v[j] < -pad && v[k] < -pad)) {
        return;
      }
      const Vector3d &a = world[i], &b = world[j], &c = world[k];
      const auto t = intersectTriangle(ray.origin, ray.direction, a, b, c);
      if (!t || !(*t >= 0 && *t <= 1)) return;
      hits.push_back({*t, ray.origin + *t * ray.direction,
                      world.orientation * (b - a).cross(c - a).normalized()});
    });
  }
  std::sort(hits.begin(), hits.end(),
            [](const SurfaceHit& x, const SurfaceHit& y) { return x.t < y.t; });
  return hits;
}

std::vector<Crossing> crossings(const std::vector<PlacedMesh>& surface,
                                const std::vector<PlacedMesh>& overlays, const Ray& ray)
{
  std::vector<Crossing> all;
  for (size_t i = 0; i < overlays.size(); ++i) {
    for (const auto& hit : castRay({overlays[i]}, ray)) all.push_back({hit, i});
  }
  for (const auto& hit : castRay(surface, ray)) all.push_back({hit, std::nullopt});
  std::sort(all.begin(), all.end(),
            [](const Crossing& x, const Crossing& y) { return x.hit.t < y.hit.t; });

  BoundingBox box;
  for (const auto& mesh : surface) box.extend(worldBox(mesh));
  for (const auto& mesh : overlays) box.extend(worldBox(mesh));
  for (auto group = all.begin(); group != all.end();) {
    const Vector3d nearest = group->hit.point;
    // Lossy, as an overlay is evaluated apart from the result, which may round it differently.
    const double coincide = tolerances(nearest, box).second.distance;
    const auto end = std::find_if(group, all.end(), [&](const Crossing& crossing) {
      return (crossing.hit.point - nearest).norm() > coincide;
    });
    std::stable_sort(group, end, [](const Crossing& x, const Crossing& y) {
      return x.overlay.has_value() && !y.overlay.has_value();
    });
    group = end;
  }
  return all;
}

std::vector<int> attribute(const std::vector<PlacedMesh>& surface, const LeafTree& leaves,
                           const SurfaceHit& hit)
{
  BoundingBox box;
  for (const auto& mesh : surface) box.extend(worldBox(mesh));
  const auto [strict, lossy] = tolerances(hit.point, box);

  auto matches = matchesAt(leaves, hit, strict);
  if (matches.empty()) matches = matchesAt(leaves, hit, lossy);

  std::vector<int> indices;
  indices.reserve(matches.size());
  for (const auto& match : matches) indices.push_back(match.leaf->index);
  return indices;
}

}  // namespace pick
