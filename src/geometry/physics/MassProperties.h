#pragma once

#include <string>

#include "geometry/linalg.h"

class PolySet;

struct MassProps {
  double volume{0.0};                            // input length units cubed (mm^3)
  Vector3d com{Vector3d::Zero()};                // center of mass, input units
  Matrix3d inertiaAboutCom{Matrix3d::Zero()};    // per unit density, about CoM, world axes
};

// `ps` must be closed; either winding works. Returns false, with a message in `err`, for a
// degenerate mesh (near-zero volume or non-finite results).
bool computeMassProperties(const PolySet& ps, MassProps& out, std::string& err);
