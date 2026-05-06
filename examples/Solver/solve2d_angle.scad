// solve2d_angle.scad — read back the angle at a solved vertex.
//
// angle(sol, [a, b, c]) returns the CCW sweep in degrees from ray b->a
// to ray b->c, in [0, 360). The vertex is the middle point. Order
// matters: angle(sol, [a,b,c]) + angle(sol, [c,b,a]) == 360, so callers
// can pick either the interior or the reflex reading.

leg = 15;
thickness = 4;

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b"),
  point("c"),

  con_horizontal("a", "b"),
  con_distance("a", "b", leg),
  con_perpendicular("a", "b", "c"),
  con_distance("b", "c", leg),
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));

// Vertex is "b". The solver places a=(0,0), b=(15,0), c=(15,15), so
// b->a points in -x and b->c points in +y. CCW from -x to +y goes
// through -y and +x, sweeping three quadrants = 270 degrees. Reversing
// the order (c, b, a) gives the complementary 90 degree sweep — and
// matches the triangle's geometric corner angle at b.
abc = angle(sol, ["a", "b", "c"]);   // 270 — CCW sweep b->a to b->c
cba = angle(sol, ["c", "b", "a"]);   //  90 — CCW sweep b->c to b->a

echo(abc_deg = abc, cba_deg = cba);

assert(abs(abc - 270) < 1e-6, str("expected 270, got ", abc));
assert(abs(cba -  90) < 1e-6, str("expected 90, got ",  cba));
assert(abs((abc + cba) - 360) < 1e-6, "readings should sum to 360");

linear_extrude(thickness)
  polygon(poly(sol, ["a", "b", "c"]));
