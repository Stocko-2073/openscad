// solve2d_le_length_difference.scad — bound |ab| relative to |cd|.
// |cd| is fixed at 5. |ab|-|cd| <= 1 forces |ab| <= 6.
// Without the inequality, |ab| would be 8 (from a strong distance pull
// via the seed); the loop should activate the inequality and pull |ab| to
// exactly 6.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [8, 0]), con_horizontal("a", "b"),
  point("c", at = [0, 10]), con_fixed("c"),
  point("d", at = [5, 10]), con_horizontal("c", "d"),
  con_distance("c", "d", 5),
  con_le_length_difference("a", "b", "c", "d", 1),  // |ab| - |cd| <= 1
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));
bx = pt(sol, "b")[0];
assert(abs(bx - 6) < 1e-4, str("expected b.x == 6, got ", bx));

echo(iter = iterations(sol), active = active_inequalities(sol),
     ab = pt(sol, "b")[0], cd = pt(sol, "d")[0] - pt(sol, "c")[0]);
