#include "core/NodeDigest.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeinfo>
#include <vector>

#include "Feature.h"
#include "core/CsgOpNode.h"
#include "core/ModuleInstantiation.h"
#include "core/PickAttribution.h"
#include "core/enums.h"
#include "core/node.h"
#include "io/fileutils.h"

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kBackground = 1;
constexpr uint64_t kHighlight = 2;

// The first word of each kind of digest's input, so that no two kinds hash the same words.
constexpr uint64_t kNodeTag = 0x6e6f6465'00000001ULL;
constexpr uint64_t kGroupTag = 0x67726f75'00000002ULL;
constexpr uint64_t kRootTag = 0x726f6f74'00000003ULL;
constexpr uint64_t kListTag = 0x6c697374'00000004ULL;
constexpr uint64_t kUniqueTag = 0x756e6971'00000005ULL;

// AbstractNode::digest_state
constexpr uint8_t kKept = 1;
constexpr uint8_t kModifierBelow = 2;
constexpr uint8_t kWholeBelow = 4;

uint64_t modifiers(const AbstractNode& node)
{
  if (!node.modinst) return 0;
  return (node.modinst->isBackground() ? kBackground : 0) |
         (node.modinst->isHighlight() ? kHighlight : 0);
}

enum class Kind { Node, Group, Root, List };

// Subclasses of these are nodes of their own, which hashContent() refuses unless they have one.
Kind kindOf(const AbstractNode& node)
{
  const auto& type = typeid(node);
  if (type == typeid(GroupNode)) return Kind::Group;
  if (type == typeid(RootNode)) return Kind::Root;
  if (type == typeid(ListNode)) return Kind::List;
  return Kind::Node;
}

// Whether an operand without geometry changes the result: difference() of nothing minus
// something, or intersection() with nothing, is nothing. The other operations skip it.
bool takesEmptyOperands(const AbstractNode& node)
{
  if (const auto *csg = dynamic_cast<const CsgOpNode *>(&node)) {
    return csg->type == OpenSCADOperator::DIFFERENCE || csg->type == OpenSCADOperator::INTERSECTION;
  }
  return dynamic_cast<const AbstractIntersectionNode *>(&node) != nullptr;
}

const Hash128& emptyGroup()
{
  static const Hash128 digest = [] {
    Hasher128 h;
    h.u64(kGroupTag);
    h.u64(0);
    return h.finish();
  }();
  return digest;
}

}  // namespace

void NodeHasher::fileTime(const std::string& path)
{
  this->files = true;
  u64(static_cast<uint64_t>(fs_timestamp(fs::path(path))));
}

void AbstractNode::takeDigest(const AbstractNode& original)
{
  // A list's own modifiers are part of its digest, and the original's may not be read: the memo
  // copies nodes whose parse is gone (core/EvalMemo.h). A list's digest is cheap to recompute.
  if (kindOf(*this) == Kind::List) return;
  const uint8_t state = original.digest_state.load(std::memory_order_acquire);
  if (!(state & kKept)) return;
  this->digest_a.store(original.digest_a.load(std::memory_order_relaxed), std::memory_order_relaxed);
  this->digest_b.store(original.digest_b.load(std::memory_order_relaxed), std::memory_order_relaxed);
  this->digest_state.store(state, std::memory_order_release);
}

Hash128 NodeDigests::digest(const AbstractNode& node)
{
  return info(node).digest;
}

bool NodeDigests::hasModifierBelow(const AbstractNode& node)
{
  return info(node).modifierBelow;
}

bool NodeDigests::hasWholeBelow(const AbstractNode& node)
{
  return info(node).wholeBelow;
}

void NodeDigests::clear()
{
  const std::lock_guard<std::mutex> lock(this->mutex);
  this->readFiles.clear();
  this->anyReadFiles.store(false, std::memory_order_release);
}

NodeDigests::Info NodeDigests::info(const AbstractNode& node)
{
  if (const auto known = lookup(node)) return *known;
  return compute(node);
}

std::optional<NodeDigests::Info> NodeDigests::lookup(const AbstractNode& node)
{
  const uint8_t state = node.digest_state.load(std::memory_order_acquire);
  if (state & kKept) {
    return Info{{node.digest_a.load(std::memory_order_relaxed), node.digest_b.load(std::memory_order_relaxed)},
                (state & kModifierBelow) != 0, (state & kWholeBelow) != 0, false};
  }
  if (!this->anyReadFiles.load(std::memory_order_acquire)) return std::nullopt;
  const std::lock_guard<std::mutex> lock(this->mutex);
  const auto it = this->readFiles.find(&node);
  // A node freed since may have left its address to this one.
  if (it == this->readFiles.end() || it->second.first.lock().get() != &node) return std::nullopt;
  return it->second.second;
}

void NodeDigests::store(const AbstractNode& node, const Info& info)
{
  if (!info.readsFiles) {
    node.digest_a.store(info.digest.a, std::memory_order_relaxed);
    node.digest_b.store(info.digest.b, std::memory_order_relaxed);
    node.digest_state.store(
      kKept | (info.modifierBelow ? kModifierBelow : 0) | (info.wholeBelow ? kWholeBelow : 0),
      std::memory_order_release);
    return;
  }
  const std::lock_guard<std::mutex> lock(this->mutex);
  this->readFiles[&node] = {node.weak_from_this(), info};
  this->anyReadFiles.store(true, std::memory_order_release);
}

// Children first, without recursion: trees can be deeper than a thread's stack allows.
NodeDigests::Info NodeDigests::compute(const AbstractNode& root)
{
  struct Frame {
    const AbstractNode *node;
    size_t next;
  };
  std::vector<Frame> stack{{&root, 0}};
  std::vector<Operand> operands;
  Info result;
  while (!stack.empty()) {
    Frame& top = stack.back();
    if (top.next < top.node->children.size()) {
      const AbstractNode *child = top.node->children[top.next++].get();
      if (!lookup(*child)) stack.push_back({child, 0});
      continue;
    }
    result = combine(*top.node, operands);
    store(*top.node, result);
    stack.pop_back();
  }
  return result;
}

NodeDigests::Info NodeDigests::combine(const AbstractNode& node, std::vector<Operand>& operands)
{
  Info result;
  operands.clear();
  for (const auto& child : node.children) {
    const Info known = info(*child);
    result.readsFiles |= known.readsFiles;
    result.modifierBelow |= known.modifierBelow || modifiers(*child) != 0;
    result.wholeBelow |= known.wholeBelow || pick::isWhole(*child);
    addOperands(*child, 0, operands);
  }
  const auto dropEmpty = [&operands]() {
    operands.erase(std::remove_if(operands.begin(), operands.end(),
                                  [](const Operand& operand) { return operand.digest == emptyGroup(); }),
                   operands.end());
  };
  // An unmodified operand on its own is what a group of it amounts to.
  const auto single = [&operands]() { return operands.size() == 1 && operands.front().modifiers == 0; };

  NodeHasher h;
  switch (kindOf(node)) {
  case Kind::Group:
    dropEmpty();
    if (single()) {
      result.digest = operands.front().digest;
      return result;
    }
    h.u64(kGroupTag);
    break;
  case Kind::Root:
    dropEmpty();
    if (single()) {
      result.digest = operands.front().digest;
      return result;
    }
    h.u64(kRootTag);
    // Lazy union makes the root's operands a list rather than their union.
    h.u64(Feature::ExperimentalLazyUnion.is_enabled());
    break;
  case Kind::List: {
    // As the root, which is the only place a list is evaluated as itself.
    dropEmpty();
    const uint64_t own = modifiers(node);
    if (own == 0 && single()) {
      result.digest = operands.front().digest;
      return result;
    }
    h.u64(kListTag);
    h.u64(own);
    break;
  }
  case Kind::Node:
    h.u64(kNodeTag);
    if (!node.hashContent(h)) {
      // A class that cannot say what it holds: a digest of its own, shared by no other node.
      static std::atomic<uint64_t> unique{0};
      Hasher128 u;
      u.u64(kUniqueTag);
      u.u64(unique.fetch_add(1, std::memory_order_relaxed));
      result.digest = u.finish();
      return result;
    }
    result.readsFiles |= h.readFiles();
    if (!takesEmptyOperands(node)) dropEmpty();
    break;
  }
  h.u64(operands.size());
  for (const auto& operand : operands) {
    h.u64(operand.modifiers);
    h.h(operand.digest);
  }
  result.digest = h.finish();
  return result;
}

// What `node` adds to its parent's operands, `inherited` being the modifiers of the lists it is in.
void NodeDigests::addOperands(const AbstractNode& node, uint64_t inherited, std::vector<Operand>& operands)
{
  const uint64_t all = inherited | modifiers(node);
  if (kindOf(node) == Kind::List) {
    for (const auto& child : node.children) addOperands(*child, all, operands);
  } else {
    operands.push_back({all, info(node).digest});
  }
}
