// solve2d_opposite_side.scad — disambiguation example for con_opposite_side.
//
// Anchor a at the origin and b at [20, 0]. Equal sides + a right angle
// at b determines the square *up to* which side d lies on. Without a
// half-plane constraint, the two valid solutions are: (1) the real
// square with d = [0, ±20] (sign depends on which side multi-start
// lands c on), and (2) a degenerate configuration where d coincides
// with b (both points satisfy |ad| = |cd| = 20 with |ac| = 20*sqrt(2)).
//
// con_opposite_side("a","c","d","b") asserts that d is on the opposite
// side of line ac from b — exactly the topology of a real square's
// crossed diagonals — and rejects the degenerate basin.
//
// (A one-anchor variant — anchoring only a, leaving b, c, d all
// unseeded — also has the same two-solution structure on paper, but
// in practice the d==b basin is so dominant that the multi-start
// retry doesn't reliably reach the real-square basin. Anchoring b
// reduces the search to the half-plane choice.)

sol = solve2d([
  point("a", at = [0, 0]),  con_fixed("a"),
  point("b", at = [20, 0]), con_fixed("b"),
  point("c"),
  point("d"),
  con_equal_length("a", "b", "b", "c"),
  con_equal_length("a", "b", "c", "d"),
  con_equal_length("a", "b", "a", "d"),
  con_perpendicular("a", "b", "c"),
  con_opposite_side("a", "c", "d", "b"),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol),
           ", active=", active_inequalities(sol), ")"));

a = pt(sol, "a"); b = pt(sol, "b"); c = pt(sol, "c"); d = pt(sol, "d");
function dist(p, q) = norm([p[0]-q[0], p[1]-q[1]]);

// All four sides == 20 (the equality constraints).
assert(abs(dist(a, b) - 20) < 1e-4, str("|ab|=", dist(a, b)));
assert(abs(dist(b, c) - 20) < 1e-4, str("|bc|=", dist(b, c)));
assert(abs(dist(c, d) - 20) < 1e-4, str("|cd|=", dist(c, d)));
assert(abs(dist(d, a) - 20) < 1e-4, str("|da|=", dist(d, a)));

// The crucial assertion: |bd| ≈ 20*sqrt(2). For the degenerate b == d
// solution, |bd| ≈ 0; for the real square, |bd| is the diagonal.
assert(abs(dist(b, d) - 20*sqrt(2)) < 1e-4,
       str("expected real-square diagonal |bd| ≈ ", 20*sqrt(2),
           ", got ", dist(b, d), " (degenerate b==d basin?)"));

// The half-plane constraint is satisfied with slack at the real-square
// solution, so it should not be in the active set.
assert(len(active_inequalities(sol)) == 0,
       str("expected no active inequalities, got ",
           active_inequalities(sol)));

echo(diag_bd = dist(b, d), iter = iterations(sol),
     active = active_inequalities(sol));
