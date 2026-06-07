#include "geometry/physics/PhysicsSimulator.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Jolt.h must be included before any other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "utils/printutils.h"

namespace {

constexpr JPH::ObjectLayer LAYER_NON_MOVING = 0;
constexpr JPH::ObjectLayer LAYER_MOVING = 1;
constexpr JPH::uint NUM_OBJECT_LAYERS = 2;
constexpr JPH::uint NUM_BROADPHASE_LAYERS = 2;

// Fixed timestep; together with the single-threaded job system and Jolt's
// deterministic simulation mode this makes results reproducible.
constexpr double SIM_DT = 1.0 / 120.0;

// Deterministic tie-breaker angular velocity (rad/s, scale-invariant) used
// when nudge=true; irrational-ish ratios avoid landing on a symmetry axis.
constexpr float NUDGE_OMEGA[3] = {0.31f, 0.23f, 0.17f};

void joltTrace(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  PRINTDB("Jolt: %s", buffer);
}

void initJoltOnce()
{
  static std::once_flag flag;
  std::call_once(flag, [] {
    JPH::RegisterDefaultAllocator();
    JPH::Trace = joltTrace;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  });
}

std::mutex transformCacheMutex;
std::unordered_map<std::string, Transform3d> transformCache;

}  // namespace

void physicsTransformCacheStore(const std::string& key, const Transform3d& transform)
{
  std::lock_guard<std::mutex> lock(transformCacheMutex);
  transformCache[key] = transform;
}

bool physicsTransformCacheLookup(const std::string& key, Transform3d& transform)
{
  std::lock_guard<std::mutex> lock(transformCacheMutex);
  const auto it = transformCache.find(key);
  if (it == transformCache.end()) return false;
  transform = it->second;
  return true;
}

PhysicsResult simulatePhysics(const PhysicsInput& in)
{
  PhysicsResult out;
  initJoltOnce();

  PhysicsParams p = in.params;
  const auto clampWarn = [&out](double& value, double lo, double hi, double fallback,
                                const char *name) {
    if (!std::isfinite(value)) {
      out.warnings.push_back(std::string(name) + " is not finite, using default");
      value = fallback;
    } else if (value < lo || value > hi) {
      out.warnings.push_back(std::string(name) + " out of range, clamping");
      value = std::clamp(value, lo, hi);
    }
  };
  constexpr double inf = std::numeric_limits<double>::infinity();
  clampWarn(p.density, 1e-12, inf, 1.0, "density");
  clampWarn(p.friction, 0.0, inf, 0.5, "friction");
  clampWarn(p.restitution, 0.0, 1.0, 0.0, "restitution");
  clampWarn(p.gravity, 1e-12, inf, 9810.0, "gravity");
  clampWarn(p.max_time, SIM_DT, 3600.0, 20.0, "max_time");

  if (in.points.size() < 4 || !(in.bboxDiag > 0.0) || !(in.volume > 0.0)) {
    out.warnings.push_back("degenerate input, skipping simulation");
    return out;
  }

  // Normalize to unit scale: s = 1/bboxDiag maps the part onto ~1 simulation
  // unit, where Jolt's default tolerances and sleep thresholds (tuned for
  // meter-sized bodies) apply as-is. Scaling all lengths AND gravity by s
  // leaves trajectories and resting poses identical, with time unchanged;
  // only the final translation needs unscaling.
  const double s = 1.0 / in.bboxDiag;

  // Collision points in body-local space, centered on the true center of
  // mass, so that body origin == CoM.
  JPH::Array<JPH::Vec3> points;
  points.reserve(in.points.size());
  double minWorldZ = inf;
  for (const auto& pt : in.points) {
    const Vector3d q = s * (pt - in.com);
    points.emplace_back(static_cast<float>(q.x()), static_cast<float>(q.y()),
                        static_cast<float>(q.z()));
    minWorldZ = std::min(minWorldZ, s * pt.z());
  }

  // Small convex radius (0.5% of the bbox diagonal) keeps Jolt's hull
  // rounding from visibly altering resting poses on small features.
  JPH::ConvexHullShapeSettings hullSettings(points, 0.005f);
  auto hullResult = hullSettings.Create();
  if (hullResult.HasError()) {
    out.warnings.push_back(std::string("convex hull failed: ") +
                           hullResult.GetError().c_str());
    return out;
  }

  // Jolt rotates bodies about the shape's center of mass and applies gravity
  // there. The hull's centroid is NOT the true solid's CoM for concave
  // parts, so relocate the shape CoM to the body-local origin (= true CoM).
  JPH::OffsetCenterOfMassShapeSettings comSettings(
    -hullResult.Get()->GetCenterOfMass(), hullResult.Get());
  auto shapeResult = comSettings.Create();
  if (shapeResult.HasError()) {
    out.warnings.push_back(std::string("shape setup failed: ") +
                           shapeResult.GetError().c_str());
    return out;
  }

  // Tiny scene: one dynamic body, one static floor.
  JPH::TempAllocatorMalloc tempAllocator;
  JPH::JobSystemSingleThreaded jobSystem(JPH::cMaxPhysicsJobs);

  JPH::BroadPhaseLayerInterfaceTable bpLayers(NUM_OBJECT_LAYERS, NUM_BROADPHASE_LAYERS);
  bpLayers.MapObjectToBroadPhaseLayer(LAYER_NON_MOVING, JPH::BroadPhaseLayer(0));
  bpLayers.MapObjectToBroadPhaseLayer(LAYER_MOVING, JPH::BroadPhaseLayer(1));
  JPH::ObjectLayerPairFilterTable objectFilter(NUM_OBJECT_LAYERS);
  objectFilter.EnableCollision(LAYER_MOVING, LAYER_NON_MOVING);
  objectFilter.EnableCollision(LAYER_MOVING, LAYER_MOVING);
  JPH::ObjectVsBroadPhaseLayerFilterTable bpFilter(bpLayers, NUM_BROADPHASE_LAYERS,
                                                   objectFilter, NUM_OBJECT_LAYERS);

  JPH::PhysicsSystem system;
  system.Init(/*inMaxBodies=*/8, /*inNumBodyMutexes=*/0, /*inMaxBodyPairs=*/8,
              /*inMaxContactConstraints=*/8, bpLayers, bpFilter, objectFilter);
  system.SetGravity(JPH::Vec3(0.0f, 0.0f, -static_cast<float>(p.gravity * s)));
  JPH::BodyInterface& bi = system.GetBodyInterface();

  // Infinite floor: the solid half-space z <= 0.
  JPH::BodyCreationSettings floorSettings(
    new JPH::PlaneShape(JPH::Plane(JPH::Vec3(0, 0, 1), 0.0f)), JPH::RVec3::sZero(),
    JPH::Quat::sIdentity(), JPH::EMotionType::Static, LAYER_NON_MOVING);
  floorSettings.mFriction = static_cast<float>(p.friction);
  floorSettings.mRestitution = static_cast<float>(p.restitution);
  const JPH::BodyID floorId = bi.CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);

  // Body starts at the modeled pose (origin at the true CoM) with zero
  // velocity; if it penetrates the floor, lift it to rest just above z=0.
  Vector3d startPos = s * in.com;
  constexpr double FLOOR_EPS = 1e-3;
  if (minWorldZ < FLOOR_EPS) {
    startPos.z() += FLOOR_EPS - minWorldZ;
    if (minWorldZ < 0.0) {
      out.warnings.push_back("object starts below the floor, lifting it to z=0");
    }
  }

  JPH::BodyCreationSettings bodySettings(
    shapeResult.Get(),
    JPH::RVec3(static_cast<float>(startPos.x()), static_cast<float>(startPos.y()),
               static_cast<float>(startPos.z())),
    JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, LAYER_MOVING);
  bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
  bodySettings.mMassPropertiesOverride.mMass =
    static_cast<float>(p.density * s * s * s * in.volume);
  JPH::Mat44 inertia = JPH::Mat44::sIdentity();
  const Matrix3d scaledInertia = p.density * std::pow(s, 5) * in.inertiaPerDensityAboutCom;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      inertia(r, c) = static_cast<float>(scaledInertia(r, c));
    }
  }
  bodySettings.mMassPropertiesOverride.mInertia = inertia;
  bodySettings.mFriction = static_cast<float>(p.friction);
  bodySettings.mRestitution = static_cast<float>(p.restitution);
  // Continuous collision keeps fast drops from popping deep into the floor.
  bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
  bodySettings.mAllowSleeping = true;
  if (p.nudge) {
    bodySettings.mAngularVelocity =
      JPH::Vec3(NUDGE_OMEGA[0], NUDGE_OMEGA[1], NUDGE_OMEGA[2]);
  }
  const JPH::BodyID bodyId = bi.CreateAndAddBody(bodySettings, JPH::EActivation::Activate);

  const int maxSteps = static_cast<int>(std::ceil(p.max_time / SIM_DT));
  int stepsDone = 0;
  while (stepsDone < maxSteps) {
    const auto error = system.Update(static_cast<float>(SIM_DT), 1, &tempAllocator, &jobSystem);
    ++stepsDone;
    if (error != JPH::EPhysicsUpdateError::None) {
      out.warnings.push_back("physics update reported an internal error");
      break;
    }
    if (!bi.IsActive(bodyId)) {  // body fell asleep -> settled
      out.slept = true;
      break;
    }
  }
  if (!out.slept) {
    out.warnings.push_back("did not settle within max_time, using cutoff pose");
  }
  out.settleTime = stepsDone * SIM_DT;

  // Body origin == CoM, so the CoM position is the body position.
  const JPH::RVec3 pf = bi.GetCenterOfMassPosition(bodyId);
  const JPH::Quat qf = bi.GetRotation(bodyId);

  bi.RemoveBody(bodyId);
  bi.DestroyBody(bodyId);
  bi.RemoveBody(floorId);
  bi.DestroyBody(floorId);

  const Vector3d finalPos(pf.GetX() / s, pf.GetY() / s, pf.GetZ() / s);
  Eigen::Quaterniond rot(qf.GetW(), qf.GetX(), qf.GetY(), qf.GetZ());
  rot.normalize();

  // x_final = Pf + R * (x - CoM)
  Transform3d t = Transform3d::Identity();
  t.translate(finalPos);
  t.rotate(rot);
  t.translate(-in.com);
  out.transform = t;
  return out;
}
