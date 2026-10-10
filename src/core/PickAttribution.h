#pragma once

// The right-click picker: names the primitives whose faces make the rendered surface where the
// clicked pixel's ray meets it. A face cut by difference() belongs to the primitive that cut it.

#include <cstdint>
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

// Faces are triangles or convex polygons.
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
  int solid = -1;  // into LeafTree::solids
};

// A node of the collected subtree, as far as where it has a surface goes.
struct Solid {
  enum class Op : std::uint8_t {
    Union,  // also groups, transforms, color() and the like
    Intersection,
    Difference,  // the first operand less the others
    List,        // a lazy union's top level, drawn as it is
    Leaf,
    Unknown,  // a leaf the picker has no geometry for
  };
  Op op = Op::Union;
  int parent = -1;            // into the solids; none for the node collected
  std::vector<int> operands;  // into the solids
  int leaf = -1;              // into the leaves, for a Leaf
  BoundingBox bbox;           // world space
};

struct LeafTree {
  std::vector<Leaf> leaves;
  std::vector<Solid> solids;
};

struct SurfaceHit {
  double t = 0;
  Vector3d point;
  Vector3d normal;  // outward, unit length
};

// True for hull(), minkowski(), resize(), fill() and physics(), which the picker names whole.
bool isWhole(const AbstractNode& node);

// Möller–Trumbore, either winding.
std::optional<double> intersectTriangle(const Vector3d& origin, const Vector3d& direction,
                                        const Vector3d& a, const Vector3d& b, const Vector3d& c);

Vector3d closestPointOnTriangle(const Vector3d& p, const Vector3d& a, const Vector3d& b,
                                const Vector3d& c);

// The isWhole() nodes' geometry by digest; null for one without geometry.
using WholeGeometry = std::unordered_map<Hash128, std::shared_ptr<const Geometry>, Hash128Hash>;

// The 3D primitives that make up `node`, and how its subtree combines them; `matrix` places node's
// parent. `%` subtrees below `node` are left out; its own modifier is ignored. Logs nothing and
// never throws on hard warnings. Unless `evaluateWhole`, isWhole() nodes come only from `held` or
// the geometry cache.
LeafTree collectLeaves(const Tree& tree, const AbstractNode& node, const Transform3d& matrix,
                       bool evaluateWhole = false, const WholeGeometry *held = nullptr);

// The isWhole() nodes' geometry below `root`, `%` subtrees included, for the picker to hold while
// it shows root's geometry and overlays: the cache drops what renders do not use, and a render that
// finds a subtree cached uses nothing inside it. Reuses `previous`; `evaluator` must be of root's
// tree. Logs nothing and never throws on hard warnings.
WholeGeometry holdWholeGeometry(GeometryEvaluator& evaluator, const AbstractNode& root,
                                const WholeGeometry *previous);

// 2D parts are skipped.
std::vector<PlacedMesh> surfaceOf(const std::shared_ptr<const Geometry>& geom);

// Every point where `ray` meets `surface`, nearest first.
std::vector<SurfaceHit> castRay(const std::vector<PlacedMesh>& surface, const Ray& ray);

struct Crossing {
  SurfaceHit hit;
  std::optional<size_t> overlay;  // into the overlays; none for the surface
};

// Every point where `ray` meets `surface` or one of `overlays`, the meshes drawn translucent over
// it, nearest first. Of those that coincide, overlays come first, as they are drawn over the surface.
std::vector<Crossing> crossings(const std::vector<PlacedMesh>& surface,
                                const std::vector<PlacedMesh>& overlays, const Ray& ray);

// Node indices of the leaves whose faces make `surface` at `hit`, best first: those adding material
// before those cutting it away, then in source order. A leaf's face counts only where the nodes
// above it keep it on their surface, so not where an intersection() clips the leaf away.
std::vector<int> attribute(const std::vector<PlacedMesh>& surface, const LeafTree& leaves,
                           const SurfaceHit& hit);

}  // namespace pick
