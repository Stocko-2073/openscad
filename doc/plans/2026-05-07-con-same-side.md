# `con_same_side` / `con_opposite_side` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two new sketch constraints, `con_same_side(a, b, p, q)` and `con_opposite_side(a, b, p, q)`, that pin point `p` to one half-plane of the line through `a` and `b`, defined relative to a reference point `q`. The constraints are inequalities in the existing SQP active-set machinery.

**Architecture:** Each user-facing builtin produces an `ObjectType` of `kind = "same_side"` or `"opposite_side"`. The `solve2d` parser recognises both kinds and emits a single `InequalityDecl` (no equality piece — the "boundary" is just the line `ab`, and `pt_on_line` is the binding form). Inequality violation is `−c(p)·c(q)` for `same_side` and `c(p)·c(q)` for `opposite_side`, where `c(x)` is the signed cross product `(b−a) × (x−a)`. When the inequality activates, `build_and_solve_once()` emits `SLVS_C_PT_ON_LINE(p, line(a,b))`. Asymmetric binding: `p` is the third argument and is the only one that gets pinned; `q` is a sign reference. Residual recovery maps the active inequality to a pseudo `pt_on_line` constraint.

**Tech Stack:** C++17, CMake, libslvs (vendored at `submodules/SolveSpaceLib/libslvs/`). No new test framework — verification is via SCAD example files run through the CLI binary.

**Spec:** `doc/specs/2026-05-07-con-same-side-design.md`

**Branch:** `experiment/solve2d-inequalities` (already current branch).

**Verification convention used throughout this plan:**

```bash
# Build (parallel, all cores)
cmake --build build -j$(sysctl -n hw.ncpu)

# Run a SCAD test
build/OpenSCAD.app/Contents/MacOS/OpenSCAD <path/to/test.scad> -o /tmp/out.stl 2>&1
```

A failing `assert(...)` aborts rendering with a `Warning:` message in stderr; passing tests render geometry and echo their results. "Test passes" = exit code 0 + no `Warning:` lines about an assertion + expected `ECHO` lines present.

**File map:**
- Modify: `src/core/builtin_solve.cc` — adds two factory functions, two parser dispatch cases, two violation cases, two active-set cases, two residual-recovery cases, two registrations.
- Create: `examples/Solver/solve2d_same_side.scad` — basic smoke test for `con_same_side`.
- Create: `examples/Solver/solve2d_opposite_side.scad` — golden disambiguation test (the square).
- Modify: `doc/solve2d_guide.md` — document the new constraints in the inequalities table.

---

## Task 1: Capture pre-change baseline (regression oracle)

**Files:** none modified.

**Why:** Existing `solve2d_*.scad` examples must continue to pass after the changes. Capture their current output so we can compare in Task 10.

- [ ] **Step 1: Run all existing solver examples and save output**

```bash
cd /Users/samw3/prj/stocko/openscad
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/baseline.stl 2>&1 \
    | grep -E '^(ECHO|Warning|Error)'
done > /tmp/same_side_baseline.txt
cat /tmp/same_side_baseline.txt
```

Expected: each `=== ... ===` block lists `ECHO:` lines from the assertions in that example, and contains **no** `Warning:` lines that mention "Assertion" failures. Save `/tmp/same_side_baseline.txt` for comparison in Task 10.

---

## Task 2: Write the failing `con_same_side` smoke test

**Files:**
- Create: `examples/Solver/solve2d_same_side.scad`

**Why:** Test-first. The test exercises the *non-binding* behavior: line `ab` is the x-axis, reference `q` is above the x-axis, and `p` is constrained to the same side as `q`. Without `con_same_side`, the radius-5 circle around `a` admits two solutions; with it, only the upper one is valid. Until the new builtin and parser branch exist, OpenSCAD will warn "Ignoring unknown function `con_same_side`" and the assertion on `p.y > 0` may fail (depending on which seed wins multi-start).

- [ ] **Step 1: Create the test file**

Write the file `examples/Solver/solve2d_same_side.scad` with this exact content:

```openscad
// solve2d_same_side.scad — basic smoke test for con_same_side.
//
// Line ab is the x-axis (a at origin, b at [10,0], both anchored).
// Reference point q at [3, 5], above the x-axis.
// Free point p constrained to lie on the same side of ab as q,
// plus con_distance(a, p, 5) — so p lands on a radius-5 circle
// around a, on the upper side (p.y > 0).

sol = solve2d([
  point("a", at = [0, 0]),  con_fixed("a"),
  point("b", at = [10, 0]), con_fixed("b"),
  point("q", at = [3, 5]),  con_fixed("q"),
  point("p"),                                // unseeded
  con_distance("a", "p", 5),
  con_same_side("a", "b", "p", "q"),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol),
           ", active=", active_inequalities(sol), ")"));

p = pt(sol, "p");
assert(p[1] > 0,
       str("expected p above x-axis (same side as q), got ", p));
assert(abs(norm(p) - 5) < 1e-4,
       str("expected |ap| == 5, got ", norm(p)));
assert(len(active_inequalities(sol)) == 0,
       str("expected no active inequalities, got ",
           active_inequalities(sol)));

echo(p = p, active = active_inequalities(sol), iter = iterations(sol));
```

- [ ] **Step 2: Run the test and verify it fails**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_same_side.scad -o /tmp/out.stl 2>&1
```

Expected: a `Warning: Ignoring unknown function 'con_same_side'` line, and depending on which basin multi-start happens to converge to, possibly an `Assertion 'p[1] > 0' failed`. Either way, this is RED.

- [ ] **Step 3: Commit the test (still RED)**

```bash
git add examples/Solver/solve2d_same_side.scad
git commit -m "solve2d: failing smoke test for con_same_side"
```

---

## Task 3: Add factory builtins and registration

**Files:**
- Modify: `src/core/builtin_solve.cc`

**Why:** The factory builtins are pure value constructors — they emit an `ObjectType` and don't touch the solver. Registration wires them up so OpenSCAD recognises the new functions. After this task, calling `con_same_side(...)` no longer warns "unknown function," but the test still fails at the parser stage (kind not recognised in `builtin_solve2d`).

- [ ] **Step 1: Add the two factory builtins**

Locate `Value builtin_con_perpendicular(...)` near `src/core/builtin_solve.cc:194`. Just before that function, add the following two factories (the "Constraint factories" comment block is at line 99 of the file):

```cpp
Value builtin_con_same_side(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 4 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_same_side() expects four point names: (a, b, p, q)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "same_side");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("p", arguments[2]->clone());
  obj.set("q", arguments[3]->clone());
  return obj;
}

Value builtin_con_opposite_side(Arguments arguments, const Location& loc)
{
  EvaluationSession *session = arguments.session();
  if (arguments.size() != 4 ||
      arguments[0]->type() != Value::Type::STRING ||
      arguments[1]->type() != Value::Type::STRING ||
      arguments[2]->type() != Value::Type::STRING ||
      arguments[3]->type() != Value::Type::STRING) {
    LOG(message_group::Warning, loc, arguments.documentRoot(),
        "con_opposite_side() expects four point names: (a, b, p, q)");
    return Value::undefined.clone();
  }
  ObjectType obj = make_kind_obj(session, "opposite_side");
  obj.set("a", arguments[0]->clone());
  obj.set("b", arguments[1]->clone());
  obj.set("p", arguments[2]->clone());
  obj.set("q", arguments[3]->clone());
  return obj;
}
```

- [ ] **Step 2: Register the new builtins**

In `register_builtin_solve()` (around `src/core/builtin_solve.cc:1873`), find the `con_pt_on_segment` registration line (approximately line 1909). Just after it, add:

```cpp
  Builtins::init("con_same_side", new BuiltinFunction(&builtin_con_same_side),
                 {"con_same_side(a, b, p, q) -> sketch constraint (p on same side of line ab as q)"});
  Builtins::init("con_opposite_side", new BuiltinFunction(&builtin_con_opposite_side),
                 {"con_opposite_side(a, b, p, q) -> sketch constraint (p on opposite side of line ab from q)"});
```

- [ ] **Step 3: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build. The new factory functions are reachable from OpenSCAD code, but the parser doesn't understand the kinds yet — calling them produces a kind-unknown warning at solve time.

- [ ] **Step 4: Sanity-check the test progresses to a parser warning**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_same_side.scad -o /tmp/out.stl 2>&1 \
  | grep -i "unknown\|assertion"
```

Expected: a single line resembling `Warning: solve2d: unknown item kind 'same_side'`. The `Ignoring unknown function` warning from Task 2 is gone.

- [ ] **Step 5: Commit**

```bash
git add src/core/builtin_solve.cc
git commit -m "solve2d: add con_same_side and con_opposite_side factory builtins"
```

---

## Task 4: Parser dispatch — recognise `same_side` / `opposite_side` kinds

**Files:**
- Modify: `src/core/builtin_solve.cc`

**Why:** The solve2d parser walks each item in the `items` vector and dispatches on `kind`. Until it knows about the new kinds it emits the "unknown item kind" warning. After this task, both kinds are translated into `InequalityDecl` items with the right `points` order and a human-readable name.

- [ ] **Step 1: Add the two parser cases**

Find the `else if (kind == "pt_on_segment")` block in `builtin_solve2d` (around `src/core/builtin_solve.cc:1573`). Just before that block, add the following two cases. They must come **before** the `pt_on_segment` case because the dispatch chain is a long `else if` ladder; new kinds slot in alphabetically-adjacent or by-related-feature, but order doesn't affect correctness as long as each kind is unique.

```cpp
      } else if (kind == "same_side" || kind == "opposite_side") {
        InequalityDecl ineq;
        ineq.kind = kind;
        std::string a, b, p, q;
        field_string(obj, "a", a);
        field_string(obj, "b", b);
        field_string(obj, "p", p);
        field_string(obj, "q", q);
        ineq.points.push_back(a);
        ineq.points.push_back(b);
        ineq.points.push_back(p);
        ineq.points.push_back(q);
        ineq.name = (kind == "same_side" ? "con_same_side(" : "con_opposite_side(") +
                    a + "," + b + "," + p + "," + q + ")";
        inequalities.push_back(std::move(ineq));
        continue;
```

- [ ] **Step 2: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build.

- [ ] **Step 3: Run the test — should now reach the solver but report violation = 0 always**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_same_side.scad -o /tmp/out.stl 2>&1
```

Expected: no "unknown kind" warning anymore. Whether the assertion `p[1] > 0` passes is luck-of-the-seed: `inequality_violation` returns `0.0` for unknown kinds (default fall-through at `src/core/builtin_solve.cc:790`), so the half-plane check is silently skipped. The test is still effectively RED — the solve runs but the constraint is dead. Tasks 5 and 6 add the real semantics.

- [ ] **Step 4: Commit**

```bash
git add src/core/builtin_solve.cc
git commit -m "solve2d: parser dispatch for same_side/opposite_side kinds"
```

---

## Task 5: Implement `inequality_violation` for both kinds

**Files:**
- Modify: `src/core/builtin_solve.cc`

**Why:** `inequality_violation` is what `solve_with_inequalities` consults after each inner solve to decide whether the bound is violated and should join the active set. After this task the violation is correctly signed: `same_side` reports a positive violation when `p` and `q` ended up on opposite sides; `opposite_side` reports a positive violation when they ended up on the same side.

- [ ] **Step 1: Add the violation case**

In `inequality_violation` (around `src/core/builtin_solve.cc:727`), find the existing `pt_on_segment_lower / pt_on_segment_upper` block (around line 773). Immediately after that block (just before the `return 0.0;` fall-through at line 790), add:

```cpp
  if ((ineq.kind == "same_side" || ineq.kind == "opposite_side")
      && ineq.points.size() == 4) {
    P a_, b_, p_, q_;
    if (!get(ineq.points[0], a_) || !get(ineq.points[1], b_) ||
        !get(ineq.points[2], p_) || !get(ineq.points[3], q_)) return 0.0;
    // Signed cross product (b-a) × (x-a). Positive = left of directed
    // line a→b, negative = right, zero = on the line.
    auto cross_z = [&](const P& x) {
      return (b_[0]-a_[0])*(x[1]-a_[1]) - (b_[1]-a_[1])*(x[0]-a_[0]);
    };
    double cp = cross_z(p_);
    double cq = cross_z(q_);
    // same_side violated when signs differ: cp*cq < 0 ⇒ violation = -cp*cq > 0.
    // opposite_side violated when signs agree: cp*cq > 0 ⇒ violation =  cp*cq > 0.
    return (ineq.kind == "same_side") ? -(cp * cq) : (cp * cq);
  }
```

- [ ] **Step 2: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build.

- [ ] **Step 3: Run the test — should now pass when initial seed lands on the wrong side**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_same_side.scad -o /tmp/out.stl 2>&1
```

What happens now:
- If multi-start attempt 0 happens to seed `p` above the x-axis, the violation is 0 and the test passes immediately (constraint slack).
- If attempt 0 seeds `p` below the x-axis, the violation is positive → the active-set loop adds the inequality to the active set and re-solves. **But task 5 hasn't taught `build_and_solve_once` how to enforce the active form yet** — it falls through with no SolveSpace constraint added. The "active" inequality contributes nothing, the violation remains, and the active-set loop hits the cycle/max-iter cap and reports `solved=false`.

So the assertion `solved(sol)` may pass or fail depending on which basin attempt 0 happens to land in. Don't commit yet; finish the active-set wiring in Task 6 first so the result is deterministic.

---

## Task 6: Active-set enforcement + residual recovery mapping

**Files:**
- Modify: `src/core/builtin_solve.cc`

**Why:** When the active-set loop adds the inequality to the active set, `build_and_solve_once` must translate that activation into a SolveSpace constraint (`SLVS_C_PT_ON_LINE` pinning `p` to line `ab`). The residual-recovery path also needs to know how to score an active half-plane bound for the `REDUNDANT_OKAY → INCONSISTENT` recovery.

- [ ] **Step 1: Add the active-set switch case**

In `build_and_solve_once` (around `src/core/builtin_solve.cc:789`), find the inequality dispatch block (around line 1148). Locate the `pt_on_segment_lower / pt_on_segment_upper` case (around line 1198) and add the new case right after its closing brace, before the `}` that ends the for-loop over inequalities (around line 1213):

```cpp
    } else if ((ineq.kind == "same_side" || ineq.kind == "opposite_side")
               && ineq.points.size() == 4) {
      // Active ⇒ pin p to the line through a and b. Both kinds share this
      // binding form: the dividing line between the two half-planes is the
      // same line ab. q is a sign reference only and never gets pinned.
      Slvs_hEntity a = pt_entity(ineq.kind, ineq.points[0]);
      Slvs_hEntity b = pt_entity(ineq.kind, ineq.points[1]);
      Slvs_hEntity p = pt_entity(ineq.kind, ineq.points[2]);
      if (!a || !b || !p) continue;
      Slvs_hConstraint ch = next_constraint++;
      Slvs_hEntity line = make_line(a, b);
      sconstraints.push_back(Slvs_MakeConstraint(ch, g_solve,
                                                 SLVS_C_PT_ON_LINE,
                                                 wrkpl, 0.0, p, 0, line, 0));
      constraint_name_by_h[ch] = ineq.name;
      out.active_ineq_handles[idx] = ch;
    }
```

- [ ] **Step 2: Add the residual-recovery mapping**

In `build_and_solve_once`'s residual loop (around `src/core/builtin_solve.cc:1275`), the existing chain ends with the `pt_on_segment_upper` case followed by a catch-all `else { continue; }`. Insert a new `else if` for the half-plane kinds **between** them. Concretely: find this block in the source:

```cpp
    } else if (ineq.kind == "pt_on_segment_upper" && ineq.points.size() == 3) {
      // Active ⇒ p coincides with b. Pseudo: coincident{p, b}.
      pseudo.kind = "coincident";
      pseudo.points = {ineq.points[0], ineq.points[2]};
    } else {
      continue;
    }
```

and replace it with:

```cpp
    } else if (ineq.kind == "pt_on_segment_upper" && ineq.points.size() == 3) {
      // Active ⇒ p coincides with b. Pseudo: coincident{p, b}.
      pseudo.kind = "coincident";
      pseudo.points = {ineq.points[0], ineq.points[2]};
    } else if ((ineq.kind == "same_side" || ineq.kind == "opposite_side")
               && ineq.points.size() == 4) {
      // Active ⇒ p lies on line ab. Pseudo: pt_on_line{p, a, b}.
      pseudo.kind = "pt_on_line";
      pseudo.points = {ineq.points[2], ineq.points[0], ineq.points[1]};
    } else {
      continue;
    }
```

- [ ] **Step 3: Build**

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

Expected: clean build.

- [ ] **Step 4: Run the smoke test — should now pass deterministically**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_same_side.scad -o /tmp/out.stl 2>&1
```

Expected: PASS (no Warning lines about assertions). The deterministic golden-angle seed for the first unseeded point is approximately `(-0.74, 0.67)` (above the x-axis, same side as `q`), so the violation is zero from the start, the constraint stays slack, and Newton converges to `p` on the upper half of the radius-5 circle around `a`. Output should look like:

```
ECHO: p = [-3.7..., 3.3...], active = [], iter = 1
```

(Sign and magnitude of the components depend on the exact seed and the solver's path; the assertion only requires `p[1] > 0` and `|ap| ≈ 5`.)

**A note on what could go wrong but doesn't here:** if `p` had been seeded below the x-axis or unlucky in some way, iter 1 would solve below the line, the violation check would activate the constraint, iter 2 would pin `p` to the x-axis (`p[1] = 0`), and the assertion `p[1] > 0` would fail. The test relies on the deterministic golden-angle default landing in the slack basin. If this assumption ever changes (different seed scheme, different multi-start logic), revisit by either seeding `p` above the line explicitly (e.g. `point("p", at = [3, 1])`) or rephrasing the assertion as `p[1] >= 0`.

- [ ] **Step 5: Commit**

```bash
git add src/core/builtin_solve.cc
git commit -m "solve2d: violation, active-set, and residual recovery for same_side/opposite_side"
```

---

## Task 7: Run all examples to confirm same_side smoke test passes alongside existing examples

**Files:** none modified.

- [ ] **Step 1: Re-run all solver examples**

```bash
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/out.stl 2>&1 \
    | grep -E '^(ECHO|Warning|Error)'
done
```

Expected: every block (including the new `solve2d_same_side`) shows `ECHO:` lines and no `Warning:` lines about assertions. Existing examples are unchanged.

---

## Task 8: Write the failing `con_opposite_side` disambiguation test (the square)

**Files:**
- Create: `examples/Solver/solve2d_opposite_side.scad`

**Why:** The motivating use case for half-plane constraints. The user-discovered failing case ("square with only one corner seeded") has two basins after the multi-start fix from commit `b0fa5a542`: a real square and a degenerate `b == d` configuration. `con_opposite_side("a","c","d","b")` selects the real-square basin.

- [ ] **Step 1: Create the test file**

Write `examples/Solver/solve2d_opposite_side.scad` with this exact content:

```openscad
// solve2d_opposite_side.scad — disambiguation example for con_opposite_side.
//
// Anchor a at the origin. Equal sides + one right angle at b determines
// a square *up to* the discrete choice of which side d is on. Without a
// half-plane constraint, the system has two valid solutions: a real
// square, and a degenerate configuration where d coincides with b
// (both points satisfy |ad| = |cd| = 20 with |ac| = 20*sqrt(2)).
//
// con_opposite_side("a","c","d","b") asserts that d is on the opposite
// side of line ac from b — exactly the topology of a real square's
// crossed diagonals — and selects the real-square basin.

sol = solve2d([
  point("a", at = [0, 0]), con_fixed("a"),
  point("b"),
  point("c"),
  point("d"),
  con_distance("a", "b", 20),
  con_equal_length("a", "b", "b", "c"),
  con_equal_length("a", "b", "c", "d"),
  con_equal_length("a", "b", "a", "d"),
  con_perpendicular("a", "b", "c"),
  con_opposite_side("a", "c", "d", "b"),
]);

assert(solved(sol),
       str("solver failed: ", failed_constraints(sol),
           " (iters=", iterations(sol),
           ", active=", active_inequalities(sol), ")"));

a = pt(sol, "a"); b = pt(sol, "b"); c = pt(sol, "c"); d = pt(sol, "d");
function dist(p, q) = norm([p[0]-q[0], p[1]-q[1]]);

// All four sides == 20 (the equality constraints).
assert(abs(dist(a, b) - 20) < 1e-4, str("|ab|=", dist(a, b)));
assert(abs(dist(b, c) - 20) < 1e-4, str("|bc|=", dist(b, c)));
assert(abs(dist(c, d) - 20) < 1e-4, str("|cd|=", dist(c, d)));
assert(abs(dist(d, a) - 20) < 1e-4, str("|da|=", dist(d, a)));

// The crucial assertion: |bd| ≈ 20*sqrt(2). For the degenerate b == d
// solution, |bd| ≈ 0; for the real square, |bd| is the diagonal.
assert(abs(dist(b, d) - 20*sqrt(2)) < 1e-4,
       str("expected real-square diagonal |bd| ≈ ", 20*sqrt(2),
           ", got ", dist(b, d), " (degenerate b==d basin?)"));

// The half-plane constraint is satisfied with slack at the real-square
// solution, so it should not be in the active set.
assert(len(active_inequalities(sol)) == 0,
       str("expected no active inequalities, got ",
           active_inequalities(sol)));

echo(diag_bd = dist(b, d), iter = iterations(sol),
     active = active_inequalities(sol));
```

- [ ] **Step 2: Run the test and verify it passes**

```bash
build/OpenSCAD.app/Contents/MacOS/OpenSCAD \
  examples/Solver/solve2d_opposite_side.scad -o /tmp/out.stl 2>&1
```

Expected: PASS (no Warning lines about assertions). `diag_bd ≈ 28.2843` (i.e. `20*sqrt(2)`), `active = []`, `iter` is 1 or 2.

If the test fails, the most likely causes are:
- The active-set machinery is binding the constraint instead of leaving it slack — check the `active_inequalities` output. The fix is in Task 6's logic.
- Multi-start landed in the b==d basin but the half-plane constraint should have rejected it — re-check the violation function in Task 5.
- The signed cross product convention disagrees with the example geometry — check the `cross_z` definition in Task 5.

- [ ] **Step 3: Commit the test**

```bash
git add examples/Solver/solve2d_opposite_side.scad
git commit -m "solve2d: con_opposite_side example disambiguating the square"
```

---

## Task 9: Regression check against baseline

**Files:** none modified.

- [ ] **Step 1: Re-run all examples and capture output**

```bash
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/post.stl 2>&1 \
    | grep -E '^(ECHO|Warning|Error)'
done > /tmp/same_side_post.txt
```

- [ ] **Step 2: Compare to baseline**

```bash
# Baseline doesn't include the two new files; filter them out of the post run.
grep -v -E "solve2d_(same|opposite)_side" /tmp/same_side_post.txt > /tmp/same_side_post_filtered.txt
diff /tmp/same_side_baseline.txt /tmp/same_side_post_filtered.txt
```

Expected: empty diff. If anything differs, investigate before continuing — the existing examples must produce the same output as before.

---

## Task 10: Update the user guide

**Files:**
- Modify: `doc/solve2d_guide.md`

**Why:** The guide documents what constraints are available. The new constraints belong in the inequalities table, with a short note explaining the disambiguation use case.

- [ ] **Step 1: Add rows to the inequalities table**

Find the inequalities table at `doc/solve2d_guide.md:158-163`. The current table looks like:

```markdown
| Constraint | Bound |
|---|---|
| `con_le_distance(p1, p2, d)` | `\|p1–p2\| <= d` |
| `con_le_pt_line_distance(p, la, lb, d)` | signed distance from `p` to la–lb is `<= d` |
| `con_le_length_difference(a, b, c, d, diff)` | `\|a–b\| - \|c–d\| <= diff` |
| `con_le_angle(p1, p2, p3, p4, deg)` | undirected angle (in `[0, 180]`) `<= deg` |
```

Add two rows so the table reads:

```markdown
| Constraint | Bound |
|---|---|
| `con_le_distance(p1, p2, d)` | `\|p1–p2\| <= d` |
| `con_le_pt_line_distance(p, la, lb, d)` | signed distance from `p` to la–lb is `<= d` |
| `con_le_length_difference(a, b, c, d, diff)` | `\|a–b\| - \|c–d\| <= diff` |
| `con_le_angle(p1, p2, p3, p4, deg)` | undirected angle (in `[0, 180]`) `<= deg` |
| `con_same_side(a, b, p, q)` | `p` is on the same side of line ab as `q` |
| `con_opposite_side(a, b, p, q)` | `p` is on the opposite side of line ab from `q` |
```

- [ ] **Step 2: Add an explanatory paragraph after the caveats list**

The caveats list ends near `doc/solve2d_guide.md:204` with the `con_le_angle` note. Immediately after that bullet (still within the bullet list, or as a fresh paragraph below it — pick whichever matches the surrounding markdown style), add this paragraph:

```markdown
* `con_same_side` and `con_opposite_side` are **half-plane** constraints
  used to break the discrete multiplicity that distance/angle systems
  leave behind. A square anchored at one corner with three side-equality
  constraints and one right angle has both a real-square solution and a
  degenerate one with two coincident vertices; adding
  `con_opposite_side("a","c","d","b")` selects the real-square basin.
  These constraints bind asymmetrically: when active, only `p` (the
  third argument) is pinned to the line through `a` and `b`. `q` is a
  sign reference only. Swap the argument order (`con_same_side(a,b,q,p)`)
  if you want `q` to be the pinned one instead.
```

- [ ] **Step 3: Verify the guide renders**

```bash
# Markdown sanity-check: no broken tables or unmatched fences.
grep -c "^| \`con_" doc/solve2d_guide.md
```

Expected: count went up by exactly 2 from the previous count.

- [ ] **Step 4: Commit**

```bash
git add doc/solve2d_guide.md
git commit -m "solve2d: document con_same_side and con_opposite_side in user guide"
```

---

## Task 11: Final verification

**Files:** none modified.

- [ ] **Step 1: Re-run every solver example**

```bash
for f in examples/Solver/solve2d_*.scad; do
  echo "=== $f ==="
  build/OpenSCAD.app/Contents/MacOS/OpenSCAD "$f" -o /tmp/out.stl 2>&1 \
    | grep -E '^(ECHO|Warning|Error)'
done
```

Expected: every example, including both new ones, shows `ECHO:` lines and no `Warning:` lines about assertion failures.

- [ ] **Step 2: Verify the originally-failing square case now succeeds with `con_opposite_side` added**

```bash
cat > /tmp/user_case.scad << 'EOF'
sol=solve2d([
    point("a",at=[0,0]),
    point("b"),
    point("c"),
    point("d"),
    con_distance("a","b",20),
    con_equal_length("a","b","b","c"),
    con_equal_length("a","b","c","d"),
    con_equal_length("a","b","a","d"),
    con_perpendicular("a","b","c"),
    con_opposite_side("a","c","d","b"),
]);
echo(sol);
a = pt(sol,"a"); b = pt(sol,"b"); c = pt(sol,"c"); d = pt(sol,"d");
function dist(p,q) = norm([p[0]-q[0], p[1]-q[1]]);
echo(diag_bd = dist(b, d));
assert(solved(sol));
assert(abs(dist(b, d) - 20*sqrt(2)) < 1e-4);
EOF
build/OpenSCAD.app/Contents/MacOS/OpenSCAD /tmp/user_case.scad -o /tmp/out.stl 2>&1
```

Expected: `solved=true`, `diag_bd ≈ 28.2843`, no Warning lines about assertions.

- [ ] **Step 3: Confirm git log shape**

```bash
git log --oneline -8
```

Expected: a clean sequence of small commits — failing test, factory builtins, parser dispatch, violation, active-set + residual + smoke fix, opposite_side example, user guide.
