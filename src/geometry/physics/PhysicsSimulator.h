#pragma once

#include <string>
#include <vector>

#include "geometry/linalg.h"

// Parameters of the physics() module, in model units (mm, mm/s^2, seconds).
struct PhysicsParams {
  double density{1.0};
  double friction{0.5};
  double restitution{0.0};
  double gravity{9810.0};
  double max_time{20.0};
  bool nudge{false};
};

// Input to the settling simulation, in model units (mm).
struct PhysicsInput {
  // Collision points; the simulator builds their convex hull. Against the
  // half-space floor a convex hull is exact: any contact between a solid and
  // a half-space happens on the solid's convex hull.
  std::vector<Vector3d> points;
  Vector3d com{Vector3d::Zero()};  // center of mass of the true solid
  double volume{0.0};              // mm^3
  Matrix3d inertiaPerDensityAboutCom{Matrix3d::Zero()};  // mm^5, world axes
  double bboxDiag{0.0};            // mm, used for scale normalization
  PhysicsParams params;
};

struct PhysicsResult {
  // Model-space rigid transform mapping the input solid to its resting pose:
  // x_final = translate(Pf) * rotate(Qf) * translate(-com) * x.
  Transform3d transform{Transform3d::Identity()};
  double settleTime{0.0};  // simulated seconds until sleep (or cutoff)
  bool slept{false};
  std::vector<std::string> warnings;
};

// Drops the solid described by `in` from its modeled pose onto the infinite
// static floor z=0 under -Z gravity, stepping a fixed-timestep simulation
// until the body sleeps or params.max_time simulated seconds elapse.
// Deterministic (fixed timestep, single-threaded solver, no RNG) and
// thread-safe (each call uses an isolated physics world).
PhysicsResult simulatePhysics(const PhysicsInput& in);
