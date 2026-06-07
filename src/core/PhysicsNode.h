#pragma once

#include <string>

#include "core/ModuleInstantiation.h"
#include "core/node.h"

// physics() renders its children, unions them into a single rigid body and
// drops it onto the infinite floor z=0 until it comes to rest, then applies
// the resulting rigid transform to the geometry.
class PhysicsNode : public AbstractNode
{
public:
  VISITABLE();
  PhysicsNode(const ModuleInstantiation *mi) : AbstractNode(mi) {}
  std::string toString() const override;
  std::string name() const override { return "physics"; }

  double density{1.0};
  double friction{0.5};
  double restitution{0.0};
  double gravity{9810.0};  // mm/s^2
  double max_time{20.0};   // simulated seconds
  bool nudge{false};
  int convexity{1};
};
