// solve2d_scale_mm.scad — rectangle-by-diagonal at mm scale.
//
// Same geometry as solve2d_rect_by_diagonal.scad but scaled 1000×. The
// solver should produce the same relative result regardless of the absolute
// magnitudes, because tolerances scale with the sketch's length scale
// (median magnitude of seeds and length-bearing constraint values).

width = 30000;     // mm-scale: 30000 = 30m, or interpret as 30000 mm = 30m
diagonal = 50000;

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
       str("mm-scale solve failed: ", failed_constraints(sol)));

// Height = sqrt(diagonal^2 - width^2). The rectangle is symmetric about the
// x-axis so d may land at +height or -height; either is a correct solution.
expected_height = sqrt(diagonal*diagonal - width*width);
height = abs(pt(sol, "d")[1]);
rel_err = abs((height - expected_height) / expected_height);
assert(rel_err < 1e-6,
       str("mm-scale height off: got ", height,
           ", expected ", expected_height, ", rel_err=", rel_err));

echo(scale = "mm (1000x)", width = width, diagonal = diagonal,
     height = height, rel_err = rel_err, residual = residual(sol));

linear_extrude(0.001 * width)
  polygon(poly(sol, ["a", "b", "c", "d"]));
