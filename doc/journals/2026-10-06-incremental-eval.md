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

### Known gaps (as of the spike; superseded in Phase 1)

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

## 2026-10-06 — Phase 1: core

The spike's mechanism, made fit to keep: reused nodes point into the current
parse, so the table needs no old parse; a boundary costs no more stack than any
other call; the syntax tree is written only while parsing; and the memo has
unit tests and an opt-in ctest configuration.

### What changed, and why

**Node copy** (`e8dd7b57d`). `AbstractNode::copy()` is pure virtual and every
concrete class implements it as `copyAs(*this)`, which refuses when the dynamic
type is not that class: a new node class without one does not compile, and a
subclass of a concrete class that forgets is refused rather than sliced. A
copy has a fresh index and no children. The old `clone()` (`node_clone.cc`)
leaked a `ModuleInstantiation` per node into a global list, copied `GroupNode`
subclasses as plain groups and fell back on `shared_ptr(this)`; its only
callers, in the Python bindings, now get a deep copy built on `copy()`.

**Accumulator marks at parse time** (`ea7ff0227`). `memo::annotate()` runs at
the end of `parse()`, on the main file and every `use`d one, before anything
can evaluate them. `EvalMemoSession::prepare()` wrote `Lookup::accumulator` at
the start of every memoized evaluation, which the GUI's prefetch threads would
have raced with.

**Stack** (`bafb8c2ed`). The spike split `UserModule::instantiate` in two and
routed boundaries through two frames of its own: every user-module call paid
an extra frame, memo or not, and a boundary three. `instantiate` is one
function again. `EvalMemoSession::enter()` decides out of line and returns
before the body runs (`Plain`, `Reused`, or `Recording`); the body runs from
`instantiate`'s own frame on every path, then `leave()` (or `abandon()` on an
exception). Recorders live on the heap, pooled per evaluation.

| `recursion-test-module.scad` | with memo | without |
|---|---|---|
| dev, before the spike | — | ~23,900 (estimated) |
| spike | 13,533 | 20,581 |
| Phase 1 | 23,391 | 23,391 |

The frame is 480 bytes, 16 more than dev's, from which dev's count is
estimated. Both recursion stress tests now pass `--memo-selftest`. A call that
throws also marks the recording call around it impure: had anything caught the
error, the result would depend on it, and an error can depend on stack depth.

**Locations** (`9386a619e`, `3754ef33a`): the next section.

**Deprecations** (`95f5e54fe`). `make_message_obj()` drops a repeated
deprecation before `PRINT` sees it, so a call recorded after the same warning
had been printed elsewhere stored nothing, and lost the warning where it was
reused alone (a known gap of the spike). Repeats now reach `g_message_capture`
marked `Message::repeat`, unprinted; replaying prints a recorded deprecation if
it is new in that evaluation and passes it on as a repeat otherwise.

**Tests** (`4672674ba`, `228791c09`). Fifteen `[memo]` unit test cases (ctest
label `memo`), listed under Correctness. `ctest -C MemoSelftest` runs
`--memo-selftest` on each of the 545 corpus scripts (label `memo-selftest`; also
part of `-C All`, not of the default run): 140 s of tests at `-j8`. ctest raises
the stack limit to 64 MB, so the two deep-vector recursion scripts run with
their echo tests' `--trace-usermodule-parameters=false`; with traced
parameters one of them ran for over 20 minutes.

### Reused nodes point into the current parse

Every node points at the statement that made it (`modinst`): the GUI reads its
location from it, and geometry its `! # %` tags. Stored nodes point into the
parse that produced them, and so did the spike's copies.

An entry now records, when it is stored, where each of its statements is, in
terms that a later parse of the same text can answer:

- an **anchor**: the body of a file-scope module definition (by file, name, and
  how many definitions of that name follow it); or the children block of the
  reused call itself; or that of the k-th enclosing user-module call, found
  through the `UserModuleContext` chain, which is exactly what `children()`
  inside a children block can reach;
- a **path** down from it: a statement's index, then into its children or else
  block, or into a module definition nested in a body, by name.

Children blocks are anchored at the call that runs them, and the innermost
anchor wins, so an edit to a caller does not invalidate the parts it calls. On
reuse each path is followed in the current parse; each anchor must still have
its structural hash and each statement its fingerprint (name, modifiers,
argument and statement counts), or the call is evaluated instead. A children
block whose own call ran again inside the subtree (recursion through one call
site) could have run as either call's children, which a statement's place
cannot tell apart, so that call is not stored; nor is one more than 256
enclosing calls deep.

Where this differs from the plan, and why:

- **No index per evaluation.** `annotate()` links each scope, statement and
  module definition to what contains it (`LocalScope::origin`,
  `parent_scope`, `parent_index`): one pass at parse time, then O(1) lookups.
- **A fingerprint per statement, not a structural hash.** The anchor's hash
  covers everything under it, so a statement's own hash could only catch a
  wrong path, and the fingerprint does that for far less.
- **The table keeps no parse.** Nothing reads a stored node's statement (reuse
  writes the copies' from the paths), so rather than have entries hold the
  parses they reference, the table holds none and each parse is freed when its
  owner is done with it. The harness keeps the parse being evaluated and the
  one behind the tree on screen; `use`d files reparsed by `SourceFileCache`
  under the table, which the spike would have handed out dangling, are found
  again by path.
- **Nested entries are coded, not walked.** Recording a site for every node of
  every stored subtree came to 8.2M codes for u-bot's 4,850 entries, 17 times
  the tree, and 204 ms of a cold evaluation. An entry codes only its own nodes;
  the subtree of a call stored or reused inside it is one code and a
  translation of that entry's sites. Storing now costs what is new: 82 ms cold
  (289k nodes), 5-9 ms for an edit.
- **The table follows the latest tree.** Reuse hands each nested entry its part
  of the copy, so entries move to the current tree and nothing old is kept for
  them. `takeReplaced()` returns the trees they held, so that a caller can free
  them after showing its result (~10 ms for a u-bot tree).

### Results

Evaluation only, `model_named`, every step verified against a fresh evaluation
(now also that each node's statement is the very one a fresh evaluation
picks). Same machine and day; spike = `abd5686ba`.

| edit | spike | Phase 1 |
|---|---|---|
| first evaluation (cold table) | 4.53 s | 4.45 s |
| unchanged / comment at top / undo | 30-33 ms | 26-28 ms |
| literal in `drive_gear()` | 109 ms | 87 ms |
| feature count in `wheel()` | 104 ms | 76 ms |
| leg position, `fwd(30)` → `fwd(31)` | 85 ms | 65 ms |
| `wheel_teeth` (used by two parts) | 153 ms | 130 ms |
| `$slop` (read everywhere) | 2.80 s | 2.67 s |

With the model's own unnamed tag scopes (`model`) both take 0.66-0.77 s for the
small edits and 3.2 s for `$slop`: the impure `tag_scope()` subtrees dominate.

Edits that move code, next to the base: two lines above everything, 27 ms (one
hit); a statement at the top of `robot()`, the caller of every part, 50 ms; one
inside `leg()`, 66 ms; an unused definition above `robot()`, 26 ms. A statement
between two parts in a children block takes 447 ms: it shifts the block's
children, which BOSL2's `children(i)` sees, so the model really changes.

50 random literal mutations: median 66 ms, p90 121 ms, max 845 ms (spike 87,
160, 830 ms).

Peak RSS of the 13-step sequence with verification (`run_sequence.sh`): 1.26 GB
→ 1.02 GB on `model_named`, 1.35 GB → 1.05 GB on `model`; without verification
1.21 GB → 0.93 GB. One plain CSG export of u-bot peaks at 1.07 GB, so the
verified run is now bounded by its fresh comparisons. A cold memo evaluation
peaks 36 MB higher than the spike's (409 MB): the locations, 361k sites, 967k
path words, 100k anchors and 399k translation entries.

### Correctness

- u-bot scripted edits, 13 steps on each model: identical, statements included.
- 50 random literal mutations: 51/51 identical.
- u-bot git history (5 commits + working copy, then back and forth): 8/8.
- The code-moving edits above: 7/7.
- `--memo-selftest` over the 550 corpus scripts: 545 pass, and the 5 others are
  the excluded ones (the 4 `issue1890-*` parse errors, `dim-all.scad`). The two
  recursion stress tests pass now.
- `[memo]` unit tests: literals hash exactly, modifiers count, locations do not;
  a second evaluation hits and gives the same tree; `$` reads must match, an
  accumulated one need not unless read for real below; messages and repeated
  deprecations replay; `rands()` keeps a call out; function values hash by
  what they capture; the depth cap; every node class copies itself, a
  subclass without its own `copy()` is refused; and reuse after inserting
  lines above and statements inside callers, for nodes from module bodies, own
  children and enclosing children, with the old parse freed first, and through
  recursion over one call site's children. Breaking the remap fails three of
  them, dropping the deprecation capture one.
- `ctest` (default): 1810/1813, the 1798 tests from before plus the 15 `[memo]`
  cases; the three failures are the pre-existing `export-svg*_spec-paths-arcs01`
  diffs.

### Known gaps (as of Phase 1; superseded in Phase 2)

- **Accumulator reads and warnings.** Unchanged: a pending read is not
  validated, so a reused call would replay a stale warning from its
  accumulator expression (`undef * matrix`).
- **Calls to modules defined inside module bodies** are still not boundaries,
  and a children block naming a local definition still makes its call
  ineligible. Not attempted in Phase 1.
- **`import()`/`surface()` path resolution**, and the 128-bit hash, as before.
- **Recursion.** A boundary more than 256 enclosing calls deep, or whose
  children block a recursion re-entered, is not stored. Copying a reused
  subtree near the stack limit gives up and evaluates instead.
- **Memory.** The locations take ~30 MB for u-bot's table. Definition anchors
  repeat in every entry and could be shared by the table; `Site::end` could
  go.
- **Threads.** A table serves one evaluation at a time; nothing in it is
  synchronized.

## 2026-10-06 — Phase 2: the GUI

Each document keeps a memo table from one render to the next. A save of
u-bot now refreshes in 1.3 s when nothing changed and in 1.65–1.95 s after
editing a part, against 9.3–10 s (`model_named`; 1.9 s and 2.3–2.6 s with the
model's own unnamed tag scopes). The "Script evaluation" line says how much
was reused.

### What changed, and why

**Calls counted as a fresh evaluation makes them** (`d6bcf0831`). A reused
call is one hit however many calls it skips, so an unchanged u-bot read as one
hit out of one boundary. Each entry now records how many boundaries its
evaluation reached, its own and those of calls reused inside it included;
`Stats::userCalls` is then the number of module calls the user's files make
(6,835 for u-bot) whatever was reused, and `Stats::userCallsReused` the part
that reuse stood for.

**A table per document** (`da634bfe0`). The window's `rootFile` is only the
last tab parsed; the documents are the editor tabs, so each
`EditorInterface` holds a `shared_ptr<memo::MemoTable>`, made by its first
render and freed with the tab. `instantiateRoot()` evaluates with an
`EvalMemoSession` on it, detached by a scope guard and destroyed before the
`EvaluationSession` on every path.

- *Re-entrancy.* Printing during an evaluation processes events
  (`consoleOutput()` calls `processEvents()`), so Flush Caches, the
  preference or closing the tab can run in the middle of one. Clearing the
  table in place would free the entry list `enter()` is walking; instead they
  drop the editor's reference, and the render holds one of its own until it
  is done.
- *Exceptions.* "Stop on first warning" throws out of the whole evaluation.
  A unit test (`aeada9cf3`) stops one while a call is recorded and one while
  a reuse replays the warning; either way the next evaluation reuses what it
  can and matches a fresh one.
- *Freeing.* `compileEnded()`, once the result is on screen, frees the trees
  that reused entries gave up (`takeReplaced()`: the whole previous tree
  when nothing changed) and then evicts, so neither lands in the evaluation
  time.

**Eviction.** `evict(2)` after every render: an entry survives two renders
that do not use it. Without eviction every edit leaves behind the results of
the calls it changed, the top-level call's among them, and those hold the
whole old tree. Fifty successive single-literal edits of u-bot
(`mut_01`…`mut_50` after the base, with `--memo-replay --memo-keep N` from
`61d448572`; "beside the tree" is the footprint the table frees when cleared
with the last tree still held, which is approximate since the two share
pages):

| keep | entries | beside the tree | peak footprint | eval median / p90 / max |
|---|---|---|---|---|
| no eviction | 7,323 | 2,842 MB | 3,214 MB | 67 / 126 / 853 ms |
| 0 | 3,794 | 17 MB | 374 MB | 87 / 158 / 2,139 ms |
| 1 | 3,531 | 31 MB | 439 MB | 74 / 124 / 853 ms |
| **2** | 3,526 | 42 MB | 512 MB | 71 / 122 / 859 ms |
| 4 | 3,637 | 48 MB | 646 MB | 71 / 121 / 859 ms |

Keeping nothing beyond the last render makes every revert evaluate again
(after `$slop`, going back to the base takes 2.67 s instead of 29 ms). With
two, the next render can return in full to either of the two versions before
the current one, as an undo of one or two edits does, and a part disabled
with `*` for up to two renders comes back without running; from a version
further back only what it alone had runs again. Each further render kept
costs up to a tree, about 70 MB of peak footprint here. The table's steady
state is 3,500–5,300 entries and about 40 MB beside the tree on screen, which
takes about as much again: small next to the two 5,000 MB geometry caches. No
size guard on top: what survives is what the last three renders used or
made, so the table cannot outgrow three renders' trees and entries.

**Console** (`e9156ce79`). A render-statistic phase can carry a note, printed
after its time: `Script evaluation: 0:00:00.060 ( 4.6%), reused 6835 of 6835
module calls`. One line per render, the one already there; none when the
preference is off or the design calls no modules.

**Flush Caches and the preference** (`2dfa84366`). Flush Caches drops the
tables of every window's documents, as it empties the other, global, caches.
Preferences → Advanced → 3D Rendering, below the cache sizes: "Reuse unchanged
module results between renders", on by default (`advanced/reuseModuleResults`
like the other Settings entries); turning it off drops the tables at once.

**Threads.** Animation prefetch workers (`AnimateFrameTask`) make their own
`EvaluationSession` and never get a memo; they share only the parse, which
nothing writes after `parse()`. Renders on the GUI thread are serialized by
`GuiLocker`, which is global across windows, and the CGAL worker only reads
the finished tree, whose nodes the table shares. Main-thread animation steps
(typing a time, stepping, dump-pictures) use the memo: `$t` is a recorded `$`
read, so at a new time only the calls that read it run.

**What reads the tree.** The picker (`getNodeByID()`), the editor highlight,
`#`/`%` overlays, the interference report and exports all read nodes'
statements, which for reused nodes point into the parse on screen (Phase 1).
`--memo-verify` and the unit tests already compare each node's statement
pointer with a fresh evaluation's; the GUI tests add the window's view: its
tree equals a fresh evaluation of the same parse, a reused cube found by its
index is on its new line, and `overlay::collect()` sees a `#` added to a
reused call.

**GUI tests** (`2c1aa8f59`, `086ebed15`). `TestEvalMemo` renders documents in
a real window (`ENABLE_GUI_TESTS`, `--run-all-gui-tests`): reuse across
renders and after a save that moves the text, Flush Caches and the
preference, `$t`, and a render while the animation plays with frames
prefetched on worker threads. Test runs now get settings of their own: they
used to rewrite the recent files, auto-reload and view settings of the user's
OpenSCAD. A `QSignalSpy` on `compilationDone` aborted with `qBadAlloc` (Qt
6.11) after renders of u-bot, with or without the memo, so the tests wait
through a plain connection. `benchmarkEditSequence()` times a series of
saves through the window when `OPENSCAD_MEMO_BENCH` lists them.

### Results

Through the window (`benchmarkEditSequence`): each version saved over one
document and refreshed as auto-reload does, `model_named`, geometry caches at
5,000 MB. "Refresh" is the console's "Total rendering time":

| edit | eval | geometry | refresh | without memo |
|---|---|---|---|---|
| first render (empty table) | 4.59 s | 2.20 s | 7.67 s | 10.5 s |
| unchanged / comment at top / undo | 44–60 ms | 1.21 s | 1.30–1.33 s | 9.3–9.5 s |
| literal in `drive_gear()` | 119 ms | 1.66 s | 1.80 s | 9.6 s |
| feature count in `wheel()` | 103 ms | 1.71 s | 1.83 s | 9.8 s |
| leg position | 91 ms | 1.53 s | 1.65 s | 9.6 s |
| `wheel_teeth` | 161 ms | 1.75 s | 1.93 s | 9.8 s |
| `$slop` | 2.77 s | 1.98 s | 4.80 s | 10.0 s |

With the model's own unnamed tag scopes (`model`): 1.93–1.97 s unchanged
(0.69–0.71 s of it evaluation), 2.28–2.56 s for the part edits, 5.24 s for
`$slop`, against 9.2–10.6 s.

An auto-reload takes about 0.5 s more than its total: the 200 ms polling
period (half of it on average), the 200 ms that `waitAfterReload()` waits for
further changes to included files, and the parse (~0.17 s), which the
console omits because the second `compile()` restarts the statistic.
Geometry is now 90% of an unchanged refresh.

### Correctness

- u-bot scripted edits, 13 steps on each model, with `--memo-keep 2`: every
  tree, statements included, and every message stream identical to a fresh
  evaluation.
- 50 random literal mutations: 51/51 identical; median 75 ms, p90 123 ms, max
  870 ms against a fresh median of 8.3 s. u-bot git history: 8/8.
- `[memo]` unit tests: 17 (the call counts and the throwing evaluation are
  new), and one for the phase note; `ctest` 1813/1816 with the three known
  `export-svg*_spec-paths-arcs01` failures; `ctest -C MemoSelftest`
  545/545.
- GUI tests: all four classes pass.

### Known gaps

- **Accumulator reads and warnings**, **calls to modules defined inside
  module bodies**, **`import()`/`surface()` path resolution**, the **128-bit
  hash** and the **recursion** limits: as in Phase 1.
- **Memory.** Each kept render can hold a whole old tree, about 70 MB of
  peak footprint on u-bot. A budget in nodes would let the table keep more
  renders when they are cheap, and fewer when they are not. The locations
  take ~30 MB of the table, as before.
- **Unnamed `tag_scope()`** costs 0.7 s of every refresh of u-bot as it is.
- **Auto-reload's** console total omits its settle time and the parse.
- **A tab reused for another file** (File → Open into an empty, unmodified
  tab) keeps its table; what the new file cannot use is evicted within three
  renders.
- The `QSignalSpy` abort in the GUI tests is unexplained.

## Next

1. **Phase 3, geometry keys:** per-node digests computed bottom-up, reused
   subtrees keeping theirs, so the whole-tree dump goes away: 1.2 s of the
   1.3 s that an unchanged u-bot refresh now takes is geometry.
2. **Random tag scopes:** either name them in the model, or seed the RNG
   deterministically per refresh and record RNG position as a dependency, so
   unnamed `tag_scope()` becomes reusable. The second changes what unseeded
   `rands()` returns from refresh to refresh (the same values every time).
3. **Auto-reload latency:** the 200 ms wait for further changes after a
   reload, and a statistic that starts with the save.
4. **Nested definitions** as boundaries, carried over from Phase 1.
5. **Eviction by budget** rather than by a fixed number of renders.

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

# The unit tests, and --memo-selftest over the whole regression corpus
build-release/OpenSCADUnitTests "[memo]"
ctest --test-dir build-release -C MemoSelftest -L memo-selftest -j8

# Random single-literal edits, written next to the base so includes resolve
python3 -I doc/journals/memo-mutate.py base.scad . 50 7

# Evict after each step as the GUI does, and report what the table holds
$BIN --memo-replay base.scad edit1.scad base.scad --memo-keep 2

# The GUI tests need a build of their own; a run opens a window, uses
# settings of its own and exits with the number of failures
cmake -B build-guitest -DCMAKE_BUILD_TYPE=Release -DEXPERIMENTAL=1 -DENABLE_GUI_TESTS=ON
cmake --build build-guitest -j10 --target OpenSCADExe
build-guitest/OpenSCAD.app/Contents/MacOS/OpenSCAD --run-all-gui-tests

# ...and time refreshes through the window, with the memo and without: the
# versions are saved in turn over memo-bench.scad, next to the first
M=/path/to/a/copy/of/u-bot
OPENSCAD_MEMO_BENCH=$M/base.scad:$M/edit1.scad:$M/base.scad \
  build-guitest/OpenSCAD.app/Contents/MacOS/OpenSCAD --run-all-gui-tests
```

Edit variants must sit in the model's own directory: relative `include`/`use`
paths and the key's directory component both depend on it. Copy the model
(and `lib/`) somewhere else first; never generate variants in the live tree.
