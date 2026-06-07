#pragma once

#include <string>

#include "geometry/linalg.h"

class PolySet;

// Mass properties of a closed triangulated solid, computed by exact
// closed-form polyhedral integrals (Mirtich 1996 / Eberly, "Polyhedral Mass
// Properties"). The inertia tensor is per unit density; multiply by the
// desired density to obtain physical values (mass = density * volume).
struct MassProps {
  double volume{0.0};                            // input length units cubed (mm^3)
  Vector3d com{Vector3d::Zero()};                // center of mass, input units
  Matrix3d inertiaAboutCom{Matrix3d::Zero()};    // per unit density, about CoM, world axes
};

// Computes volume, center of mass and inertia tensor of a triangulated,
// closed (watertight) PolySet. Faces are assumed CCW-outward; an
// inward-wound mesh (negative signed volume) is handled by flipping the sign
// of all integrals. Returns false with a human-readable message in err if
// the mesh is degenerate (near-zero volume, e.g. an open mesh or a planar
// "solid") or produces non-finite values.
bool computeMassProperties(const PolySet& ps, MassProps& out, std::string& err);
