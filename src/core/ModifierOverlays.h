#pragma once

// What the 3D view draws over the rendered result: the `#` (highlight) and `%` (background)
// subtrees, each in its world placement.
//
// F6 renders a `#` subtree as ordinary geometry and leaves a `%` subtree out. To show them, each is
// evaluated on its own and drawn translucent over the result. Nothing here needs a GL context.

#include <cstdint>
#include <memory>
#include <vector>

class AbstractNode;
class PolySet;
class Tree;

namespace overlay {

enum class Kind : std::uint8_t {
  Highlight,     // `#`: drawn pink, and part of the result
  Background,    // `%`: drawn grey, and not part of the result
  Interference,  // where two top-level parts overlap: drawn red, through the result
};

struct Mesh {
  Kind kind;
  // In world space, with triangular faces. A 2D shape is flat, in the plane its placement puts it.
  std::shared_ptr<const PolySet> polyset;
};

// The `#` and `%` subtrees of `root`, evaluated and placed in the world. Children of transforms,
// booleans, groups, color(), render(), hull() and the other whole-object operations, and physics()
// (at its simulated pose) are looked into. Children of extrusions, projection(), offset() and
// roof() are not: they are outlines that the node reshapes.
//
// A `#` subtree is drawn whole, so `#` inside it adds nothing; `%` inside either kind is left out of
// its geometry and drawn on its own. The root's own `%` is ignored, since F6 renders the root anyway.
//
// Evaluates the `%` subtrees, which F6 skips; the rest normally comes from the geometry cache.
// Exceptions from evaluation, such as cancellation or a hard warning, are passed on.
std::vector<Mesh> collect(const Tree& tree, const AbstractNode& root);

}  // namespace overlay
