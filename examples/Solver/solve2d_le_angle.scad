// solve2d_le_angle.scad — bound the angle between two lines.
// Line ab is horizontal. Line cd starts at a steep angle (seed places d
// roughly 60° above the x-axis). The inequality requires angle <= 30°.
// Loop should activate the inequality and pull the angle to exactly 30°.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [5, 0]), con_horizontal("a", "b"), con_distance("a", "b", 5),
  point("c", at = [0, 0]),                            // overlap with a; a separate constraint pins c
  con_coincident("a", "c"),
  point("d", at = [3, 5]),                            // seed: ~60° above x-axis
  con_distance("c", "d", 6),
  con_le_angle("a", "b", "c", "d", 30),
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));

// angle(sol, ["b","a","d"]) returns CCW degrees in [0, 360); we expect
// either ~30 or ~330 depending on which side d settles on. Compare
// the absolute angle to 30.
ang = angle(sol, ["b", "a", "d"]);
ang_unsigned = (ang > 180) ? 360 - ang : ang;
assert(abs(ang_unsigned - 30) < 1e-3,
       str("expected unsigned angle == 30, got ", ang_unsigned));

echo(iter = iterations(sol), active = active_inequalities(sol),
     ang = ang, ang_unsigned = ang_unsigned, d = pt(sol, "d"));
