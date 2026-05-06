# solve2d: Inequality Constraints via Outer SQP-style Active-Set Loop

**Status:** Experiment / Draft

**Date:** 2026-05-06

**Branch:** to be created — feature branch off `dev`.

## Goal

Add support for inequality constraints (`g(x) ≤ k`) to `solve2d` without
modifying SolveSpace. Implement as an outer active-set loop around the existing
`Slvs_Solve` call. SolveSpace continues to handle only equality constraints; the
inequality logic lives entirely in `src/core/builtin_solve.cc`.

This is a research experiment to discover whether a simple add-on-violation /
drop-on-failure active-set loop converges adequately on real geometric sketches,
given that SolveSpace doesn't expose Lagrange multipliers needed for a textbook
KKT check.

## Scope

### In scope

Four new built-in inequality constraints, all of the `_le` family (≤). For ≥
the user must wait for a follow-up or rewrite the sketch:

| Builtin                                                 | Semantic                  | Reuses on activation |
|---------------------------------------------------------|---------------------------|----------------------|
| `con_le_distance(a, b, d)`                              | `|ab| ≤ d`                | `SLVS_C_PT_PT_DISTANCE` with value `d` |
| `con_le_pt_line_distance(p, la, lb, d)`                 | signed dist `≤ d`         | `SLVS_C_PT_LINE_DISTANCE` with value `d` |
| `con_le_length_difference(a, b, c, d, diff)`            | `|ab| − |cd| ≤ diff`      | `SLVS_C_LENGTH_DIFFERENCE` with value `diff` |
| `con_le_angle(p1, p2, p3, p4, deg)`                     | angle (in `[0,180]`) `≤ deg` | `SLVS_C_ANGLE` with value `deg` |

`con_le_pt_line_distance` uses **signed** distance — same semantics as the
existing `con_pt_line_distance`. Users wanting "within d on either side" must
combine two constraints. Documented caveat.

Two new accessors:

- `active_inequalities(sol) -> [string]` — names of inequalities that are tight
  (active in the final solution).
- `iterations(sol) -> number` — outer iteration count of the SQP loop. `0` if
  no inequalities were present (single Slvs call, equivalent to today).

Backward compatibility: when the input vector contains zero `con_le_*` items,
behavior is **byte-for-byte identical** to today — single `Slvs_Solve` call,
same code path, no extra Slvs system rebuilds.

### Out of scope (future work)

- `con_ge_*` family.
- "Within d of line on either side" two-sided distance.
- Probe-based dropping of loose actives (current design only drops on Slvs
  failure).
- Warm-starting between iterations (each call re-solves from scratch).
- User-tunable max-iterations / tolerance kwargs to `solve2d()` (hardcoded for
  the experiment).

## Algorithm

### High-level loop

```
inequalities = [...]      // parsed con_le_* list, separate from equalities
equalities   = [...]      // parsed equality constraints, as today
A            = {}         // active set: subset of inequalities, by index
visited      = {}         // set of frozensets we've tried — cycle detection
MAX_ITER     = 50

for iter = 0..MAX_ITER:
    if A in visited: return INFEASIBLE  // would oscillate
    visited.add(A)

    sys = build_slvs(equalities + [ineq[i] as equality for i in A])
    Slvs_Solve(sys)
    apply_residual_recovery(sys)        // existing REDUNDANT_OKAY handling

    if sys.solved:
        violations = [i in inequalities \ A : eval(ineq[i], x) > tol]
        if violations is empty:
            return SUCCESS               // all inactive ≤ are slack
        A = A ∪ violations               // add ALL violators at once
    else:                                 // INCONSISTENT
        active_failed = [i in A : ineq[i].handle in sys.failed]
        if active_failed not empty:
            A = A \ active_failed         // drop all SolveSpace-flagged actives
        else:
            return FAIL                   // base equality system is infeasible
```

Notes:

- "Add all violators at once" — bias toward fast convergence. Most geometric
  sketches converge in 1–2 outer iterations.
- "Drop all flagged actives at once" — symmetric with add. Cycle detection
  catches the cases where this oscillates.
- `MAX_ITER = 50` is generous; in practice convergence is 1–4 iterations or
  divergence. Hardcoded for the experiment; can be exposed later.
- `tol` reuses the existing `RESIDUAL_TOLERANCE = 1e-6` constant.

### What "violation" means per kind

Reuse the existing `constraint_residual()` function but interpret as a *signed*
violation for inequalities:

- `le_distance`:        `|ab| − d`           (positive ⇒ violated)
- `le_pt_line_distance`: `signed_dist − d`    (positive ⇒ violated; signed!)
- `le_length_difference`: `(|ab|−|cd|) − diff` (positive ⇒ violated)
- `le_angle`:           `angle_deg − deg`    (positive ⇒ violated)

Add a parallel helper `inequality_violation()` that returns the *signed* value
(not absolute, unlike `constraint_residual`). Equality-style residual remains
unchanged.

### Result-phase changes

`SolutionType::Data` gains two fields:

- `std::vector<std::string> active_inequalities` — names of inequalities tight
  at the solution.
- `int iterations` — outer-loop iteration count (0 when no inequalities).

The existing `failed_constraints` and `residual` semantics stay as today; the
loop's final `Slvs_Solve` call populates them. If the loop bails with FAIL or
INFEASIBLE we set `solved = false` and surface the most recent Slvs failure
list.

## Data structures

### Parsing phase (extension)

A new `InequalityDecl` mirrors `ConstraintDecl`:

```cpp
struct InequalityDecl {
  std::string kind;                   // "le_distance", "le_pt_line_distance", ...
  std::string name;                   // for active_inequalities reporting
  std::vector<std::string> points;
  double valA = 0.0;                  // d / diff / deg
  Slvs_hConstraint slvs_handle = 0;   // assigned when activated, used to
                                      // recognize entries in sys.failed
};
```

The parse loop in `builtin_solve2d` recognizes `le_distance`, `le_pt_line_distance`,
`le_length_difference`, `le_angle` kinds and routes them into a new
`std::vector<InequalityDecl> inequalities` vector instead of the existing
`constraints` vector.

### Active-set representation

A `std::set<size_t>` of indices into `inequalities`. `frozenset` semantics for
cycle-detection use a `std::set<std::set<size_t>>` (or a `std::set<std::vector<size_t>>`
of sorted indices — cheaper to hash if we needed unordered_set, but for `MAX_ITER=50`
either is fine).

### System rebuild per iteration

Each outer iteration rebuilds the entire `Slvs_System` (params, entities,
constraints) from scratch. The user accepts the cost: "no warm-start between
iterations." This matches the simplicity goal — no incremental Slvs API
gymnastics, no stale-handle bugs.

To avoid duplicating the build code, refactor the current monolithic
`builtin_solve2d` body so that `build_and_solve_once(equalities, active_ineqs)`
is a callable helper. Inputs: parsed declarations, current active subset.
Output: solved point coordinates, Slvs result code, failed list, residual.

This refactor is part of the experiment (necessary to keep the loop readable),
not collateral cleanup.

## File-level changes

`src/core/builtin_solve.cc`:

1. New `con_le_*` builtin factories (factory style mirrors existing `con_*`).
2. Parser additions for the four new kinds, populating `inequalities`.
3. New `inequality_violation()` helper.
4. Refactor of build/solve into `build_and_solve_once()`.
5. New outer-loop function `solve_with_inequalities()` invoked when
   `inequalities.size() > 0`. When zero, fall through to the existing single
   `Slvs_Solve` path verbatim (zero behavior change).
6. New accessor builtins `active_inequalities`, `iterations`.
7. Registration entries for everything new.

`src/core/SolutionType.h` (or wherever `SolutionType::Data` lives):

8. Add `active_inequalities` and `iterations` fields.
9. Add corresponding const accessors.

Optionally a small fragment of `doc/solve2d_guide.md` describing the new
constraints — defer this to after the experiment validates.

## Failure modes & reporting

| Outcome                   | `solved()` | `failed_constraints()` | `iterations()` | `active_inequalities()` |
|---------------------------|------------|------------------------|----------------|--------------------------|
| No inequalities, OK       | true       | []                     | 0              | []                       |
| Inequalities, all slack   | true       | []                     | ≥1             | []                       |
| Inequalities, some active | true       | []                     | ≥1             | non-empty                |
| Cycle detected            | false      | last Slvs failures     | iter when caught | last A                 |
| MAX_ITER exhausted        | false      | last Slvs failures     | 50             | last A                  |
| Base equalities inconsistent | false   | Slvs failures (no inequalities involved) | 1   | []                       |

`failed_constraints()` distinguishes equality vs inequality entries by the
already-existing summary string format ("`con_distance(a,b)`" vs
"`con_le_distance(a,b,d)`").

## Testing strategy

Existing examples in `examples/Solver/` should continue to solve identically
(no `con_le_*` → identical code path). Add 2–3 new examples exercising the
loop:

- A basic case: triangle with `con_le_distance(a, b, 5)` where the unconstrained
  solution gives `|ab| = 3` (inequality slack — should report 0 active, 1
  iteration).
- A binding case: same triangle with `con_le_distance(a, b, 2)` (active —
  should report 1 active, 2 iterations).
- An infeasible case: contradictory inequalities — should report unsolved with
  cycle detection or MAX_ITER.

Manual verification via the existing example-runner; no new unit-test
infrastructure for the experiment.

## Risks / open questions

1. **Cycle detection granularity.** Storing every visited active-set as a
   `std::set` is O(MAX_ITER × |A|) memory — trivial. But two visited states
   that differ by a single index could legitimately both be paths to a
   solution; we'd reject the second visit. Acceptable for an experiment;
   document the limitation.

2. **Add-all vs add-one.** Adding all violators at once may over-activate
   compared to add-most-violated-only. If we hit cases where add-all
   over-constrains and triggers Slvs failure that drops the wrong constraint,
   we may want to try add-one as a fallback. Defer.

3. **Drop-all vs drop-one.** Same consideration in reverse. Defer.

4. **Probe-based dropping** (option (c) from brainstorming) is a known follow-up
   if (b) proves insufficient on real sketches.

5. **`con_le_pt_line_distance` signed semantics** may surprise users who think
   in absolute distance. Documented; we'll see if it bites.
