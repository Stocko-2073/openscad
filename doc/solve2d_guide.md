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
  functions (`pt`, `poly`, `solved`, `dof`, `residual`,
  `failed_constraints`) read values out. This is by design — see the spec.

* **Names are case-sensitive strings.** `pt(sol, "A")` and `pt(sol, "a")`
  are different points. A typo in a constraint silently warns at solve
  time and the constraint is dropped. Watch the console.

* **Constraint names are prefixed.** Every constraint built-in starts
  with `con_` (`con_distance`, `con_angle`, ...) so they don't collide
  with names you'd naturally define yourself. The non-prefixed accessors
  and entity helpers (`point`, `solve2d`, `solved`, `pt`, `poly`,
  `dof`, `residual`, `failed_constraints`) are still subject to OpenSCAD's
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

## Where to go next

* Read the [formal spec](specs/solve2d.md) for normative behavior.
* `submodules/SolveSpaceLib/include/slvs.h` — the underlying C API. The
  full constraint vocabulary it offers is much larger than `solve2d`'s v1
  surface.
