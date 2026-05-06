# `solve2d` and the `Solution` Value Type

This is the normative specification for the 2D geometric constraint solver
exposed via the built-in `solve2d` function and the opaque `Solution` value
type. The implementation is in `src/core/SolutionType.{h,cc}` and
`src/core/builtin_solve.cc`, backed by `submodules/SolveSpaceLib`.

A user guide with worked examples lives at [`doc/solve2d_guide.md`](../solve2d_guide.md).

## Definition of Terms

* "Sketch input" is the single positional vector argument to `solve2d`. Each
  element is either a sketch entity or a sketch constraint, both produced by
  the helper built-in functions documented below.
* "Sketch entity" is a built-in function call that produces a tagged
  `ObjectType` describing a geometric primitive (currently only points).
* "Sketch constraint" is a built-in function call that produces a tagged
  `ObjectType` describing a relation between sketch entities.
* "Anchored point" is a point whose `at` keyword argument is supplied: its
  coordinates are fixed before solving.
* "Free point" is a point whose `at` keyword argument is omitted: its
  coordinates are solved for.
* "Workplane" is the fixed XY plane at the world origin (Z = 0). All sketch
  geometry lies on it.
* "Solution" is the opaque value type returned by `solve2d`. It has its own
  `Value::Type::SOLUTION` variant and is not interchangeable with `object`.
* "Degree of freedom" (DOF) is the count of unconstrained scalar parameters
  remaining after the solver runs, as reported by SolveSpace.

## Interpretation

### `solve2d`

* `solve2d(items)` shall accept exactly one positional argument: a vector of
  sketch entities and sketch constraints.
* `solve2d` shall return a value of type `solution` in all cases where the
  argument is a vector, regardless of whether the constraint system is
  satisfiable.
* `solve2d` shall return `undef` when the argument is not a vector or when
  the argument count is not exactly one.
* When the constraint system is satisfiable, the returned solution shall
  have `solved(sol) == true`.
* When the constraint system is unsatisfiable, over-constrained, or fails
  to converge, the returned solution shall have `solved(sol) == false`,
  and `failed_constraints(sol)` shall list a non-empty subset of constraints
  identified by the solver as causing the failure.
* The returned solution shall record the final (post-solve) `[x, y]` of
  every point declared in the input under its declared name, regardless of
  whether the overall solve succeeded.
* `solve2d` shall warn (via `message_group::Warning`) and skip — but not
  abort — any input element that is not a recognized sketch entity or
  sketch constraint, any constraint with the wrong number or kind of
  arguments, or any constraint referencing an unknown point name.
* Free points whose initial position is not otherwise determined shall be
  seeded with implementation-defined non-coincident initial values. The
  exact seeding scheme is not part of this specification and may change.
* `solve2d` shall not be re-entrant on a single thread; it owns the
  underlying SolveSpace state for the duration of one call. Independent
  calls in sequence are safe.

### Solution accessors

* `solved(sol)` shall return a `bool`. It shall be `true` when SolveSpace
  returned `SLVS_RESULT_OKAY`, and shall additionally be `true` when
  SolveSpace returned `SLVS_RESULT_INCONSISTENT` but the maximum
  per-constraint residual computed against the solved point coordinates is
  below an implementation-defined tolerance — this recovers from
  SolveSpace's rank-deficient-Jacobian false positives without masking
  genuine failures.
* `dof(sol)` shall return a `number` equal to the SolveSpace-reported degree
  of freedom count.
* `residual(sol)` shall return a `number` equal to the maximum absolute
  per-constraint residual computed against the solved point coordinates.
  Constraint kinds whose residuals are not locally computed contribute
  `0`; callers shall not rely on a non-zero value as proof of failure
  beyond the `solved(sol)` flag.
* `pt(sol, name)` shall return a 2-element vector `[x, y]` for the named
  point, or `undef` with a warning if no such point exists in the solution.
* `poly(sol, names)` shall return a list of 2-element vectors in the order
  given by `names`. If any name is missing, the function shall return
  `undef` with a warning.
* `failed_constraints(sol)` shall return a vector of strings naming
  constraints SolveSpace identified as failing. The strings are descriptive
  summaries (e.g. `"distance(a,b)"`) and shall not be parsed programmatically.
* All accessors shall return `undef` and warn when given a non-`solution`
  first argument.

### The `solution` value type

* The `solution` type is opaque. The language shall not expose its
  internal fields through indexing (`sol[k]`), member access, or iteration.
* Two `solution` values shall compare equal iff they share the same
  underlying data buffer (i.e., one was cloned from the other).
* The relational operators `<`, `<=`, `>`, `>=` shall return `undef`.
* The string representation (used by `echo`) shall be of the form
  `Solution(solved=<bool>, dof=<int>, residual=<number>, points={<names>})`.
  The exact formatting is not part of this specification.
* `solution` is a truthy value: `bool(sol) == true` regardless of
  `solved(sol)`.
* `solution` may be passed as an argument, returned from user functions,
  and stored in vectors and objects. It may not be serialized to or
  reconstructed from any export format.

## Argument Parsing

### Entities

* `point(name)` and `point(name, at = [x, y])`:
  * `name` shall be a `string`. If not, the call shall return `undef` with
    a warning.
  * `at`, if supplied, shall be a 2-element vector of numbers. If supplied
    but malformed, the value shall be ignored and a warning emitted; the
    resulting point shall be treated as free.

### Constraints

In every constraint, point-name arguments shall be `string`s and numeric
arguments shall be `number`s. Mismatched arity or types shall cause the
constraint factory to return `undef` with a warning.

| Form | Arity | Meaning |
|---|---|---|
| `coincident(p1, p2)` | 2 strings | Points coincide. |
| `distance(p1, p2, d)` | 2 strings + 1 number | Euclidean distance is `d`. |
| `horizontal(p1, p2)` | 2 strings | Segment p1–p2 is horizontal. |
| `vertical(p1, p2)` | 2 strings | Segment p1–p2 is vertical. |
| `perpendicular(p1, vertex, p2)` | 3 strings | Segments vertex–p1 and vertex–p2 are perpendicular. |
| `parallel(p1, p2, p3, p4)` | 4 strings | Segments p1–p2 and p3–p4 are parallel. |
| `angle(p1, p2, p3, p4, deg)` | 4 strings + 1 number | Signed angle between p1–p2 and p3–p4 is `deg` degrees. |
| `fixed(p)` | 1 string | The point shall remain at its current position. |
| `pt_on_line(p, la, lb)` | 3 strings | Point `p` shall lie on the infinite line through la–lb. |
| `pt_line_distance(p, la, lb, d)` | 3 strings + 1 number | Signed perpendicular distance from `p` to the line la–lb shall be `d`. The sign selects which side of the line; flipping the sign mirrors the solution. |
| `at_midpoint(m, la, lb)` | 3 strings | Point `m` shall be at the midpoint of segment la–lb. |
| `equal_length(a, b, c, d)` | 4 strings | `|a–b| == |c–d|`. |
| `length_ratio(a, b, c, d, r)` | 4 strings + 1 number | `|a–b| / |c–d| == r`. |
| `length_difference(a, b, c, d, diff)` | 4 strings + 1 number | `|a–b| - |c–d| == diff`. |
| `eq_len_pt_line_d(p, la, lb, da, db)` | 5 strings | Length of segment la–lb shall equal the unsigned perpendicular distance from `p` to line da–db. (SolveSpace solves the squared form so the equality is on absolute distance.) |
| `eq_pt_ln_distances(p1, l1a, l1b, p2, l2a, l2b)` | 6 strings | Unsigned distance from `p1` to line l1 shall equal unsigned distance from `p2` to line l2. |
| `equal_angle(a, b, c, d, e, f, g, h)` | 8 strings | Angle between segments a–b and c–d shall equal angle between e–f and g–h. |
| `symmetric_horiz(p1, p2)` | 2 strings | Points are mirror images across the workplane's V-axis (the segment p1–p2 is horizontal). |
| `symmetric_vert(p1, p2)` | 2 strings | Points are mirror images across the workplane's U-axis (the segment p1–p2 is vertical). |
| `symmetric_line(p1, p2, la, lb)` | 4 strings | Points `p1` and `p2` are mirror images across the line la–lb. |

### Built-in name shadowing

* Each entity and constraint built-in is registered as an ordinary
  function name and is subject to OpenSCAD's normal lookup rules. A
  user-defined `function point(...) = ...;` or `module distance(...)` shall
  shadow the built-in within its scope. This is by design and is not
  considered an error.

## Out of Scope

The following are explicitly outside this specification and reserved for
future revisions:

* 3D entities and 3D constraint solving.
* Curved entities (arcs, circles, splines).
* Mate constraints between OpenSCAD geometry trees.
* Caching of solver results across `solve2d` calls.
* Importing native SolveSpace `.slvs` files.
* The numeric value of `residual(sol)` beyond zero on success.
