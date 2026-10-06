#pragma once

#include <string>

#include "core/ModuleInstantiation.h"
#include "core/node.h"

class ProjectionNode : public AbstractPolyNode
{
public:
  VISITABLE();
  ProjectionNode(const ModuleInstantiation *mi) : AbstractPolyNode(mi) {}
  std::string toString() const override;
  bool hashContent(NodeHasher& h) const override;
  std::string name() const override { return "projection"; }
  std::shared_ptr<AbstractNode> copy() const override { return copyAs(*this); }

  int convexity{1};
  bool cut_mode{false};
};
