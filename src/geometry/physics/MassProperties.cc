#include "geometry/physics/MassProperties.h"

#include <cmath>
#include <cstddef>
#include <string>

#include "geometry/PolySet.h"

namespace {

// Common subexpressions for the per-face integrals; see Eberly,
// "Polyhedral Mass Properties (Revisited)", Geometric Tools.
inline void subexpressions(double w0, double w1, double w2,
                           double& f1, double& f2, double& f3,
                           double& g0, double& g1, double& g2)
{
  const double temp0 = w0 + w1;
  const double temp1 = w0 * w0;
  const double temp2 = temp1 + w1 * temp0;
  f1 = temp0 + w2;
  f2 = temp2 + w2 * f1;
  f3 = w0 * temp1 + w1 * temp2 + w2 * f2;
  g0 = f2 + w0 * (f1 + w0);
  g1 = f2 + w1 * (f1 + w1);
  g2 = f2 + w2 * (f1 + w2);
}

}  // namespace

bool computeMassProperties(const PolySet& ps, MassProps& out, std::string& err)
{
  // Volume integrals of 1, x, y, z, x^2, y^2, z^2, xy, yz, zx over the solid,
  // accumulated as surface integrals via the divergence theorem.
  double intg[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  for (const auto& face : ps.indices) {
    if (face.size() < 3) continue;
    // Faces are expected to be triangles; fan defensively if not.
    const Vector3d& p0 = ps.vertices[face[0]];
    for (size_t i = 1; i + 1 < face.size(); ++i) {
      const Vector3d& p1 = ps.vertices[face[i]];
      const Vector3d& p2 = ps.vertices[face[i + 1]];

      const double a1 = p1.x() - p0.x(), b1 = p1.y() - p0.y(), c1 = p1.z() - p0.z();
      const double a2 = p2.x() - p0.x(), b2 = p2.y() - p0.y(), c2 = p2.z() - p0.z();
      const double d0 = b1 * c2 - b2 * c1;
      const double d1 = a2 * c1 - a1 * c2;
      const double d2 = a1 * b2 - a2 * b1;

      double f1x, f2x, f3x, g0x, g1x, g2x;
      double f1y, f2y, f3y, g0y, g1y, g2y;
      double f1z, f2z, f3z, g0z, g1z, g2z;
      subexpressions(p0.x(), p1.x(), p2.x(), f1x, f2x, f3x, g0x, g1x, g2x);
      subexpressions(p0.y(), p1.y(), p2.y(), f1y, f2y, f3y, g0y, g1y, g2y);
      subexpressions(p0.z(), p1.z(), p2.z(), f1z, f2z, f3z, g0z, g1z, g2z);

      intg[0] += d0 * f1x;
      intg[1] += d0 * f2x;
      intg[2] += d1 * f2y;
      intg[3] += d2 * f2z;
      intg[4] += d0 * f3x;
      intg[5] += d1 * f3y;
      intg[6] += d2 * f3z;
      intg[7] += d0 * (p0.y() * g0x + p1.y() * g1x + p2.y() * g2x);
      intg[8] += d1 * (p0.z() * g0y + p1.z() * g1y + p2.z() * g2y);
      intg[9] += d2 * (p0.x() * g0z + p1.x() * g1z + p2.x() * g2z);
    }
  }

  intg[0] /= 6.0;
  for (int i = 1; i < 4; ++i) intg[i] /= 24.0;
  for (int i = 4; i < 7; ++i) intg[i] /= 60.0;
  for (int i = 7; i < 10; ++i) intg[i] /= 120.0;

  // An inward-wound mesh yields a negative signed volume; flipping the sign
  // of every integral is equivalent to flipping all face normals.
  if (intg[0] < 0.0) {
    for (double& v : intg) v = -v;
  }

  const double volume = intg[0];
  const double diag = ps.getBoundingBox().diagonal().norm();
  if (!std::isfinite(volume) || diag <= 0.0 ||
      volume < 1e-12 * diag * diag * diag) {
    err = "zero or near-zero volume (open mesh or degenerate solid)";
    return false;
  }

  const Vector3d com(intg[1] / volume, intg[2] / volume, intg[3] / volume);

  // Inertia tensor per unit density about the center of mass, world axes.
  // Off-diagonals carry the standard minus sign of products of inertia.
  const double ixx = intg[5] + intg[6] - volume * (com.y() * com.y() + com.z() * com.z());
  const double iyy = intg[4] + intg[6] - volume * (com.z() * com.z() + com.x() * com.x());
  const double izz = intg[4] + intg[5] - volume * (com.x() * com.x() + com.y() * com.y());
  const double ixy = -(intg[7] - volume * com.x() * com.y());
  const double iyz = -(intg[8] - volume * com.y() * com.z());
  const double ixz = -(intg[9] - volume * com.z() * com.x());

  Matrix3d inertia;
  inertia << ixx, ixy, ixz,
             ixy, iyy, iyz,
             ixz, iyz, izz;

  if (!is_finite(com) || !is_finite(inertia)) {
    err = "non-finite mass properties (degenerate mesh)";
    return false;
  }

  out.volume = volume;
  out.com = com;
  out.inertiaAboutCom = inertia;
  return true;
}
