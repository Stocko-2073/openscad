#pragma once

#include <string>
#include <vector>

#include "geometry/linalg.h"
#include "utils/Hash128.h"

// Parameters of the physics() module, in model units (mm, mm/s^2, seconds).
struct PhysicsParams {
  double density{1.0};
  double friction{0.5};
  double restitution{0.0};
  double gravity{9810.0};
  double max_time{20.0};
  bool nudge{false};
};

// In model units (mm).
struct PhysicsInput {
  // The simulator collides their convex hull, which is exact against the half-space floor.
  std::vector<Vector3d> points;
  Vector3d com{Vector3d::Zero()};  // center of mass of the true solid
  double volume{0.0};              // mm^3
  Matrix3d inertiaPerDensityAboutCom{Matrix3d::Zero()};  // mm^5, world axes
  double bboxDiag{0.0};            // mm, used for scale normalization
  PhysicsParams params;
};

struct PhysicsResult {
  // Maps the input solid to its resting pose: translate(Pf) * rotate(Qf) * translate(-com).
  Transform3d transform{Transform3d::Identity()};
  double settleTime{0.0};  // simulated seconds until sleep (or cutoff)
  bool slept{false};
  std::vector<std::string> warnings;
};

// Drops the solid from its modeled pose onto the floor z=0 under -Z gravity, until it sleeps or
// params.max_time elapses. Deterministic, and thread-safe: each call has its own physics world.
PhysicsResult simulatePhysics(const PhysicsInput& in);

// Settled transforms by the physics node's Tree::digest(), for the overlays (core/ModifierOverlays)
// to move the # and % subtrees below it. Entries are tiny and never evicted. Thread-safe.
void physicsTransformCacheStore(const Hash128& key, const Transform3d& transform);
bool physicsTransformCacheLookup(const Hash128& key, Transform3d& transform);
