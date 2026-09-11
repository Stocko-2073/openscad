#pragma once

// Static interference check shared by the GUI preview and the command line.
//
// Each direct child of the root node is treated as one "part". Pairs whose
// bounding boxes touch are confirmed with an exact Manifold intersection, and a
// pair interferes when the intersection volume exceeds kVolumeEps. Optionally,
// every colliding pair is attributed to the primitives (CSG leaves) that
// actually contribute volume to the overlap, each with an ancestor chain
// formatted like the 3D-view right-click picker menu.

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "geometry/linalg.h"

class AbstractNode;
class ManifoldGeometry;
class Tree;

namespace interference {

// Minimum overlap volume (model units^3) for a pair to count as interfering.
// Flush mating faces yield ~zero-volume intersections, so they stay below this.
constexpr double kVolumeEps = 1e-5;

// A node's source location, resolved for reporting. `valid` is false when the
// node has no source reference (e.g. implicit group nodes).
struct SourceRef {
  bool valid = false;
  std::string absPath;  // Location::fileName(): absolute, generic form
  std::string relFile;  // path relative to the document directory
  int line = 0;
  int column = 0;
  int endLine = 0;
  int endColumn = 0;
};

// One step of a picker-style ancestor chain.
struct ChainStep {
  int nodeIndex = 0;
  std::string name;   // picker display name (verbose_name without "module ")
  std::string label;  // "<name> (<file>:<line>)" as the picker menu shows it
  SourceRef loc;
};

// A CSG leaf primitive that contributes volume to a collision.
struct Primitive {
  int part = 0;  // part number the primitive belongs to
  int nodeIndex = 0;
  std::string name;
  std::string description;  // AbstractNode::toString()
  SourceRef loc;
  double volume = 0.0;           // volume of primitive ∩ overlap region
  std::vector<ChainStep> chain;  // outermost (part) first, primitive last
};

enum class PartStatus : std::uint8_t {
  Checked,
  SkippedNull,
  SkippedBackground,  // % modifier
  SkippedEmpty,
  Skipped2D,
  SkippedNotManifold,
};

const char *partStatusName(PartStatus status);

struct Part {
  int number = 0;  // 1-based position among the root's children
  PartStatus status = PartStatus::SkippedNull;
  std::shared_ptr<const AbstractNode> node;
  std::string name;
  std::string description;
  SourceRef loc;
  BoundingBox bbox;
  std::shared_ptr<const ManifoldGeometry> manifold;
};

struct Collision {
  int partA = 0;
  int partB = 0;
  double volume = 0.0;
  std::vector<Primitive> primitives;
};

struct Report {
  std::vector<Part> parts;
  int pairsTested = 0;  // pairs that survived the bounding-box cull
  std::vector<Collision> collisions;

  [[nodiscard]] int checkedParts() const;
  [[nodiscard]] const Part *part(int number) const;
};

struct Options {
  // Absolute, generic-form path of the main file (as Location::fileName()
  // reports it). When non-empty, chain steps from any other file, from library
  // files, or without a source reference are dropped, mirroring the GUI's
  // "restrict right-click menu to current file" setting.
  std::string currentFile;
  // Attribute collisions to the primitives that overlap and build their
  // chains. The GUI only needs the pair result, so it turns this off.
  bool primitives = true;
};

// Runs the check over the direct children of tree.root().
Report run(const Tree& tree, const Options& opts);

// Logs one Warning per colliding pair plus an Echo summary, exactly as the GUI
// console has always shown them.
void logReport(const Report& report, const Tree& tree);

// Node indices of every node inside a colliding part (used to recolor leaves).
std::unordered_set<int> conflictingNodeIndices(const Report& report);

// Writes the agent-oriented JSON document (pretty-printed, trailing newline).
void writeJson(const Report& report, const Tree& tree, const Options& opts, std::ostream& out);

// Picker helpers, kept identical to MainWindow::rightClick().
std::string pickerDisplayName(const AbstractNode& node);
bool isCurrentFileStep(const AbstractNode& node, const std::string& currentFile);
SourceRef sourceRef(const AbstractNode& node, const Tree& tree);

}  // namespace interference
