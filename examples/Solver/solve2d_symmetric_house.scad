// solve2d_symmetric_house.scad — house silhouette using con_pt_line_distance
// and con_at_midpoint.
//
// Wall and ridge heights are set with con_pt_line_distance: the solver does
// not handle con_vertical() and con_distance() on the same pair of points
// robustly.

base = 16;
wall = 12;
ridge_height = 6;

sol = solve2d([
  // Bottom corners: pinned — they define the world frame. `at=` seeds the
  // location; `con_fixed` is what actually keeps the solver from moving them.
  point("bl", at = [0,    0]),
  point("br", at = [base, 0]),
  con_fixed("bl"),
  con_fixed("br"),

  // Wall tops: free; verticals only set orientation, not length.
  point("tl"),
  point("tr"),
  con_vertical("bl", "tl"),
  con_vertical("br", "tr"),
  con_horizontal("tl", "tr"),

  // Wall height: tl sits 'wall' units away from the base line.
  // (Negative sign places it above; the sign selects which side.)
  con_pt_line_distance("tl", "bl", "br", -wall),

  // Eaves midpoint: midpoint of tl/tr.
  point("eaves_mid"),
  con_at_midpoint("eaves_mid", "tl", "tr"),

  // Ridge: free; sits on the vertical centerline above eaves_mid at a
  // known perpendicular distance from the eaves.
  point("ridge"),
  con_vertical("eaves_mid", "ridge"),
  con_pt_line_distance("ridge", "tl", "tr", -ridge_height),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol)));

echo(remaining_dof = dof(sol));   // expect 0 — fully constrained
echo(ridge = pt(sol, "ridge"));

linear_extrude(3)
  polygon(poly(sol, ["bl", "br", "tr", "ridge", "tl"]));
