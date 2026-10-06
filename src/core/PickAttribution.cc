#include "core/PickAttribution.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/CgalAdvNode.h"
#include "core/ModuleInstantiation.h"
#include "core/NodeVisitor.h"
#include "core/State.h"
#include "core/TransformNode.h"
#include "core/node.h"
#ifdef ENABLE_PHYSICS
#include "core/PhysicsNode.h"
#endif
#include "geometry/Geometry.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/PolySet.h"
#include "geometry/PolySetUtils.h"
#include "geometry/linalg.h"
#include "utils/printutils.h"

namespace pick {

namespace {

// A triangle this flat (|ab × ac| against |ab||ac|) is skipped, and a ray this close to a triangle's
// plane misses it.
constexpr double kDegenerate = 1e-12;
// Barycentric slack, so a ray through a shared edge hits both triangles.
constexpr double kEdgeSlack = 1e-9;

// How close a primitive's face must come to the hit point, and how parallel to the hit face.
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

// Faces we can split into triangle fans: triangles, or the convex polygons of a convex PolySet.
// GeometryEvaluator::evaluateGeometry() applies the same rule before handing out a mesh.
std::shared_ptr<const PolySet> drawable(const std::shared_ptr<const PolySet>& ps)
{
  if (ps->isTriangular() || bool(ps->convexValue())) return ps;
  return PolySetUtils::tessellate_faces(*ps);
}

// A mesh's vertices in world space. A mirroring matrix turns its faces inside out, which
// `orientation` undoes for their normals.
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

// Calls f(i, j, k) with the vertex indices of each triangle of `ps`, splitting faces into fans.
template <typename F>
void forEachTriangle(const PolySet& ps, F&& f)
{
  for (const auto& face : ps.indices) {
    for (size_t i = 1; i + 1 < face.size(); ++i) f(face[0], face[i], face[i + 1]);
  }
}

// Whether triangle abc lies wholly to one side of the box [lo, hi] along some axis. Cheap enough
// to run on every triangle before the exact tests; kept to plain doubles for debug builds.
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

// What one primitive offers at the hit point.
struct Match {
  int index;
  bool parallel;     // has a face through the point, parallel to the hit face
  bool material;     // that face points the way the surface does (it adds material there)
  double alignment;  // best |cos| between a face through the point and the hit face
};

std::vector<Match> matchesAt(const std::vector<Leaf>& leaves, const SurfaceHit& hit,
                             const Tolerance& tolerance)
{
  const Vector3d pad = Vector3d::Constant(tolerance.distance);
  const Vector3d lo = hit.point - pad, hi = hit.point + pad;
  const BoundingBox probe(lo, hi);
  std::vector<Match> matches;
  for (const auto& leaf : leaves) {
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
    if (near) matches.push_back({leaf.index, parallel, material, alignment});
  }

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
      return x.index < y.index;
    });
  } else {
    std::stable_sort(matches.begin(), matches.end(), [](const Match& x, const Match& y) {
      if (x.alignment != y.alignment) return x.alignment > y.alignment;
      return x.index < y.index;
    });
  }
  return matches;
}

// Walks a subtree the way GeometryEvaluator builds it, collecting the primitives that end up in
// its geometry.
class LeafCollector : public NodeVisitor
{
public:
  LeafCollector(const Tree& tree, const AbstractNode& start) : start(start), evaluator(tree) {}

  Response visit(State& state, const AbstractNode& node) override
  {
    if (state.isPrefix() && isBackground(node)) return Response::PruneTraversal;
    return Response::ContinueTraversal;
  }

  Response visit(State& state, const TransformNode& node) override
  {
    if (state.isPrefix()) {
      if (isBackground(node)) return Response::PruneTraversal;
      // Removed from the geometry, as in GeometryEvaluator.
      if (matrix_contains_infinity(node.matrix) || matrix_contains_nan(node.matrix)) {
        return Response::PruneTraversal;
      }
      state.setMatrix(state.matrix() * node.matrix);
    }
    return Response::ContinueTraversal;
  }

  Response visit(State& state, const AbstractPolyNode& node) override
  {
    return addLeaf(state, node, false);
  }
  Response visit(State& state, const CgalAdvNode& node) override { return addLeaf(state, node, true); }
#ifdef ENABLE_PHYSICS
  // The simulated pose is baked into the geometry, so it is placed by its parent's matrix.
  Response visit(State& state, const PhysicsNode& node) override { return addLeaf(state, node, true); }
#endif

  std::vector<Leaf> leaves;

private:
  // `%` subtrees are not part of the geometry. The start node's own modifier is ignored, so a
  // `%render()` drawn in preview can still be looked into.
  [[nodiscard]] bool isBackground(const AbstractNode& node) const
  {
    return &node != &this->start && node.modinst && node.modinst->isBackground();
  }

  Response addLeaf(const State& state, const AbstractNode& node, bool cachedOnly)
  {
    if (!state.isPrefix()) return Response::ContinueTraversal;
    if (isBackground(node)) return Response::PruneTraversal;
    // A hull() or physics() can take long to evaluate; a right-click must not re-run one the
    // cache has dropped.
    if (!cachedOnly || this->evaluator.isSmartCached(node)) {
      const auto ps =
        std::dynamic_pointer_cast<const PolySet>(this->evaluator.evaluateGeometry(node, false));
      if (ps && !ps->isEmpty() && ps->getDimension() == 3) {
        Leaf leaf;
        leaf.index = node.index();
        leaf.mesh = {drawable(ps), state.matrix()};
        leaf.bbox = worldBox(leaf.mesh);
        this->leaves.push_back(std::move(leaf));
      }
    }
    return Response::PruneTraversal;
  }

  const AbstractNode& start;
  GeometryEvaluator evaluator;
};

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

std::optional<double> intersectTriangle(const Vector3d& origin, const Vector3d& direction,
                                        const Vector3d& a, const Vector3d& b, const Vector3d& c)
{
  const Vector3d e1 = b - a, e2 = c - a;
  const Vector3d p = direction.cross(e2);
  const double det = e1.dot(p);
  // Parallel to the triangle's plane, or a degenerate triangle.
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

std::vector<Leaf> collectLeaves(const Tree& tree, const AbstractNode& node, const Transform3d& matrix)
{
  const PrintSuppressGuard quiet;
  LeafCollector collector(tree, node);
  State state(nullptr);
  state.setMatrix(matrix);
  collector.traverse(node, state);
  return std::move(collector.leaves);
}

std::vector<PlacedMesh> surfaceOf(const std::shared_ptr<const Geometry>& geom)
{
  const PrintSuppressGuard quiet;
  std::vector<PlacedMesh> surface;
  appendSurface(geom, surface);
  return surface;
}

std::optional<SurfaceHit> castRay(const std::vector<PlacedMesh>& surface, const Ray& ray)
{
  std::optional<SurfaceHit> best;
  double bestT = std::numeric_limits<double>::infinity();
  if (!(ray.direction.squaredNorm() > 0)) return best;
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
      if (!t || *t < 0 || *t > 1 || *t >= bestT) return;
      bestT = *t;
      best = SurfaceHit{*t, ray.origin + *t * ray.direction,
                        world.orientation * (b - a).cross(c - a).normalized()};
    });
  }
  return best;
}

std::vector<int> attribute(const std::vector<PlacedMesh>& surface, const std::vector<Leaf>& leaves,
                           const Ray& ray)
{
  const auto hit = castRay(surface, ray);
  if (!hit) return {};

  BoundingBox box;
  for (const auto& mesh : surface) box.extend(worldBox(mesh));
  const double maxCoordinate =
    std::max({maxAbsCoordinate(hit->point), maxAbsCoordinate(box.min()), maxAbsCoordinate(box.max())});
  const Tolerance strict = strictTolerance(maxCoordinate, box.diagonal().norm());

  auto matches = matchesAt(leaves, *hit, strict);
  if (matches.empty()) matches = matchesAt(leaves, *hit, lossyTolerance(maxCoordinate, strict));

  std::vector<int> indices;
  indices.reserve(matches.size());
  for (const auto& match : matches) indices.push_back(match.index);
  return indices;
}

}  // namespace pick
