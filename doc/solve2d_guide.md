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
  point("b"),
  distance("a", "b", 10),
]);

echo(solved(sol));   // true
echo(pt(sol, "b"));  // [10, 0]  (or some other 10-unit-distant point)
```

Three things happen here:

1. **`point("a", at = [0, 0])`** declares an *anchored* point. Its position
   is fixed before the solver starts.
2. **`point("b")`** declares a *free* point. The solver will place it.
3. **`distance("a", "b", 10)`** says: the distance between `a` and `b` must
   be 10.

The solver places `b` somewhere 10 units from `a`. Since the only constraint
is the distance, `b` could be anywhere on a circle of radius 10. The solver
picks one based on `b`'s initial position. You can verify how
under-constrained the system is with `dof(sol)` — for this sketch, it's 1
(the angle around `a` is unconstrained).

## Building a real shape: a right triangle

```scad
sol = solve2d([
  point("a", at = [0, 0]),
  point("b"),
  point("c"),
  distance("a", "b", 10),
  horizontal("a", "b"),
  distance("b", "c", 10),
  perpendicular("a", "b", "c"),
]);

linear_extrude(5) polygon(poly(sol, ["a", "b", "c"]));
```

Read it as: *put `a` at the origin; `b` is 10 units from `a` along a
horizontal line; `c` is 10 units from `b`; the corner at `b` is a right
angle.* The result is the upright right-isoceles triangle a 7th-grader would
draw. `dof(sol)` is 0 — fully constrained.

`poly(sol, [...])` packages up the resolved coordinates in the order you
give it, ready to drop into `polygon()`.

## Anchored vs. free points

This is the most important distinction:

* **Anchored** (`at = [x, y]`): "I know where this is. Don't move it."
  Useful for one origin point per sketch, fixed reference positions, or
  positions you've computed elsewhere in OpenSCAD.

* **Free** (no `at`): "Figure out where this goes." The solver decides,
  subject to whatever constraints reference the point.

A common pattern is to anchor exactly one point — usually `(0, 0)` — and
let everything else be solved relative to it. If you anchor *too much*, you
either over-constrain the system (solver fails) or you've turned the sketch
into a glorified `polygon([...])`.

## What constraints exist

For v1, the vocabulary is small but covers most polygon profiles:

| Constraint | What it does |
|---|---|
| `coincident(p1, p2)` | The two points share a location. |
| `distance(p1, p2, d)` | Distance is exactly `d`. |
| `horizontal(p1, p2)` | Segment p1–p2 is horizontal. |
| `vertical(p1, p2)` | Segment p1–p2 is vertical. |
| `perpendicular(p1, v, p2)` | Segments v–p1 and v–p2 form a right angle. |
| `parallel(p1, p2, p3, p4)` | Segment p1–p2 is parallel to p3–p4. |
| `angle(p1, p2, p3, p4, deg)` | Signed angle between p1–p2 and p3–p4. |
| `fixed(p)` | Pin the point to its current solved position. |
| `pt_on_line(p, la, lb)` | `p` lies on the line through la–lb. |
| `pt_line_distance(p, la, lb, d)` | Signed distance from `p` to line la–lb. |
| `at_midpoint(m, la, lb)` | `m` is the midpoint of la–lb. |
| `equal_length(a, b, c, d)` | `|a–b| = |c–d|`. |
| `length_ratio(a, b, c, d, r)` | `|a–b| / |c–d| = r`. |
| `length_difference(a, b, c, d, diff)` | `|a–b| - |c–d| = diff`. |
| `eq_len_pt_line_d(p, la, lb, da, db)` | Length of la–lb equals distance from `p` to da–db. |
| `eq_pt_ln_distances(p1, l1a, l1b, p2, l2a, l2b)` | Distance from `p1` to line l1 equals distance from `p2` to line l2. |
| `equal_angle(a, b, c, d, e, f, g, h)` | Angle(ab,cd) = Angle(ef,gh). |
| `symmetric_horiz(p1, p2)` | Mirror across V-axis (segment p1–p2 is horizontal). |
| `symmetric_vert(p1, p2)` | Mirror across U-axis (segment p1–p2 is vertical). |
| `symmetric_line(p1, p2, la, lb)` | `p1` and `p2` are mirror images across line la–lb. |

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
    point("b"),
    point("c"),
    distance("a", "b", leg),
    horizontal("a", "b"),
    distance("b", "c", leg),
    perpendicular("a", "b", "c"),
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
  point("b", at = [5, 0]),     // anchored at distance 5 ...
  distance("a", "b", 10),       // ... but constraint says 10. Conflict!
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

* **All free points start near the origin.** `solve2d` seeds free points
  with small staggered initial positions to avoid degenerate starts (every
  point at `(0,0)` makes constraints like "horizontal" undefined). For
  simple sketches this is invisible. For complex ones, the solver may pick
  a layout you didn't expect — anchor a couple of points to nudge it.

* **The opaque `Solution` is opaque.** You can't index `sol[0]`, you can't
  iterate it, you can't compare it to an object. Only the accessor
  functions (`pt`, `poly`, `solved`, `dof`, `residual`,
  `failed_constraints`) read values out. This is by design — see the spec.

* **Names are case-sensitive strings.** `pt(sol, "A")` and `pt(sol, "a")`
  are different points. A typo in a constraint silently warns at solve
  time and the constraint is dropped. Watch the console.

* **Built-ins shadow.** If you've already defined `function distance(a, b)`
  in your project, you'll keep using yours, not the sketch helper. Use
  different names if you want both. (You probably already have `distance`
  defined; consider renaming sketch helpers locally if so:
  `_d = distance; ... _d("a","b",10) ...`.)

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
  using `pt_line_distance` and `at_midpoint` to size the walls and place
  the ridge.

## Where to go next

* Read the [formal spec](specs/solve2d.md) for normative behavior.
* `submodules/SolveSpaceLib/include/slvs.h` — the underlying C API. The
  full constraint vocabulary it offers is much larger than `solve2d`'s v1
  surface.
