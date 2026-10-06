# Work journal: incremental evaluation

**Goal:** a small edit to `~/prj/Make/stocko/u-bot/u-bot.scad` should not cost
a full re-evaluation. Every refresh took ~9.7 s in the GUI, 7.96 s of it script
evaluation, regardless of the size of the edit.

**Machine:** Apple M1 Max, Release build (`build-release`, `-DEXPERIMENTAL=1`).

**Branch:** `perf/incremental-eval` (off `dev`).

---

## 2026-10-06 — Phase 0: the spike

### Why evaluation, and why every time

The geometry stage already caches per subtree; evaluation has no cache at all,
so the 82% that is evaluation is redone on every refresh. On the live model:

```
parsing 0.18 s | evaluation 7.95 s | geometry 2.3 s cold, 1.76 s warm (GUI)
37.3M function calls, 12.7M loop iterations, 523k module instantiations
420,870 group() nodes for ~5,200 real primitives; mean depth 171, max 369
```

99% of interpreter events are inside BOSL2. The script's own files make only
**7,442 module calls at 906 call sites** per evaluation; BOSL2 makes the other
516k. Those 7,442 are the reuse points: a reused call skips all the library
work beneath it.

Side measurement, not pursued here: neutralising BOSL2's 3,129 `assert()`s in a
scratch copy took evaluation from 7.9 s to 4.4 s with a byte-identical STL;
~73% of interpreter events exist only to evaluate assert conditions.

### What was built

`core/EvalMemo.{h,cc}`, `core/EvalMemoAST.cc`, and a command-line harness
`--memo-replay FILE... [--memo-verify] [--memo-geometry]` / `--memo-selftest
FILE` (`memo_replay.cc`). Off unless an `EvaluationSession` is given a memo; the
normal paths are unchanged apart from where `UserModule::instantiate` evaluates
its arguments (same order as before).

A **boundary** is a call to a file-scope user module whose call site is in a
non-library file. Its result is the node subtree the call produced.

The **key** covers what is lexical: the module's code and everything reachable
from it by name (structural AST hash, exact literals -- the AST printer rounds to
six digits, so printed text is unusable), the file-scope values that code
references, the argument values, the children block (syntax, the caller-scope
values it names, and the enclosing children key when it calls `children()`),
and the directories the code and call site live in (relative `import()` paths).

**`$` variables are recorded, not keyed.** A stored call records each `$`
variable it reads from frames below its own, with the value's hash, and an entry
is reused only where those reads would see the same values. Every `$` read goes
through `EvaluationSession::try_lookup_special_variable` (and the special
function/module lookups), so the hook is exact. `parent_module()` records the
module-name stack the same way.

Reuse clones the stored subtree with fresh node indices (they restart every
refresh and must be unique per tree), points the root group at the current call
site, replays the messages the call printed, and re-reads its `$` dependencies
so the enclosing call records them too.

### What the first version got wrong

Each of these was found with `OPENSCAD_MEMO_DEBUG=1`, which itemises why calls
were not reused.

1. **Function values everywhere.** The first key refused function values. BOSL2
   keeps them in `$parent_geom` (geometry descriptors carry `override`
   functions) and at file scope (`_lead_in_table` in skin.scad), which made
   1,851 of ~4,000 boundaries unkeyable. A function value now hashes as its
   literal's syntax plus the values of the names it captures, with a cycle guard.
   Unchanged-file evaluation went from 3.8 s to 0.9 s.

2. **`tag_scope()` without a name** calls `rand_str(20)`, i.e. `rands()`. u-bot
   has four (two in u-bot.scad, two in battery_mount.scad); their subtrees are
   impure and re-run on every refresh. Naming them (`tag_scope("front")`) makes
   them reusable. Without that, an unchanged refresh evaluates in ~0.7 s instead
   of ~0.03 s.

3. **BOSL2 overrides `translate`, `rotate`, `scale`, `mirror` and `multmatrix`**
   to keep `$transform = $transform * m` (transforms.scad:1573-1631). With the
   whole visible `$` state in the key, moving a part changed `$transform` for
   everything beneath it: the leg-position edit took 3.4 s. Recording reads
   instead of hashing the visible state, and treating a read that only feeds the
   same variable as *pending* -- a dependency only if something under the call
   reads the variable for real (`parent()`, `desc_*`) -- took it to 0.09 s.

4. **Deeply nested vectors crashed the hasher** (two corpus stress tests,
   `issue4172-echo-vector-stack-exhaust.scad` and `recursion-test-vector.scad`;
   SIGSEGV in `ValueHashCache::hash`). Value hashing now stops at depth 200 and
   treats deeper values as unhashable.

### Results

Evaluation only, named tag scopes, each step verified against a fresh
evaluation (`--memo-verify`):

| edit | memo | fresh |
|---|---|---|
| first evaluation (cold table) | 4.66 s | 7.90 s |
| unchanged / comment at top / undo | 0.03 s | 8.0 s |
| literal in `drive_gear()` | 0.11 s | 8.0 s |
| feature count in `wheel()` | 0.10 s | 7.8 s |
| leg position, `fwd(30)` → `fwd(31)` | 0.09 s | 7.9 s |
| `wheel_teeth` (used by two parts) | 0.15 s | 7.9 s |
| `$slop` (read everywhere) | 2.8 s | 8.0 s |

The cold evaluation is 41% *faster* than without the memo: identical calls
inside one evaluation (repeated parts, BOSL2 passes that do not read the tag
variables) are reused too. Key computation costs ~55 ms per cold evaluation.

50 random single-literal mutations of u-bot.scad (`doc/journals/memo-mutate.py`,
seed 7): median 85 ms, p90 160 ms, max 830 ms, against a fresh median of
7,978 ms.

Projected refresh with geometry (`--memo-geometry`, geometry caches at the
GUI's 5,000 MB so nothing is evicted between steps):

| edit | eval | geometry | total |
|---|---|---|---|
| unchanged / undo | 0.03 s | 1.00 s | 1.0 s |
| part literal / feature / position | 0.09–0.11 s | 1.32–1.49 s | 1.4–1.6 s |
| `wheel_teeth` | 0.16 s | 1.60 s | 1.75 s |
| `$slop` | 2.8 s | 1.8 s | 4.7 s |

With the model's tag scopes as they are, add ~0.65 s of evaluation to each.
Geometry is now ~85% of a refresh. Its ~1.0 s floor on an unchanged tree is the
whole-tree NodeDumper pass that builds cache keys for 470k nodes; that is
Phase 3.

### Correctness

- u-bot scripted edits (13 steps × 3 runs): every tree and message stream
  identical to a fresh evaluation.
- 50 random literal mutations: 51/51 identical.
- u-bot git history (5 commits + working copy, then back and forth): 8/8
  identical.
- `--memo-selftest` over all 550 files in `tests/data/scad` (evaluate twice
  with the memo, compare both with a fresh run): 543 pass and none crash. Of
  the rest, four
  are the `issue1890-*` parse-error tests (the harness refuses to parse them),
  `dim-all.scad` differs only because the global DXF cache is warm for the
  second run, and two are recursion stress tests whose *error message* differs
  because the memo path uses more stack per module call (below).
- `ctest` with the memo off: 1795/1798; the three failures are the pre-existing
  `export-svg*_spec-paths-arcs01` diffs.

Peak RSS for a 13-step replay with verification, every syntax tree kept: 1.34 GB.

### Known gaps

- **Recursion depth.** The boundary path adds frames per user-module call:
  `recursion-test-module.scad` reaches 13,533 frames with the memo against
  20,581 without. Fine for real models; Phase 1 should fold the dispatch back
  into `UserModule::instantiate`.
- **Accumulator reads and warnings.** A pending read is not validated. If the
  accumulator expression warned on the value's type (`undef * matrix`), a reused
  call would replay a stale warning. BOSL2 initialises `$transform = IDENT`, so
  this does not arise there.
- **Locations.** Clones keep pointing into the syntax tree that produced them,
  so the harness keeps every parse alive, and the picker would show old line
  numbers for reused nodes. Phase 1 remaps `modinst` to the new tree.
- **Calls to modules defined inside module bodies** are not boundaries, and a
  children block naming a local definition makes its call ineligible.
- **`import()`/`surface()` path resolution** happens at evaluation time; a file
  appearing or disappearing at a relative path is not detected (contents are
  read at geometry time, so changed contents are).
- **Deprecation messages** are replayed through the same de-duplication set, but
  one first printed outside a reused call and then inside it is not re-captured.
- The hash is a 128-bit non-cryptographic mix; a collision would reuse the
  wrong subtree.

### Verdict

Go. Localized edits evaluate in ~0.1 s (target ≲1.5 s) and the cold cost is
negative (target ≲5%), with no mismatches on any u-bot edit set.

## Next

1. **Phase 1, core:** per-class node copy (the existing `AbstractNode::clone`
   leaks a `ModuleInstantiation` per node into a global list and slices
   `GroupNode` subclasses), `modinst` remap so old parses can be freed, a
   smaller stack footprint, nested definitions, `[memo]` unit tests, and
   memo-selftest as a ctest label.
2. **Phase 2, GUI:** a table per document across refreshes, generation-based
   eviction, Flush Caches clears it, a preference to turn it off, a console
   line ("reused X of Y calls").
3. **Phase 3, geometry keys:** per-node digests computed bottom-up, reused
   subtrees keeping theirs, so the ~1.0 s whole-tree dump goes away.
4. **Random tag scopes:** either name them in the model, or seed the RNG
   deterministically per refresh and record RNG position as a dependency, so
   unnamed `tag_scope()` becomes reusable. The second changes what unseeded
   `rands()` returns from refresh to refresh (the same values every time).

## Reproducing

```bash
BIN=build-release/OpenSCAD.app/Contents/MacOS/OpenSCAD

# A series of saves: evaluate files in order with one table, verify each step
$BIN --memo-replay base.scad edit1.scad base.scad --memo-verify

# ...and time each step's geometry with GUI-sized caches kept across steps
$BIN --memo-replay base.scad edit1.scad base.scad --memo-geometry

# Evaluate one file twice and compare both runs with a fresh one
$BIN --memo-selftest model.scad

# Why calls were not reused, itemised per step
OPENSCAD_MEMO_DEBUG=1 $BIN --memo-replay base.scad base.scad

# Random single-literal edits, written next to the base so includes resolve
python3 -I doc/journals/memo-mutate.py base.scad . 50 7
```

Edit variants must sit in the model's own directory: relative `include`/`use`
paths and the key's directory component both depend on it. Copy the model
(and `lib/`) somewhere else first; never generate variants in the live tree.
