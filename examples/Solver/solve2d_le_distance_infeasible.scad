// solve2d_le_distance_infeasible.scad — contradictory constraints.
// con_distance forces |ab| = 10. con_le_distance forces |ab| <= 5.
// No feasible solution exists. Expected: solved(sol) == false; the loop
// should report failure (cycle/max_iter/infeasible_base) in
// failed_constraints. We don't pin down the exact reason — the experiment
// is interested in *whether* failure is reported cleanly.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [10, 0]),
  con_horizontal("a", "b"),
  con_distance("a", "b", 10),       // equality: forces |ab| = 10
  con_le_distance("a", "b", 5),     // inequality: forces |ab| <= 5
]);

assert(!solved(sol),
       str("expected unsolved, but solver claimed success: b=", pt(sol, "b"),
           " active=", active_inequalities(sol)));

echo(solved = solved(sol),
     iter = iterations(sol),
     active = active_inequalities(sol),
     failed = failed_constraints(sol));
