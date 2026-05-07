// solve2d_ge_angle.scad — require the angle between two lines to be >= 60°.
// Line ab is horizontal. Line cd starts at a shallow angle (seed places d
// roughly 30° above the x-axis). The inequality requires angle >= 60°.
// Loop should activate the inequality and push the angle to exactly 60°.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [5, 0]), con_horizontal("a", "b"), con_distance("a", "b", 5),
  point("c", at = [0, 0]),                            // overlap with a; coincident pins it
  con_coincident("a", "c"),
  point("d", at = [5, 3]),                            // seed: ~30° above x-axis
  con_distance("c", "d", 6),
  con_ge_angle("a", "b", "c", "d", 60),
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));

// angle(sol, ["b","a","d"]) returns CCW degrees in [0, 360); we expect
// either ~60 or ~300 depending on which side d settles on. Compare
// the absolute angle to 60.
ang = angle(sol, ["b", "a", "d"]);
ang_unsigned = (ang > 180) ? 360 - ang : ang;
assert(abs(ang_unsigned - 60) < 1e-3,
       str("expected unsigned angle == 60, got ", ang_unsigned));

echo(iter = iterations(sol), active = active_inequalities(sol),
     ang = ang, ang_unsigned = ang_unsigned, d = pt(sol, "d"));
