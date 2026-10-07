#pragma once

/*
 * Geometry digests: the keys that the geometry caches keep a node's geometry under.
 *
 * A node's digest is a 128-bit hash of what GeometryEvaluator makes of its subtree: the node's
 * own data (AbstractNode::hashContent()) and its operands, each with its own # and % modifiers.
 * Nodes with equal digests have equal geometry. The digests replace text keys, the subtree's
 * text from a NodeDumper pass over the whole tree (Tree::getIdString(), kept for debugging),
 * which every refresh had to redo. A digest is computed once, from the node's own data and its
 * children's digests, and kept on the node, and the deep copies that the evaluation memo and
 * clone() make carry it (AbstractNode::takeDigest()), so a refresh pays only for its new nodes.
 *
 * Operands are what GeometryEvaluator combines: the children, except that a list stands for its
 * children, which take on its modifiers. As with the text keys,
 *  - a group whose only operand has no modifiers has that operand's digest, so the chains of
 *    groups that module calls make (BOSL2 makes them by the hundred thousand) key like what they
 *    hold, and modifiers on such a group are its operand's;
 *  - an empty group, having no geometry, is no operand of a group or of anything else that
 *    unions its operands, which most nodes do;
 *  - the root keys like its operand when it has one.
 * Unlike the text keys, which mixed up these different geometries,
 *  - an empty group is an operand of difference() and intersection(), whose result it empties,
 *    and a group of one modified operand, `group() %cube();`, is such an empty group, where the
 *    text `%cube();` stood for both. A modifier on an empty group no longer passes to the next
 *    operand either;
 *  - a group whose one non-empty child is a list of several operands is one operand, their union,
 *    where the text inlined the list; and the root (a list, with lazy union), a group (a union)
 *    and a list as the root (a list) of several operands each key apart;
 *  - numbers are hashed exactly, where the text had six significant digits, so that nodes
 *    differing beyond those shared an entry and got each other's geometry.
 * The text also marked every node below a %- or #-list, where the list's modifiers now go to its
 * own operands only, and keyed `group() { <empty list> cube(); }` apart from `cube();`, which
 * changed no geometry.
 *
 * import() and surface() hash their file's modification time, read when the digest is computed.
 * Such a digest, and that of every node above one, is kept by the Tree rather than the node, and
 * the Tree drops it with its root, so that each refresh reads the time anew, as the text keys did.
 *
 * Safe to use from several threads: two that compute a digest at once compute the same one.
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/Hash128.h"

class AbstractNode;

class NodeDigests
{
public:
  NodeDigests() = default;
  NodeDigests(const NodeDigests&) = delete;
  NodeDigests& operator=(const NodeDigests&) = delete;

  // The digest of `node`'s subtree, computed if need be.
  Hash128 digest(const AbstractNode& node);
  // Whether a node below `node`, not `node` itself, has a # or % modifier.
  bool hasModifierBelow(const AbstractNode& node);
  // Whether a hull(), minkowski(), resize(), fill() or physics() node is below `node`, not `node`
  // itself: the nodes that the picker names whole (pick::isWhole()).
  bool hasWholeBelow(const AbstractNode& node);
  // Forgets the digests that include a file's modification time.
  void clear();

private:
  struct Info {
    Hash128 digest;
    bool modifierBelow{false};
    bool wholeBelow{false};
    bool readsFiles{false};
  };
  struct Operand {
    uint64_t modifiers;
    Hash128 digest;
  };

  Info info(const AbstractNode& node);
  std::optional<Info> lookup(const AbstractNode& node);
  void store(const AbstractNode& node, const Info& info);
  Info compute(const AbstractNode& root);
  Info combine(const AbstractNode& node, std::vector<Operand>& operands);
  void addOperands(const AbstractNode& node, uint64_t inherited, std::vector<Operand>& operands);

  std::mutex mutex;
  std::atomic<bool> anyReadFiles{false};
  std::unordered_map<const AbstractNode *, std::pair<std::weak_ptr<const AbstractNode>, Info>> readFiles;
};
