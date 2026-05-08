// solve2d_directed_angle.scad — reflex (> 180°) angle at a vertex.
//
// con_directed_angle(p1, p2, p3, p4, deg, ref) lets you specify a CCW
// angle in [0, 360) between two lines. It expands internally into a
// con_angle (with magnitude min(deg, 360-deg) in [0, 180]) plus a
// con_same_side half-plane that binds p4 to the same side of line p1p2
// as the reference point ref.
//
// Pick ref on the side where p4 should land. For deg in (0, 180) that's
// the left of the directed line p1->p2; for deg in (180, 360) it's the
// right. At deg == 0 or deg == 180 the rays are parallel and the
// half-plane is undefined — only the magnitude is emitted; ref is
// ignored.
//
// This sketch is a chevron (an arrowhead): the tip vertex has a 270°
// reflex angle on the inside, which can't be written as con_angle alone.

sol = solve2d([
  // Tip vertex pinned at origin.
  point("v", at = [0, 0]), con_fixed("v"),
  // Right wing along +x.
  point("a", at = [5, 0]), con_distance("v", "a", 5), con_horizontal("v", "a"),
  // Left wing — to be solved.
  point("b"),
  // Reference point on the right side of v->a (i.e., -y), since for a
  // 270° CCW angle b ends up at (-y).
  point("ref", at = [0, -1]), con_fixed("ref"),
  con_distance("v", "b", 4),
  con_directed_angle("v", "a", "v", "b", 270, "ref"),
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));

b = pt(sol, "b");
// 270° CCW from +x is -y, so b should be at (0, -4).
assert(abs(b[0]) < 1e-3 && abs(b[1] + 4) < 1e-3,
       str("expected b ~= (0, -4), got ", b));

echo(b = b, iter = iterations(sol), active = active_inequalities(sol));
