// solve2d_basic.scad — minimal sketch using the 2D constraint solver.
//
// solve2d() takes a list of points and constraints between them, runs a
// solver, and returns an opaque Solution value. The accessor functions
// pt(), poly(), solved(), dof(), failed_constraints() read coordinates
// and status back out.

leg = 15;
thickness = 4;

sol = solve2d([
  // Three points: 'a' is seeded at the origin and pinned there with
  // con_fixed so the whole sketch hangs off a known reference.
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b"),
  point("c"),

  // The shape: a horizontal leg of length 'leg', then a perpendicular
  // upward leg of the same length.
  con_horizontal("a", "b"),
  con_distance("a", "b", leg),
  con_perpendicular("a", "b", "c"),
  con_distance("b", "c", leg),
]);

// Always check before using.
assert(solved(sol),
       str("solver failed: ", failed_constraints(sol)));

echo(a = pt(sol, "a"), b = pt(sol, "b"), c = pt(sol, "c"));
echo(remaining_dof = dof(sol));    // expect 0 — fully constrained

// poly() collects named points in the given order, ready for polygon().
linear_extrude(thickness)
  polygon(poly(sol, ["a", "b", "c"]));
