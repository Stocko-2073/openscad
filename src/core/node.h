#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <ostream>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "core/AST.h"
#include "core/BaseVisitable.h"
#include "core/ModuleInstantiation.h"
#include "utils/Hash128.h"

extern int progress_report_count;
extern void (*progress_report_f)(const std::shared_ptr<const AbstractNode>&, void *, int);
extern void *progress_report_vp;

void progress_report_prep(const std::shared_ptr<AbstractNode>& root,
                          void (*f)(const std::shared_ptr<const AbstractNode>& node, void *vp, int mark),
                          void *vp);
void progress_report_fin();

/*
 * What AbstractNode::hashContent() hashes a node's own data into, for its geometry digest
 * (core/NodeDigest.h).
 */
class NodeHasher : public Hasher128
{
public:
  // A file's modification time, read now. A digest that includes one is good only until the next
  // refresh, which may find the file changed.
  void fileTime(const std::string& path);
  [[nodiscard]] bool readFiles() const { return this->files; }

private:
  bool files{false};
};

/*!

   The node tree is the result of evaluation of a module instantiation
   tree.  Both the module tree and the node tree are regenerated from
   scratch for each compile.

 */
class AbstractNode : public BaseVisitable, public std::enable_shared_from_this<AbstractNode>
{
  // FIXME: the idx_counter/idx is mostly (only?) for debugging.
  // We can hash on pointer value or smth. else.
  //  -> remove and
  // use smth. else to display node identifier in CSG tree output?
  // Per thread: indices must be unique within a tree, and a tree is built on one thread. The
  // GUI resets its counter for each tree it builds while Animate's frame workers build theirs;
  // with one counter between them, a reset partway through a worker's tree numbered its nodes
  // again from 1, and GeometryEvaluator, which keys children by index, mixed them up.
  static thread_local size_t idx_counter;  // Node instantiation index
public:
  VISITABLE();
  AbstractNode(const ModuleInstantiation *mi);
  virtual std::string toString() const;
  /*! The 'OpenSCAD name' of this node, defaults to classname, but can be
      overloaded to provide specialization for e.g. CSG nodes, primitive nodes etc.
      Used for human-readable output. */
  virtual std::string name() const = 0;

  /*| When a more specific name for user interaction shall be used, such as module names,
      the verbose name shall be overloaded. */
  virtual std::string verbose_name() const { return this->name(); }

  const std::vector<std::shared_ptr<AbstractNode>>& getChildren() const { return this->children; }
  int index() const { return this->idx; }

  // Numbers the nodes that this thread makes next from 1.
  static void resetIndexCounter() { idx_counter = 1; }

  // FIXME: Make protected
  std::vector<std::shared_ptr<AbstractNode>> children;
  const ModuleInstantiation *modinst;

  // progress_mark is a running number used for progress indication
  // FIXME: Make all progress handling external, put it in the traverser class?
  int progress_mark{0};
  void progress_prepare();
  void progress_report() const;

  int idx;  // Node index (unique per tree)

  std::shared_ptr<const AbstractNode> getNodeByID(
    int idx, std::deque<std::shared_ptr<const AbstractNode>>& path) const;

  // returns the precise source code location associated with the node
  void getCodeLocation(int currentLevel, int includeLevel, int *firstLine, int *firstColumn,
                       int *lastLine, int *lastColumn, int nestedModuleDepth) const;

  void findNodesWithSameMod(const std::shared_ptr<const AbstractNode>& node_mod,
                            std::vector<std::shared_ptr<const AbstractNode>>& nodes) const;

  /*
   * A copy of this node without its children, under a fresh index, or null
   * if the node's class has no copy() of its own. Every concrete class
   * implements it as copyAs(*this); one that forgets inherits its base's,
   * which refuses rather than slicing the node to the base class.
   */
  [[nodiscard]] virtual std::shared_ptr<AbstractNode> copy() const = 0;
  // A deep copy of this subtree under fresh indices, or null if some node in it cannot be copied.
  [[nodiscard]] std::shared_ptr<AbstractNode> clone() const;

  /*
   * Hashes this node's own data for its geometry digest (core/NodeDigest.h): what toString()
   * writes, numbers exactly, and not the children. Every concrete class implements it next to its
   * toString(), first checking hashAs(*this): a subclass without a hashContent() of its own is
   * refused rather than hashed as its base, and its digest is then unlike any other node's.
   */
  [[nodiscard]] virtual bool hashContent(NodeHasher& h) const = 0;

  // Gives a copy the digest of the subtree it copies, which copy() does not: only a deep copy
  // whose children are copies of the original's may have it.
  void takeDigest(const AbstractNode& original);
  // Whether the node keeps its digest, computed or taken.
  [[nodiscard]] bool hasDigest() const;

protected:
  // What copy() copies: everything but the children, the index and the digest.
  AbstractNode(const AbstractNode& other);
  AbstractNode& operator=(const AbstractNode&) = delete;

  template <class T>
  static std::shared_ptr<AbstractNode> copyAs(const T& node)
  {
    if (typeid(node) != typeid(T)) return nullptr;
    return std::make_shared<T>(node);
  }

  // For hashContent(): false if `node` is of a subclass of T.
  template <class T>
  static bool hashAs(const T& node)
  {
    return typeid(node) == typeid(T);
  }

private:
  friend class NodeDigests;
  // The subtree's digest, kept once computed unless it depends on files (core/NodeDigest.h).
  // Atomic, since the GUI may ask for it while the render thread computes it.
  mutable std::atomic<uint64_t> digest_a{0};
  mutable std::atomic<uint64_t> digest_b{0};
  mutable std::atomic<uint8_t> digest_state{0};
};

class AbstractIntersectionNode : public AbstractNode
{
public:
  VISITABLE();
  AbstractIntersectionNode(const ModuleInstantiation *mi) : AbstractNode(mi) {}
  std::string toString() const override;
  std::string name() const override;
  std::shared_ptr<AbstractNode> copy() const override { return copyAs(*this); }
  bool hashContent(NodeHasher& h) const override;
};

class AbstractPolyNode : public AbstractNode
{
public:
  VISITABLE();
  AbstractPolyNode(const ModuleInstantiation *mi) : AbstractNode(mi) {}

};

/*!
   Used for organizing objects into lists which should not be grouped but merely
   unpacked by the parent node.
 */
class ListNode : public AbstractNode
{
public:
  VISITABLE();
  ListNode(const ModuleInstantiation *mi) : AbstractNode(mi) {}
  std::string name() const override;
  std::shared_ptr<AbstractNode> copy() const override { return copyAs(*this); }
  // Nothing of its own: its digest is that of its children (core/NodeDigest.h).
  bool hashContent(NodeHasher&) const override { return hashAs(*this); }
};

/*!
   Logically groups objects together. Used as a way of passing
   objects around without having to perform unions on them.
 */
class GroupNode : public AbstractNode
{
public:
  VISITABLE();
  GroupNode(const ModuleInstantiation *mi, std::string name = "")
    : AbstractNode(mi), _name(std::move(name))
  {
  }
  std::string name() const override;
  std::string verbose_name() const override;
  std::shared_ptr<AbstractNode> copy() const override { return copyAs(*this); }
  // Nothing of its own: its digest is that of its children (core/NodeDigest.h).
  bool hashContent(NodeHasher&) const override { return hashAs(*this); }

private:
  const std::string _name;
};

/*!
   Only instantiated once, for the top-level file.
 */
class RootNode : public GroupNode
{
public:
  VISITABLE();
  RootNode() : GroupNode(&mi), mi("group") {}
  // A copy would point at the original's mi.
  RootNode(const RootNode&) = delete;
  std::string name() const override;
  std::shared_ptr<AbstractNode> copy() const override;
  // Nothing of its own: its digest is that of its children (core/NodeDigest.h).
  bool hashContent(NodeHasher&) const override { return hashAs(*this); }

private:
  ModuleInstantiation mi;
};

class LeafNode : public AbstractPolyNode
{
public:
  VISITABLE();
  LeafNode(const ModuleInstantiation *mi) : AbstractPolyNode(mi) {}
  virtual std::unique_ptr<const class Geometry> createGeometry() const = 0;
};

std::ostream& operator<<(std::ostream& stream, const AbstractNode& node);
std::shared_ptr<AbstractNode> find_root_tag(const std::shared_ptr<AbstractNode>& node,
                                            const Location **nextLocation = nullptr);
