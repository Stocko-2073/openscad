#include "core/RenderVariables.h"

#include "core/BuiltinContext.h"
#include "core/Context.h"

void RenderVariables::applyToContext(ContextHandle<BuiltinContext>& context) const
{
  // Every view is a render, so designs build their final geometry.
  context->set_variable("$preview", false);
  context->set_variable("$t", time);

  const auto vpr = camera.getVpr();
  context->set_variable("$vpr", VectorType(context->session(), vpr.x(), vpr.y(), vpr.z()));
  const auto vpt = camera.getVpt();
  context->set_variable("$vpt", VectorType(context->session(), vpt.x(), vpt.y(), vpt.z()));
  const auto vpd = camera.zoomValue();
  context->set_variable("$vpd", vpd);
  const auto vpf = camera.fovValue();
  context->set_variable("$vpf", vpf);
}
