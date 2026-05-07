// solve2d_ge_distance_binding.scad — binding con_ge_distance.
// b is unconstrained except con_horizontal("a","b") and con_ge_distance("a","b", 4).
// The seed places b at [1,0] so |ab| = 1 < 4: the inequality is violated and
// should activate, pulling |ab| to exactly 4.
//
// Outer loop:
//   iter 1: solve with empty active set → b stays near seed; |ab| < 4 ⇒ activate
//   iter 2: solve with {ge_distance} active → |ab| = 4
// Final: solved, iterations >= 2, 1 active inequality.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [1, 0]),          // seed close to origin
  con_horizontal("a", "b"),
  con_ge_distance("a", "b", 4),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol), ", active=", active_inequalities(sol), ")"));
assert(iterations(sol) >= 2,
       str("expected at least 2 iterations, got ", iterations(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));

bx = pt(sol, "b")[0];
// |bx| should equal 4 — seed at [1,0] biases to +4, but accept symmetric -4
// solution too in case SolveSpace's seed handling drifts.
assert(abs(abs(bx) - 4) < 1e-4,
       str("expected |b.x| == 4 (binding), got ", bx));

echo(iter = iterations(sol),
     active = active_inequalities(sol),
     b = pt(sol, "b"));
