// solve2d_le_distance_binding.scad — binding con_le_distance.
// Without the inequality, |ab| = 10 (from con_distance). The inequality
// |ab| <= 4 conflicts and forces removal of con_distance — but con_distance
// is an equality, so the system as posed is infeasible. Instead, drop
// con_distance and rely only on the inequality to bound |ab|.
//
// Setup: a is fixed at origin, b is unconstrained except by con_le_distance.
// Outer loop should:
//   iter 1: solve with empty active set → b drifts to seed; |ab| might
//           exceed 4, triggering activation
//   iter 2: solve with {le_distance} active → |ab| = 4 exactly
// Final: solved, iterations==2, 1 active inequality.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [10, 0]),         // seed far from origin
  con_horizontal("a", "b"),
  con_le_distance("a", "b", 4),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol), ", active=", active_inequalities(sol), ")"));
assert(iterations(sol) >= 2,
       str("expected at least 2 iterations, got ", iterations(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));

bx = pt(sol, "b")[0];
// |bx| should equal 4 — seed at [10,0] biases to +4, but accept symmetric -4
// solution too in case SolveSpace's seed handling drifts.
assert(abs(abs(bx) - 4) < 1e-4,
       str("expected |b.x| == 4 (binding), got ", bx));

echo(iter = iterations(sol),
     active = active_inequalities(sol),
     b = pt(sol, "b"));
