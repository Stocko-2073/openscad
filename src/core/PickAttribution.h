#pragma once

// Geometric attribution for the 3D-view right-click picker.
//
// The rendered result is one mesh, which hides the primitives it was made from. Given the ray
// through the clicked pixel, this finds where it meets the result and names the primitives whose
// faces pass through that point. A face cut by difference() belongs to the primitive that cut it.
// Nothing here needs a GL context.

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "geometry/linalg.h"
#include "utils/Hash128.h"

class AbstractNode;
class Geometry;
class GeometryEvaluator;
class PolySet;
class Tree;

namespace pick {

// A world-space ray: t = 0 on the near clipping plane, t = 1 on the far plane.
struct Ray {
  Vector3d origin;
  Vector3d direction;
};

// A mesh as drawn, placed by `matrix`. Its faces are triangles or convex polygons.
struct PlacedMesh {
  std::shared_ptr<const PolySet> polyset;
  Transform3d matrix = Transform3d::Identity();
};

// A primitive that went into a surface.
struct Leaf {
  int index = 0;  // AbstractNode::idx
  PlacedMesh mesh;
  BoundingBox bbox;  // world space
  // Cuts material away rather than adding it: it is subtracted by an odd number of difference()s.
  // (a - (b - c) = a - b + a∩c, so c adds material again.)
  bool subtracted = false;
};

// Where a ray meets a surface.
struct SurfaceHit {
  double t = 0;
  Vector3d point;
  Vector3d normal;  // outward, unit length
};

// Whether the picker names `node` whole: hull(), minkowski(), resize(), fill() and physics(). The
// others are looked through, or are primitives.
bool isWhole(const AbstractNode& node);

// Möller–Trumbore, either winding. The ray parameter of the hit, if the ray meets the triangle.
std::optional<double> intersectTriangle(const Vector3d& origin, const Vector3d& direction,
                                        const Vector3d& a, const Vector3d& b, const Vector3d& c);

// The point of triangle abc closest to p.
Vector3d closestPointOnTriangle(const Vector3d& p, const Vector3d& a, const Vector3d& b,
                                const Vector3d& c);

// The geometry of hull(), minkowski(), resize(), fill() and physics() nodes, by geometry digest
// (core/NodeDigest.h). Null for one that has none.
using WholeGeometry = std::unordered_map<Hash128, std::shared_ptr<const Geometry>, Hash128Hash>;

// The primitives that make up `node`, placed in the world by `matrix` (node's own transform).
// Groups, modules, booleans, color() and render() are looked through. Primitives, extrusions,
// imports, hull(), minkowski(), resize(), fill() and physics() stay whole. `%` subtrees below `node`
// are left out, as from F6; node's own `%` or `#` is ignored. Only 3D primitives are kept. Logs
// nothing and never throws on hard warnings. Unless `evaluateWhole`, hull() and the like are taken
// from `held` or the geometry cache only, so a right-click never re-runs one.
std::vector<Leaf> collectLeaves(const Tree& tree, const AbstractNode& node, const Transform3d& matrix,
                                bool evaluateWhole = false, const WholeGeometry *held = nullptr);

// The geometry of the hull() and other nodes that collectLeaves() takes whole below `root`, for a
// picker to hold for as long as it shows root's geometry: the geometry cache drops what renders do
// not use, and a render that finds a subtree in the cache uses nothing inside it. Each comes from
// `previous` where it has it, from the cache, or is evaluated (with `evaluator`, which must be of
// root's tree) if the cache has dropped it. Logs nothing and never throws on hard warnings.
WholeGeometry holdWholeGeometry(GeometryEvaluator& evaluator, const AbstractNode& root,
                                const WholeGeometry *previous);

// The 3D meshes of an F6 result. 2D parts are skipped.
std::vector<PlacedMesh> surfaceOf(const std::shared_ptr<const Geometry>& geom);

// Where `ray` first meets `surface`.
std::optional<SurfaceHit> castRay(const std::vector<PlacedMesh>& surface, const Ray& ray);

// Node indices of the primitives whose faces make `surface` where `ray` meets it, best first:
// primitives adding material before ones that cut it away, then in source order. Empty when the ray
// misses or no primitive matches.
std::vector<int> attribute(const std::vector<PlacedMesh>& surface, const std::vector<Leaf>& leaves,
                           const Ray& ray);

}  // namespace pick
