#pragma once

#include <string>

#include "core/ModuleInstantiation.h"
#include "core/node.h"

class RenderNode : public AbstractNode
{
public:
  VISITABLE();
  RenderNode(const ModuleInstantiation *mi) : AbstractNode(mi) {}
  std::string toString() const override;
  bool hashContent(NodeHasher& h) const override;
  std::string name() const override { return "render"; }
  std::shared_ptr<AbstractNode> copy() const override { return copyAs(*this); }

  int convexity{1};
};
