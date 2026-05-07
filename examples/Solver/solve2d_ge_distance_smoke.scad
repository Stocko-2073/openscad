// solve2d_ge_distance_smoke.scad — slack con_ge_distance.
// |ab| = 5 from con_distance; the inequality |ab| >= 0.1 is slack and
// should NOT end up in the active set. Loop should converge in 1 iteration.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [5, 0]),
  con_horizontal("a", "b"),
  con_distance("a", "b", 5),
  con_ge_distance("a", "b", 0.1),
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(iterations(sol) == 1,
       str("expected 1 iteration, got ", iterations(sol)));
assert(len(active_inequalities(sol)) == 0,
       str("expected empty active set, got ", active_inequalities(sol)));

echo(iter = iterations(sol), active = active_inequalities(sol), b = pt(sol, "b"));
