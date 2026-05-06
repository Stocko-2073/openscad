// solve2d_symmetric_house.scad — house silhouette demonstrating several of
// the constraints added after solve2d v1: pt_line_distance and at_midpoint.
//
// The walls are constrained to rise perpendicularly from the base; the
// eaves are levelled by horizontal(); the ridge is placed on a vertical
// centerline at a fixed perpendicular distance above the eaves. Using
// pt_line_distance to set wall and ridge heights avoids combining
// vertical() and distance() on the same point pair, which the underlying
// solver does not handle robustly.

base = 16;
wall = 12;
ridge_height = 6;

sol = solve2d([
  // Bottom corners: anchored — they define the world frame.
  point("bl", at = [0,    0]),
  point("br", at = [base, 0]),

  // Wall tops: free; verticals only set orientation, not length.
  point("tl"),
  point("tr"),
  vertical("bl", "tl"),
  vertical("br", "tr"),
  horizontal("tl", "tr"),

  // Wall height: tl sits 'wall' units away from the base line.
  // (Negative sign places it above; the sign selects which side.)
  pt_line_distance("tl", "bl", "br", -wall),

  // Eaves midpoint: midpoint of tl/tr.
  point("eaves_mid"),
  at_midpoint("eaves_mid", "tl", "tr"),

  // Ridge: free; sits on the vertical centerline above eaves_mid at a
  // known perpendicular distance from the eaves.
  point("ridge"),
  vertical("eaves_mid", "ridge"),
  pt_line_distance("ridge", "tl", "tr", -ridge_height),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol)));

echo(remaining_dof = dof(sol));   // expect 0 — fully constrained
echo(ridge = pt(sol, "ridge"));

// Sanity: ridge x-coordinate should be the centerline of the base.
ridge_pt = pt(sol, "ridge");
assert(abs(ridge_pt[0] - base/2) < 1e-6,
       str("ridge not centered: ", ridge_pt));

linear_extrude(3)
  polygon(poly(sol, ["bl", "br", "tr", "ridge", "tl"]));
