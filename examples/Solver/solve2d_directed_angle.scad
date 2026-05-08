// solve2d_directed_angle.scad — reflex (> 180°) angle at a vertex.
//
// con_directed_angle(p1, p2, p3, p4, deg) pins the CCW (counter-
// clockwise) angle from line p1->p2 to line p3->p4 to deg, where deg
// is in [0, 360).
//
// Internally it expands into a regular con_angle (with magnitude
// min(deg, 360 - deg) in [0, 180]) plus an oriented half-plane that
// puts p4 on the LEFT of directed line p1->p2 for deg in (0, 180), or
// on the RIGHT for deg in (180, 360). At deg == 0 or deg == 180 the
// rays are colinear and only the magnitude is enforced.
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
  con_distance("v", "b", 4),
  // 270° CCW from v->a (which points along +x) lands b on the right
  // of v->a — i.e., -y. The solver picks that branch automatically.
  con_directed_angle("v", "a", "v", "b", 270),
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));

b = pt(sol, "b");
// 270° CCW from +x is -y, so b should be at (0, -4).
assert(abs(b[0]) < 1e-3 && abs(b[1] + 4) < 1e-3,
       str("expected b ~= (0, -4), got ", b));

echo(b = b, iter = iterations(sol), active = active_inequalities(sol));
