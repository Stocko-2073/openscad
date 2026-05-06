# solve2d Inequality Constraints (SQP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four `con_le_*` inequality constraints to `solve2d` via an outer active-set loop around `Slvs_Solve`, with no SolveSpace modifications. Backward-compatible: zero-inequality input takes the existing single-`Slvs_Solve` path verbatim.

**Architecture:** A new `InequalityDecl` struct collects parsed `con_le_*` items into a separate vector. When non-empty, an outer SQP-style loop treats active inequalities as equalities, calls `Slvs_Solve`, then either adds violators (on success) or drops SolveSpace-flagged actives (on `INCONSISTENT`). Cycle detection bails out cleanly. The `Slvs_System` is rebuilt from scratch each iteration via a new `build_and_solve_once()` helper extracted from the current monolithic `builtin_solve2d`.

**Tech Stack:** C++17, CMake, libslvs (vendored at `submodules/SolveSpaceLib/libslvs/`). Build is the existing CMake setup. No new test framework — verification is via SCAD example files run through the CLI binary.

**Spec:** `doc/specs/2026-05-06-solve2d-inequality-sqp-design.md`

**Branch:** `experiment/solve2d-inequalities` (already created off `dev`)

**Verification convention used throughout this plan:**

```bash
# Build (parallel, all cores)
cmake --build build -j$(sysctl -n hw.ncpu)

# Run a SCAD test
build/OpenSCAD.app/Contents/MacOS/OpenSCAD <path/to/test.scad> -o /tmp/out.stl 2>&1
```

Tests are SCAD files using `assert(...)` and `echo(...)`. A failing `assert` aborts rendering with a `Warning:` message in stderr; passing tests render geometry and echo their results. "Test passes" = exit code 0 + no `Warning:` lines about the assertion + expected `ECHO` lines present.

---

## Task 1: Extract `build_and_solve_once()` helper (refactor, no behavior change)

**Files:**
- Modify: `src/core/builtin_solve.cc` (lines ~607–1118 — the `builtin_solve2d` function body)

**Why:** The outer SQP loop will call the build/solve cycle multiple times. To keep the loop readable, we extract the entire "build Slvs_System + solve + recover residuals" block into a callable helper. This task introduces no new behavior — `builtin_solve2d` calls the new helper exactly once with no inequalities, and the existing examples must produce identical output.

- [ ] **Step 1: Capture baseline output of all existing solver examples**

```bash
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/baseline.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
done > /tmp/solver_baseline.txt
cat /tmp/solver_baseline.txt
```

Save the output. This is the regression oracle for steps below.

- [ ] **Step 2: Add the helper struct and function declaration in the anonymous namespace near `constraint_residual()` (around line 605)**

Insert before the `}  // namespace` that closes around line 605:

```cpp
struct SolveOnceResult {
  int slvs_result = -1;
  bool solved = false;
  int dof = 0;
  double residual = 0.0;
  std::map<std::string, SolutionType::Point2d> points;
  std::vector<std::string> failed_constraint_names;
};

// One Slvs_Solve cycle: build the Slvs_System from `points` and `constraints`,
// run the solver, apply REDUNDANT_OKAY residual recovery, return results.
// `points` is non-const because each PointDecl receives assigned u_param /
// v_param / entity handles (used here to read back coordinates).
SolveOnceResult build_and_solve_once(
    std::vector<PointDecl>& points,
    const std::map<std::string, size_t>& name_to_idx,
    const std::vector<ConstraintDecl>& constraints,
    const Location& loc,
    const std::string& doc_root);
```

- [ ] **Step 3: Add the helper function body after `constraint_residual()` (still in anonymous namespace, just before the closing `}  // namespace`)**

Move the existing code from `builtin_solve2d` lines ~746–1115 (everything from `// Build phase: assemble Slvs_System` through the end of the residual-recovery block) into this new function body, mechanically. No semantic changes. The function returns a `SolveOnceResult` populated from the local variables that previously wrote into `data->...`.

Concrete shape:

```cpp
SolveOnceResult build_and_solve_once(
    std::vector<PointDecl>& points,
    const std::map<std::string, size_t>& name_to_idx,
    const std::vector<ConstraintDecl>& constraints,
    const Location& loc,
    const std::string& doc_root)
{
  SolveOnceResult out;

  // [moved verbatim from old builtin_solve2d lines ~746-1067:
  //   - Slvs_hGroup constants
  //   - sparams / sentities / sconstraints vectors and counters
  //   - workplane setup (origin, normal, workplane entity)
  //   - point param/entity creation (writes p.u_param, p.v_param, p.entity)
  //   - the make_line / pt_entity lambdas
  //   - constraint_name_by_h map
  //   - the big if/else chain over constraint kinds
  //   - Slvs_System sys; std::memset...; Slvs_Solve(&sys, g_solve);
  // ]

  out.slvs_result = sys.result;
  out.solved = (sys.result == SLVS_RESULT_OKAY);
  out.dof = sys.dof;

  std::map<Slvs_hParam, double> param_value;
  for (int i = 0; i < sys.params; ++i) {
    param_value[sys.param[i].h] = sys.param[i].val;
  }
  for (const auto& p : points) {
    out.points[p.name] = {param_value[p.u_param], param_value[p.v_param]};
  }

  for (int i = 0; i < sys.faileds; ++i) {
    auto it = constraint_name_by_h.find(sys.failed[i]);
    out.failed_constraint_names.push_back(
        it != constraint_name_by_h.end()
            ? it->second
            : "constraint #" + std::to_string(sys.failed[i]));
  }

  // REDUNDANT_OKAY residual recovery (existing logic).
  constexpr double RESIDUAL_TOLERANCE = 1e-6;
  double max_residual = 0.0;
  for (const auto& c : constraints) {
    double r = constraint_residual(c, out.points);
    if (r > max_residual) max_residual = r;
  }
  out.residual = max_residual;

  if (sys.result == SLVS_RESULT_INCONSISTENT && max_residual < RESIDUAL_TOLERANCE) {
    out.solved = true;
    out.failed_constraint_names.clear();
  }

  return out;
}
```

(In Task 4 we extend the recovery branch to also clear `failed_active_ineqs`
so the outer loop doesn't drop actives that SolveSpace only flagged due to
rank-deficiency.)

- [ ] **Step 4: Replace the moved code in `builtin_solve2d` with a call to the helper**

Replace `builtin_solve2d` lines ~746–1115 with:

```cpp
  SolveOnceResult once = build_and_solve_once(points, name_to_idx, constraints, loc, doc_root);

  auto data = std::make_shared<SolutionType::Data>();
  data->result_code = once.slvs_result;
  data->solved = once.solved;
  data->dof = once.dof;
  data->residual = once.residual;
  data->points = once.points;
  for (const auto& p : points) data->ordered_names.push_back(p.name);
  data->failed_constraints = once.failed_constraint_names;

  return Value(SolutionPtr(SolutionType(std::move(data))));
}
```

(The for-loop over `ordered_names` was previously inside the build phase — move it here so the return path stays in `builtin_solve2d`.)

- [ ] **Step 5: Build and confirm it compiles**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build, no errors.

- [ ] **Step 6: Re-run all solver examples and diff against baseline**

```bash
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/refactored.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
done > /tmp/solver_refactored.txt
diff /tmp/solver_baseline.txt /tmp/solver_refactored.txt
```

Expected: empty diff. If any line differs the refactor changed behavior — investigate before moving on.

- [ ] **Step 7: Commit**

```bash
git add src/core/builtin_solve.cc
git commit -m "$(cat <<'EOF'
solve2d: extract build_and_solve_once() helper (no behavior change)

Mechanical refactor pulling the Slvs_System build + Slvs_Solve cycle out of
builtin_solve2d into a callable helper. Sets up for the SQP outer loop that
needs to invoke the cycle repeatedly with different active sets.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Extend `SolutionType::Data` with `iterations` and `active_inequalities`

**Files:**
- Modify: `src/core/SolutionType.h:18-26` (Data struct), `:37-47` (accessors)

**Why:** The new accessors `iterations(sol)` and `active_inequalities(sol)` need backing fields. Add them now so subsequent tasks can populate them. Default values (0 and empty vector) are correct for the existing single-solve path.

- [ ] **Step 1: Add fields to `SolutionType::Data`**

In `src/core/SolutionType.h`, replace the `Data` struct (lines 18-26) with:

```cpp
struct Data {
  bool solved = false;
  int result_code = -1;
  double residual = 0.0;
  int dof = 0;
  int iterations = 0;
  std::map<std::string, Point2d> points;
  std::vector<std::string> ordered_names;
  std::vector<std::string> failed_constraints;
  std::vector<std::string> active_inequalities;
};
```

- [ ] **Step 2: Add const accessors**

In `src/core/SolutionType.h`, after the existing `failed_constraints()` accessor (around line 45), add:

```cpp
[[nodiscard]] int iterations() const { return data_->iterations; }
[[nodiscard]] const std::vector<std::string>& active_inequalities() const
{
  return data_->active_inequalities;
}
```

- [ ] **Step 3: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build. The new fields default-initialize to `0` and empty vector, so no existing code needs to change.

- [ ] **Step 4: Confirm no regression**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD examples/Solver/solve2d_basic.scad -o /tmp/out.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
```

Expected output unchanged from baseline:

```
ECHO: a = [0, 0], b = [15, 0], c = [15, 15]
ECHO: remaining_dof = 0
```

- [ ] **Step 5: Commit**

```bash
git add src/core/SolutionType.h
git commit -m "$(cat <<'EOF'
solve2d: add iterations and active_inequalities fields to SolutionType

Backing fields for the upcoming iterations(sol) and active_inequalities(sol)
accessors. Default values (0 and empty vector) match the existing single-solve
path so no other code needs to change yet.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add `con_le_distance` factory + `InequalityDecl` parser routing (parsed but not enforced)

**Files:**
- Modify: `src/core/builtin_solve.cc` (add factory, parser case, struct declaration, registration)

**Why:** Get the parser plumbing in place. Inequalities are recognized and stored in a separate vector but are not yet enforced — the solver call still ignores them. This validates the parsing layer in isolation before adding loop logic.

- [ ] **Step 1: Add the `InequalityDecl` struct in the anonymous namespace, just after `ConstraintDecl` (around line 440)**

```cpp
struct InequalityDecl {
  std::string kind;                  // "le_distance", "le_pt_line_distance", ...
  std::string name;                  // user-visible summary for active_inequalities
  std::vector<std::string> points;
  double valA = 0.0;                 // d / diff / deg, depending on kind
};
```

- [ ] **Step 2: Add the `con_le_distance` factory function (place after `builtin_con_distance` around line 148)**

```cpp
Value builtin_con_le_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 3 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_le_distance() expects (point_name, point_name, number)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "le_distance");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("d", arguments[2]->clone());
  return obj;
}
```

- [ ] **Step 3: Add the inequalities vector and parser case in `builtin_solve2d`**

In `builtin_solve2d`, just after `std::vector<ConstraintDecl> constraints;` (around line 620), add:

```cpp
  std::vector<InequalityDecl> inequalities;
```

In the parser if/else chain (around line 657, where kinds like `coincident`, `distance`, etc. are matched), add a new branch BEFORE the `else` that emits the unknown-kind warning:

```cpp
      } else if (kind == "le_distance") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string a, b;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        field_double(obj, "d", ineq.valA);
        ineq.name = "con_le_distance(" + a + "," + b + "," +
                    std::to_string(ineq.valA) + ")";
        inequalities.push_back(std::move(ineq));
        continue;  // skip the constraints.push_back below
```

Note: the existing parser block ends with `constraints.push_back(std::move(c));` after the if/else. The `continue` ensures the inequality doesn't fall into that push.

- [ ] **Step 4: Register the new builtin**

In `register_builtin_solve()` (around line 1311, near `con_distance` registration), add:

```cpp
  Builtins::init("con_le_distance", new BuiltinFunction(&builtin_con_le_distance),
                 {"con_le_distance(p1, p2, d) -> sketch constraint (|p1p2| <= d)"});
```

- [ ] **Step 5: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build.

- [ ] **Step 6: Write a smoke-test SCAD file that uses `con_le_distance` with a slack value**

Create `examples/Solver/solve2d_le_distance_smoke.scad`:

```scad
// solve2d_le_distance_smoke.scad — verify con_le_distance parses without
// crashing. Inequality is not yet enforced (Task 3 only wires parsing).
// The unconstrained solution gives |ab| = 5; the slack inequality |ab| <= 100
// should be silently ignored at this stage.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [5, 0]),
  con_horizontal("a", "b"),
  con_distance("a", "b", 5),
  con_le_distance("a", "b", 100),  // slack — ignored at this stage
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
echo(b = pt(sol, "b"));
echo(remaining_dof = dof(sol));
```

- [ ] **Step 7: Run smoke test**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD examples/Solver/solve2d_le_distance_smoke.scad -o /tmp/out.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
```

Expected:

```
ECHO: b = [5, 0]
ECHO: remaining_dof = 0
```

No warnings about unknown kind. (No geometry rendered — that's fine, this is an echo-only smoke test.)

- [ ] **Step 8: Re-run all existing solver examples to confirm no regression**

```bash
for f in examples/Solver/solve2d_*.scad; do
  [ "$f" = "examples/Solver/solve2d_le_distance_smoke.scad" ] && continue
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/check.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
done > /tmp/solver_after_task3.txt
diff /tmp/solver_baseline.txt /tmp/solver_after_task3.txt
```

Expected: empty diff.

- [ ] **Step 9: Commit**

```bash
git add src/core/builtin_solve.cc examples/Solver/solve2d_le_distance_smoke.scad
git commit -m "$(cat <<'EOF'
solve2d: parse con_le_distance into separate inequalities vector

Adds the InequalityDecl struct and parser routing for con_le_distance.
Inequalities are stored but not yet enforced — the solver loop in the next
task wires them up. Smoke test confirms parsing doesn't crash and slack
inequalities are silently ignored as expected.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Implement SQP outer loop, wire up, add `iterations()` and `active_inequalities()` accessors

**Files:**
- Modify: `src/core/builtin_solve.cc` (extend `build_and_solve_once`, add `solve_with_inequalities`, add accessors, register)

**Why:** The core experiment. Extends `build_and_solve_once()` to accept an active inequality set, adds `inequality_violation()` for the `le_distance` kind, implements the active-set loop with cycle detection, and exposes the new accessors. After this task, slack inequalities solve in 1 iteration with empty active set.

- [ ] **Step 1: Extend `build_and_solve_once()` signature and body to accept active inequalities**

Update the declaration and definition in the anonymous namespace.

New signature:

```cpp
struct SolveOnceResult {
  int slvs_result = -1;
  bool solved = false;
  int dof = 0;
  double residual = 0.0;
  std::map<std::string, SolutionType::Point2d> points;
  std::vector<std::string> failed_constraint_names;

  // For active-set loop: handle assigned to each active inequality, so the
  // outer loop can intersect this with sys.failed to know which actives to
  // drop. Keys are indices into the inequalities vector.
  std::map<size_t, Slvs_hConstraint> active_ineq_handles;
  // Indices of active inequalities found in sys.failed.
  std::vector<size_t> failed_active_ineqs;
};

SolveOnceResult build_and_solve_once(
    std::vector<PointDecl>& points,
    const std::map<std::string, size_t>& name_to_idx,
    const std::vector<ConstraintDecl>& constraints,
    const std::vector<InequalityDecl>& inequalities,
    const std::set<size_t>& active_set,
    const Location& loc,
    const std::string& doc_root);
```

In the body, after the existing constraint loop, add a second loop that creates Slvs constraints for each active inequality. For `le_distance`, this is `SLVS_C_PT_PT_DISTANCE` with `valA` as the value:

```cpp
  for (size_t idx : active_set) {
    const auto& ineq = inequalities[idx];
    Slvs_hConstraint ch = next_constraint++;

    if (ineq.kind == "le_distance" && ineq.points.size() == 2) {
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      if (!a || !b) continue;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_PT_DISTANCE,
                                                 wrkpl, ineq.valA, a, b, 0, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    }
    // [other kinds added in later tasks]
  }
```

After `Slvs_Solve` returns and the failed list is processed, also populate `failed_active_ineqs`:

```cpp
  for (int i = 0; i < sys.faileds; ++i) {
    Slvs_hConstraint h = sys.failed[i];
    for (const auto& [idx, hh] : out.active_ineq_handles) {
      if (hh == h) out.failed_active_ineqs.push_back(idx);
    }
  }
```

Extend the REDUNDANT_OKAY recovery branch to also clear the new failed-active list — otherwise the outer loop would drop actives just because SolveSpace was confused about rank:

```cpp
  if (sys.result == SLVS_RESULT_INCONSISTENT && max_residual < RESIDUAL_TOLERANCE) {
    out.solved = true;
    out.failed_constraint_names.clear();
    out.failed_active_ineqs.clear();   // new: don't punish a recovered solve
  }
```

For the residual recovery: include active inequalities in `max_residual` since they are now equalities in the solver:

```cpp
  for (const auto& c : constraints) {
    double r = constraint_residual(c, out.points);
    if (r > max_residual) max_residual = r;
  }
  for (size_t idx : active_set) {
    // Treat active inequalities as equalities for residual computation.
    // We synthesize a ConstraintDecl-shaped query using the same kind names
    // that constraint_residual recognizes ("distance" for le_distance, etc).
    ConstraintDecl pseudo;
    if (inequalities[idx].kind == "le_distance") pseudo.kind = "distance";
    // [more mappings in later tasks]
    pseudo.points = inequalities[idx].points;
    pseudo.valA = inequalities[idx].valA;
    double r = constraint_residual(pseudo, out.points);
    if (r > max_residual) max_residual = r;
  }
  out.residual = max_residual;
```

Update the call site in `builtin_solve2d` (the no-inequalities path) to pass the new args:

```cpp
  if (inequalities.empty()) {
    SolveOnceResult once = build_and_solve_once(points, name_to_idx, constraints,
                                                inequalities, /*active_set=*/{},
                                                loc, doc_root);
    // ... existing population of `data` from `once` ...
    return Value(SolutionPtr(SolutionType(std::move(data))));
  }
  // [solve_with_inequalities path, added below]
```

- [ ] **Step 2: Add `inequality_violation()` helper for `le_distance`**

In the anonymous namespace, after `constraint_residual()` and before `build_and_solve_once()`, add:

```cpp
// Signed violation for inequality g(x) <= rhs:
//   positive ⇒ violated by that amount
//   non-positive ⇒ slack (constraint satisfied)
// Returns 0 for unknown kinds and degenerate inputs (no false positives).
double inequality_violation(const InequalityDecl& ineq,
                            const std::map<std::string, SolutionType::Point2d>& pts)
{
  using P = SolutionType::Point2d;
  auto get = [&](const std::string& n, P& out) -> bool {
    auto it = pts.find(n);
    if (it == pts.end()) return false;
    out = it->second;
    return true;
  };
  auto sub = [](const P& a, const P& b) -> P { return {a[0]-b[0], a[1]-b[1]}; };
  auto norm = [](const P& v) { return std::sqrt(v[0]*v[0] + v[1]*v[1]); };

  P a, b;
  if (ineq.kind == "le_distance" && ineq.points.size() == 2) {
    if (!get(ineq.points[0], a) || !get(ineq.points[1], b)) return 0.0;
    return norm(sub(a, b)) - ineq.valA;
  }
  // [other kinds added in later tasks]
  return 0.0;
}
```

- [ ] **Step 3: Add `solve_with_inequalities()` outer loop**

After `build_and_solve_once()`, in the same anonymous namespace:

```cpp
constexpr int MAX_OUTER_ITERATIONS = 50;
constexpr double VIOLATION_TOLERANCE = 1e-6;

struct SolveLoopResult {
  SolveOnceResult last;          // final solve's results
  int iterations = 0;            // outer-loop iteration count
  std::set<size_t> active_set;   // final active set
  bool converged = false;        // true ⇒ feasible solution found
  std::string failure_reason;    // "cycle" / "max_iter" / "infeasible_base" / ""
};

SolveLoopResult solve_with_inequalities(
    std::vector<PointDecl>& points,
    const std::map<std::string, size_t>& name_to_idx,
    const std::vector<ConstraintDecl>& constraints,
    const std::vector<InequalityDecl>& inequalities,
    const Location& loc,
    const std::string& doc_root)
{
  SolveLoopResult result;
  std::set<std::set<size_t>> visited;

  for (int iter = 0; iter < MAX_OUTER_ITERATIONS; ++iter) {
    if (visited.count(result.active_set)) {
      result.failure_reason = "cycle";
      return result;
    }
    visited.insert(result.active_set);

    result.iterations = iter + 1;
    result.last = build_and_solve_once(points, name_to_idx, constraints,
                                       inequalities, result.active_set,
                                       loc, doc_root);

    if (result.last.solved) {
      // Check inactive inequalities for violations.
      std::vector<size_t> violators;
      for (size_t i = 0; i < inequalities.size(); ++i) {
        if (result.active_set.count(i)) continue;
        double v = inequality_violation(inequalities[i], result.last.points);
        if (v > VIOLATION_TOLERANCE) violators.push_back(i);
      }
      if (violators.empty()) {
        result.converged = true;
        return result;
      }
      for (size_t i : violators) result.active_set.insert(i);
    } else {
      // Slvs failed; drop SolveSpace-flagged active inequalities.
      if (result.last.failed_active_ineqs.empty()) {
        result.failure_reason = "infeasible_base";
        return result;
      }
      for (size_t i : result.last.failed_active_ineqs) result.active_set.erase(i);
    }
  }

  result.failure_reason = "max_iter";
  return result;
}
```

- [ ] **Step 4: Wire `solve_with_inequalities()` into `builtin_solve2d`**

Replace the no-inequalities-only path with:

```cpp
  auto data = std::make_shared<SolutionType::Data>();

  if (inequalities.empty()) {
    SolveOnceResult once = build_and_solve_once(points, name_to_idx, constraints,
                                                inequalities, /*active_set=*/{},
                                                loc, doc_root);
    data->result_code = once.slvs_result;
    data->solved = once.solved;
    data->dof = once.dof;
    data->residual = once.residual;
    data->points = once.points;
    for (const auto& p : points) data->ordered_names.push_back(p.name);
    data->failed_constraints = once.failed_constraint_names;
    data->iterations = 0;
  } else {
    SolveLoopResult loop = solve_with_inequalities(points, name_to_idx, constraints,
                                                   inequalities, loc, doc_root);
    data->result_code = loop.last.slvs_result;
    data->solved = loop.converged;
    data->dof = loop.last.dof;
    data->residual = loop.last.residual;
    data->points = loop.last.points;
    for (const auto& p : points) data->ordered_names.push_back(p.name);
    data->failed_constraints = loop.last.failed_constraint_names;
    data->iterations = loop.iterations;
    for (size_t idx : loop.active_set) {
      data->active_inequalities.push_back(inequalities[idx].name);
    }
    if (!loop.converged && data->failed_constraints.empty()) {
      data->failed_constraints.push_back("solve2d: " + loop.failure_reason);
    }
  }

  return Value(SolutionPtr(SolutionType(std::move(data))));
}
```

- [ ] **Step 5: Add `iterations()` and `active_inequalities()` builtin accessors**

After `builtin_failed_constraints()` (around line 1246), add:

```cpp
Value builtin_iterations(Arguments arguments, const Location& loc)
{
  if (!require_solution("iterations", arguments, loc, 1)) return Value::undefined.clone();
  return Value(static_cast<double>(arguments[0]->toSolution().iterations()));
}

Value builtin_active_inequalities(Arguments arguments, const Location& loc)
{
  if (!require_solution("active_inequalities", arguments, loc, 1)) {
    return Value::undefined.clone();
  }
  const auto& items = arguments[0]->toSolution().active_inequalities();
  VectorType result(arguments.session());
  result.reserve(items.size());
  for (const auto& s : items) result.emplace_back(s);
  return Value(std::move(result));
}
```

In `register_builtin_solve()`, after the `failed_constraints` registration (around line 1364), add:

```cpp
  Builtins::init("iterations", new BuiltinFunction(&builtin_iterations),
                 {"iterations(sol) -> outer-loop iteration count (0 if no inequalities)"});
  Builtins::init("active_inequalities", new BuiltinFunction(&builtin_active_inequalities),
                 {"active_inequalities(sol) -> [string] inequalities that are tight"});
```

- [ ] **Step 6: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build.

- [ ] **Step 7: Update the smoke test from Task 3 to use the new accessors and verify the slack case**

Replace `examples/Solver/solve2d_le_distance_smoke.scad` with:

```scad
// solve2d_le_distance_smoke.scad — slack con_le_distance.
// |ab| = 5 from con_distance; the inequality |ab| <= 100 is slack and
// should NOT end up in the active set. Loop should converge in 1 iteration.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [5, 0]),
  con_horizontal("a", "b"),
  con_distance("a", "b", 5),
  con_le_distance("a", "b", 100),
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(iterations(sol) == 1,
       str("expected 1 iteration, got ", iterations(sol)));
assert(len(active_inequalities(sol)) == 0,
       str("expected empty active set, got ", active_inequalities(sol)));

echo(iter = iterations(sol), active = active_inequalities(sol), b = pt(sol, "b"));
```

- [ ] **Step 8: Run the slack smoke test**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD examples/Solver/solve2d_le_distance_smoke.scad -o /tmp/out.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
```

Expected:

```
ECHO: iter = 1, active = [], b = [5, 0]
```

No warnings.

- [ ] **Step 9: Re-run all existing solver examples to confirm no regression**

```bash
for f in examples/Solver/solve2d_*.scad; do
  [ "$f" = "examples/Solver/solve2d_le_distance_smoke.scad" ] && continue
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/check.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
done > /tmp/solver_after_task4.txt
diff /tmp/solver_baseline.txt /tmp/solver_after_task4.txt
```

Expected: empty diff (the existing examples don't use inequalities, so they take the `inequalities.empty()` branch verbatim).

- [ ] **Step 10: Commit**

```bash
git add src/core/builtin_solve.cc examples/Solver/solve2d_le_distance_smoke.scad
git commit -m "$(cat <<'EOF'
solve2d: implement SQP outer loop for con_le_distance

Adds inequality_violation() and solve_with_inequalities() outer loop with
add-on-violation / drop-on-Slvs-failure / cycle-detection. Wires up the
iterations() and active_inequalities() accessors. Slack inequalities
converge in 1 iteration with empty active set; existing equality-only
examples are unaffected.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Add binding-case example for `con_le_distance`

**Files:**
- Create: `examples/Solver/solve2d_le_distance_binding.scad`

**Why:** Verify the loop correctly activates an inequality that would otherwise be violated.

- [ ] **Step 1: Create the binding-case test SCAD**

```scad
// solve2d_le_distance_binding.scad — binding con_le_distance.
// Without the inequality, |ab| = 10 (from con_distance). The inequality
// |ab| <= 4 conflicts and forces removal of con_distance — but con_distance
// is an equality, so the system as posed is infeasible. Instead, drop
// con_distance and rely only on the inequality to bound |ab|.
//
// Setup: a is fixed at origin, b is unconstrained except by con_le_distance.
// Outer loop should:
//   iter 1: solve with empty active set → b drifts to seed; |ab| might
//           exceed 4, triggering activation
//   iter 2: solve with {le_distance} active → |ab| = 4 exactly
// Final: solved, iterations==2, 1 active inequality.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [10, 0]),         // seed far from origin
  con_horizontal("a", "b"),
  con_le_distance("a", "b", 4),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol), ", active=", active_inequalities(sol), ")"));
assert(iterations(sol) >= 2,
       str("expected at least 2 iterations, got ", iterations(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));

bx = pt(sol, "b")[0];
// |bx| should equal 4 — seed at [10,0] biases to +4, but accept symmetric -4
// solution too in case SolveSpace's seed handling drifts.
assert(abs(abs(bx) - 4) < 1e-4,
       str("expected |b.x| == 4 (binding), got ", bx));

echo(iter = iterations(sol),
     active = active_inequalities(sol),
     b = pt(sol, "b"));
```

- [ ] **Step 2: Run it**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD examples/Solver/solve2d_le_distance_binding.scad -o /tmp/out.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
```

Expected (active set name will include the formatted distance):

```
ECHO: iter = 2, active = ["con_le_distance(a,b,4.000000)"], b = [4, 0]
```

If iterations is 1 with empty active set, the inequality wasn't violated by the unconstrained solve — meaning the seed `at = [10, 0]` didn't carry through and SolveSpace landed inside the bound. Investigate by reducing the slack (use `con_le_distance("a", "b", 1)`) or tightening the seed.

- [ ] **Step 3: Commit**

```bash
git add examples/Solver/solve2d_le_distance_binding.scad
git commit -m "$(cat <<'EOF'
solve2d: add binding-case example for con_le_distance

Verifies the SQP loop activates an inequality that would otherwise be
violated and converges to the binding value (|ab| = 4 exactly).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Add infeasible-case example for `con_le_distance`

**Files:**
- Create: `examples/Solver/solve2d_le_distance_infeasible.scad`

**Why:** Verify the loop bails cleanly with a clear failure reason when no feasible solution exists.

- [ ] **Step 1: Create the infeasible test SCAD**

```scad
// solve2d_le_distance_infeasible.scad — contradictory constraints.
// con_distance forces |ab| = 10. con_le_distance forces |ab| <= 5.
// No feasible solution exists. Expected: solved(sol) == false; the loop
// should report failure (cycle/max_iter/infeasible_base) in
// failed_constraints. We don't pin down the exact reason — the experiment
// is interested in *whether* failure is reported cleanly.

sol = solve2d([
  point("a", at = [0, 0]),
  con_fixed("a"),
  point("b", at = [10, 0]),
  con_horizontal("a", "b"),
  con_distance("a", "b", 10),       // equality: forces |ab| = 10
  con_le_distance("a", "b", 5),     // inequality: forces |ab| <= 5
]);

assert(!solved(sol),
       str("expected unsolved, but solver claimed success: b=", pt(sol, "b"),
           " active=", active_inequalities(sol)));

echo(solved = solved(sol),
     iter = iterations(sol),
     active = active_inequalities(sol),
     failed = failed_constraints(sol));
```

- [ ] **Step 2: Run it**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD examples/Solver/solve2d_le_distance_infeasible.scad -o /tmp/out.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
```

Expected: an `ECHO` line showing `solved = false`, non-zero `iter`, some `failed` content. The exact `failed` content depends on which loop branch caught the failure — acceptable values include `["solve2d: cycle"]`, `["solve2d: max_iter"]`, or actual SolveSpace failed-constraint names.

- [ ] **Step 3: Commit**

```bash
git add examples/Solver/solve2d_le_distance_infeasible.scad
git commit -m "$(cat <<'EOF'
solve2d: add infeasible-case example for con_le_distance

Contradictory equality+inequality constraints. Verifies the SQP loop bails
cleanly and reports failure rather than looping forever or producing a
silently-wrong solution.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Add `con_le_pt_line_distance`

**Files:**
- Modify: `src/core/builtin_solve.cc` (factory, parser, violation, activation, registration)
- Create: `examples/Solver/solve2d_le_pt_line_distance.scad`

**Why:** Second of the four inequality kinds. Signed semantics — matches existing `con_pt_line_distance`.

- [ ] **Step 1: Add factory `builtin_con_le_pt_line_distance`**

After `builtin_con_pt_line_distance` (around line 290) in `src/core/builtin_solve.cc`:

```cpp
Value builtin_con_le_pt_line_distance(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_le_pt_line_distance", arguments, loc, 3)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "le_pt_line_distance");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  return obj;
}
```

- [ ] **Step 2: Add parser case**

In the parser if/else chain (where `le_distance` was added in Task 3), add:

```cpp
      } else if (kind == "le_pt_line_distance") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string p, a, b;
        field_string(obj, "p", p);
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        ineq.points.push_back(p);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        field_double(obj, "d", ineq.valA);
        ineq.name = "con_le_pt_line_distance(" + p + "," + a + "," + b + "," +
                    std::to_string(ineq.valA) + ")";
        inequalities.push_back(std::move(ineq));
        continue;
```

- [ ] **Step 3: Add `inequality_violation()` case**

In `inequality_violation()` (added in Task 4), add before the final `return 0.0;`:

```cpp
  if (ineq.kind == "le_pt_line_distance" && ineq.points.size() == 3) {
    P p_, a_, b_;
    if (!get(ineq.points[0], p_) || !get(ineq.points[1], a_) ||
        !get(ineq.points[2], b_)) return 0.0;
    P v = sub(b_, a_);
    double m = norm(v);
    if (m < 1e-12) return 0.0;
    // signed distance: cross_z(p-a, v) / |v|
    double signed_dist = ((p_[0]-a_[0])*v[1] - (p_[1]-a_[1])*v[0]) / m;
    return signed_dist - ineq.valA;
  }
```

- [ ] **Step 4: Add activation case in `build_and_solve_once`**

In the active-set constraint loop in `build_and_solve_once()` (added in Task 4), add after the `le_distance` branch:

```cpp
    } else if (ineq.kind == "le_pt_line_distance" && ineq.points.size() == 3) {
      Slvs_hEntity p = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[2]);
      if (!p || !a || !b) continue;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_LINE_DISTANCE,
                                                 wrkpl, ineq.valA, p, 0, line, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
```

In the residual-recovery synth-mapping (also added in Task 4), add:

```cpp
    if (inequalities[idx].kind == "le_pt_line_distance") pseudo.kind = "pt_line_distance";
```

- [ ] **Step 5: Register**

In `register_builtin_solve()`:

```cpp
  Builtins::init("con_le_pt_line_distance",
                 new BuiltinFunction(&builtin_con_le_pt_line_distance),
                 {"con_le_pt_line_distance(p, la, lb, d) -> sketch constraint (signed dist <= d)"});
```

- [ ] **Step 6: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build.

- [ ] **Step 7: Create example**

`examples/Solver/solve2d_le_pt_line_distance.scad`:

```scad
// solve2d_le_pt_line_distance.scad — point bounded above a horizontal line.
// Line a-b lies on the x-axis. signed_dist(p, line) = p.y (with v=(b-a)
// pointing along +x the formula reduces to p.y - a.y).
// con_le_pt_line_distance(p, a, b, 3) requires p.y <= 3.
//
// p has p.x pinned to a.x = 0 via con_vertical (sharing the vertical line
// a-p), but p.y is left free. Seed [5, 8] gives p.y = 8 after the first
// solve; the inequality activates and pulls p.y down to exactly 3.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("p", at = [5, 8]),                            // seed; p.x will get
                                                       // pulled to 0 by con_vertical
  con_vertical("p", "a"),                             // p.x = a.x = 0
  con_le_pt_line_distance("p", "a", "b", 3),          // signed_dist(p, line ab) <= 3
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));

py = pt(sol, "p")[1];
assert(abs(py - 3) < 1e-4, str("expected p.y == 3, got ", py));

echo(iter = iterations(sol), active = active_inequalities(sol), p = pt(sol, "p"));
```

- [ ] **Step 8: Run example**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD examples/Solver/solve2d_le_pt_line_distance.scad -o /tmp/out.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
```

Expected: `ECHO` shows iter ≥ 1, 1 active, `p` with y component near 3.

If the assertion fails because the unconstrained solve already lands at p.y=4 (sqrt(25-9)) due to `con_distance("p","a",5)`, that's fine — it just means the inequality activates earlier. Adjust the seed or the geometry until the test reliably exercises an active inequality.

- [ ] **Step 9: Re-run all existing examples for no regression**

```bash
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/check.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
done
```

Expected: every file echoes its expected output, no `Warning:` about asserts.

- [ ] **Step 10: Commit**

```bash
git add src/core/builtin_solve.cc examples/Solver/solve2d_le_pt_line_distance.scad
git commit -m "$(cat <<'EOF'
solve2d: add con_le_pt_line_distance (signed)

Same signed semantics as con_pt_line_distance. Wires through factory,
parser, violation, activation, and example.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Add `con_le_length_difference`

**Files:**
- Modify: `src/core/builtin_solve.cc`
- Create: `examples/Solver/solve2d_le_length_difference.scad`

**Why:** Third inequality kind. `(|ab| - |cd|) ≤ diff`.

- [ ] **Step 1: Add factory `builtin_con_le_length_difference`**

After `builtin_con_length_difference` (around line 343) in `src/core/builtin_solve.cc`:

```cpp
Value builtin_con_le_length_difference(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings_then_number("con_le_length_difference", arguments, loc, 4)) {
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "le_length_difference");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("diff", arguments[4]->clone());
  return obj;
}
```

- [ ] **Step 2: Add parser case**

```cpp
      } else if (kind == "le_length_difference") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string a, b, c_, d_;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        field_string(obj, "c", c_);
        field_string(obj, "d", d_);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        ineq.points.push_back(c_);
        ineq.points.push_back(d_);
        field_double(obj, "diff", ineq.valA);
        ineq.name = "con_le_length_difference(" + a + "," + b + "," + c_ + "," + d_ + "," +
                    std::to_string(ineq.valA) + ")";
        inequalities.push_back(std::move(ineq));
        continue;
```

- [ ] **Step 3: Add `inequality_violation()` case**

```cpp
  if (ineq.kind == "le_length_difference" && ineq.points.size() == 4) {
    P a_, b_, c_, d_;
    if (!get(ineq.points[0], a_) || !get(ineq.points[1], b_) ||
        !get(ineq.points[2], c_) || !get(ineq.points[3], d_)) return 0.0;
    return (norm(sub(b_, a_)) - norm(sub(d_, c_))) - ineq.valA;
  }
```

- [ ] **Step 4: Add activation case in `build_and_solve_once`**

```cpp
    } else if (ineq.kind == "le_length_difference" && ineq.points.size() == 4) {
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity c = pt_entity(ineq.kind, ineq.points[2]);
      Slvs_hEntity d = pt_entity(ineq.kind, ineq.points[3]);
      if (!a || !b || !c || !d) continue;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(c, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_LENGTH_DIFFERENCE,
                                                 wrkpl, ineq.valA, 0, 0, l1, l2));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
```

In the residual-recovery synth-mapping:

```cpp
    if (inequalities[idx].kind == "le_length_difference") pseudo.kind = "length_difference";
```

- [ ] **Step 5: Register**

```cpp
  Builtins::init("con_le_length_difference",
                 new BuiltinFunction(&builtin_con_le_length_difference),
                 {"con_le_length_difference(a, b, c, d, diff) -> sketch constraint (|ab|-|cd| <= diff)"});
```

- [ ] **Step 6: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

- [ ] **Step 7: Create example**

`examples/Solver/solve2d_le_length_difference.scad`:

```scad
// solve2d_le_length_difference.scad — bound |ab| relative to |cd|.
// |cd| is fixed at 5. |ab|-|cd| <= 1 forces |ab| <= 6.
// Without the inequality, |ab| would be 8 (from a strong distance pull
// via the seed); the loop should activate the inequality and pull |ab| to
// exactly 6.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [8, 0]), con_horizontal("a", "b"),
  point("c", at = [0, 10]), con_fixed("c"),
  point("d", at = [5, 10]), con_horizontal("c", "d"),
  con_distance("c", "d", 5),
  con_le_length_difference("a", "b", "c", "d", 1),  // |ab| - |cd| <= 1
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));
bx = pt(sol, "b")[0];
assert(abs(bx - 6) < 1e-4, str("expected b.x == 6, got ", bx));

echo(iter = iterations(sol), active = active_inequalities(sol),
     ab = pt(sol, "b")[0], cd = pt(sol, "d")[0] - pt(sol, "c")[0]);
```

- [ ] **Step 8: Run example**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD examples/Solver/solve2d_le_length_difference.scad -o /tmp/out.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
```

Expected: 1 active inequality, |ab| = 6.

- [ ] **Step 9: Commit**

```bash
git add src/core/builtin_solve.cc examples/Solver/solve2d_le_length_difference.scad
git commit -m "$(cat <<'EOF'
solve2d: add con_le_length_difference

|ab| - |cd| <= diff. Wires through factory, parser, violation, activation,
and example.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Add `con_le_angle`

**Files:**
- Modify: `src/core/builtin_solve.cc`
- Create: `examples/Solver/solve2d_le_angle.scad`

**Why:** Fourth and final inequality kind. Angle in `[0, 180]` ≤ deg.

- [ ] **Step 1: Add factory `builtin_con_le_angle`**

After `builtin_con_angle` (around line 207) in `src/core/builtin_solve.cc`:

```cpp
Value builtin_con_le_angle(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 5 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING ||
      arguments[4]->type() != Value::Type::NUMBER) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_le_angle() expects four point names and a degree value");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "le_angle");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("c", arguments[2]->clone());
  obj.set("d", arguments[3]->clone());
  obj.set("deg", arguments[4]->clone());
  return obj;
}
```

- [ ] **Step 2: Add parser case**

```cpp
      } else if (kind == "le_angle") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string a, b, c_, d_;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        field_string(obj, "c", c_);
        field_string(obj, "d", d_);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        ineq.points.push_back(c_);
        ineq.points.push_back(d_);
        field_double(obj, "deg", ineq.valA);
        ineq.name = "con_le_angle(" + a + "," + b + "," + c_ + "," + d_ + "," +
                    std::to_string(ineq.valA) + ")";
        inequalities.push_back(std::move(ineq));
        continue;
```

- [ ] **Step 3: Add `inequality_violation()` case**

```cpp
  if (ineq.kind == "le_angle" && ineq.points.size() == 4) {
    P a_, b_, c_, d_;
    if (!get(ineq.points[0], a_) || !get(ineq.points[1], b_) ||
        !get(ineq.points[2], c_) || !get(ineq.points[3], d_)) return 0.0;
    P v1 = sub(b_, a_), v2 = sub(d_, c_);
    double m1 = norm(v1), m2 = norm(v2);
    if (m1 < 1e-12 || m2 < 1e-12) return 0.0;
    double cosA = std::max(-1.0, std::min(1.0, (v1[0]*v2[0] + v1[1]*v2[1]) / (m1*m2)));
    double angle_deg = std::acos(cosA) * 180.0 / M_PI;
    return angle_deg - std::abs(ineq.valA);
  }
```

- [ ] **Step 4: Add activation case in `build_and_solve_once`**

```cpp
    } else if (ineq.kind == "le_angle" && ineq.points.size() == 4) {
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity c = pt_entity(ineq.kind, ineq.points[2]);
      Slvs_hEntity d = pt_entity(ineq.kind, ineq.points[3]);
      if (!a || !b || !c || !d) continue;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(c, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_ANGLE,
                                                 wrkpl, ineq.valA, 0, 0, l1, l2));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
```

In the residual-recovery synth-mapping:

```cpp
    if (inequalities[idx].kind == "le_angle") pseudo.kind = "angle";
```

- [ ] **Step 5: Register**

```cpp
  Builtins::init("con_le_angle",
                 new BuiltinFunction(&builtin_con_le_angle),
                 {"con_le_angle(p1, p2, p3, p4, deg) -> sketch constraint (angle <= deg)"});
```

- [ ] **Step 6: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

- [ ] **Step 7: Create example**

`examples/Solver/solve2d_le_angle.scad`:

```scad
// solve2d_le_angle.scad — bound the angle between two lines.
// Line ab is horizontal. Line cd starts at a steep angle (seed places d
// roughly 60° above the x-axis). The inequality requires angle <= 30°.
// Loop should activate the inequality and pull the angle to exactly 30°.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [5, 0]), con_horizontal("a", "b"), con_distance("a", "b", 5),
  point("c", at = [0, 0]),                            // overlap with a; a separate constraint pins c
  con_coincident("a", "c"),
  point("d", at = [3, 5]),                            // seed: ~60° above x-axis
  con_distance("c", "d", 6),
  con_le_angle("a", "b", "c", "d", 30),
]);

assert(solved(sol), str("solver failed: ", failed_constraints(sol)));
assert(len(active_inequalities(sol)) == 1,
       str("expected 1 active inequality, got ", active_inequalities(sol)));

// angle(sol, ["b","a","d"]) returns CCW degrees in [0, 360); we expect
// either ~30 or ~330 depending on which side d settles on. Compare
// the absolute angle to 30.
ang = angle(sol, ["b", "a", "d"]);
ang_unsigned = (ang > 180) ? 360 - ang : ang;
assert(abs(ang_unsigned - 30) < 1e-3,
       str("expected unsigned angle == 30, got ", ang_unsigned));

echo(iter = iterations(sol), active = active_inequalities(sol),
     ang = ang, ang_unsigned = ang_unsigned, d = pt(sol, "d"));
```

- [ ] **Step 8: Run example**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD examples/Solver/solve2d_le_angle.scad -o /tmp/out.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
```

Expected: 1 active inequality, unsigned angle ≈ 30°.

- [ ] **Step 9: Final regression sweep — run every solver example and confirm no failures**

```bash
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/check.stl 2>&1 | grep -E '^(ECHO|Warning|Error)'
done
```

Expected: every file produces its expected `ECHO` lines, zero `Warning: Assertion` lines.

- [ ] **Step 10: Commit**

```bash
git add src/core/builtin_solve.cc examples/Solver/solve2d_le_angle.scad
git commit -m "$(cat <<'EOF'
solve2d: add con_le_angle

Angle in [0, 180] <= deg. Wires through factory, parser, violation,
activation, and example. Completes the four-constraint con_le_* family
of the inequality experiment.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
