// solve2d_rect_by_diagonal.scad — a rectangle defined by width and diagonal.
//
// square(w, h) wants width and height. If what you know is the width and the
// diagonal (say, a panel cut to a target diagonal), the solver works out the
// height for you.

width = 30;
diagonal = 50;     // try other values: 40, 60, 100

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b"),
  point("c"),
  point("d"),

  // Two adjacent sides aligned to axes.
  con_horizontal("a", "b"),
  con_vertical("a", "d"),

  // Width along the bottom.
  con_distance("a", "b", width),

  // The "given" measurement is the diagonal a-c.
  con_distance("a", "c", diagonal),

  // Closure: c must be directly above b and directly right of d.
  con_vertical("b", "c"),
  con_horizontal("d", "c"),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol)));

// Height fell out of the solve.
height = pt(sol, "d")[1];
echo(width = width, diagonal = diagonal, height = height);

linear_extrude(3)
  polygon(poly(sol, ["a", "b", "c", "d"]));
