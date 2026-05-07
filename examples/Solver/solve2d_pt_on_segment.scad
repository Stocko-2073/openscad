// solve2d_pt_on_segment.scad — golden path: p lies strictly inside segment a-b.
//
// Segment: a=[0,0] (anchored), b=[10,0] (anchored). Point p is constrained
// con_pt_on_segment("p","a","b") plus con_distance("a","p", 5).
// p must end at [5, 0]: on the segment, 5 units from a, on the x-axis.
// Neither bound binds (t = 0.5 ∈ (0,1)), so active_inequalities is empty.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("p", at = [3, 2]),                  // off-segment seed; on_line pulls it down
  con_pt_on_segment("p", "a", "b"),
  con_distance("a", "p", 5),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol),
           ", active=", active_inequalities(sol), ")"));

p = pt(sol, "p");
assert(abs(p[0] - 5) < 1e-4, str("expected p.x == 5, got ", p[0]));
assert(abs(p[1]) < 1e-4,     str("expected p.y == 0, got ", p[1]));
assert(len(active_inequalities(sol)) == 0,
       str("expected no active inequalities, got ",
           active_inequalities(sol)));

echo(iter = iterations(sol),
     active = active_inequalities(sol),
     p = pt(sol, "p"));
