#pragma once

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "core/NodeCache.h"
#include "core/NodeDigest.h"
#include "core/node.h"
#include "utils/Hash128.h"

/*!
   For now, just an abstraction of the node tree which keeps a dump
   cache based on node indices around, and the geometry digests that
   depend on files.

   Note that since node trees don't survive a recompilation, the tree cannot either.
 */
class Tree
{
public:
  Tree(std::shared_ptr<const AbstractNode> root = nullptr, std::string path = {})
    : root_node(std::move(root)), document_path(std::move(path))
  {
  }
  ~Tree();

  void setRoot(const std::shared_ptr<const AbstractNode>& root);
  void setDocumentPath(const std::string& path);
  const std::shared_ptr<const AbstractNode>& root() const { return this->root_node; }

  const std::string getString(const AbstractNode& node, const std::string& indent) const;
  // For debugging: dumps the whole tree.
  const std::string getIdString(const AbstractNode& node) const;
  const std::string getDocumentPath() const;

  // What the geometry caches keep `node`'s geometry under (core/NodeDigest.h).
  Hash128 digest(const AbstractNode& node) const { return this->digests.digest(node); }
  bool hasModifierBelow(const AbstractNode& node) const { return this->digests.hasModifierBelow(node); }
  // Whether pick::isWhole() holds for a node below `node`.
  bool hasWholeBelow(const AbstractNode& node) const { return this->digests.hasWholeBelow(node); }

private:
  std::shared_ptr<const AbstractNode> root_node;
  // keep a separate nodecache per tuple of NodeDumper constructor parameters
  mutable std::map<std::tuple<std::string, bool>, NodeCache> nodecachemap;
  mutable NodeDigests digests;
  std::string document_path;
};
