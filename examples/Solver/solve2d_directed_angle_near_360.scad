// solve2d_directed_angle_near_360.scad — directed angles near the 360 boundary.
//
// con_directed_angle accepts deg in [0, 360); the parser wraps any input
// into that range via fmod, so values like 360 - epsilon, exactly 360, or
// negative numbers are handled gracefully. Previously such inputs were
// rejected with a warning, which broke arithmetic-derived deg values.

// Case 1: deg = 359.999 — was previously silently dropped on the strict
// `>= 360` check after rounding (no longer applicable, but exercises the
// near-boundary parallel-eps gate).
sol1 = solve2d([
  point("v", at = [0, 0]), con_fixed("v"),
  point("a", at = [5, 0]),
  con_distance("v", "a", 5),
  con_horizontal("v", "a"),
  point("b"),
  con_distance("v", "b", 3),
  con_directed_angle("v", "a", "v", "b", 359.999),
]);
assert(solved(sol1),
       str("near-360 solve failed: ", failed_constraints(sol1)));

// Case 2: deg arithmetic that rounds to exactly 360 — wrap should map to 0.
sol2 = solve2d([
  point("v", at = [0, 0]), con_fixed("v"),
  point("a", at = [5, 0]),
  con_distance("v", "a", 5),
  con_horizontal("v", "a"),
  point("b"),
  con_distance("v", "b", 3),
  con_directed_angle("v", "a", "v", "b", 360 * (1 + 1e-15)),
]);
assert(solved(sol2),
       str("deg=360 wrap solve failed: ", failed_constraints(sol2)));

// Case 3: negative deg — wrap into [0, 360) means -90 → 270, so b should
// land near (5, -3) from the chevron geometry (270° CCW from +x is -y).
sol3 = solve2d([
  point("v", at = [0, 0]), con_fixed("v"),
  point("a", at = [5, 0]),
  con_distance("v", "a", 5),
  con_horizontal("v", "a"),
  point("b"),
  con_distance("v", "b", 3),
  con_directed_angle("v", "a", "v", "b", -90),
]);
assert(solved(sol3),
       str("deg=-90 wrap solve failed: ", failed_constraints(sol3)));
b3 = pt(sol3, "b");
// -90 should map to 270, putting b on the -y side.
assert(b3[1] < 0,
       str("expected -90 to wrap to 270 (b on -y), got ", b3));

echo(near_360 = pt(sol1, "b"),
     wrap_360 = pt(sol2, "b"),
     wrap_neg = b3);
