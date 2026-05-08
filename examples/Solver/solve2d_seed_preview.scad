// solve2d_seed_preview.scad — preview points at their `at=` seeds without
// running the solver.
//
// Pass `solve = false` to short-circuit Slvs_Solve: each point reports its
// `at=` seed verbatim, and unseeded points report the deterministic
// golden-angle default. `solved(sol) == false` because no solve happened —
// pt(), pts(), and poly() still work for visualization.
//
// A common workflow is to ghost the seed sketch alongside the solved one
// so you can see how Newton moved the geometry:
//
//   sol_seed = solve2d(items, solve = false);
//   sol      = solve2d(items);
//   %polygon(poly(sol_seed, ["a", "b", "c", "d"]));   // ghost = before
//    polygon(poly(sol,      ["a", "b", "c", "d"]));   // solid = after

items = [
  point("a", at = [0, 0]),
  point("b", at = [10, 5]),
  con_fixed("a"),
  con_distance("a", "b", 7),    // forces |ab| = 7; the seed's 11.18 will move
];

sol_seed = solve2d(items, solve = false);
sol      = solve2d(items);

assert(!solved(sol_seed),
       "solve=false: solved(sol) should be false (no solve was run)");
assert(solved(sol),
       str("real solve failed: ", failed_constraints(sol)));

// Seed b: reported verbatim from at=[10,5].
b_seed = pt(sol_seed, "b");
assert(b_seed == [10, 5], str("expected seed b==[10,5], got ", b_seed));

// Solved b: pulled in to satisfy |ab|=7.
b_solved = pt(sol, "b");
assert(abs(sqrt(b_solved[0]*b_solved[0] + b_solved[1]*b_solved[1]) - 7) < 1e-4,
       str("expected solved |ab|==7, got ", b_solved));

echo(seed = pt(sol_seed, "b"), solved = pt(sol, "b"));
