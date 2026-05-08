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
  `ObjectType` describing an equality relation between sketch entities.
* "Sketch inequality" is a built-in function call (the `con_le_*` and
  `con_ge_*` families) that produces a tagged `ObjectType` describing a
  one-sided relation of the form `g(x) <= rhs` (or `g(x) >= rhs`) between
  sketch entities.
* "Active inequality" is a sketch inequality whose bound is enforced as
  an equality (`g(x) == rhs`) in the final solver state. The set of
  active inequalities is determined by `solve2d` and exposed via
  `active_inequalities(sol)`.
* "Outer iteration" is one cycle of the active-set loop `solve2d` uses
  when the input contains inequalities. When no inequalities are present,
  `solve2d` performs a single SolveSpace solve and the iteration count is
  `0`.
* "Seeded point" is a point whose `at` keyword argument is supplied: its
  coordinates are used as the solver's initial guess for that point. The
  point remains free to move during the solve unless additionally
  constrained by `con_fixed`.
* "Free point" is a point whose `at` keyword argument is omitted: its
  initial coordinates are implementation-defined.
* "Anchored point" is a point constrained by `con_fixed`: the solver
  shall not move it from its initial position.
* "Workplane" is the fixed XY plane at the world origin (Z = 0). All sketch
  geometry lies on it.
* "Solution" is the opaque value type returned by `solve2d`. It has its own
  `Value::Type::SOLUTION` variant and is not interchangeable with `object`.
* "Degree of freedom" (DOF) is the count of unconstrained scalar parameters
  remaining after the solver runs, as reported by SolveSpace.

## Interpretation

### `solve2d`

* `solve2d(items)` shall accept one positional argument `items`: a vector
  of sketch entities and sketch constraints. It shall additionally accept
  one optional named argument `solve` of type `bool`, defaulting to `true`.
* `solve2d` shall return a value of type `solution` in all cases where
  `items` is a vector, regardless of whether the constraint system is
  satisfiable.
* `solve2d` shall return `undef` when `items` is not a vector.
* When `solve = false`, `solve2d` shall parse the input and return a
  solution whose point coordinates are the seed positions: each declared
  point reports its `at=` value if present, or the implementation-defined
  default seed otherwise. No SolveSpace solve shall be performed; the
  returned solution shall have `solved(sol) == false`,
  `dof(sol) == 0`, `residual(sol) == 0`, `iterations(sol) == 0`, and
  empty `failed_constraints(sol)` and `active_inequalities(sol)`.
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
* Points without an explicit `at` seed shall be assigned
  implementation-defined non-coincident initial values to avoid degenerate
  starting geometry. The exact seeding scheme is not part of this
  specification and may change.
* `solve2d` shall not be re-entrant on a single thread; it owns the
  underlying SolveSpace state for the duration of one call. Independent
  calls in sequence are safe.
* When the input contains zero inequalities, `solve2d` shall perform a
  single SolveSpace solve and `iterations(sol)` shall be `0`. Behavior
  in this path shall be identical to revisions of this specification that
  predate inequality support.
* When the input contains one or more inequalities, `solve2d` shall use
  an outer active-set loop: it determines an active set of inequalities,
  treats each active inequality as the equality `g(x) == rhs` for the
  underlying SolveSpace call, checks inactive inequalities for violations,
  and iterates. The strategy for choosing and updating the active set is
  implementation-defined. `iterations(sol)` shall report the number of
  outer iterations performed (at least `1`); `active_inequalities(sol)`
  shall report the inequalities in the active set when `solve2d` returned.
* The outer loop shall always terminate. If it cannot find a feasible
  active set — because the equality-only base system is itself infeasible,
  because the active-set search cycles, or because an implementation-defined
  iteration cap is reached — `solve2d` shall return a solution with
  `solved(sol) == false`. `failed_constraints(sol)` shall describe the
  failure: either a SolveSpace-reported list, or a string of the form
  `"solve2d: <reason>"`.

### Solution accessors

* `solved(sol)` shall return a `bool`. It shall be `true` when SolveSpace
  returned `SLVS_RESULT_OKAY`, and shall additionally be `true` when
  SolveSpace returned `SLVS_RESULT_INCONSISTENT` but every per-constraint
  residual computed against the solved point coordinates is below a
  unit-appropriate, scale-aware tolerance (length-unit residuals scale with
  the sketch; angle and dimensionless residuals do not) — this recovers from
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
* `pts(sol)` shall return a vector of 2-element vectors `[[x,y], ...]`
  containing every point in the solution, in the order the points were
  declared in the original `solve2d` input. If the solution contains no
  points, the result shall be an empty vector.
* `poly(sol, names)` shall return a list of 2-element vectors in the order
  given by `names`. If any name is missing, the function shall return
  `undef` with a warning.
* `failed_constraints(sol)` shall return a vector of strings naming
  constraints SolveSpace identified as failing. The strings are descriptive
  summaries (e.g. `"con_distance(a,b)"`) and shall not be parsed programmatically.
  When `solved(sol)` is `false` because the inequality outer loop bailed,
  this vector shall instead (or additionally) contain a string of the form
  `"solve2d: <reason>"`.
* `iterations(sol)` shall return a `number` equal to the number of outer
  active-set iterations performed by `solve2d`. It shall be `0` when no
  inequalities were present in the input, and at least `1` otherwise.
* `active_inequalities(sol)` shall return a vector of strings naming the
  inequalities in the active set when `solve2d` returned. The strings are
  descriptive summaries (e.g. `"con_le_distance(a,b,4.000000)"`) and shall
  not be parsed programmatically. When the input contained no
  inequalities, this vector shall be empty.
* All accessors shall return `undef` and warn when given a non-`solution`
  first argument.

### Numeric scale and tolerances

`solve2d` derives an internal length scale from the median magnitude of
user-supplied seeds (`at=` values) and length-bearing constraint values
(`con_distance` / `con_pt_line_distance` / `con_length_difference` and their
`con_le_*` / `con_ge_*` counterparts). Tolerances for length-unit residuals
and length-unit inequality violations are expressed as fractions of this
scale; angle (degree) and dimensionless residuals use scale-independent
tolerances. As a consequence, the same sketch geometry produces equivalent
solver behaviour at millimetre, metre, or micrometre scales — the active-set
loop, the REDUNDANT_OKAY recovery, and the unseeded-point spiral all
participate in the same scaling. Implementations may pick any reasonable
scale-derivation strategy as long as it is monotone in the magnitudes of
seeds and length-bearing constraint values.

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
  * `at`, if supplied, shall be a 2-element vector of numbers used as the
    solver's initial guess for the point's position. `at` does not pin the
    point; use `con_fixed(name)` to anchor it. If `at` is supplied but
    malformed, the value shall be ignored and a warning emitted; the
    resulting point shall be treated as if no `at` were given.

### Constraints

In every constraint, point-name arguments shall be `string`s and numeric
arguments shall be `number`s. Mismatched arity or types shall cause the
constraint factory to return `undef` with a warning.

| Form | Arity | Meaning |
|---|---|---|
| `con_coincident(p1, p2)` | 2 strings | Points coincide. |
| `con_distance(p1, p2, d)` | 2 strings + 1 number | Euclidean distance is `d`. |
| `con_horizontal(p1, p2)` | 2 strings | Segment p1–p2 is horizontal. |
| `con_vertical(p1, p2)` | 2 strings | Segment p1–p2 is vertical. |
| `con_perpendicular(p1, vertex, p2)` | 3 strings | Segments vertex–p1 and vertex–p2 are perpendicular. |
| `con_parallel(p1, p2, p3, p4)` | 4 strings | Segments p1–p2 and p3–p4 are parallel. |
| `con_angle(p1, p2, p3, p4, deg)` | 4 strings + 1 number | Signed angle between p1–p2 and p3–p4 is `deg` degrees. |
| `con_fixed(p)` | 1 string | The point shall remain at its current position. |
| `con_pt_on_line(p, la, lb)` | 3 strings | Point `p` shall lie on the infinite line through la–lb. |
| `con_pt_on_segment(p, la, lb)` | 3 strings | Point `p` shall lie on the closed line segment from la to lb (endpoints included). Composite: see "Composite constraints" below. |
| `con_pt_line_distance(p, la, lb, d)` | 3 strings + 1 number | Signed perpendicular distance from `p` to the line la–lb shall be `d`. The sign selects which side of the line; flipping the sign mirrors the solution. |
| `con_at_midpoint(m, la, lb)` | 3 strings | Point `m` shall be at the midpoint of segment la–lb. |
| `con_equal_length(a, b, c, d)` | 4 strings | `|a–b| == |c–d|`. |
| `con_length_ratio(a, b, c, d, r)` | 4 strings + 1 number | `|a–b| / |c–d| == r`. |
| `con_length_difference(a, b, c, d, diff)` | 4 strings + 1 number | `|a–b| - |c–d| == diff`. |
| `con_eq_len_pt_line_d(p, la, lb, da, db)` | 5 strings | Length of segment la–lb shall equal the unsigned perpendicular distance from `p` to line da–db. (SolveSpace solves the squared form so the equality is on absolute distance.) |
| `con_eq_pt_ln_distances(p1, l1a, l1b, p2, l2a, l2b)` | 6 strings | Unsigned distance from `p1` to line l1 shall equal unsigned distance from `p2` to line l2. |
| `con_equal_angle(a, b, c, d, e, f, g, h)` | 8 strings | Angle between segments a–b and c–d shall equal angle between e–f and g–h. |
| `con_symmetric_horiz(p1, p2)` | 2 strings | Points are mirror images across the workplane's V-axis (the segment p1–p2 is horizontal). |
| `con_symmetric_vert(p1, p2)` | 2 strings | Points are mirror images across the workplane's U-axis (the segment p1–p2 is vertical). |
| `con_symmetric_line(p1, p2, la, lb)` | 4 strings | Points `p1` and `p2` are mirror images across the line la–lb. |

### Inequalities

Inequalities express one-sided relations of the form `g(x) <= rhs`. The
solver enforces an inequality only when needed: if the equality-only solve
already satisfies `g(x) <= rhs`, the inequality is left slack and adds
no DOF cost. If the unconstrained solve would violate the bound, the
inequality is added to the active set and `solve2d` re-solves with
`g(x) == rhs` enforced.

In every inequality, point-name arguments shall be `string`s and numeric
arguments shall be `number`s. Mismatched arity or types shall cause the
inequality factory to return `undef` with a warning.

| Form | Arity | Meaning |
|---|---|---|
| `con_le_distance(p1, p2, d)` | 2 strings + 1 number | `|p1–p2| <= d`. |
| `con_ge_distance(p1, p2, d)` | 2 strings + 1 number | `|p1–p2| >= d`. |
| `con_le_pt_line_distance(p, la, lb, d)` | 3 strings + 1 number | Signed perpendicular distance from `p` to la–lb shall be `<= d`. The sign convention matches `con_pt_line_distance`. |
| `con_ge_pt_line_distance(p, la, lb, d)` | 3 strings + 1 number | Signed perpendicular distance from `p` to la–lb shall be `>= d`. Sign convention as for the `<=` form. |
| `con_le_length_difference(a, b, c, d, diff)` | 4 strings + 1 number | `|a–b| - |c–d| <= diff`. |
| `con_ge_length_difference(a, b, c, d, diff)` | 4 strings + 1 number | `|a–b| - |c–d| >= diff`. |
| `con_le_angle(p1, p2, p3, p4, deg)` | 4 strings + 1 number | The undirected angle between p1–p2 and p3–p4 (in `[0, 180]`) shall be `<= deg`. The argument `deg` shall be non-negative; behavior on negative `deg` is implementation-defined. SolveSpace's `SLVS_C_ANGLE` admits a supplementary-angle solution; in rare configurations the solver may converge to `180 - deg` rather than `deg`. Use seeds to nudge the geometry if this matters. |
| `con_ge_angle(p1, p2, p3, p4, deg)` | 4 strings + 1 number | Same as `con_le_angle` but the bound is `>= deg`. The same supplementary-angle caveat applies. |

For each `con_ge_*` form, when active the solver enforces the same equality
`g(x) == rhs` as the corresponding `con_le_*` form; the two only differ in
which side of the bound counts as a violation.

### Composite constraints

Some user-facing constraints expand into multiple internal items. The
`solve2d` parser handles the expansion; the user calls a single factory
function and sees consolidated reporting under a shared name prefix.

* `con_pt_on_segment(p, la, lb)` expands into:
  * one equality `pt_on_line` with points `(p, la, lb)`, named
    `con_pt_on_segment(p,la,lb):on_line`;
  * one inequality with bound `−dot(p − la, lb − la) ≤ 0` (i.e. the
    segment parameter `t ≥ 0`), named
    `con_pt_on_segment(p,la,lb):start`;
  * one inequality with bound `dot(p − lb, lb − la) ≤ 0` (i.e. `t ≤ 1`),
    named `con_pt_on_segment(p,la,lb):end`.
  When the start/end bound is active, the solver enforces `p == la` /
  `p == lb` respectively (geometrically equivalent under the on-line
  equality) and *suppresses* emission of the on-line equality for the
  duration of that solve, since `POINTS_COINCIDENT(p, endpoint)`
  already implies it and emitting both makes the system rank-deficient.
  The `:on_line` suffix never appears in `active_inequalities(sol)`;
  the `:start` and `:end` suffixes never appear there unless the
  corresponding bound is in the active set. Any of the three suffixed
  names may appear in `failed_constraints(sol)` if SolveSpace flags
  the underlying piece.

* `con_directed_angle(p1, p2, p3, p4, deg)` expands into:
  * one equality `angle` with points `(p1, p2, p3, p4)` and value
    `min(deg, 360 − deg) ∈ [0, 180]`, named
    `con_directed_angle(p1,p2,p3,p4,deg):angle`;
  * when `deg ∉ {0, 180}`, one inequality named
    `con_directed_angle(p1,p2,p3,p4,deg):side` with points
    `(p1, p2, p4)`. The kind is `oriented_left` for
    `deg ∈ (0, 180)` and `oriented_right` for `deg ∈ (180, 360)`,
    enforcing that the signed cross product
    `(p2 − p1) × (p4 − p1)` is non-negative or non-positive
    respectively. At `deg == 0` and `deg == 180` the rays are colinear
    and no half-plane is emitted.
  `deg` is interpreted modulo 360°; values outside `[0, 360)` (including
  `360`, negative numbers, and the result of arithmetic that lands at
  `360 − ε`) shall be wrapped into `[0, 360)` before decomposition. Because
  the half-plane pins `p4` to line `p1 → p2` when active (which conflicts
  with the magnitude angle), this composite relies on multi-start to escape
  a wrong-side initial Newton landing. Multi-start operates on every sketch
  containing inequalities, including fully-seeded sketches: on attempts
  after the first, user seeds receive a small jitter (a fraction of the
  sketch's length scale) so the solver can hop branches without the user
  having to leave a point unseeded.

### Built-in name shadowing

* Each entity, constraint, and accessor built-in is registered as an
  ordinary function name and is subject to OpenSCAD's normal lookup rules.
  Constraint built-ins are prefixed with `con_` (`con_distance`,
  `con_angle`, ...) so that user-defined names like `function distance(...)`
  do not shadow them in practice. The non-prefixed entity and accessor
  names (`point`, `solve2d`, `solved`, `pt`, `poly`, `dof`, `residual`,
  `failed_constraints`) remain shadowable by user definitions of the same
  name. This is by design and is not considered an error.

## Out of Scope

The following are explicitly outside this specification and reserved for
future revisions:

* 3D entities and 3D constraint solving.
* Curved entities (arcs, circles, splines).
* Mate constraints between OpenSCAD geometry trees.
* Caching of solver results across `solve2d` calls.
* Importing native SolveSpace `.slvs` files.
* The numeric value of `residual(sol)` beyond zero on success.
* Inequality constraints of the form `g(x) >= rhs` (a `con_ge_*` family).
* Two-sided distance constraints (e.g. `|signed_dist(p, line)| <= d` as a
  single built-in).
