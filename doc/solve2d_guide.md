# A User Guide to `solve2d`

`solve2d` is a built-in function that takes a list of points and constraints
between them, runs a geometric constraint solver, and gives you back an
opaque `Solution` value you can query for resolved coordinates. It's a
sketch-driven way to build 2D profiles you'd then `linear_extrude` or feed
to `polygon`.

> Looking for the formal spec? See [`doc/specs/solve2d.md`](specs/solve2d.md).

## Why use it?

The plain OpenSCAD way to draw a square with a 45° corner cut is to compute
the corner coordinates yourself:

```scad
// Without solve2d: do the math
polygon([[0,0], [10,0], [10,7], [3,10], [0,10]]);
```

That works for simple shapes. It gets miserable when the shape is described
by relations rather than coordinates ("two of these segments must be the
same length", "this corner is a right angle", "this point is 5 units below
that one"). `solve2d` lets you write down the relations directly and lets
the solver figure out the coordinates.

## Your first sketch

The smallest useful sketch is two points and a distance:

```scad
sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b"),
  con_distance("a", "b", 10),
]);

echo(solved(sol));   // true
echo(pt(sol, "b"));  // [10, 0]  (or some other 10-unit-distant point)
```

Four things happen here:

1. **`point("a", at = [0, 0])`** declares a point and *seeds* it at the
   origin. The seed is the solver's initial guess; on its own it does not
   pin the point.
2. **`con_fixed("a")`** pins `a` to its initial position. With the seed
   above, that pins it to the origin.
3. **`point("b")`** declares a free point. The solver will place it.
4. **`con_distance("a", "b", 10)`** says: the distance between `a` and `b`
   must be 10.

The solver places `b` somewhere 10 units from `a`. Since the only
relational constraint is the distance, `b` could be anywhere on a circle of
radius 10. The solver picks one based on `b`'s initial position. You can
verify how under-constrained the system is with `dof(sol)` — for this
sketch, it's 1 (the angle around `a` is unconstrained).

## Building a real shape: a right triangle

```scad
sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b"),
  point("c"),
  con_distance("a", "b", 10),
  con_horizontal("a", "b"),
  con_distance("b", "c", 10),
  con_perpendicular("a", "b", "c"),
]);

linear_extrude(5) polygon(poly(sol, ["a", "b", "c"]));
```

Read it as: *pin `a` at the origin; `b` is 10 units from `a` along a
horizontal line; `c` is 10 units from `b`; the corner at `b` is a right
angle.* The result is the upright right-isoceles triangle a 7th-grader would
draw. `dof(sol)` is 0 — fully constrained.

`poly(sol, [...])` packages up the resolved coordinates in the order you
give it, ready to drop into `polygon()`.

`pts(sol)` returns every solved point in declaration order — useful when you
want the full point set without listing names, e.g. `polygon(pts(sol))`.

## Seeding vs. anchoring vs. free

A point's role in the sketch is set by two independent things: whether it
has a *seed* (initial guess from `at=`) and whether it's *anchored* by
`con_fixed`.

* **Free** (no `at`, no `con_fixed`): "Figure out where this goes." The
  solver picks a starting position and moves the point freely to satisfy
  constraints.

* **Seeded** (`at = [x, y]`, no `con_fixed`): "Start looking near here, but
  move me as needed." The seed is just a hint to the solver — useful for
  picking one solution out of many in under-constrained sketches, or for
  guiding the solver away from a degenerate start. The point is still free
  to move during the solve.

* **Anchored** (`con_fixed`, with or without `at=`): "Don't move this
  point." `con_fixed` pins the point to its initial position. Almost
  always you want to combine it with `at=` so you control *where* it's
  pinned: `point("a", at=[0,0]), con_fixed("a")`. Without an `at=`, the
  pin lands at the implementation's seeding offset, which is rarely what
  you want.

A common pattern is to anchor exactly one point — usually `(0, 0)` — and
let everything else be solved relative to it. If you anchor *too much*, you
either over-constrain the system (solver fails) or you've turned the sketch
into a glorified `polygon([...])`.

### Previewing seeds before the solve

To see exactly where the solver will *start* — before any constraint
pulls a point — pass `solve = false`:

```scad
items = [
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 5]),
  con_distance("a", "b", 7),
];

sol_seed = solve2d(items, solve = false);   // no Newton run
sol      = solve2d(items);

%polygon(poly(sol_seed, ["a", "b"]));  // ghost: where the points start
 polygon(poly(sol,      ["a", "b"]));  // solid: where the solver landed
```

`solve = false` short-circuits the solver entirely:

* Each seeded point reports its `at=` value verbatim.
* Unseeded points report the deterministic golden-angle default (the
  same start position attempt 0 of the multi-start uses).
* `solved(sol_seed)` is `false` (no solve has happened).
* `pt`, `pts`, and `poly` return the seed coords; `dof`, `residual`,
  `iterations`, `failed_constraints`, and `active_inequalities` are all
  zero/empty.

This is purely for visualization and debugging; it never modifies how
the actual solve runs.

## What constraints exist

For v1, the vocabulary is small but covers most polygon profiles:

All constraint built-ins are prefixed with `con_` so they don't collide
with user-defined names like `distance`, `angle`, or `parallel`.

| Constraint | What it does |
|---|---|
| `con_coincident(p1, p2)` | The two points share a location. |
| `con_distance(p1, p2, d)` | Distance is exactly `d`. |
| `con_horizontal(p1, p2)` | Segment p1–p2 is horizontal. |
| `con_vertical(p1, p2)` | Segment p1–p2 is vertical. |
| `con_perpendicular(p1, v, p2)` | Segments v–p1 and v–p2 form a right angle. |
| `con_parallel(p1, p2, p3, p4)` | Segment p1–p2 is parallel to p3–p4. |
| `con_angle(p1, p2, p3, p4, deg)` | Signed angle between p1–p2 and p3–p4. |
| `con_fixed(p)` | Pin the point to its initial position (the `at=` seed if given, otherwise the default seeding offset). |
| `con_pt_on_line(p, la, lb)` | `p` lies on the line through la–lb. |
| `con_pt_on_segment(p, la, lb)` | `p` lies on the closed segment la–lb (endpoints included). |
| `con_pt_line_distance(p, la, lb, d)` | Signed distance from `p` to line la–lb. |
| `con_at_midpoint(m, la, lb)` | `m` is the midpoint of la–lb. |
| `con_equal_length(a, b, c, d)` | `|a–b| = |c–d|`. |
| `con_length_ratio(a, b, c, d, r)` | `|a–b| / |c–d| = r`. |
| `con_length_difference(a, b, c, d, diff)` | `|a–b| - |c–d| = diff`. |
| `con_eq_len_pt_line_d(p, la, lb, da, db)` | Length of la–lb equals distance from `p` to da–db. |
| `con_eq_pt_ln_distances(p1, l1a, l1b, p2, l2a, l2b)` | Distance from `p1` to line l1 equals distance from `p2` to line l2. |
| `con_equal_angle(a, b, c, d, e, f, g, h)` | Angle(ab,cd) = Angle(ef,gh). |
| `con_symmetric_horiz(p1, p2)` | Mirror across V-axis (segment p1–p2 is horizontal). |
| `con_symmetric_vert(p1, p2)` | Mirror across U-axis (segment p1–p2 is vertical). |
| `con_symmetric_line(p1, p2, la, lb)` | `p1` and `p2` are mirror images across line la–lb. |

Things *not* yet supported: arcs, circles, tangency, 3D, and `SLVS_C_SYMMETRIC`
(which needs an explicit symmetry-plane entity). (See the "Out of Scope"
section of the spec.)

## Bounded sketches with inequalities

Equality constraints fix the geometry exactly. Sometimes you want to
*bound* a value rather than nail it down — "this segment is at most 6
units long", "this corner angle is no more than 30°". `solve2d` supports
four scalar inequalities in both `<=` (`con_le_*`) and `>=` (`con_ge_*`)
forms, plus the half-plane `same_side`/`opposite_side` pair:

| Constraint | Bound |
|---|---|
| `con_le_distance(p1, p2, d)` | `\|p1–p2\| <= d` |
| `con_ge_distance(p1, p2, d)` | `\|p1–p2\| >= d` |
| `con_le_pt_line_distance(p, la, lb, d)` | signed distance from `p` to la–lb is `<= d` |
| `con_ge_pt_line_distance(p, la, lb, d)` | signed distance from `p` to la–lb is `>= d` |
| `con_le_length_difference(a, b, c, d, diff)` | `\|a–b\| - \|c–d\| <= diff` |
| `con_ge_length_difference(a, b, c, d, diff)` | `\|a–b\| - \|c–d\| >= diff` |
| `con_le_angle(p1, p2, p3, p4, deg)` | undirected angle (in `[0, 180]`) `<= deg` |
| `con_ge_angle(p1, p2, p3, p4, deg)` | undirected angle (in `[0, 180]`) `>= deg` |
| `con_same_side(a, b, p, q)` | `p` is on the same side of line ab as `q` |
| `con_opposite_side(a, b, p, q)` | `p` is on the opposite side of line ab from `q` |

The solver decides per-call which inequalities matter. If your sketch
already satisfies an inequality without it being enforced, the solver
leaves it slack — the constraint adds no DOF cost. If the unconstrained
solve would violate the inequality, the solver activates it (treats it as
`g(x) == bound`) and re-solves. You can inspect what happened:

```scad
sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_horizontal("a", "b"),
  con_le_distance("a", "b", 4),     // |ab| <= 4
]);

echo(iterations(sol));            // 2 — one solve, found violation, re-solved
echo(active_inequalities(sol));   // ["con_le_distance(a,b,4.000000)"]
echo(pt(sol, "b"));               // [4, 0] — the bound is binding
```

`iterations(sol)` returns the outer-loop count (`0` if no inequalities,
otherwise at least `1`). `active_inequalities(sol)` lists the inequalities
that ended up being binding at the solution.

If two inequalities contradict each other, or an inequality contradicts
your equality constraints, the solver bails the same way it does for
infeasible equality-only systems: `solved(sol) == false`, with the
reason listed in `failed_constraints(sol)`.

A few caveats:

* `con_le_pt_line_distance` and `con_ge_pt_line_distance` are **signed**,
  mirroring `con_pt_line_distance`. `signed_dist(p, line) <= d` bounds
  `p` only on one side of the line; the `ge` form bounds it from the
  other side. For "within `d` on either side", combine an `le` and a
  `ge` form, or two `le_pt_line_distance` with opposite line orientations.
* For **directed angles in `[0, 360)`** (including reflex angles > 180°),
  use `con_directed_angle`. See the next section.
* `con_le_angle` and `con_ge_angle` measure the undirected angle in `[0, 180]`. Pass
  non-negative `deg`. SolveSpace's underlying angle constraint has a
  line-direction ambiguity; on rare configurations the solver may converge
  to the supplementary angle (`180 - deg`). Seed your points to nudge the
  geometry if this matters.
* `con_same_side` and `con_opposite_side` are **half-plane** constraints
  used to break the discrete multiplicity that distance/angle systems
  leave behind. A square anchored at two adjacent corners with three
  side-equality constraints and one right angle has both a real-square
  solution and a degenerate one with two coincident vertices; adding
  `con_opposite_side("a","c","d","b")` selects the real-square basin.
  (The one-anchor variant has the same algebraic structure but the
  degenerate basin dominates the seed-perturbation multi-start in
  practice, so a second anchor is needed.)
  These constraints bind asymmetrically: when active, only `p` (the
  third argument) is pinned to the line through `a` and `b`. `q` is a
  sign reference only. Swap the argument order (`con_same_side(a,b,q,p)`)
  if you want `q` to be the pinned one instead.

## Directed angles (including reflex angles)

`con_angle` measures the *undirected* angle between two segments — a
value in `[0, 180]`. That's fine for "this corner is 90°" or "these
segments meet at 30°", but it can't express the 270° reflex angle on the
inside of a chevron's tip, or distinguish a 60° turn from a 300° turn at
the same vertex. For those, use `con_directed_angle`:

```
con_directed_angle(p1, p2, p3, p4, deg)
```

This pins the **CCW (counter-clockwise) angle** from line `p1→p2` to
line `p3→p4` to `deg`, where `deg` can be anywhere in `[0, 360)`. There's
no extra reference argument — the rotation direction is implicit in the
value of `deg`:

| `deg` | `p4` lands on |
|---|---|
| in `(0, 180)` | left of directed line `p1→p2` |
| in `(180, 360)` | right of directed line `p1→p2` |
| `0` or `180` | colinear with `p1→p2` (degenerate; only the magnitude is enforced) |

"Left" and "right" are taken in the standard CCW orientation: from the
perspective of someone walking along `p1→p2`, left is the half-plane
where the cross product `(p2−p1) × (q−p1)` is positive.

### A worked example: the chevron tip

A chevron (arrowhead) has a tip vertex with a 270° reflex angle on the
inside. Pin the tip at the origin with one wing pointing along `+x`, and
ask the solver to place the other wing 270° CCW from there:

```scad
sol = solve2d([
  // Tip vertex pinned at origin.
  point("v", at = [0, 0]), con_fixed("v"),
  // Right wing along +x.
  point("a", at = [5, 0]),
  con_distance("v", "a", 5),
  con_horizontal("v", "a"),
  // Left wing — solver places it.
  point("b"),
  con_distance("v", "b", 4),
  con_directed_angle("v", "a", "v", "b", 270),
]);

echo(pt(sol, "b"));   // [0, -4]
```

`v→a` points along `+x`. 270° CCW from `+x` is straight down (`-y`),
which is the **right** half-plane of `v→a`, so the solver places `b` at
`(0, -4)`. If you change `270` to `90`, the solver instead places `b` at
`(0, +4)` — same magnitude, opposite half-plane.

### Why this exists (and how it works)

Internally, `con_directed_angle` expands into two pieces:

1. A regular `con_angle` with magnitude `min(deg, 360 − deg)` —
   always in `[0, 180]`. This pins the *undirected* angle.
2. An oriented half-plane that binds `p4` to the appropriate side of
   line `p1→p2`. This picks one of the two algebraic solutions the
   undirected angle leaves behind.

So a 270° directed angle is really a 90° undirected angle plus a "must
be on the right of `p1→p2`" hint.

### Caveats

* **Leave at least one point unseeded.** If Newton's first guess lands
  on the wrong branch (the `360 − deg` mirror image), the half-plane
  conflicts with the angle equality and the solve bails. Multi-start
  then perturbs the *unseeded* points to retry from a different
  starting layout. If every point is seeded, there's nothing to
  perturb. In the chevron above, `b` is unseeded for exactly this
  reason.
* **`deg` near 0 or 180 is degenerate.** The two rays are colinear, so
  "left" and "right" of `p1→p2` are undefined and only the magnitude is
  enforced.
* **`deg` is in `[0, 360)`, not `(-180, 180]`.** `con_directed_angle(...,
  -90)` is rejected — use `270` instead.

## Parametric sketches

`solve2d` is a function: it can be called with computed inputs. This makes
it easy to wrap a sketch in a parametric module:

```scad
module right_tri(leg = 10, thickness = 5) {
  sol = solve2d([
    point("a", at = [0, 0]),
    con_fixed("a"),
    point("b"),
    point("c"),
    con_distance("a", "b", leg),
    con_horizontal("a", "b"),
    con_distance("b", "c", leg),
    con_perpendicular("a", "b", "c"),
  ]);
  if (solved(sol)) {
    linear_extrude(thickness)
      polygon(poly(sol, ["a", "b", "c"]));
  }
}

right_tri(leg = 20, thickness = 8);
translate([30, 0]) right_tri(leg = 15, thickness = 3);
```

## Diagnosing failures

When the solver can't satisfy your constraints, it returns a `Solution`
with `solved(sol) == false`. Don't silently ignore that — wrap usage in an
assertion or a diagnostic:

```scad
sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [5, 0]),
  con_fixed("b"),               // pinned at distance 5 ...
  con_distance("a", "b", 10),   // ... but constraint says 10. Conflict!
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
```

`failed_constraints(sol)` returns a list of human-readable strings
identifying the constraints SolveSpace blamed. They're for your eyes, not
for parsing — formatting may change.

You can also inspect `dof(sol)`:

* `dof = 0` and `solved = true` — fully constrained, deterministic.
* `dof > 0` and `solved = true` — under-constrained; the solver picked
  *one* of many valid layouts. Often fine for a profile, but if you keep
  getting different results, add a constraint.
* `solved = false` — over-constrained or contradictory.

## Pitfalls

* **Unseeded points start near the origin.** Points without an `at=`
  seed get small staggered initial positions to avoid degenerate starts
  (every point at `(0,0)` makes constraints like "horizontal" undefined).
  For simple sketches this is invisible. For complex ones, the solver may
  pick a layout you didn't expect — supply `at=` on a few points to nudge
  it toward the solution branch you want, and `con_fixed` to pin the rest
  of the geometry to a reference frame.

* **The opaque `Solution` is opaque.** You can't index `sol[0]`, you can't
  iterate it, you can't compare it to an object. Only the accessor
  functions (`pt`, `pts`, `poly`, `solved`, `dof`, `residual`,
  `failed_constraints`, `iterations`, `active_inequalities`) read values
  out. This is by design — see the spec.

* **Names are case-sensitive strings.** `pt(sol, "A")` and `pt(sol, "a")`
  are different points. A typo in a constraint silently warns at solve
  time and the constraint is dropped. Watch the console.

* **Constraint names are prefixed.** Every constraint built-in starts
  with `con_` (`con_distance`, `con_angle`, `con_le_distance`, ...) so
  they don't collide with names you'd naturally define yourself. The
  non-prefixed accessors and entity helpers (`point`, `solve2d`, `solved`,
  `pt`, `pts`, `poly`, `dof`, `residual`, `failed_constraints`,
  `iterations`, `active_inequalities`) are still subject to OpenSCAD's
  normal lookup rules — defining your own `function pt(...)` shadows the
  accessor.

## Examples in the repo

Runnable examples ship with OpenSCAD; all are accessible from
**File → Examples → Solver** in the GUI:

* `examples/Solver/solve2d_basic.scad` — minimal sketch with constraint
  inspection.
* `examples/Solver/solve2d_rect_by_diagonal.scad` — a rectangle defined
  by its width and diagonal length, with the height emerging from the
  solve. Demonstrates the headline value of constraint solving: specify
  the dimensions you have, not the ones a `module` happens to ask for.
* `examples/Solver/solve2d_symmetric_house.scad` — a house silhouette
  using `con_pt_line_distance` and `con_at_midpoint` to size the walls
  and place the ridge.
* `examples/Solver/solve2d_le_distance_smoke.scad` — slack `con_le_distance`
  (loop converges in one iteration with empty active set).
* `examples/Solver/solve2d_le_distance_binding.scad` — binding
  `con_le_distance` (loop activates the constraint to push the bound).
* `examples/Solver/solve2d_le_distance_infeasible.scad` — contradictory
  equality + inequality (loop bails cleanly, `solved == false`).
* `examples/Solver/solve2d_le_pt_line_distance.scad` — signed-distance
  inequality.
* `examples/Solver/solve2d_le_length_difference.scad` — bound on the
  difference of two segment lengths.
* `examples/Solver/solve2d_le_angle.scad` — bound on the angle between two
  segments.
* `examples/Solver/solve2d_ge_distance_smoke.scad`,
  `solve2d_ge_distance_binding.scad`,
  `solve2d_ge_distance_infeasible.scad`,
  `solve2d_ge_pt_line_distance.scad`,
  `solve2d_ge_length_difference.scad`,
  `solve2d_ge_angle.scad` — `>=` counterparts of the above.
* `examples/Solver/solve2d_seed_preview.scad` — `solve = false` returns a
  Solution at the seed coords for ghosting alongside the solved sketch.
* `examples/Solver/solve2d_directed_angle.scad` — reflex (270°) angle at
  a chevron tip via `con_directed_angle`.

## Where to go next

* Read the [formal spec](specs/solve2d.md) for normative behavior.
* `submodules/SolveSpaceLib/include/slvs.h` — the underlying C API. The
  full constraint vocabulary it offers is much larger than `solve2d`'s v1
  surface.
