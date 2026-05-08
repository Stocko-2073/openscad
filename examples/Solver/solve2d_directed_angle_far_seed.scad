// solve2d_directed_angle_far_seed.scad — directed angle whose target point
// is seeded > 90° away from the target angle.
//
// SLVS_C_ANGLE's residual cos(actual)-cos(target) is locally flat in actual
// near 0° and 180°. When the user's seed for p4 leaves Newton needing to
// swing more than ~90° through that flat ridge to reach the target, libslvs
// settles on a wrong-angle local minimum (REDUNDANT_OKAY internally,
// reported as SLVS_RESULT_INCONSISTENT) and pure Cartesian jitter rarely
// escapes the basin. The fix: on retry attempts, rotate p4's seed onto the
// target angle ray (preserving |p4 - p3|) only when the current seed sits
// outside a 90° wedge of the target — close-enough seeds are left alone so
// their carefully-tuned distances to other anchors don't get shaken loose.
//
// This case is a stripped-down version of a multi-point sketch whose
// failure spanned several discontinuous ~0.1° bands around 178–180°.

l = 3;
sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("e", at = [-16, 0]), con_fixed("e"),
  // Seed for d sits at angle ~63° from e, while the directed angle pins
  // it to 179° from e (relative to e->a, which is +x). The 116° gap is
  // exactly the regime where Newton stalls without retry projection.
  point("d", at = [-13, 6]),
  con_distance("d", "e", l * 1.5),
  con_directed_angle("e", "a", "e", "d", 179),
]);

assert(solved(sol),
       str("far-seed directed-angle solve failed: ", failed_constraints(sol)));

d = pt(sol, "d");
// 179° CCW from +x lands d just barely above -x at radius 4.5 from e.
expected_x = -16 + 4.5 * cos(179);
expected_y = 0 + 4.5 * sin(179);
assert(abs(d[0] - expected_x) < 1e-3 && abs(d[1] - expected_y) < 1e-3,
       str("expected d ~= (", expected_x, ", ", expected_y, "), got ", d));

echo(d = d, residual = residual(sol),
     iter = iterations(sol), active = active_inequalities(sol));
