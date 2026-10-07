#pragma once

#include <string>

#include "core/ModuleInstantiation.h"
#include "core/node.h"

// Drops the union of its children onto the floor z=0 and moves it to its resting pose.
class PhysicsNode : public AbstractNode
{
public:
  VISITABLE();
  PhysicsNode(const ModuleInstantiation *mi) : AbstractNode(mi) {}
  std::string toString() const override;
  bool hashContent(NodeHasher& h) const override;
  std::string name() const override { return "physics"; }
  std::shared_ptr<AbstractNode> copy() const override { return copyAs(*this); }

  double density{1.0};
  double friction{0.5};
  double restitution{0.0};
  double gravity{9810.0};  // mm/s^2
  double max_time{20.0};   // simulated seconds
  bool nudge{false};
  int convexity{1};
};
