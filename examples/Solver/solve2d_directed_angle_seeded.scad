// solve2d_directed_angle_seeded.scad — fully-seeded sketch with inequalities.
//
// con_directed_angle decomposes into a magnitude angle + a half-plane
// inequality. If the user's seeds happen to land on the wrong half-plane
// for the requested deg, the active-set loop pins the offending point to
// the dividing line, conflicts with the angle equality, and bails. To
// recover, the multi-start retry now runs even when every point has an
// `at=` seed (previously max_attempts collapsed to 1 in that case),
// jittering seeded points on attempts >= 1 by a fraction of the sketch's
// length scale that grows with attempt number. The first attempt always
// honours the user's seeds verbatim.

// Chevron with 270° CCW directed angle. Correct answer puts b at -y. The
// seed for b is on the WRONG side of the dividing line (positive y) but
// close to the boundary — the typical branch-cut case where Newton happens
// to converge to the wrong half-plane and the active-set loop fails on
// attempt 0. The growing-radius jitter on subsequent attempts has to
// reflect b across y=0 within the 8-attempt budget. con_fixed'd points
// (v) are exempt from jitter so the reference frame doesn't drift.
sol = solve2d([
  point("v", at = [0, 0]), con_fixed("v"),
  point("a", at = [5, 0]),
  point("b", at = [0.5, 0.5]),  // wrong side, near boundary
  con_distance("v", "a", 5),
  con_horizontal("v", "a"),
  con_distance("v", "b", 4),
  con_directed_angle("v", "a", "v", "b", 270),
]);

assert(solved(sol),
       str("directed-angle-seeded solve failed: ", failed_constraints(sol)));

b = pt(sol, "b");
// 270° CCW from +x is -y, so b should be at ~(0, -4).
assert(abs(b[0]) < 1e-3 && abs(b[1] + 4) < 1e-3,
       str("expected b ~= (0, -4), got ", b,
           " (jitter retry failed to flip half-plane)"));

echo(b = b, residual = residual(sol),
     iter = iterations(sol), active = active_inequalities(sol));
