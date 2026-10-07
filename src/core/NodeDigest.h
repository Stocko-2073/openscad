#pragma once

/*
 * Geometry digests key the geometry caches: equal digests mean equal geometry. A digest hashes a
 * node's own data (hashContent()) and its operands as GeometryEvaluator combines them, modifiers
 * included. Deep copies carry it (AbstractNode::takeDigest()). One that includes a file's
 * modification time (import(), surface()), and every one above it, lives on the Tree and is
 * recomputed each refresh. Concurrent computations of a digest agree.
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

  Hash128 digest(const AbstractNode& node);
  bool hasModifierBelow(const AbstractNode& node);
  // Whether pick::isWhole() holds for a node below `node`.
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
