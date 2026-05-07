// solve2d_pt_on_segment_binding.scad — upper bound activates.
//
// Segment a=[0,0] (anchored), b=[10,0] (anchored). Point p is seeded at
// [15, 0], which is on the line through a-b but past b (t = 1.5).
// con_pt_on_segment forces p back onto the closed segment. With no other
// along-line constraint, the upper bound activates and p is pinned to b.
// Expected:
//   solved == true
//   active_inequalities == ["con_pt_on_segment(p,a,b):end"]
//   pt(p) == [10, 0]
//   iterations >= 2 (one feasibility-check pass + one with the bound active)

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("p", at = [15, 0]),                 // seed past b
  con_pt_on_segment("p", "a", "b"),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol),
           ", active=", active_inequalities(sol), ")"));

p = pt(sol, "p");
assert(abs(p[0] - 10) < 1e-4, str("expected p.x == 10, got ", p[0]));
assert(abs(p[1])      < 1e-4, str("expected p.y == 0, got ", p[1]));

active = active_inequalities(sol);
assert(len(active) == 1,
       str("expected 1 active inequality, got ", active));
assert(active[0] == "con_pt_on_segment(p,a,b):end",
       str("expected :end bound active, got ", active[0]));

echo(iter = iterations(sol), active = active, p = p);
