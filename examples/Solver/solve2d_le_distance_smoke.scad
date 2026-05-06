// solve2d_le_distance_smoke.scad — verify con_le_distance parses without
// crashing. Inequality is not yet enforced (Task 3 only wires parsing).
// The unconstrained solution gives |ab| = 5; the slack inequality |ab| <= 100
// should be silently ignored at this stage.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [5, 0]),
  con_horizontal("a", "b"),
  con_distance("a", "b", 5),
  con_le_distance("a", "b", 100),  // slack — ignored at this stage
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
echo(b = pt(sol, "b"));
echo(remaining_dof = dof(sol));
