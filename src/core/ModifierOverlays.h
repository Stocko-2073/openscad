#pragma once

// What the 3D view draws over the rendered result.

#include <cstdint>
#include <memory>
#include <vector>

#include "geometry/linalg.h"
#include "utils/Hash128.h"

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
  // Meshes with equal identities are equal; zero is none.
  Hash128 identity;
  // For the picker: the node this is the geometry of (AbstractNode::idx; -1 for an interference
  // overlap), and the matrix placing that node's parent.
  int index = -1;
  Transform3d matrix = Transform3d::Identity();
};

// The `#` and `%` subtrees of `root`, not looking into extrusions, projection(), offset() or
// roof(), whose children are outlines they reshape. `#` inside `#` adds nothing; `%` inside either
// is drawn on its own; the root's own `%` is ignored. Throws what evaluation throws.
std::vector<Mesh> collect(const Tree& tree, const AbstractNode& root);

}  // namespace overlay
