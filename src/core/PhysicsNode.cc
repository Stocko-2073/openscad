#include "core/PhysicsNode.h"

#include <memory>
#include <utility>

#include "core/Builtins.h"
#include "core/Children.h"
#include "core/Identifier.h"
#include "core/ModuleInstantiation.h"
#include "core/Parameters.h"
#include "core/module.h"

static std::shared_ptr<AbstractNode> builtin_physics(const ModuleInstantiation *inst,
                                                     Arguments arguments,
                                                     const Children& children)
{
  auto node = std::make_shared<PhysicsNode>(inst);

  static const std::vector<Identifier> required{"density", "friction", "restitution", "gravity", "max_time", "nudge", "convexity"};
  Parameters parameters = Parameters::parse(
    std::move(arguments), inst->location(), required);
  const auto getNumber = [&parameters](const char *name, double& target) {
    if (parameters[name].type() == Value::Type::NUMBER) {
      target = parameters[name].toDouble();
    }
  };
  getNumber("density", node->density);
  getNumber("friction", node->friction);
  getNumber("restitution", node->restitution);
  getNumber("gravity", node->gravity);
  getNumber("max_time", node->max_time);
  if (parameters["nudge"].type() == Value::Type::BOOL) {
    node->nudge = parameters["nudge"].toBool();
  }
  if (parameters["convexity"].type() == Value::Type::NUMBER) {
    node->convexity = static_cast<int>(parameters["convexity"].toDouble());
  }

  return children.instantiate(node);
}

std::string PhysicsNode::toString() const
{
  return STR(this->name(), "(density = ", density, ", friction = ", friction,
             ", restitution = ", restitution, ", gravity = ", gravity,
             ", max_time = ", max_time, ", nudge = ", (nudge ? "true" : "false"),
             ", convexity = ", convexity, ")");
}

bool PhysicsNode::hashContent(NodeHasher& h) const
{
  if (!hashAs(*this)) return false;
  h.str("physics");
  h.f64(density);
  h.f64(friction);
  h.f64(restitution);
  h.f64(gravity);
  h.f64(max_time);
  h.u64(nudge);
  h.u64(static_cast<uint64_t>(convexity));
  return true;
}

void register_builtin_physics()
{
  Builtins::init("physics", new BuiltinModule(builtin_physics),
                 {
                   "physics(density = 1, friction = 0.5, restitution = 0, gravity = 9810, "
                   "max_time = 20, nudge = false, convexity = 1)",
                 });
}
