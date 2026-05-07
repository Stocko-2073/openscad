# `con_pt_on_segment` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new sketch constraint `con_pt_on_segment(p, a, b)` that asserts `p` lies on the closed segment from `a` to `b`. Internally expands to one always-on `pt_on_line` equality plus two new inequality kinds (`pt_on_segment_lower` / `pt_on_segment_upper`) that participate in the existing SQP active-set loop.

**Architecture:** One user-facing builtin produces one `ObjectType` of `kind = "pt_on_segment"`. The `solve2d` parser recognises that kind and emits three internal items: one `ConstraintDecl{kind="pt_on_line"}` plus two `InequalityDecl`s (`pt_on_segment_lower`, `pt_on_segment_upper`). Inequality violation is `−dot(p−a, b−a)` and `dot(p−b, b−a)` respectively. When an inequality activates, the active-set switch in `build_and_solve_once()` emits `SLVS_C_POINTS_COINCIDENT(p, endpoint)` — geometrically equivalent to the bound being tight (given the on-line equality). Reporting names use suffixes `:on_line` / `:start` / `:end` so all three internal pieces share the user-visible `con_pt_on_segment(p,a,b)` prefix.

**Tech Stack:** C++17, CMake, libslvs (vendored at `submodules/SolveSpaceLib/libslvs/`). No new test framework — verification is via SCAD example files run through the CLI binary.

**Spec:** `doc/specs/2026-05-06-pt-on-segment-design.md`

**Branch:** `experiment/solve2d-inequalities` (already current branch).

**Verification convention used throughout this plan:**

```bash
# Build (parallel, all cores)
cmake --build build -j$(sysctl -n hw.ncpu)

# Run a SCAD test
build/OpenSCAD.app/Contents/MacOS/OpenSCAD <path/to/test.scad> -o /tmp/out.stl 2>&1
```

A failing `assert(...)` aborts rendering with a `Warning:` message in stderr; passing tests render geometry and echo their results. "Test passes" = exit code 0 + no `Warning:` lines about an assertion + expected `ECHO` lines present.

---

## Task 1: Capture pre-change baseline (regression oracle)

**Files:** none modified.

**Why:** Existing `solve2d_*.scad` examples must continue to pass after the changes. Capture their current output so we can compare.

- [ ] **Step 1: Run all existing solver examples and save output**

```bash
cd /Users/samw3/prj/stocko/openscad
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/baseline.stl 2>&1 \
    | grep -E '^(ECHO|Warning|Error)'
done > /tmp/pt_on_segment_baseline.txt
cat /tmp/pt_on_segment_baseline.txt
```

Expected: each `=== ... ===` block lists `ECHO:` lines from the assertions in that example, and contains **no** `Warning:` lines (other than benign warnings unrelated to assertions). Save `/tmp/pt_on_segment_baseline.txt` for comparison in Task 7.

---

## Task 2: Write the failing golden-path test

**Files:**
- Create: `examples/Solver/solve2d_pt_on_segment.scad`

**Why:** Test-first. The test exercises the *non-binding* behavior: `p` is constrained to the segment but a separate constraint pins its position to a non-endpoint, so neither bound activates. Until the new builtin and parser branch exist, OpenSCAD will warn "Ignoring unknown function `con_pt_on_segment`" and the assertion on the point coordinate will fail.

- [ ] **Step 1: Create the test file**

Write the file `examples/Solver/solve2d_pt_on_segment.scad` with this exact content:

```openscad
// solve2d_pt_on_segment.scad — golden path: p lies strictly inside segment a-b.
//
// Segment: a=[0,0] (anchored), b=[10,0] (anchored). Point p is constrained
// con_pt_on_segment("p","a","b") plus con_distance("a","p", 5).
// p must end at [5, 0]: on the segment, 5 units from a, on the x-axis.
// Neither bound binds (t = 0.5 ∈ (0,1)), so active_inequalities is empty.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("p", at = [3, 2]),                  // off-segment seed; on_line pulls it down
  con_pt_on_segment("p", "a", "b"),
  con_distance("a", "p", 5),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol),
           ", active=", active_inequalities(sol), ")"));

p = pt(sol, "p");
assert(abs(p[0] - 5) < 1e-4, str("expected p.x == 5, got ", p[0]));
assert(abs(p[1]) < 1e-4,     str("expected p.y == 0, got ", p[1]));
assert(len(active_inequalities(sol)) == 0,
       str("expected no active inequalities, got ",
           active_inequalities(sol)));

echo(iter = iterations(sol),
     active = active_inequalities(sol),
     p = pt(sol, "p"));
```

- [ ] **Step 2: Run the test and verify it fails**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_pt_on_segment.scad -o /tmp/out.stl 2>&1
```

Expected: a `WARNING:` line about `con_pt_on_segment` being undefined (or the assertion failing because `p` is at the seed `[3, 2]` not `[5, 0]`). Either failure mode is fine — both confirm the builtin doesn't exist yet.

- [ ] **Step 3: Commit the failing test**

```bash
git add examples/Solver/solve2d_pt_on_segment.scad
git commit -m "$(cat <<'EOF'
solve2d: failing test for con_pt_on_segment golden path

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add the `con_pt_on_segment` factory built-in and registration

**Files:**
- Modify: `src/core/builtin_solve.cc`

**Why:** A factory function that returns an `ObjectType` of `kind = "pt_on_segment"` carrying the three point-name fields. Mirrors `builtin_con_pt_on_line` exactly — three string args, no number. Registration adds it to the builtin table next to the related `con_pt_on_line` entry.

- [ ] **Step 1: Insert the factory function after `builtin_con_pt_on_line`**

Find this block in `src/core/builtin_solve.cc` (around line 308):

```cpp
Value builtin_con_pt_on_line(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_pt_on_line", arguments, loc, 3)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "pt_on_line");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  return obj;
}
```

Immediately *after* it, insert:

```cpp
Value builtin_con_pt_on_segment(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (!require_strings("con_pt_on_segment", arguments, loc, 3)) return Value::undefined.clone();
  ObjectType obj = make_kind_obj(session, "pt_on_segment");
  obj.set("p", arguments[0]->clone());
  obj.set("a", arguments[1]->clone());
  obj.set("b", arguments[2]->clone());
  return obj;
}
```

- [ ] **Step 2: Register the builtin**

Find this block in `register_builtin_solve()` (around line 1745):

```cpp
  Builtins::init("con_pt_on_line", new BuiltinFunction(&builtin_con_pt_on_line),
                 {"con_pt_on_line(p, la, lb) -> sketch constraint"});
```

Immediately *after* it, insert:

```cpp
  Builtins::init("con_pt_on_segment", new BuiltinFunction(&builtin_con_pt_on_segment),
                 {"con_pt_on_segment(p, la, lb) -> sketch constraint (p on closed segment la-lb)"});
```

- [ ] **Step 3: Build to verify the factory compiles**

```bash
cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -20
```

Expected: build succeeds. The test from Task 2 will still fail (the parser doesn't recognize the kind yet), but `con_pt_on_segment` is now a known function name and the warning shifts from "unknown function" to "unknown item kind 'pt_on_segment'".

- [ ] **Step 4: Verify the warning shift**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_pt_on_segment.scad -o /tmp/out.stl 2>&1 \
  | grep -E '(unknown|WARNING)'
```

Expected: a warning like `solve2d: unknown item kind 'pt_on_segment'`. This confirms the factory ran and produced the tagged ObjectType, but the parser hasn't been taught what to do with it.

---

## Task 4: Teach the parser to expand `pt_on_segment` into 1 equality + 2 inequalities

**Files:**
- Modify: `src/core/builtin_solve.cc`

**Why:** The parser branch for `kind == "pt_on_segment"` is the linchpin. It produces one `ConstraintDecl` (the on-line equality) and two `InequalityDecl`s (the start and end bounds), all sharing the same user-visible name prefix.

- [ ] **Step 1: Add the parser branch**

Find this block in `builtin_solve2d` (around line 1451 — the `else if (kind == "le_angle")` branch — search for `"le_angle"`). Immediately *after* the closing `continue;` of that block (and before the `else { LOG(...) "unknown item kind"` block at line 1468), insert:

```cpp
      } else if (kind == "pt_on_segment") {
        std::string p, a, b;
        field_string(obj, "p", p);
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        const std::string prefix = "con_pt_on_segment(" + p + "," + a + "," + b + ")";

        // 1. Always-on equality: p lies on the infinite line through a, b.
        ConstraintDecl on_line;
        on_line.kind = "pt_on_line";
        on_line.points = {p, a, b};
        on_line.name = prefix + ":on_line";
        constraints.push_back(std::move(on_line));

        // 2. Lower bound: g_lower = -dot(p - a, b - a) <= 0  (i.e. t >= 0).
        InequalityDecl lower;
        lower.kind = "pt_on_segment_lower";
        lower.points = {p, a, b};
        lower.name = prefix + ":start";
        inequalities.push_back(std::move(lower));

        // 3. Upper bound: g_upper = dot(p - b, b - a) <= 0  (i.e. t <= 1).
        InequalityDecl upper;
        upper.kind = "pt_on_segment_upper";
        upper.points = {p, a, b};
        upper.name = prefix + ":end";
        inequalities.push_back(std::move(upper));

        continue;  // skip the constraints.push_back(std::move(c)) below
```

Note: this branch must `continue` because it pushes its own decls and must not fall through to `constraints.push_back(std::move(c))` at the end of the loop.

- [ ] **Step 2: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 3: Run the golden test — expect a different failure mode**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_pt_on_segment.scad -o /tmp/out.stl 2>&1
```

Expected behavior change: the "unknown item kind" warning is gone. The test may still fail because the two new inequality kinds are unknown to `inequality_violation` (returns 0, treated as never-violated, never-activated) and to `build_and_solve_once` (silently skipped when active). For the *non-binding* golden path, neither bound should be activated and the on-line equality + con_distance should drive `p` to `[5, 0]`. So the test may *already pass*. If it does, great — Tasks 5 and 6 still need to happen for the binding case to work.

---

## Task 5: Implement `inequality_violation` for the two new kinds

**Files:**
- Modify: `src/core/builtin_solve.cc`

**Why:** The active-set loop needs to know when each bound is violated. Without these cases, `inequality_violation` returns 0 for the new kinds, the loop never activates them, and the binding case from Task 8 will silently let `p` drift outside the segment.

- [ ] **Step 1: Add the two cases to `inequality_violation`**

Find this block (around line 721):

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
  return 0.0;
}
```

Immediately *before* `return 0.0;`, insert:

```cpp
  if ((ineq.kind == "pt_on_segment_lower" || ineq.kind == "pt_on_segment_upper")
      && ineq.points.size() == 3) {
    P p_, a_, b_;
    if (!get(ineq.points[0], p_) || !get(ineq.points[1], a_) ||
        !get(ineq.points[2], b_)) return 0.0;
    P v = sub(b_, a_);
    if (norm(v) < 1e-12) return 0.0;  // degenerate segment
    if (ineq.kind == "pt_on_segment_lower") {
      // g_lower = -dot(p - a, b - a). Positive ⇒ t < 0.
      P pa = sub(p_, a_);
      return -(pa[0]*v[0] + pa[1]*v[1]);
    } else {
      // g_upper = dot(p - b, b - a). Positive ⇒ t > 1.
      P pb = sub(p_, b_);
      return pb[0]*v[0] + pb[1]*v[1];
    }
  }
```

- [ ] **Step 2: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
```

Expected: clean build.

---

## Task 6: Implement active-set enforcement (POINTS_COINCIDENT) and residual mapping

**Files:**
- Modify: `src/core/builtin_solve.cc`

**Why:** When the active-set loop activates a `pt_on_segment_*` bound, it needs to translate that into a SolveSpace constraint. Geometrically, with the on-line equality in force, "lower bound tight" means `p == a` and "upper bound tight" means `p == b`. `SLVS_C_POINTS_COINCIDENT` is the right primitive. The residual-mapping case lets the `REDUNDANT_OKAY → INCONSISTENT` recovery path score the active bound correctly.

- [ ] **Step 1: Add the active-set cases in `build_and_solve_once`**

Find this block (around line 1114):

```cpp
    } else if (ineq.kind == "le_angle" && ineq.points.size() == 4) {
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity c = pt_entity(ineq.kind, ineq.points[2]);
      Slvs_hEntity d = pt_entity(ineq.kind, ineq.points[3]);
      if (!a || !b || !c || !d) continue;
      Slvs_hConstraint ch = next_constraint++;
      Slvs_hEntity l1 = make_line(a, b);
      Slvs_hEntity l2 = make_line(c, d);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_ANGLE,
                                                 wrkpl, ineq.valA, 0, 0, l1, l2));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    }
  }
```

Immediately *before* the closing `}` of the `for (size_t idx : active_set)` loop, insert:

```cpp
    } else if ((ineq.kind == "pt_on_segment_lower" ||
                ineq.kind == "pt_on_segment_upper") && ineq.points.size() == 3) {
      // When this bound is active, p coincides with the corresponding endpoint.
      // Lower bound active ⇒ p == a; upper bound active ⇒ p == b.
      Slvs_hEntity p = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity endpoint = pt_entity(
          ineq.kind,
          ineq.kind == "pt_on_segment_lower" ? ineq.points[1] : ineq.points[2]);
      if (!p || !endpoint) continue;
      Slvs_hConstraint ch = next_constraint++;
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_POINTS_COINCIDENT,
                                                 wrkpl, 0.0, p, endpoint, 0, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    }
```

- [ ] **Step 2: Add the residual-mapping cases**

Find this block (around line 1190):

```cpp
  // Also include active inequalities in residual (they are treated as equalities).
  for (size_t idx : active_set) {
    ConstraintDecl pseudo;
    if (inequalities[idx].kind == "le_distance") pseudo.kind = "distance";
    else if (inequalities[idx].kind == "le_pt_line_distance") pseudo.kind = "pt_line_distance";
    else if (inequalities[idx].kind == "le_length_difference") pseudo.kind = "length_difference";
    else if (inequalities[idx].kind == "le_angle") pseudo.kind = "angle";
    pseudo.points = inequalities[idx].points;
    pseudo.valA = inequalities[idx].valA;
    double r = constraint_residual(pseudo, out.points);
    if (r > max_residual) max_residual = r;
  }
```

Replace the loop body with this version (the new kinds need a *different* `points` layout than the inequality decl — `[p, endpoint]` rather than `[p, a, b]`):

```cpp
  // Also include active inequalities in residual (they are treated as equalities).
  for (size_t idx : active_set) {
    const auto& ineq = inequalities[idx];
    ConstraintDecl pseudo;
    pseudo.valA = ineq.valA;
    if (ineq.kind == "le_distance") {
      pseudo.kind = "distance";
      pseudo.points = ineq.points;
    } else if (ineq.kind == "le_pt_line_distance") {
      pseudo.kind = "pt_line_distance";
      pseudo.points = ineq.points;
    } else if (ineq.kind == "le_length_difference") {
      pseudo.kind = "length_difference";
      pseudo.points = ineq.points;
    } else if (ineq.kind == "le_angle") {
      pseudo.kind = "angle";
      pseudo.points = ineq.points;
    } else if (ineq.kind == "pt_on_segment_lower" && ineq.points.size() == 3) {
      // Active ⇒ p coincides with a. Pseudo: coincident{p, a}.
      pseudo.kind = "coincident";
      pseudo.points = {ineq.points[0], ineq.points[1]};
    } else if (ineq.kind == "pt_on_segment_upper" && ineq.points.size() == 3) {
      // Active ⇒ p coincides with b. Pseudo: coincident{p, b}.
      pseudo.kind = "coincident";
      pseudo.points = {ineq.points[0], ineq.points[2]};
    } else {
      continue;  // unknown active kind — skip residual contribution
    }
    double r = constraint_residual(pseudo, out.points);
    if (r > max_residual) max_residual = r;
  }
```

- [ ] **Step 3: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 4: Run the golden test — expect it to pass**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_pt_on_segment.scad -o /tmp/out.stl 2>&1 \
  | grep -E '^(ECHO|Warning|Error)'
```

Expected:
- An `ECHO:` line containing `iter = ..., active = [], p = [5, 0]` (or values within 1e-4 of `[5, 0]`).
- **No** `Warning:` lines about assertions.

If the test fails, debug before continuing.

- [ ] **Step 5: Commit the implementation**

```bash
git add src/core/builtin_solve.cc
git commit -m "$(cat <<'EOF'
solve2d: implement con_pt_on_segment via composite expansion

con_pt_on_segment(p, a, b) is a new sketch constraint that asserts
p lies on the closed segment a-b. The parser expands it into one
pt_on_line equality plus two pt_on_segment_lower / pt_on_segment_upper
inequalities. When a bound activates, the active-set loop emits
SLVS_C_POINTS_COINCIDENT(p, endpoint), which is geometrically
equivalent given the on-line equality. Reporting names share a
con_pt_on_segment(p,a,b) prefix with :on_line / :start / :end suffixes.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Regression check against baseline

**Files:** none modified.

**Why:** Confirm no existing example regressed.

- [ ] **Step 1: Re-run all existing solver examples**

```bash
cd /Users/samw3/prj/stocko/openscad
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/out.stl 2>&1 \
    | grep -E '^(ECHO|Warning|Error)'
done > /tmp/pt_on_segment_after.txt
diff /tmp/pt_on_segment_baseline.txt /tmp/pt_on_segment_after.txt
```

Expected: the diff shows **only** the new `solve2d_pt_on_segment.scad` block (which the baseline did not contain — it predates the file's creation in Task 2; if the baseline was captured *after* Task 2, the new file's output should match in both runs and the diff is empty). All other examples must produce identical output.

If any pre-existing example shows a different `ECHO` value or a new `Warning:`, debug before continuing.

---

## Task 8: Add the binding-case test

**Files:**
- Create: `examples/Solver/solve2d_pt_on_segment_binding.scad`

**Why:** Exercises the active-set machinery: `p`'s seed is past the end of the segment, so the upper bound must activate and the solver must pin `p` to `b`.

- [ ] **Step 1: Write the test file**

Create `examples/Solver/solve2d_pt_on_segment_binding.scad` with this content:

```openscad
// solve2d_pt_on_segment_binding.scad — upper bound activates.
//
// Segment a=[0,0] (anchored), b=[10,0] (anchored). Point p is seeded at
// [15, 0], which is on the line through a-b but past b (t = 1.5).
// con_pt_on_segment forces p back onto the closed segment. With no other
// along-line constraint, the upper bound activates and p is pinned to b.
// Expected:
//   solved == true
//   active_inequalities == ["con_pt_on_segment(p,a,b):end"]
//   pt(p) == [10, 0]
//   iterations >= 2 (one feasibility-check pass + one with the bound active)

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("p", at = [15, 0]),                 // seed past b
  con_pt_on_segment("p", "a", "b"),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol),
           ", active=", active_inequalities(sol), ")"));

p = pt(sol, "p");
assert(abs(p[0] - 10) < 1e-4, str("expected p.x == 10, got ", p[0]));
assert(abs(p[1])      < 1e-4, str("expected p.y == 0, got ", p[1]));

active = active_inequalities(sol);
assert(len(active) == 1,
       str("expected 1 active inequality, got ", active));
assert(active[0] == "con_pt_on_segment(p,a,b):end",
       str("expected :end bound active, got ", active[0]));

echo(iter = iterations(sol), active = active, p = p);
```

- [ ] **Step 2: Run the test**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_pt_on_segment_binding.scad -o /tmp/out.stl 2>&1 \
  | grep -E '^(ECHO|Warning|Error)'
```

Expected:
- An `ECHO:` line: `iter = <n>, active = ["con_pt_on_segment(p,a,b):end"], p = [10, 0]`.
- No `Warning:` lines about assertions.

If the assertion on `active[0]` fails, check the name format generated in Task 4's parser branch — the prefix string concatenation must match exactly.

- [ ] **Step 3: Commit**

```bash
git add examples/Solver/solve2d_pt_on_segment_binding.scad
git commit -m "$(cat <<'EOF'
solve2d: add binding-case example for con_pt_on_segment

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Add the infeasible-case test

**Files:**
- Create: `examples/Solver/solve2d_pt_on_segment_infeasible.scad`

**Why:** Exercises failure reporting: the segment can't accommodate `p` while satisfying the other constraints, and the solver should report failure cleanly with one of the segment-related names in `failed_constraints`.

- [ ] **Step 1: Write the test file**

Create `examples/Solver/solve2d_pt_on_segment_infeasible.scad` with this content:

```openscad
// solve2d_pt_on_segment_infeasible.scad — contradictory constraints.
//
// Segment a=[0,0] (anchored), b=[10,0] (anchored). p must be on the segment
// AND 100 units from a. The segment is only 10 units long, so no feasible
// position exists. The solver should report failure; we don't pin down
// the exact failure mode beyond requiring solved == false.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("p", at = [5, 0]),
  con_pt_on_segment("p", "a", "b"),
  con_distance("a", "p", 100),               // demands |ap| = 100, segment is only 10
]);

assert(!solved(sol),
       str("expected unsolved, but solver claimed success: p=", pt(sol, "p"),
           " active=", active_inequalities(sol)));

// Failure should surface in failed_constraints — exact contents depend on
// which constraint SolveSpace pins the blame on. Just require non-empty.
failed = failed_constraints(sol);
assert(len(failed) > 0,
       str("expected at least one failed constraint, got empty list"));

echo(solved = solved(sol),
     iter = iterations(sol),
     active = active_inequalities(sol),
     failed = failed);
```

- [ ] **Step 2: Run the test**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_pt_on_segment_infeasible.scad -o /tmp/out.stl 2>&1 \
  | grep -E '^(ECHO|Warning|Error)'
```

Expected:
- `ECHO:` line shows `solved = false` and a non-empty `failed` vector. The exact contents may include any of `con_distance(...)`, `con_pt_on_segment(p,a,b):on_line`, `con_pt_on_segment(p,a,b):start`, or `con_pt_on_segment(p,a,b):end` — the test only requires *some* failure is reported.
- No assertion failures.

If `solved(sol)` is unexpectedly `true`, the active-set loop is silently dropping the conflicting bound rather than reporting infeasibility. Investigate the loop's drop logic in `solve_with_inequalities` before relaxing the test.

- [ ] **Step 3: Commit**

```bash
git add examples/Solver/solve2d_pt_on_segment_infeasible.scad
git commit -m "$(cat <<'EOF'
solve2d: add infeasible-case example for con_pt_on_segment

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Update spec doc (`doc/specs/solve2d.md`)

**Files:**
- Modify: `doc/specs/solve2d.md`

**Why:** The constraint table in the spec must list the new constraint, and the inequality-semantics section should mention the composite-expansion pattern.

- [ ] **Step 1: Add the new constraint to the equality constraint table**

Find this row in the equality constraint table (around line 183):

```markdown
| `con_pt_on_line(p, la, lb)` | 3 strings | Point `p` shall lie on the infinite line through la–lb. |
```

Immediately *after* it, insert:

```markdown
| `con_pt_on_segment(p, la, lb)` | 3 strings | Point `p` shall lie on the closed line segment from la to lb (endpoints included). Composite: see "Composite constraints" below. |
```

- [ ] **Step 2: Add a "Composite constraints" subsection at the end of the Inequalities section**

Find the end of the `### Inequalities` section (just before `### Built-in name shadowing`, around line 215). Immediately *before* the `### Built-in name shadowing` heading, insert:

```markdown
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
  equality). The `:on_line` suffix never appears in
  `active_inequalities(sol)`; the `:start` and `:end` suffixes never
  appear there unless the corresponding bound is in the active set.
  Any of the three suffixed names may appear in `failed_constraints(sol)`
  if SolveSpace flags the underlying piece.
```

- [ ] **Step 3: Verify the file still parses and reads cleanly**

```bash
grep -nE '^(### |\| `con_pt_on_segment)' doc/specs/solve2d.md | head -10
```

Expected: shows the new constraint row and the new "Composite constraints" heading in the right context.

- [ ] **Step 4: Commit**

```bash
git add doc/specs/solve2d.md
git commit -m "$(cat <<'EOF'
solve2d: spec for con_pt_on_segment and composite constraints

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Update user guide (`doc/solve2d_guide.md`)

**Files:**
- Modify: `doc/solve2d_guide.md`

**Why:** The constraint reference table in the guide must list the new constraint so users discover it.

- [ ] **Step 1: Add the new constraint to the reference table**

Find this row in the constraint reference table (around line 133):

```markdown
| `con_pt_on_line(p, la, lb)` | `p` lies on the line through la–lb. |
```

Immediately *after* it, insert:

```markdown
| `con_pt_on_segment(p, la, lb)` | `p` lies on the closed segment la–lb (endpoints included). |
```

- [ ] **Step 2: Verify**

```bash
grep -n 'con_pt_on_segment' doc/solve2d_guide.md
```

Expected: one match, in the constraint-table row added above.

- [ ] **Step 3: Commit**

```bash
git add doc/solve2d_guide.md
git commit -m "$(cat <<'EOF'
solve2d: document con_pt_on_segment in the user guide

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Final verification

**Files:** none modified.

**Why:** End-to-end check that the entire family of examples (old + new) all pass.

- [ ] **Step 1: Re-run every solver example**

```bash
cd /Users/samw3/prj/stocko/openscad
for f in examples/Solver/solve2d_*.scad; do
  printf '=== %s ===\n' "$f"
  out=$(build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/out.stl 2>&1)
  echo "$out" | grep -E '^(ECHO|WARNING|Warning|ERROR|Error)'
done
```

Expected: every example block ends with the `ECHO:` lines its own assertions print, and **no** `WARNING:` / `Warning:` lines appear about assertions or unknown names. Fixture-level warnings about unrelated subjects (e.g. `Could not find...` from rendering) are acceptable.

- [ ] **Step 2: Confirm git history is clean**

```bash
git log --oneline experiment/solve2d-inequalities ^master | head -20
```

Expected: a sequence of commits matching the task structure: failing test, factory + registration, parser + violation + active-set + residual (one commit), regression check (no commit — verification only), binding test, infeasible test, spec, guide.

- [ ] **Step 3: Confirm the working tree is clean**

```bash
git status --short
```

Expected: empty output (all changes committed). The untracked `.claude/` directory is fine — that's session state.
