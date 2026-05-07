# `con_pt_on_segment` — point-on-line-segment constraint

Status: design, 2026-05-06.

A new sketch constraint asserting that a point lies on the closed line
segment between two other points. Builds on the existing inequality
SQP active-set machinery (see
[`2026-05-06-solve2d-inequality-sqp-design.md`](2026-05-06-solve2d-inequality-sqp-design.md))
by composing one equality with two new inequality kinds.

## Motivation

`con_pt_on_line(p, a, b)` constrains a point to the *infinite* line
through `a` and `b`. There is no equivalent for the bounded line
segment from `a` to `b`. Sketches that model finite geometric features
(walls of a fixed length, a bead sliding on a rod with stops, a hinge
attached at any point along a finite link) cannot express the
"between the endpoints" requirement today.

The recently added inequality SQP loop is a clean fit: "between the
endpoints" naturally factors into one always-on equality (point on the
infinite line) plus two one-sided inequalities (the projection
parameter `t = dot(p - a, b - a) / |b - a|²` must satisfy
`0 ≤ t ≤ 1`). When neither inequality binds, the cost is essentially
that of an existing `con_pt_on_line`. When a bound binds, the active
inequality reduces to "p coincides with the corresponding endpoint",
expressible with a primitive SolveSpace constraint.

## User-facing API

A new built-in function:

```
con_pt_on_segment(p, a, b) -> sketch constraint
```

Arguments are three strings naming previously declared points. Returns
a sketch constraint object with `kind = "pt_on_segment"`. The
constraint asserts that `p` lies on the closed segment `a–b`,
endpoints included.

Argument validation, error reporting, and registration follow the
existing pattern of `con_pt_on_line` (see `builtin_con_pt_on_line` in
`src/core/builtin_solve.cc`).

## Internal expansion

When the `solve2d` parser encounters an item with
`kind == "pt_on_segment"` it emits **three** internal items:

1. A `ConstraintDecl` of `kind = "pt_on_line"` with points
   `[p, a, b]`. This reuses the existing equality path and the
   existing SolveSpace `SLVS_C_PT_ON_LINE` translation. It is named
   `con_pt_on_segment(p,a,b):on_line` for failure reporting.

2. An `InequalityDecl` of new `kind = "pt_on_segment_lower"` with
   points `[p, a, b]` and no `valA` (rhs is implicitly zero). Named
   `con_pt_on_segment(p,a,b):start`. Violation function:
   ```
   g_lower(p, a, b) = -dot(p - a, b - a)
   ```
   Positive when `t < 0` (i.e. `p` is past `a`, away from `b`).

3. An `InequalityDecl` of new `kind = "pt_on_segment_upper"` with
   points `[p, a, b]` and no `valA`. Named
   `con_pt_on_segment(p,a,b):end`. Violation function:
   ```
   g_upper(p, a, b) = dot(p - b, b - a)
   ```
   Positive when `t > 1` (i.e. `p` is past `b`, away from `a`).

The three items participate in the parser, solver, and reporting
pipelines as independent declarations from that point on. There is no
"compound" item type and no special-case linkage between them.

### Why dot products and not the parameter `t`

The dot-product form avoids a division by `|b - a|²` in the violation
calculation, which sidesteps a degenerate-segment guard at the cost of
making the violation magnitude scale with `|b - a|`. The active-set
loop only consults the *sign* of the violation when deciding whether
to activate a bound and the magnitude only enters the convergence
test. The convergence tolerance (`VIOLATION_TOLERANCE = 1e-6`) is
already in absolute units of the residual computation; for the
expected sketch scales (single-digit to thousands of model units) the
unscaled dot product is well-behaved. The simpler formulation is
preferred.

## Active-set behavior

In `build_and_solve_once`, when an inequality of `kind ==
"pt_on_segment_lower"` is in the active set, emit:

```
SLVS_C_POINTS_COINCIDENT(p, a)
```

When `kind == "pt_on_segment_upper"` is active, emit:

```
SLVS_C_POINTS_COINCIDENT(p, b)
```

The on-line equality emitted in step 1 above remains in force
regardless of active-set state.

### Geometric correctness

With `p` constrained to the line through `a–b`, the on-line equality
implies `p = a + t·(b - a)` for some `t`. Then:

* `g_lower(p, a, b) = -dot((a + t·(b - a)) - a, b - a) = -t·|b - a|²`,
  which equals zero iff `t = 0` (modulo `|b - a| = 0`), i.e. `p = a`.
* `g_upper(p, a, b) = dot((a + t·(b - a)) - b, b - a) = (t - 1)·|b - a|²`,
  which equals zero iff `t = 1`, i.e. `p = b`.

So `POINTS_COINCIDENT(p, a)` correctly enforces `g_lower = 0` and
`POINTS_COINCIDENT(p, b)` correctly enforces `g_upper = 0`, given the
on-line equality. No new SolveSpace primitive is needed.

### Residual recovery

The active-set loop maps each active inequality to a pseudo
`ConstraintDecl` so `constraint_residual` can score it for the
`REDUNDANT_OKAY → INCONSISTENT` recovery path
(`builtin_solve.cc:1190-1200`). Add cases:

* `pt_on_segment_lower` → `ConstraintDecl{kind="coincident", points=[p, a]}`
* `pt_on_segment_upper` → `ConstraintDecl{kind="coincident", points=[p, b]}`

`constraint_residual` already handles `coincident` (returns
`max(|Δx|, |Δy|)`).

## Reporting

### `active_inequalities(sol)`

Only inequalities (not the equality piece) can appear here. Each
active bound surfaces under its own derived name:

* Only the lower bound active: `["con_pt_on_segment(p,a,b):start"]`
* Only the upper bound active: `["con_pt_on_segment(p,a,b):end"]`
* Both active (degenerate `a == b` case): both names appear.

This preserves the existing one-entry-per-`g(x) ≤ rhs`-relation
convention. The `:on_line` suffix never appears here.

### `failed_constraints(sol)`

Any of the three internal pieces (`:on_line`, `:start`, `:end`) may
appear independently when SolveSpace flags it. This is the
intentional behavior: the failure mode (over-constrained on-line,
infeasible at the start endpoint, infeasible at the end endpoint) is
information the user needs.

The names are chosen so that grepping for `con_pt_on_segment(p,a,b)`
finds all three. The suffix vocabulary matches the existing
inequality-name pattern: human-readable labels rather than numeric
indices.

## Edge cases

1. **Degenerate segment (`a == b`).** Both `g_lower = 0` and
   `g_upper = 0` for any `p` (since `b - a = 0`). The on-line equality
   on a zero-length line is degenerate; existing `pt_on_line` residual
   returns `0` when `|b - a| < 1e-12`. SolveSpace's existing handling
   of zero-direction lines applies. No special-casing required.

2. **Seed exactly at an endpoint.** `t = 0` (or `1`) gives
   `g_lower = 0` (or `g_upper = 0`). Slack is zero, not violated.
   The bound stays inactive. Correct.

3. **`p` seeded far past `b`.** Initial solve places `p` somewhere on
   the line at `t > 1`. `g_upper > 0` triggers activation; the next
   solve enforces `p = b`. If other constraints pull `p` away from
   `b`, the system is infeasible and reported as such.

4. **Cycling.** The existing SQP cycle detection (`visited` set on
   the active set in `solve_with_inequalities`) covers the new kinds
   with no extra code.

5. **Both bounds violated simultaneously** — only possible for a
   degenerate `a == b` segment with `p ≠ a`. The loop adds both to
   the active set, forcing `p = a` AND `p = b`, which forces
   `a = b`. If `a` is anchored, infeasible — correctly reported.

## Testing

Three new example files under `examples/Solver/`:

* **`solve2d_pt_on_segment.scad`** (golden path). Anchored segment
  `a = [0, 0]`, `b = [10, 0]`. Free point `p` seeded at `[3, 2]`,
  constrained by `con_pt_on_segment("p", "a", "b")` plus
  `con_distance("a", "p", 5)` to pin its position along the segment.
  Asserts `solved(sol)`, `pt(sol, "p") ≈ [5, 0]` (within tolerance),
  and `active_inequalities(sol) == []` (no bound binds). The
  iteration count is echoed but not asserted on (depends on
  active-set loop bookkeeping).

* **`solve2d_pt_on_segment_binding.scad`** (binding case). Anchored
  `a = [0, 0]`, `b = [10, 0]`. Free `p` seeded at `[15, 0]`. No
  along-line constraint other than the segment. Asserts
  `solved(sol)`, `pt(sol, "p") == [10, 0]`, and
  `active_inequalities(sol) == ["con_pt_on_segment(p,a,b):end"]`.

* **`solve2d_pt_on_segment_infeasible.scad`** (infeasible case).
  Anchored `a = [0, 0]`, `b = [10, 0]`. Free `p` constrained
  `con_pt_on_segment("p", "a", "b")` and `con_distance("a", "p", 100)`.
  Asserts `!solved(sol)` and that one of the segment-related names
  appears in `failed_constraints(sol)`.

Each example is registered in `examples/Solver/example-dir.json`.

## Documentation updates

* **`doc/specs/solve2d.md`**: add `con_pt_on_segment` to the
  constraint table; add a paragraph under the inequality-semantics
  section describing the composite expansion (one equality + two
  inequalities) and active-set behavior; document the `:on_line` /
  `:start` / `:end` reporting suffixes.

* **`doc/solve2d_guide.md`**: add `con_pt_on_segment` to the
  reference table; add a worked example in the prose section showing
  a typical binding case.

## Out of scope (for this design)

* The `con_le_pt_segment_distance(p, a, b, d)` companion (point
  within distance `d` of the segment, with true segment-distance
  semantics that account for endpoints). Adds value but the user
  scoped this iteration to the equality only.

* A native SolveSpace `SLVS_C_PT_IN_SEGMENT` primitive. SolveSpace is
  an equality solver and does not represent inequalities directly;
  the active-set decomposition above is the right pattern for this
  codebase.
