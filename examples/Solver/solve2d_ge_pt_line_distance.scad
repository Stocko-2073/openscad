// solve2d_ge_pt_line_distance.scad — point bounded below by signed distance to a line.
//
// Line a-b runs along the x-axis (a=[0,0], b=[10,0]).
// SolveSpace's signed distance convention for the 2D workplane case gives
//   signed_dist(p, line a->b) = -p.y  (positive below the line, negative above).
// con_ge_pt_line_distance("p","a","b", 3) requires signed_dist(p) >= 3,
// i.e. p.y <= -3.
//
// p is seeded at [0, -1] (slightly below the line, signed_dist = 1 < 3: violation).
// The inequality activates and pushes p down to p.y = -3 (signed_dist = 3).

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("p", at = [0, -1]),                          // seed: signed_dist = 1 < 3
  con_vertical("p", "a"),                            // p.x = a.x = 0
  con_ge_pt_line_distance("p", "a", "b", 3),         // signed_dist(p, line ab) >= 3
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));

py = pt(sol, "p")[1];
assert(abs(py - (-3)) < 1e-4, str("expected p.y == -3, got ", py));

echo(iter = iterations(sol), active = active_inequalities(sol), p = pt(sol, "p"));
