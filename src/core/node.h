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

class NodeHasher : public Hasher128
{
public:
  // A digest that includes a file's modification time holds only until the next refresh.
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
  // Per thread: indices must be unique within a tree (GeometryEvaluator keys children by index),
  // and the GUI resets the counter while Animate's frame workers build their trees.
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

  // A copy without the children, under a fresh index. Null for a class without a copy() of its
  // own: copyAs() refuses rather than slicing the node to the base class.
  [[nodiscard]] virtual std::shared_ptr<AbstractNode> copy() const = 0;
  // A deep copy under fresh indices, or null if some node in it cannot be copied.
  [[nodiscard]] std::shared_ptr<AbstractNode> clone() const;

  // Hashes what toString() writes, numbers exactly, but not the children. Checks hashAs(*this)
  // first, so a subclass without its own is refused and gets a digest unlike any other node's.
  [[nodiscard]] virtual bool hashContent(NodeHasher& h) const = 0;

  // Only for a deep copy whose children are copies of the original's.
  void takeDigest(const AbstractNode& original);

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

  template <class T>
  static bool hashAs(const T& node)
  {
    return typeid(node) == typeid(T);
  }

private:
  friend class NodeDigests;
  // The subtree's digest, unless it depends on files. Atomic, since the GUI may ask for it while
  // the render thread computes it.
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
