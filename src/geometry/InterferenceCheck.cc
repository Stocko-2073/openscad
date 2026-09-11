#include "geometry/InterferenceCheck.h"

#include <cmath>
#include <deque>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/AST.h"
#include "core/CSGNode.h"
#include "core/CSGTreeEvaluator.h"
#include "core/ModuleInstantiation.h"
#include "core/Tree.h"
#include "core/enums.h"
#include "core/node.h"
#include "core/parsersettings.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/manifold/ManifoldGeometry.h"
#include "geometry/manifold/manifoldutils.h"
#include "io/fileutils.h"
#include "json/json.hpp"
#include "utils/printutils.h"
#include "version.h"

namespace fs = std::filesystem;

namespace interference {

namespace {

double manifoldVolume(const ManifoldGeometry& geom)
{
  return geom.isEmpty() ? 0.0 : geom.getManifold().Volume();
}

// Collects the node indices of an entire subtree. CSGLeaf::index records the
// index of the leaf primitive node, so to recolor a whole top-level part we need
// the indices of all its descendants.
void collectSubtreeIndices(const std::shared_ptr<const AbstractNode>& node,
                           std::unordered_set<int>& out)
{
  if (!node) return;
  out.insert(node->index());
  for (const auto& child : node->getChildren()) collectSubtreeIndices(child, out);
}

// Gathers the CSG leaves that add material to a term. A leaf is subtracted
// when it sits on the right side of an odd number of DIFFERENCE ancestors
// (a - (b - c) = a - b + a∩c, so c is positive again); everything else,
// including both sides of an intersection, is positive.
void collectPositiveLeaves(const std::shared_ptr<CSGNode>& term, bool negative,
                           std::vector<std::shared_ptr<CSGLeaf>>& out)
{
  if (!term) return;
  if (auto leaf = std::dynamic_pointer_cast<CSGLeaf>(term)) {
    if (!negative && leaf->polyset && !leaf->isEmptySet() && leaf->index != 0) {
      out.push_back(leaf);
    }
    return;
  }
  if (auto op = std::dynamic_pointer_cast<CSGOperation>(term)) {
    collectPositiveLeaves(op->left(), negative, out);
    const bool flips = op->getType() == OpenSCADOperator::DIFFERENCE;
    collectPositiveLeaves(op->right(), negative != flips, out);
  }
}

std::shared_ptr<const AbstractNode> findNode(const std::shared_ptr<const AbstractNode>& root,
                                             int index)
{
  std::deque<std::shared_ptr<const AbstractNode>> path;
  return root->getNodeByID(index, path);
}

// Builds the picker-style chain for `nodeIndex` inside `part`: the node itself,
// its ancestors, up to and including the top-level part, filtered to the
// current file when requested. Returned outermost first.
std::vector<ChainStep> buildChain(const Part& part, int nodeIndex, const Tree& tree,
                                  const Options& opts)
{
  std::vector<ChainStep> chain;
  std::deque<std::shared_ptr<const AbstractNode>> path;
  if (!part.node->getNodeByID(nodeIndex, path)) return chain;

  // getNodeByID appends ancestors while unwinding, so path[0] is the primitive
  // and path.back() is the part node. Walk it in reverse for outermost-first.
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    const auto& step = *it;
    if (step->name() == "root") continue;
    if (!isCurrentFileStep(*step, opts.currentFile)) continue;
    ChainStep cs;
    cs.nodeIndex = step->index();
    cs.name = pickerDisplayName(*step);
    cs.loc = sourceRef(*step, tree);
    if (cs.loc.valid) {
      cs.label = STR(cs.name, " (", fs::path(cs.loc.absPath).filename().string(), ":", cs.loc.line,
                     ")");
    } else {
      cs.label = STR(cs.name, " (no source reference)");
    }
    chain.push_back(std::move(cs));
  }
  return chain;
}

// Attributes a collision to the primitives of each part that overlap the pair's
// intersection region. Terms and per-leaf manifolds are memoized across
// collisions so a part involved in several pairs is only evaluated once.
class PrimitiveAttributor
{
public:
  PrimitiveAttributor(const Tree& tree, GeometryEvaluator& geomevaluator, const Options& opts)
    : tree_(tree), csgevaluator_(tree, &geomevaluator), opts_(opts)
  {
  }

  void attribute(const Part& part, const ManifoldGeometry& overlap, Collision& collision)
  {
    for (const auto& leaf : positiveLeaves(part)) {
      const auto& leafManifold = worldManifold(leaf);
      if (!leafManifold || leafManifold->isEmpty()) continue;
      if (!leafManifold->getBoundingBox().intersects(overlap.getBoundingBox())) continue;
      const double vol = manifoldVolume(*leafManifold * overlap);
      if (vol <= kVolumeEps) continue;

      Primitive prim;
      prim.part = part.number;
      prim.nodeIndex = leaf->index;
      prim.volume = vol;
      if (const auto node = findNode(part.node, leaf->index)) {
        prim.name = pickerDisplayName(*node);
        prim.description = node->toString();
        prim.loc = sourceRef(*node, tree_);
      } else {
        prim.name = leaf->label;
      }
      prim.chain = buildChain(part, leaf->index, tree_, opts_);
      collision.primitives.push_back(std::move(prim));
    }
  }

private:
  const std::vector<std::shared_ptr<CSGLeaf>>& positiveLeaves(const Part& part)
  {
    auto it = leavesByPart_.find(part.number);
    if (it == leavesByPart_.end()) {
      std::vector<std::shared_ptr<CSGLeaf>> leaves;
      // Top-level parts are direct children of the root, so the identity
      // matrix the traversal starts from is the true world frame.
      collectPositiveLeaves(csgevaluator_.buildCSGTree(*part.node), false, leaves);
      it = leavesByPart_.emplace(part.number, std::move(leaves)).first;
    }
    return it->second;
  }

  const std::shared_ptr<const ManifoldGeometry>& worldManifold(const std::shared_ptr<CSGLeaf>& leaf)
  {
    auto it = manifoldByLeaf_.find(leaf->index);
    if (it == manifoldByLeaf_.end()) {
      std::shared_ptr<ManifoldGeometry> mani = ManifoldUtils::createManifoldFromPolySet(*leaf->polyset);
      if (mani) mani->transform(leaf->matrix);
      it = manifoldByLeaf_.emplace(leaf->index, std::move(mani)).first;
    }
    return it->second;
  }

  const Tree& tree_;
  CSGTreeEvaluator csgevaluator_;
  const Options& opts_;
  std::unordered_map<int, std::vector<std::shared_ptr<CSGLeaf>>> leavesByPart_;
  std::unordered_map<int, std::shared_ptr<const ManifoldGeometry>> manifoldByLeaf_;
};

double roundVolume(double v) { return std::round(v * 1e6) / 1e6; }

nlohmann::ordered_json locationJson(const SourceRef& loc)
{
  if (!loc.valid) return nullptr;
  return {
    {"file", loc.relFile},   {"path", loc.absPath},       {"line", loc.line},
    {"column", loc.column},  {"end_line", loc.endLine},   {"end_column", loc.endColumn},
  };
}

nlohmann::ordered_json bboxJson(const BoundingBox& bbox)
{
  return {
    {"min", {bbox.min().x(), bbox.min().y(), bbox.min().z()}},
    {"max", {bbox.max().x(), bbox.max().y(), bbox.max().z()}},
  };
}

}  // namespace

const char *partStatusName(PartStatus status)
{
  switch (status) {
  case PartStatus::Checked: return "checked";
  case PartStatus::SkippedNull: return "skipped-null";
  case PartStatus::SkippedBackground: return "skipped-background";
  case PartStatus::SkippedEmpty: return "skipped-empty";
  case PartStatus::Skipped2D: return "skipped-2d";
  case PartStatus::SkippedNotManifold: return "skipped-not-manifold";
  }
  return "unknown";
}

int Report::checkedParts() const
{
  int n = 0;
  for (const auto& part : parts) {
    if (part.status == PartStatus::Checked) ++n;
  }
  return n;
}

const Part *Report::part(int number) const
{
  for (const auto& part : parts) {
    if (part.number == number) return &part;
  }
  return nullptr;
}

std::string pickerDisplayName(const AbstractNode& node)
{
  // Remove the "module" prefix if any, as it induces confusion between the
  // module declaration and the instantiation (same rule as the picker menu).
  const std::string vname = node.verbose_name();
  const size_t first_position = (vname.find("module") == std::string::npos) ? 0 : 7;
  if (!vname.empty()) return vname.size() > first_position ? vname.substr(first_position) : "";
  // verbose_name can be empty (e.g. for-loop groups); fall back to the
  // instantiation's name so the entry isn't blank.
  if (node.modinst) return node.modinst->name().str();
  return "?";
}

bool isCurrentFileStep(const AbstractNode& node, const std::string& currentFile)
{
  if (currentFile.empty()) return true;
  const bool hasSourceRef = node.modinst && !node.modinst->location().isNone();
  if (!hasSourceRef) return false;
  const auto& location = node.modinst->location();
  if (!get_library_for_path(location.filePath()).empty()) return false;
  return location.fileName() == currentFile;
}

SourceRef sourceRef(const AbstractNode& node, const Tree& tree)
{
  SourceRef ref;
  if (!node.modinst || node.modinst->location().isNone()) return ref;
  const auto& location = node.modinst->location();
  ref.valid = true;
  ref.absPath = location.fileName();
  try {
    ref.relFile = fs_uncomplete(location.filePath(), tree.getDocumentPath()).generic_string();
  } catch (const fs::filesystem_error&) {
    ref.relFile = location.filePath().filename().generic_string();
  }
  ref.line = location.firstLine();
  ref.column = location.firstColumn();
  ref.endLine = location.lastLine();
  ref.endColumn = location.lastColumn();
  return ref;
}

Report run(const Tree& tree, const Options& opts)
{
  Report report;
  const auto& root = tree.root();
  if (!root) return report;

  GeometryEvaluator geomevaluator(tree);

  int number = 0;
  for (const auto& child : root->getChildren()) {
    Part part;
    part.number = ++number;
    part.node = child;
    if (!child) {
      part.status = PartStatus::SkippedNull;
      report.parts.push_back(std::move(part));
      continue;
    }
    part.name = pickerDisplayName(*child);
    part.description = child->toString();
    part.loc = sourceRef(*child, tree);
    if (child->modinst && child->modinst->isBackground()) {  // skip % ghosts
      part.status = PartStatus::SkippedBackground;
      report.parts.push_back(std::move(part));
      continue;
    }
    auto geom = geomevaluator.evaluateGeometry(*child, true);
    if (!geom || geom->isEmpty()) {
      part.status = PartStatus::SkippedEmpty;
      report.parts.push_back(std::move(part));
      continue;
    }
    if (geom->getDimension() != 3) {
      part.status = PartStatus::Skipped2D;
      report.parts.push_back(std::move(part));
      continue;
    }
    auto mani = ManifoldUtils::createManifoldFromGeometry(geom);
    if (!mani || mani->isEmpty()) {
      part.status = PartStatus::SkippedNotManifold;
      report.parts.push_back(std::move(part));
      continue;
    }
    part.status = PartStatus::Checked;
    part.bbox = geom->getBoundingBox();
    part.manifold = std::move(mani);
    report.parts.push_back(std::move(part));
  }

  std::unique_ptr<PrimitiveAttributor> attributor;
  if (opts.primitives) {
    attributor = std::make_unique<PrimitiveAttributor>(tree, geomevaluator, opts);
  }

  const auto& parts = report.parts;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].status != PartStatus::Checked) continue;
    for (size_t j = i + 1; j < parts.size(); ++j) {
      if (parts[j].status != PartStatus::Checked) continue;
      if (!parts[i].bbox.intersects(parts[j].bbox)) continue;  // cheap cull
      ++report.pairsTested;
      const ManifoldGeometry overlap = *parts[i].manifold * *parts[j].manifold;
      const double vol = manifoldVolume(overlap);
      if (vol <= kVolumeEps) continue;  // flush faces / numerical noise
      Collision collision;
      collision.partA = parts[i].number;
      collision.partB = parts[j].number;
      collision.volume = vol;
      if (attributor) {
        attributor->attribute(parts[i], overlap, collision);
        attributor->attribute(parts[j], overlap, collision);
      }
      report.collisions.push_back(std::move(collision));
    }
  }
  return report;
}

void logReport(const Report& report, const Tree& tree)
{
  for (const auto& collision : report.collisions) {
    const Part *a = report.part(collision.partA);
    const Part *b = report.part(collision.partB);
    if (!a || !b) continue;
    const Location locA = a->node && a->node->modinst ? a->node->modinst->location() : Location::NONE;
    const Location locB = b->node && b->node->modinst ? b->node->modinst->location() : Location::NONE;
    LOG(message_group::Warning, locA, tree.getDocumentPath(), "%1$s",
        STR("Interference: part ", a->number, " (line ", locA.firstLine(), ") overlaps part ",
            b->number, " (line ", locB.firstLine(), "), overlap volume = ", collision.volume));
  }
  LOG(message_group::Echo, "%1$s",
      STR("Interference check: ", report.checkedParts(), " part(s), ", report.collisions.size(),
          " overlapping pair(s)."));
}

std::unordered_set<int> conflictingNodeIndices(const Report& report)
{
  std::unordered_set<int> conflicting;
  std::unordered_set<int> numbers;
  for (const auto& collision : report.collisions) {
    numbers.insert(collision.partA);
    numbers.insert(collision.partB);
  }
  for (const int number : numbers) {
    if (const Part *part = report.part(number)) collectSubtreeIndices(part->node, conflicting);
  }
  return conflicting;
}

void writeJson(const Report& report, const Tree& tree, const Options& opts, std::ostream& out)
{
  using json = nlohmann::ordered_json;
  json doc;
  doc["schema_version"] = 1;
  doc["generator"] = {{"name", "openscad"}, {"version", openscad_versionnumber}};
  doc["input"] = {
    {"path", opts.currentFile},
    {"directory", tree.getDocumentPath()},
  };
  doc["settings"] = {
    {"volume_epsilon", kVolumeEps},
    {"chain_filter", opts.currentFile.empty() ? "none" : "current-file-only"},
    {"chain_order", "outermost-first"},
  };
  doc["summary"] = {
    {"parts_total", report.parts.size()},
    {"parts_checked", report.checkedParts()},
    {"pairs_tested", report.pairsTested},
    {"collisions", report.collisions.size()},
    {"has_interference", !report.collisions.empty()},
  };

  json parts = json::array();
  for (const auto& part : report.parts) {
    json p;
    p["number"] = part.number;
    p["status"] = partStatusName(part.status);
    p["name"] = part.name;
    p["description"] = part.description;
    p["location"] = locationJson(part.loc);
    if (part.status == PartStatus::Checked) p["bbox"] = bboxJson(part.bbox);
    parts.push_back(std::move(p));
  }
  doc["parts"] = std::move(parts);

  json collisions = json::array();
  for (const auto& collision : report.collisions) {
    json c;
    c["parts"] = {collision.partA, collision.partB};
    c["volume"] = roundVolume(collision.volume);
    json prims = json::array();
    for (const auto& prim : collision.primitives) {
      json pj;
      pj["part"] = prim.part;
      pj["node_index"] = prim.nodeIndex;
      pj["name"] = prim.name;
      pj["description"] = prim.description;
      pj["location"] = locationJson(prim.loc);
      pj["volume"] = roundVolume(prim.volume);
      json chain = json::array();
      for (const auto& step : prim.chain) {
        json sj;
        sj["node_index"] = step.nodeIndex;
        sj["name"] = step.name;
        sj["label"] = step.label;
        sj["location"] = locationJson(step.loc);
        chain.push_back(std::move(sj));
      }
      pj["chain"] = std::move(chain);
      prims.push_back(std::move(pj));
    }
    c["primitives"] = std::move(prims);
    collisions.push_back(std::move(c));
  }
  doc["collisions"] = std::move(collisions);

  out << doc.dump(2) << "\n";
}

}  // namespace interference
