#pragma once

// Static interference check shared by the GUI render (F6) and the command line.

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "geometry/linalg.h"

class AbstractNode;
class ManifoldGeometry;
class Tree;

namespace interference {

// Overlap volume (model units^3) a pair must exceed to interfere; flush mating faces stay below it.
constexpr double kVolumeEps = 1e-5;

struct SourceRef {
  bool valid = false;
  std::string absPath;  // Location::fileName(): absolute, generic form
  std::string relFile;  // path relative to the document directory
  int line = 0;
  int column = 0;
  int endLine = 0;
  int endColumn = 0;
};

struct ChainStep {
  int nodeIndex = 0;
  std::string name;   // picker display name (verbose_name without "module ")
  std::string label;  // "<name> (<file>:<line>)" as the picker menu shows it
  SourceRef loc;
};

struct Primitive {
  int part = 0;
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
  std::shared_ptr<const ManifoldGeometry> overlap;  // where the parts overlap, for drawing
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
  // Location::fileName() of the main file. If set, chains keep only the steps located in it.
  std::string currentFile;
  // Attribute collisions to the primitives that overlap and build their chains.
  bool primitives = true;
};

// Runs the check over the direct children of tree.root().
Report run(const Tree& tree, const Options& opts);

// One Warning per colliding pair, then an Echo summary.
void logReport(const Report& report, const Tree& tree);

void writeJson(const Report& report, const Tree& tree, const Options& opts, std::ostream& out);

// Picker helpers, kept identical to MainWindow::pickerStepText().
std::string pickerDisplayName(const AbstractNode& node);
bool isCurrentFileStep(const AbstractNode& node, const std::string& currentFile);
SourceRef sourceRef(const AbstractNode& node, const Tree& tree);

}  // namespace interference
