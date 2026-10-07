// solve2d_pts.scad — pts(sol) returns every point in the order solve2d()
// declared them: here z, a, m, not alphabetically.
sol = solve2d([
  point("z", at = [0, 0]),
  con_fixed("z"),
  point("a"),
  point("m"),
  con_horizontal("z", "a"),
  con_distance("z", "a", 10),
  con_perpendicular("z", "a", "m"),
  con_distance("a", "m", 10),
]);

assert(solved(sol));
echo(pts(sol));
// Expected: ECHO: [[0, 0], [10, 0], [10, 10]]   (z, a, m — insertion order)

linear_extrude(3) polygon(pts(sol));
