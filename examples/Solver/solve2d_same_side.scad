// solve2d_same_side.scad — basic smoke test for con_same_side.
//
// Line ab is the x-axis (a at origin, b at [10,0], both anchored).
// Reference point q at [3, 5], above the x-axis.
// Free point p constrained to lie on the same side of ab as q,
// plus con_distance(a, p, 5) — so p lands on a radius-5 circle
// around a, on the upper side (p.y > 0).

sol = solve2d([
  point("a", at = [0, 0]),  con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("q", at = [3, 5]),  con_fixed("q"),
  point("p"),                                // unseeded
  con_distance("a", "p", 5),
  con_same_side("a", "b", "p", "q"),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol),
           ", active=", active_inequalities(sol), ")"));

p = pt(sol, "p");
assert(p[1] > 0,
       str("expected p above x-axis (same side as q), got ", p));
assert(abs(norm(p) - 5) < 1e-4,
       str("expected |ap| == 5, got ", norm(p)));
assert(len(active_inequalities(sol)) == 0,
       str("expected no active inequalities, got ",
           active_inequalities(sol)));

echo(p = p, active = active_inequalities(sol), iter = iterations(sol));
