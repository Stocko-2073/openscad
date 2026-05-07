// solve2d_pt_on_segment_infeasible.scad — contradictory constraints.
//
// Segment a=[0,0] (anchored), b=[10,0] (anchored). p must be on the segment
// AND 100 units from a. The segment is only 10 units long, so no feasible
// position exists. The solver should report failure; we don't pin down
// the exact failure mode beyond requiring solved == false.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("p", at = [5, 0]),
  con_pt_on_segment("p", "a", "b"),
  con_distance("a", "p", 100),               // demands |ap| = 100, segment is only 10
]);

assert(!solved(sol),
       str("expected unsolved, but solver claimed success: p=", pt(sol, "p"),
           " active=", active_inequalities(sol)));

// Failure should surface in failed_constraints — exact contents depend on
// which constraint SolveSpace pins the blame on. Just require non-empty.
failed = failed_constraints(sol);
assert(len(failed) > 0,
       str("expected at least one failed constraint, got empty list"));

echo(solved = solved(sol),
     iter = iterations(sol),
     active = active_inequalities(sol),
     failed = failed);
