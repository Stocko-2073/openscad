// solve2d_rect_by_diagonal.scad — a rectangle defined by width and diagonal.
//
// Why use solve2d for this? Because OpenSCAD's built-in square(w, h) wants
// width and height. If you know width and diagonal — for example, a panel
// cut to a target diagonal — you'd otherwise have to compute the height
// yourself. The constraint solver lets you specify the dimensions you
// actually have and figures out the rest.

width = 30;
diagonal = 50;     // try other values: 40 (square), 60, 100

sol = solve2d([
  point("a", at = [0, 0]),
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
