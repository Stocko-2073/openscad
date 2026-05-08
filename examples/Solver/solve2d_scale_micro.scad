// solve2d_scale_micro.scad — rectangle-by-diagonal at micron scale.
//
// Same geometry as solve2d_rect_by_diagonal.scad but scaled 0.001×. The
// solver should produce the same relative result regardless of the absolute
// magnitudes; this is the small-scale companion to solve2d_scale_mm.scad.
// Together they verify the active-set and recovery thresholds scale
// correctly across six orders of magnitude.

width = 0.030;     // 0.030 = 30 micron in mm units
diagonal = 0.050;

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b"),
  point("c"),
  point("d"),

  con_horizontal("a", "b"),
  con_vertical("a", "d"),
  con_distance("a", "b", width),
  con_distance("a", "c", diagonal),
  con_vertical("b", "c"),
  con_horizontal("d", "c"),
]);

assert(solved(sol),
       str("micro-scale solve failed: ", failed_constraints(sol)));

// The rectangle is symmetric about the x-axis so d may land at +height or
// -height; either is a correct solution.
expected_height = sqrt(diagonal*diagonal - width*width);
height = abs(pt(sol, "d")[1]);
rel_err = abs((height - expected_height) / expected_height);
assert(rel_err < 1e-6,
       str("micro-scale height off: got ", height,
           ", expected ", expected_height, ", rel_err=", rel_err));

echo(scale = "micro (0.001x)", width = width, diagonal = diagonal,
     height = height, rel_err = rel_err, residual = residual(sol));

linear_extrude(width)
  polygon(poly(sol, ["a", "b", "c", "d"]));
