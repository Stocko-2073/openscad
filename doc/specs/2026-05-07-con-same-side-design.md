# `con_same_side` / `con_opposite_side` — half-plane constraints

Status: design, 2026-05-07.

Two new sketch constraints asserting that a point lies on a specified
side of a line through two other points. Builds on the existing
inequality SQP active-set machinery (see
[`2026-05-06-solve2d-inequality-sqp-design.md`](2026-05-06-solve2d-inequality-sqp-design.md))
by adding two new inequality kinds with a shared binding form.

## Motivation

`solve2d` problems regularly have multiple solutions because constraint
systems written in terms of distances and angles are sign-blind. A
square anchored at one corner with three side-equality constraints and
one right angle (the canonical Euclidean square) has both a "real
square" solution and a degenerate one in which two non-adjacent
vertices coincide. Newton's method picks whichever basin its starting
seed falls into; perturbing seeds (multi-start retry, see commit
`b0fa5a542`) reaches *some* feasible solution but cannot pick a
particular one.

A half-plane constraint disambiguates by pinning a point to one side
of an oriented line. It does not over-constrain — the inequality is
slack as long as the point stays on the chosen side, and only binds
when geometry forces it onto the dividing line itself. This is the
standard tool in 2D constraint solvers for breaking the discrete
multiplicity that distance/angle constraints leave behind.

## User-facing API

Two new built-in functions:

```
con_same_side(a, b, p, q)     -> sketch constraint
con_opposite_side(a, b, p, q) -> sketch constraint
```

All four arguments are strings naming previously declared points.

* `con_same_side(a, b, p, q)` asserts that `p` lies on the *same* side
  of the infinite line through `a` and `b` as `q`.
* `con_opposite_side(a, b, p, q)` asserts that `p` lies on the
  *opposite* side from `q`.

Returns a sketch constraint object with
`kind = "same_side"` or `kind = "opposite_side"` respectively.

Argument validation, error reporting, and registration follow the
existing pattern of `con_pt_on_line` and `con_pt_on_segment`.

## Internal expansion

When the `solve2d` parser encounters an item with `kind == "same_side"`
or `kind == "opposite_side"`, it emits a single `InequalityDecl`:

* `kind = "same_side"` (or `"opposite_side"`)
* `points = [a, b, p, q]`
* No `valA` (rhs is implicitly zero)
* Name: `con_same_side(a,b,p,q)` or `con_opposite_side(a,b,p,q)`

Unlike `con_pt_on_segment`, no equality is emitted — the constraint is
purely a half-plane gate. There is no separate `_lower` / `_upper`
split because both `same_side` and `opposite_side` have a single
boundary (the line `ab`) and a single binding form.

### Violation function

For both kinds, define the signed cross product

```
c(x) = (b.x - a.x)(x.y - a.y) - (b.y - a.y)(x.x - a.x)
```

`c(x)` is positive when `x` is on the left of the directed line
`a → b`, negative on the right, zero on the line.

* `same_side`: violation `g = -c(p) · c(q)`. Positive when the signs
  differ (opposite sides) → bound violated.
* `opposite_side`: violation `g = c(p) · c(q)`. Positive when the
  signs agree (same side) → bound violated.

Magnitude scales with `|ab|·|ap|·|aq|`. The existing
`VIOLATION_TOLERANCE = 1e-6` is in absolute residual units; for
realistic sketch scales this is well behaved. (Consistent with the
choice in `con_pt_on_segment` to use unscaled dot/cross products.)

## Active-set behavior

When either `same_side` or `opposite_side` enters the active set,
`build_and_solve_once` emits

```
SLVS_C_PT_ON_LINE(p, line(a, b))
```

The boundary between the two half-planes is the line `ab` itself.
Pinning `p` onto that line satisfies the inequality at equality,
which is the standard active-set semantics.

### Asymmetric binding

The binding form pins `p` (the third argument), not `q` (the
fourth). `q` serves as a sign reference only. If a user has a problem
where they want `q` to be the one that binds, they swap the argument
order: `con_same_side(a, b, q, p)`.

This is asymmetric but predictable. The alternative (auto-detecting
which of `p` or `q` is closer to the line at activation time) is
implementable but harder to reason about, and the binding case is rare
in the typical use of these constraints. YAGNI.

### Geometric correctness

When `p` is pinned to line `ab`, `c(p) = 0` exactly, so the violation
function evaluates to zero regardless of `c(q)`'s sign. The active
inequality is satisfied at equality, and `solve_with_inequalities`
deactivates it on the next iteration (per the existing post-solve
violation check). If geometry prevents `p` from leaving the line,
the inequality stays bound — the same trapping behavior as
`con_pt_on_segment`.

### Residual recovery

For the `REDUNDANT_OKAY → INCONSISTENT` recovery path
(see existing residual-recovery code in `build_and_solve_once`),
map each active half-plane inequality to a pseudo `ConstraintDecl`:

* `same_side`     → `ConstraintDecl{kind="pt_on_line", points=[p, a, b]}`
* `opposite_side` → `ConstraintDecl{kind="pt_on_line", points=[p, a, b]}`

`constraint_residual` already handles `pt_on_line`.

## Reporting

### `active_inequalities(sol)`

When bound:

* `con_same_side(a,b,p,q)` active → `["con_same_side(a,b,p,q)"]`
* `con_opposite_side(a,b,p,q)` active → `["con_opposite_side(a,b,p,q)"]`

Single name per constraint, matching the convention for
`con_le_distance` etc.

### `failed_constraints(sol)`

If SolveSpace flags the active-set equality as failed, the
constraint's name appears in `failed_constraints(sol)`.

## Interaction with multi-start retry

The seed-perturbation multi-start loop (`MAX_SEED_ATTEMPTS = 8`)
remains the primary mechanism for escaping the wrong basin. A
half-plane constraint converts the per-attempt outcome from "any
feasible solution" to "a feasible solution on the chosen side." If
the first attempt's seed lands `p` on the wrong side, the inequality
flags violation, the active-set loop activates it (binding `p` to the
line), and the next solve places `p` on the line. If that's also
infeasible against other constraints, the retry loop tries a fresh
seed. Across the eight retries, at least one should land in the
correct basin for typical disambiguation problems.

If no attempt converges, the failure surfaces with
`active_inequalities(sol)` showing the bound name and `solved=false`.

## Edge cases

1. **Degenerate `a == b`.** `c(x) = 0` for all `x`, so violation is
   identically zero; the constraint is vacuously satisfied. The user's
   sketch is geometrically incoherent, but our constraint contributes
   nothing harmful. (`con_pt_on_line` already tolerates degenerate
   lines via existing zero-direction handling.)

2. **`q` exactly on line `ab`.** `c(q) = 0`, so violation is zero
   regardless of `c(p)`. The constraint is vacuously satisfied. This
   is correct: with no reference side, there is no preferred side.
   The user is responsible for placing `q` off the line if they want
   the constraint to bite.

3. **`p` seeded exactly on line `ab`.** `c(p) = 0`, no violation, no
   activation. The constraint is technically satisfied at equality
   from the start. If geometry then pulls `p` off the line during
   solve, the post-solve check evaluates the violation and activates
   if needed. Correct.

4. **Both `p` and `q` free.** The constraint is symmetric in
   *intent* (sign-product) but the binding rule prefers `p`. If the
   solve naturally pulls `q` onto the line and `p` stays off, the
   active-set re-solve pins `p` to the line too — possibly degenerate.
   Documented behavior: anchor `q` (or use `con_same_side(a,b,q,p)`
   for the opposite role) when both points are otherwise free.

5. **Cycling.** Existing SQP cycle detection in
   `solve_with_inequalities` covers the new kinds with no extra code.

## Testing

Two new example files under `examples/Solver/`, each named for the
constraint it primarily exercises:

* **`solve2d_same_side.scad`** (smoke test for `con_same_side`).
  Anchored line: `a = [0, 0]`, `b = [10, 0]` (so line `ab` is the
  x-axis). Free point `p` with no `at=`. Reference point
  `q = [3, 5]` (above the x-axis).
  `con_same_side("a", "b", "p", "q")` plus
  `con_distance("a", "p", 5)`. Without the side constraint, `p` lands
  on a radius-5 circle around `a` — could be above or below the
  x-axis depending on seed. With it, `p.y > 0`. Asserts `solved(sol)`
  and `pt(sol, "p").y > 0`.

* **`solve2d_opposite_side.scad`** (golden path for `con_opposite_side`,
  also the canonical disambiguation example). Anchor `a = [0, 0]` and
  `b = [20, 0]`, leave `c` and `d` unseeded. The four
  side-equality constraints plus a right angle at `b`, plus
  `con_opposite_side("a", "c", "d", "b")` — `d` on the opposite side
  of line `ac` from `b`. The constraint says "the two diagonals of a
  square cross, so `b` and `d` are on opposite sides of `ac`."
  Asserts `solved(sol)`, all four sides ≈ 20 within tolerance, and
  diagonal `|bd| ≈ 20·sqrt(2)` (i.e. real square, not the b=d
  degenerate). Asserts `active_inequalities(sol) == []` (constraint
  satisfied with slack, not bound).

  *Implementation note:* the original brainstorming targeted a
  one-anchor variant (only `a` fixed). In practice the `d == b`
  degenerate basin is too dominant for the seed-perturbation
  multi-start to escape — all eight retries land there. The
  active-set form (pin `d` on line `ac`) is also infeasible with
  `|cd| = 20` because `|ac| = 20·sqrt(2) ≠ 0` and `≠ 40`. Anchoring
  `b` reduces the remaining ambiguity to a single half-plane choice,
  which `con_opposite_side` resolves cleanly.

Both examples document the constraint in their header comments.

The existing `solve2d_pts.scad` shape integration test serves as
regression coverage that adding the new constraint kinds doesn't
disturb the parser dispatch on unrelated kinds.

## Out of scope

* `con_strictly_same_side` (open half-plane, `c(p) · c(q) > 0`
  strict). The active-set machinery is closed-half-plane natively;
  strict inequalities require a different mechanism. Not needed for
  the disambiguation use case.

* Symmetric binding (auto-pick `p` or `q` to put on the line). See
  asymmetric-binding rationale above. Implementable in a later
  iteration if a real use case demands it.

* Three-or-more-point variants ("`p` on the same side of line `ab`
  as both `q1` and `q2`"). The user can compose these as multiple
  `con_same_side` calls. No need to special-case.
