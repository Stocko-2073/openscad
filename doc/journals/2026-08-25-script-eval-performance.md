# Work journal: script evaluation performance

**Goal:** `~/prj/Make/stocko/u-bot/u-bot.scad` (BOSL2-heavy, ~500k facets) reported
`Total rendering time: 0:00:16.780`. Find out where that goes and reduce it.

**Machine:** Apple M1 Max, macOS 24.6, Release build (`build-release`, `-DEXPERIMENTAL=1`).

**Branch:** `perf/script-eval-profiling` (off `dev`).

---

## 2026-08-25 — Where does the time actually go?

### The pipeline is not two passes

Parsing and rendering are separate, but there are five stages, not two:

| Stage | Code | Produces |
|---|---|---|
| Parse | flex/bison, cached in `SourceFileCache` | AST (`SourceFile` → `LocalScope`) |
| Evaluate | `SourceFile::instantiate` → `LocalScope::instantiateModules` | node tree (`AbstractNode`) |
| CSG build | `CSGTreeEvaluator::buildCSGTree` | CSG tree (`CSGNode`) |
| Normalize | `CSGTreeNormalizer` | CSG products (`CSGProducts`) |
| Geometry | `GeometryEvaluator` (worker thread) | Manifold/CGAL geometry |

F5 runs parse → evaluate → CSG build → normalize → renderers.
F6 runs parse → evaluate → geometry. The GUI's timer starts at the top of
`MainWindow::compile()`, so its total covers everything; the CLI's used to start
after instantiation, which is why the two numbers never agreed.

### Measured breakdown (before any changes)

`OpenSCAD -o out.stl u-bot.scad`, 3 runs:

```
Parsing              0.157 - 0.189 s   (~1%)
Script evaluation   13.26  - 13.40  s   (~79%)
Geometry evaluation  2.94  -  3.09  s   (~18%)
Export               0.43  -  0.45  s   (~3%)
Total               16.78  - 17.20  s
```

**Lexing/parsing is not the problem.** All of BOSL2 plus the local libs parse in
0.19s. The cost is the tree-walking interpreter.

Note `$preview` matters a lot: F5 (preview=true) evaluates in ~10s, F6/STL
(preview=false) in ~13.3s, because BOSL2 takes cheaper paths under preview.

### The `!` (root) modifier does not help

`tag_root` is set in `parser.y:238` and read *only* by `find_root_tag`
(`core/node.cc:185`), which runs **after** `SourceFile::instantiate` has already
walked the whole script. So `!` prunes CSG build / normalize / geometry, but not
the 79%. Measured on a synthetic file:

| | Script evaluation | Geometry |
|---|---|---|
| baseline | 0.408s | 0.004s |
| `!` on the cheap part | 0.413s | 0.000s |
| `*` on the expensive part | 0.000s | 0.000s |

`*` is the one that saves evaluation — the parser does `delete $2; $$ = NULL;`
(`parser.y:250-253`), so the subtree never reaches the AST.

**Practical advice for now: use `*` (or comment out) to iterate on one part.**

### Why pruning for `!` is hard

Static AST marking doesn't produce a useful live set:

- Call edges are *name lookups* resolved at runtime (`ScopeContext::lookup_local_module`
  walks the context chain); `include` merges scopes and `LocalScope::addModule`
  lets later definitions overwrite earlier ones.
- One AST site becomes many runtime nodes (a `!` inside a module called 50 times).
- `children()` is an edge the AST doesn't contain — `Children` holds the *caller's*
  scope/context. BOSL2 is wall-to-wall `attachable(){ shape(); children(); }`, so a
  sound analysis marks nearly everything live.

Runtime short-circuiting is the viable form — see Open leads below.

---

## 2026-08-26 — Profile of the evaluation stage

`sample <pid> 12 1`, window entirely inside parse+evaluate. 9457 main-thread
leaf samples at 1ms.

### Structure: mutual recursion, enormous stacks

`LocalScope::instantiateModules` → `ModuleInstantiation::evaluate` →
`UserModule::instantiate` (builds `UserModuleContext`; `ScopeContext::init()`
eagerly evaluates every assignment in the body) → back to `instantiateModules`.
Builtins detour via `BuiltinModule::instantiate` → `builtin_children`/`builtin_if`
→ `Children::instantiate` → back in.

Frame appearances across sampled stacks:

```
299,676  ModuleInstantiation::evaluate     111,978  UserModule::instantiate
299,672  LocalScope::instantiateModules    102,054  builtin_children
192,023  Children::instantiate              43,161  builtin_if
191,026  BuiltinModule::instantiate
```

`sample` had to re-root orphaned stacks (indentation past 1000 columns) — single
stacks are hundreds of frames deep. That's the BOSL2 `attachable`/`children()`
chain, and it's why the build carries `STACKSIZE=8388608`.

### Leaf attribution

```
  3034   32.1%  malloc / free
  1263   13.4%  <deduplicated_symbol>  (ICF-folded, unresolved)
  1084   11.5%  context frame setup / teardown
   958   10.1%  expression / function eval
   910    9.6%  string hashing + map probe
   773    8.2%  Value copy / destroy
   561    5.9%  other
   528    5.6%  name lookup (context chain walk)
   226    2.4%  memcpy / memset
    68    0.7%  lexer / parser
```

**~55-60% is memory management and context plumbing; ~10% is actually
interpreting expressions.**

### Finding: mimalloc was inert on macOS

Zero `mi_*` symbols in the profile — everything in `nanov2_malloc_type`/`nanov2_free`.
`MI_OVERRIDE` is forced OFF for `APPLE` in `submodules/CMakeLists.txt:23-27`, and
the `mimalloc-new-delete.h` include in `src/openscad_mimalloc.h` was behind `#if 0`.
So mimalloc was linked but only ever served GMP.

Origin: commit `ff6e9bc36` (Hans Loeblich, 2022-01-15), message in full
*"Set MI_OVERRIDE conditionally for APPLE."* — **no recorded rationale.**
Circumstantial: macOS override uses dyld interposition + malloc zone, both dynamic
mechanisms, and OpenSCAD links mimalloc statically; SIP strips `DYLD_INSERT_LIBRARIES`
for signed apps; mimalloc didn't fix Monterey dynamic overriding until 1.7.6/2.0.6
(April 2022, *after* that commit). Best guess: interposition didn't work, not a
performance judgment.

---

## What was changed

Two commits on `perf/script-eval-profiling`:

### `7e9715fa1` stats: itemize the render time by pipeline phase

- `RenderStatistic` gains `beginPhase`/`endPhase`/`ScopedPhase` and prints a
  breakdown under the total; `--summary time` gains a `phases` array.
- GUI and CLI both instrumented. CLI's statistic now starts before parsing so its
  total matches the GUI's; animated exports restart per frame.
- Phases and the interleaved console lines renamed for what each stage *produces* —
  `CSG tree generation` had been the name for the stage that evaluates the script,
  not the one that builds the CSG tree:
  - `CSG tree generation` → `Script evaluation`
  - `CSG products generation` → `CSG tree build`
  - `CSG products normalization` → `CSG normalization`

### `97d87e05b` build: route C++ allocation through mimalloc on macOS

- `src/openscad_mimalloc.h` now includes `<mimalloc-new-delete.h>` (link-time
  `operator new`/`delete` replacement, no interposition needed), gated on
  `MI_OVERRIDE` to avoid the duplicate definition in microsoft/mimalloc#535 on
  Linux/Windows.
- **Result, median of 3:** script evaluation 13.38s → 10.66s (−20.3%);
  total 16.95s → 14.16s (−16.5%). Exported STL byte-identical.
- Regression suite 2800/2803. The three `export-svg*_spec-paths-arcs01` failures
  are content diffs that reproduce on a build with this reverted — pre-existing.
- **Unproven:** `malloc`/`free` stay system while `new`/`delete` are mimalloc, so
  any code that `new`s and `free()`s crosses heaps. The CLI suite doesn't reach the
  Qt/CGAL/lib3mf GUI paths. Needs soak time in the app.
- Only compiled and measured on macOS. Linux/MinGW link unverified.

---

## Open leads (as of 2026-08-26 morning; superseded below)

Ranked by expected value against the remaining ~10.7s:

1. **Intern identifiers.** `ContextFrame` is `unordered_map<string, Value>`; every
   variable read hashes a `std::string` and memcmps. 9.6% hashing + 5.6% chain walk.
   Interning to indices at parse time should take most of both.
2. **Resolve the 13.4% ICF blob.** Rebuild with `-g -Wl,-no_deduplicate` to find out
   what it is. Given its neighbours, likely more `Value`/shared_ptr machinery.
3. **Early termination for `!`.** Flag on `EvaluationSession`; once a node with
   `tag_root` is produced, `LocalScope::instantiateModules` stops instantiating
   siblings and unwinds. ~30 lines, no static analysis, exact under first-hit-wins.
   Loses the duplicate-`!` warning and echo/assert past the tag. Order-dependent
   win: skips everything *after* the tag in walk order.
4. **Lexical-path prune for `!`.** Walk the lexical path root→tag, instantiating only
   the index that leads onward at each level (`LocalScope::instantiateModules` already
   has an `indices` overload, used by `children(indices)`). Needs unambiguous module
   name resolution. Combined with (3) ≈ "evaluate only the path plus the subtree".
5. **`MI_PADDING` is ON** in `submodules/CMakeLists.txt:22` with a comment claiming
   it's DEBUG-only, but mimalloc defines `MI_PADDING=1` unconditionally when set.
   Worth measuring with it off.
6. **Context churn** (11.5%) is proportional to the `children()` hop count, which
   BOSL2 inflates enormously. Harder; would mean changing how `Children` works.

---

## Reproducing

```bash
BIN=build-release/OpenSCAD.app/Contents/MacOS/OpenSCAD

# Phase breakdown
$BIN -o /tmp/out.stl u-bot.scad

# Machine-readable
$BIN --summary time --summary-file - -o /tmp/out.stl u-bot.scad

# Isolate evaluation (no geometry). NB: this path runs with $preview=true
$BIN -o /tmp/out.echo u-bot.scad

# Profile — sample by PID, not by name, or you attach to the running GUI app
$BIN -o /tmp/prof.stl u-bot.scad & sample $! 12 1 -f /tmp/sample.txt; wait

# Regression suite (~2803 tests)
ctest --test-dir build-release -j$(sysctl -n hw.ncpu) --output-on-failure
```

Build note: `cmake --build` uses the cached `CMAKE_MAKE_PROGRAM`. If `gmake` is
missing, either install it or re-run `cmake -B build-release ...` to re-detect.

---

## 2026-08-26 (later) — Attacking the interpreter's variable lookup

Starting point after `97d87e05b`: script evaluation 10.48s, total 14.11s
(medians of 3).

### Measuring the shape of a context frame

Before choosing between the open leads, `ContextFrame` was instrumented with a
size histogram and a lookup counter (temporary patch, not committed). On
`u-bot.scad`:

```
frames cleared:  38,960,630   total vars: 26,948,054   mean: 0.69
  size  0 : 21,187,256   (cum  54.4%)
  size  1 : 14,514,780   (cum  91.6%)
  size  2 :  1,214,239   (cum  94.8%)
  size  5 :    691,980   (cum  98.9%)
  size  7 :    317,095   (cum  99.9%)
var lookups: 60,616,582   frames walked: 149,500,065   mean depth: 2.47
misses: 0   config lookups: 169,974 (0.3%)
```

That settled it. 39M frames averaging 0.69 variables were each backed by an
`unordered_map<std::string, Value>`, and 149.5M probes each hashed a name. A
hash map is the wrong container at this size, and a `std::string` is the wrong
key at this frequency.

### What was changed

Six commits, each measured on its own (medians of 3, `u-bot.scad`):

| Commit | Script evaluation | |
|---|---|---|
| — | 10.48s | starting point |
| `c82349d45` build: stop padding every mimalloc block | 10.29s | −1.8% |
| `4f59fdc42` perf: intern names and flatten context frames | 8.60s | −16.4% |
| `4f36a9e0b` perf: inline the variable lookup | 8.21s | −4.6% |
| `947e88331` build: let mimalloc override malloc on macOS | 7.92s | −3.5% |
| `e8bdd54e1` perf: key the builtin tables by Identifier | 7.90s | noise |
| `03b8acca1` perf: allocate contexts and control blocks together | 7.69s | −2.7% |

**Net: script evaluation 10.48s → 7.69s (−26.6%); total 14.11s → 11.28s
(−20.1%).** Exported STL byte-identical at every step. Regression suite
2721/2724 throughout, the three failures being the same pre-existing
`export-svg*_spec-paths-arcs01` content diffs.

Notes on the individual changes:

- **`MI_PADDING`** (lead 5) was exactly as suspected: the comment claimed
  DEBUG-only, mimalloc applies it unconditionally.

- **Interning** (lead 1) is the new `Identifier` class: a pointer to an entry
  in a process-wide intern table, so comparison is a pointer compare and the
  `$`-prefix test is a precomputed bit. `ValueMap` became a
  `boost::container::small_vector` of two inline entries scanned linearly.
  The two go together — the flat array alone was worth only 2%, because
  `std::string::operator==` and its `memcmp` call replaced the hashing.

  Names are interned at parse time. Everything that had been building names
  during evaluation was converted: `for`/`let` loop variables, the
  `"this"`/`"#THIS"` object-member protocol, and the 33 builtin call sites
  that had been rebuilding a `std::vector<std::string>` from literals on
  every single call. Watch for this: a `std::string` reaching an `Identifier`
  parameter converts implicitly and silently takes a lock.

- **Devirtualising the lookup**: `lookup_local_variable` was `virtual` and had
  never been overridden by anything, so all 149.5M probes paid for a dispatch.

- **`MI_OVERRIDE` on macOS** (not on the original list) turned out to be
  available after all. The `ff6e9bc36` disable predates the mimalloc release
  that fixed macOS overriding, and the submodule is now on 1.9.7. Two wins:
  the system allocator disappears entirely, closing `97d87e05b`'s open caveat
  about `malloc`/`free` and `new`/`delete` living in different heaps; and
  `MI_MALLOC_OVERRIDE` switches mimalloc to a pthread TLS slot instead of a
  `__thread` variable, which on macOS had been calling dyld's `_tlv_get_addr`
  on every allocation (247 of 6.3k profile samples before, 29 after).

- **`make_shared` for contexts**: constructors are protected so only
  `Context::create` can reach them, which normally rules out `make_shared`;
  going through a locally declared derived class works, since a derived class
  may invoke a protected base constructor. Peak RSS is unchanged
  (1.67–1.79GB either way).

### On the ICF blob (lead 2)

Relinking with `-Wl,-no_deduplicate` resolves `<deduplicated_symbol>`, and it
is worth doing whenever the profile is being read seriously — it was hiding
the single largest cost each time. Reproduce with:

```bash
cmake -B build-release -DCMAKE_EXE_LINKER_FLAGS="-Wl,-no_deduplicate"
# ... profile ...
cmake -B build-release -DCMAKE_EXE_LINKER_FLAGS=
```

The blob was hash-table machinery at first, and function lookup afterwards.

### Where the time is now

Unfolded profile, main thread, ~6.0k samples over script evaluation:

```
366  ContextFrame::lookup_local_function
351  FunctionCall::evaluate            (simplify_function_body inlined)
181  Context::lookup_variable
176  operator delete
175  operator new
164  Value::clone
138  BinaryOp::evaluate
101  multvecmat
 98  ContextFrame::~ContextFrame
 98  ValueMap::insert_or_assign
 86  FileContext::lookup_local_function
```

It is flat now — no single item dominates. **Function lookup is the largest
cluster**: `lookup_local_function` + `FileContext::lookup_local_function` +
`Context::lookup_function` + the two hash-table finds come to roughly 10%.

---

## Open leads (revised)

1. **The function-lookup return type.** Every frame in the chain builds and
   destroys a `boost::optional<CallableFunction>`, and `CallableFunction` is a
   `std::variant` holding a `shared_ptr` and a `Value` — so each frame on the
   walk runs variant destructor dispatch to return "not here". A cheaper
   protocol for the walk (out-parameter, or a two-step find-then-build) should
   take most of the ~10%.

2. **`FileContext::lookup_local_function` re-resolves `usedlibs` every time**,
   calling `SourceFileCache::instance()->lookup(m)` — a string-keyed hash — for
   each `use`d file on every miss. Caching the resolved `SourceFile *` on the
   `SourceFile` would remove it.

3. **`FunctionCall::evaluate` creates a context it usually does not need.**
   `expression_context` is a fresh empty `Context` per call, existing only as a
   slot the tail-call loop can replace. Making it lazy would remove one context
   per function call. Delicate: `ContextFrameHandle::release()` asserts
   top-of-stack, so the ordering of handle construction and replacement matters.

4. **Early termination for `!`** (was lead 3) — unchanged, still ~30 lines and
   still the only cheap way to make `!` prune evaluation.

5. **Lexical-path prune for `!`** (was lead 4) — unchanged.

6. **`Parameters`' string-keyed accessors.** `parameters["size"]` interns at
   runtime (~0.3% of evaluation). Fixing it properly means interned constants
   at every builtin accessor site, which is a few hundred edits for a small win.

7. **Context churn** (was lead 6) — `Children`/`children()` hop count. Still the
   structural one, still hard.

## Reproducing (additions)

```bash
# Frame-size / lookup-depth instrumentation: temporary counters in
# ContextFrame::clear() and Context::try_lookup_variable, dumped from a
# static destructor. Not committed; see the numbers above.

# Peak memory
/usr/bin/time -l $BIN -o /tmp/out.stl u-bot.scad 2>&1 | grep 'maximum resident'
```

---

## 2026-08-26 (later still) — Counting what the interpreter actually does

The revised leads above were ranked off the profile alone. Counting the events
first changes the ranking substantially, and corrects an earlier reading.

Temporary counters in `Context::Context`, `FunctionCall::evaluate`,
`forContext`, `Let::sequentialAssignmentContext`, `UserModule::instantiate`,
`ScopeContext::init`, `Context::lookup_function` and `Context::lookup_module`
(not committed). On `u-bot.scad`:

```
contexts constructed          : 38,933,214
FunctionCall::evaluate        : 20,909,762
forContext (for / list comp)  :  8,941,394
Let contexts                  :      40,781
UserModule::instantiate       :      81,114
ScopeContext::init assignments:     193,431

function lookups: 21,347,022   frames walked: 98,633,951   mean depth: 4.62
module lookups  :    262,419   frames walked:  1,036,689   mean depth: 3.95
```

For comparison, the node tree this produces has ~205k group nodes
(`grep -c 'group()'` on a `-o tree.csg` export), and the model is 518k facets.

### The module machinery is not where the time goes

**This corrects the 2026-08-26 profile entry.** That entry listed frame
*appearances across sampled stacks* — 299,676 `ModuleInstantiation::evaluate`,
192,023 `Children::instantiate`, 102,054 `builtin_children` — and lead 6
concluded that context churn was "proportional to the `children()` hop count,
which BOSL2 inflates enormously". Those numbers measure how deep the stacks
are, not how often anything is called. The call counts say otherwise: 262k
module instantiations total, 81k of them user modules, against 20.9M function
calls. Module instantiation is ~1% of the events.

So these are all *not* worth pursuing, and are recorded here so nobody else
costs them out:

- `StaticModuleNameStack` copies a `std::string` into a `thread_local` vector
  on every user module instantiation — 81k times.
- `UserModule::instantiate` builds `std::string("module ") + this->name` for
  every `GroupNode`, one heap allocation each, retained for the life of the
  node tree, and read only by the GUI backtrace — 81k times.
- `Children` is copied by value into `UserModuleContext` — 81k times.

Each is real waste and each is ~0.2% of the interpreter's event count.

### Where the events actually are

**20.9M function calls and 8.9M loop iterations.** Together they account for
essentially all 38.9M contexts. That is ~40 function calls per output facet;
it is what BOSL2 costs.

---

## Open leads (revised again, ranked by counted events)

1. **The function-lookup return protocol — 98.6M probes.** 21.3M lookups at a
   mean chain depth of 4.62. Each probe is a virtual call returning
   `boost::optional<CallableFunction>`, and `CallableFunction` is a
   `std::variant<const BuiltinFunction *, CallableUserFunction, Value,
   const Value *>` — it holds a `shared_ptr` and a `Value`, so *saying "not
   here"* runs variant construction and destructor dispatch, 98.6M times.
   Fix: give the walk a cheaper protocol — an out-parameter, or a `probe`
   that returns a raw discriminated pointer and only builds the
   `CallableFunction` once, at the frame that matched. This is the largest
   single item in the profile (~10%) and the arithmetic explains why.

   Note the asymmetry with variable lookup, which is 60.6M lookups at depth
   2.47 and now costs a fraction as much: variables resolve near the top of
   the chain, functions run most of the way down it.

2. **`FunctionCall::evaluate` creates a context it never uses — 20.9M of the
   38.9M contexts.** `expression_context` is an empty child of `context`,
   created only to be a slot the tail-call loop can replace with
   `simplified_expression->new_context`. Because it holds no variables, the
   first `simplify_function_body(this, *expression_context)` is equivalent to
   passing `context` itself. Making it lazy removes over half of all contexts
   built during evaluation.

   Delicate: `ContextFrameHandle::release()` asserts it is on top of the
   special-variable stack, so the order in which handles are constructed,
   replaced and destroyed matters. Wants a `boost::optional<ContextHandle>`
   plus a separate `shared_ptr<const Context>` for the current context, and
   care that `print_trace` still has something to report against.

3. **`Arguments` never reserves — 20.9M vectors grown by `emplace_back`.**
   `Arguments::Arguments` loops `emplace_back` over the argument expressions
   with no `reserve`, so a three-argument call reallocates twice.
   `std::vector<Argument>::__emplace_back_slow_path` and
   `__swap_out_circular_buffer` are both visible in the profile. One line.

4. **8.9M single-variable loop contexts.** Every `for` and list-comprehension
   iteration allocates a `Context` to hold exactly one variable
   (`forContext`). If the previous iteration's context was not captured by a
   closure — `use_count() == 1` after the body returns — it could be reused
   with the variable overwritten instead of reallocated. Needs care: function
   literals capture their defining context, and the garbage collector reasons
   about `use_count()`, so the check has to be exact.

5. **`Value::clone()` is an atomic refcount bump.** `VectorType`,
   `ObjectType`, `FunctionType` and `RangeType` are all `shared_ptr`-backed,
   so cloning a Value — which the evaluator does constantly, 164 samples'
   worth — is a lock-prefixed increment, and destroying it another. A session
   evaluates on one thread, but the process is multi-threaded, so libc++ has
   no way to know that. Would mean a non-atomic refcount inside `Value`;
   large, and only safe if nothing ever hands a `Value` to the geometry
   worker.

6. **`FileContext::lookup_local_function` re-resolves `usedlibs` every time**,
   calling `SourceFileCache::instance()->lookup(m)` — a string-keyed hash —
   for each `use`d file on every miss. Sits directly on the depth-4.62 walk in
   lead 1. Caching the resolved `SourceFile *` on the `SourceFile` removes it.

7. **Early termination for `!`** — unchanged from the original list. Still
   ~30 lines, still the only cheap way to make `!` prune evaluation.

8. **Lexical-path prune for `!`** — unchanged.

9. **`Parameters`' string-keyed accessors.** `parameters["size"]` interns at
   runtime (~0.3%). Fixing it properly means interned constants at every
   builtin accessor site: a few hundred edits for a small win.

### Beyond script evaluation

The balance has shifted. At the start of this effort script evaluation was 79%
of the total; it is now 68% (7.69s of 11.28s), and geometry evaluation is 25%
(2.87s) — untouched so far, and the next largest block once the leads above
are spent.

## Reproducing (additions)

```bash
# Node tree size
$BIN -o /tmp/tree.csg u-bot.scad && grep -c 'group()' /tmp/tree.csg

# Event counts: temporary counters as described above, dumped from a static
# destructor. Not committed; see the numbers in this entry.
```

---

## 2026-08-26 (evening) — Working the counted leads

Starting point after `e87051a1d`: script evaluation 7.78s, total 11.47s
(medians of 3). Run-to-run spread on this machine is about ±1%, so anything
under ~1.5% is not distinguishable from noise; two changes below landed in
that band and are recorded as such.

| Commit | Script evaluation | |
|---|---|---|
| — | 7.78s | starting point |
| `ecb98d8c6` perf: reserve the argument vector | 7.54s | −3.0% |
| `73f6ee66f` perf: make function lookup skip and probe more cheaply | 7.22s | −4.3% |
| `75d730448` perf: build a function call's context only when it is needed | 6.84s | −5.3% |
| `db45f4cc6` perf: index the context frames that are not small | 6.57s | −3.9% |

**Net: script evaluation 7.78s → 6.60s (−15.1%); total 11.47s → 10.34s
(−9.8%).** Exported STL byte-identical at every step. Regression suite
2721/2724 throughout, the three failures being the same pre-existing
`export-svg*_spec-paths-arcs01` content diffs. Peak RSS is now 1.55GB,
against the 1.67–1.79GB recorded earlier in this journal.

### Function lookup: the counted lead was right, the diagnosis was not

Lead 1 predicted the cost was the `boost::optional<CallableFunction>` each
frame builds to say "not here". Instrumenting the walk on `u-bot.scad`:

```
lookups 21,347,022   frames walked 98,633,951   skipped 63,445,009   probed 35,188,942
hit builtin 13,841,884   hit scope 7,495,822   hit variable 9,316   miss 0
resolution depth: modal 4, range 2-13
```

Two things fall out of that. Nothing ever misses, and **two thirds of all
calls resolve to a builtin**, which means the typical lookup walks past
several frames holding no function at all, fails a lookup in the file scope,
and succeeds in the builtin table.

So the frames were given a 64-bit filter (one bit per interned name, unioned
from the scope's functions and from any variable set to a function value; a
clear bit is conclusive). It rejects 63.4M of the 98.6M probes — and on its
own, against the hashed tables, **it measured as noise**. The frames it skips
were the cheap ones: probing an empty `ValueMap` costs almost nothing.

What did pay was replacing the scope and builtin tables with `IdentifierMap`,
open-addressed on the interned name's index instead of hashed and chained
(−3.9%). And *then* the filter is worth 2.2%, measured by stubbing
`may_hold_function()` to `return true`. Both are in `73f6ee66f`.

`boost::optional` was never the problem: it holds the variant in aligned
storage behind a bool, so an empty one neither constructs nor dispatches.
The journal's earlier reading of that — "*saying 'not here'* runs variant
construction and destructor dispatch, 98.6M times" — was wrong.

### The comment above ValueMap was measuring the wrong population

Lead 2's context saving was straightforward and landed as predicted (−5.3%,
`75d730448`; see the commit for the one semantic subtlety, which is that the
empty placeholder context had no config variables of its own to copy and that
had to be preserved deliberately).

Removing it exposed the real one. `ContextFrame::lookup_local_function` and
`Context::lookup_variable` were the two hottest entries in the profile after
`FunctionCall::evaluate`, which made no sense for frames the journal had
measured as holding 0.69 variables on average. Counting the scan itself:

```
finds 171,327,636   comparisons 1,363,408,667   mean 7.96 per find
  size 0     : 19,179,981
  size 1     : 74,840,473
  size 2     : 19,795,422
  size 3-4   : 20,135,583
  size 5-8   : 17,624,748
  size 9-16  :    746,882
  size 17-32 :  2,929,992
  size 33-64 :    355,819
  size >64   : 15,718,736
```

**1.36 billion pointer compares**, and the 15.7M finds against frames holding
more than 64 variables account for ~92% of them. The frame histogram in the
earlier entry is correct and irrelevant: it counts *frames*, and the frames
that matter are counted by *lookups*. `include` merges scopes, so a BOSL2
script's file scope holds every constant the library defines, and it sits at
the bottom of every chain — any name it does not hold is paid for by scanning
all of it.

`ValueMap` now builds a side index past 16 entries (`db45f4cc6`). Thresholds
of 8, 16 and 32 all measured the same to within noise.

### Tried and dropped

- **`Parameters::release_frame()`**, handing the parameter frame to the target
  context in place instead of returning it by value from `to_context_frame()`.
  `ContextFrame::ContextFrame(ContextFrame&&)` is 85 profile samples, but the
  change measured at zero: `small_vector` steals its heap pointer once a frame
  outgrows its two inline entries, so the move was already cheap. Reverted.

- **Lead 6, caching `usedlibs` resolution**, is not measurable on this model:
  `u-bot.scad` and everything it pulls in use `include`, not `use`, so
  `source_file->usedlibs` is empty and the loop never runs. The
  `SourceFileCache` hash lookups still visible in the profile are from parsing
  and dependency handling. The lead is still real for scripts that do `use`,
  and `FileContext` now has to accept every name in its filter when
  `usedlibs` is non-empty, which makes it slightly more expensive than before
  for those scripts.

### Where the time is now

Unfolded profile (`-Wl,-no_deduplicate`), main thread, 4810 samples. Grouped,
because nothing dominates any more:

```
~10%  context churn      ~ContextFrame 103, ContextFrame(&&) 85, insert_or_assign 108,
                          clear 49, set_variable 45, ContextHandle 30, push_frame 29,
                          release 28, ~Context 24
 ~7%  raw allocation      operator new 171, operator delete 160
 ~7%  Value copy/destroy  clone 141, variant destroy/move dispatch ~300 across alternatives,
                          VectorObjectDeleter 57, __release_weak 47
 ~7%  argument marshalling Parameters::parse 100, allocator<Argument>::construct 69,
                          Arguments::Arguments 56, vector<Argument>::reserve 55,
                          __swap_out_circular_buffer 35
 ~7%  FunctionCall::evaluate 329 (simplify_function_body inlined)
 ~6%  variable lookup     try_lookup_variable 206, lookup_variable 33,
                          try_lookup_special_variable 26
 ~5%  function lookup     FileContext 82, LocalScope::lookup 68, Context::lookup_function 54,
                          ContextFrame::lookup_local_function 30
 ~3%  expression eval     BinaryOp 143, Lookup 47, ArrayLookup 32, Vector 26, checkUndef 40
 ~2%  loop machinery      doForEach 92
 ~2%  garbage collection  collectGarbage 84
 ~2%  genuine arithmetic  multvecmat 97
```

`ContextFrame::lookup_local_function` went from 250 samples to 30, and the
file-scope scan behind `Context::lookup_variable` is gone.

---

## Open leads (revised again)

1. **Argument and parameter marshalling — ~7%, 20.9M times.** Every call
   builds an `Arguments` vector of `{optional<Identifier>, Value}`, then
   `Parameters::parse` matches it against the parameter list into a fresh
   `ContextFrame`, which is then moved into the body context. Three
   containers and two passes over the values to get N arguments into N slots.
   A call whose arguments are all positional and match the parameters in
   order — the common case — could write straight into the body context.

2. **`Value` copy and destroy — ~7%.** Unchanged from the previous list as an
   analysis (lead 5 there), but it is now proportionally larger. Two separable
   halves: the `shared_ptr` refcounting, which is atomic for no reason a
   single-threaded session needs, and libc++'s `std::variant` dispatch, which
   is an indirect call through a function table rather than a switch. The
   second may be the cheaper one to fix: a hand-rolled tag and switch in
   `Value`'s move and destroy paths, without touching the value model.

3. **Context churn — ~10%, 18M contexts.** Down from 38.9M after
   `75d730448`. 8.9M of what is left are single-variable loop contexts
   (`forContext`), which is the old lead 4: if the previous iteration's
   context was not captured by a closure, it could be overwritten instead of
   reallocated. The rest are function body contexts, which are harder to
   avoid.

4. **Variable lookup — ~6%, 60.6M lookups at depth 2.47.** The frames are
   small now and the file scope is indexed, so what is left is the walk
   itself. A filter like the one function lookup got would not help: the
   frames on a variable chain are the tiny ones, and scanning one entry is
   already cheaper than a bit test plus a branch.

5. **`collectGarbage` — ~2%.** Scans the weak-pointer list of live contexts.
   Worth looking at what triggers it now that there are half as many
   contexts; the cadence may no longer suit the population.

6. **Early termination for `!`** — unchanged from the original list. Still
   ~30 lines, still the only cheap way to make `!` prune evaluation.

7. **Lexical-path prune for `!`** — unchanged.

8. **`Parameters`' string-keyed accessors** — unchanged, still a few hundred
   edits for ~0.3%.

### Beyond script evaluation

Script evaluation is now 64% of the total (6.60s of 10.34s) and geometry
evaluation is 29% (3.00s), still untouched. It was 18% when this effort
started.

## Reproducing (additions)

```bash
# Function-lookup census: temporary counters in Context::lookup_function
# around the chain walk (lookups, frames walked, frames skipped by the
# filter, which alternative of CallableFunction won, resolution depth),
# dumped from a static destructor. Not committed.

# ValueMap scan census: temporary counters in ValueMap::find (calls, steps,
# and a histogram of map.size() at each call). Not committed.

# Is a candidate filter/branch actually paying? Stub the predicate and
# rebuild, rather than reverting the whole change:
#   bool may_hold_function(...) const { return true; }
```

---

## 2026-08-27 — A profiler for the script, not the interpreter

Every count in this journal so far came from hand-patching temporary counters
into the evaluator and rebuilding. That answered "what does the interpreter
do a lot of" but never "which of *my* lines causes it", because at the C++
level every script-level call looks like the same handful of frames.

Nothing existed for this. The tree had no profiling code at all — `--summary
time` (added earlier in this effort) and `--trace-usermodule-parameters` were
the whole toolkit. Upstream has an open request,
[#6653](https://github.com/openscad/openscad/issues/6653) with PR #6654, for
`timer_new()`/`timer_stop()` builtins: manual instrumentation you sprinkle by
hand, useful only once you already suspect the right code.

### `05cc58632` profile: count script-level calls per source location

`--profile` counts function calls, module instantiations and loop iterations
per source location and reports them after evaluation. `--profile-file` writes
every site as TSV, which a real model needs — `u-bot.scad` has 3768 sites.

Counting rather than timing, deliberately: there are ~30M script-level events
in this model, so reading a clock twice per event would cost more than the work
being measured and distort it. Counts are exact, and they localise the problem
regardless of what each event costs.

Hook points, each the single place an event can be counted neither twice nor
never:

- **calls** — the `FunctionCall` branch of `simplify_function_body`. Not
  `FunctionCall::evaluate`: the tail-call loop reaches a call's body through
  that branch too, so counting in both would double-count and counting only in
  `evaluate` would miss tail calls.
- **modules** — `ModuleInstantiation::evaluate`.
- **loop iterations** — the innermost point of `doForEach`, several frames
  below the AST node that owns the count, so the count travels down as a
  `ProfileSite`. This counts *body executions*; the journal's earlier
  `forContext` figure (8,941,394) counts *variable bindings*, which is why
  `--profile` reports slightly fewer (8,916,824): a nested `for(a=…, b=…)`
  binds twice per innermost iteration.

Sites register on first use and keep their count in the AST node, so the
steady state is an increment behind one global test. Measured on `u-bot.scad`:
6.52–6.56s with profiling off, 6.38–6.43s on, against 6.53–6.63s before the
commit — no measurable cost either way.

The registry holds pointers into the AST, so `ScriptProfile::ScopedRun` in
`SourceFile::instantiate` drops them when evaluation ends, including when it
throws. Registration and clearing take a lock because the GUI's animation
prefetch re-evaluates the same AST on a worker thread; the per-event
increments are unsynchronised by design, so two concurrent evaluations can
lose counts against each other but cannot corrupt the registry.

### What it says about u-bot.scad

```
  function calls            21,348,240  at 2,602 sites
  module instantiations        262,419  at 975 sites
  loop iterations            8,916,824  at 191 sites

Busiest names                    Busiest sites
 8,916,824  loop  for             2,350,057  call  is_num      BOSL2/utility.scad:196:25
 2,779,989  call  is_undef        2,057,701  call  is_nan      BOSL2/utility.scad:196:39
 2,778,579  call  is_num          2,004,006  call  is_finite   BOSL2/vectors.scad:49:50
 2,368,089  call  is_list         2,004,006  loop  for         BOSL2/vectors.scad:49:36
 2,350,057  call  is_finite       1,214,287  loop  for         BOSL2/utility.scad:292:16
 2,197,303  call  len               850,290  loop  for         BOSL2/linalg.scad:231:9
 2,057,701  call  is_nan            806,645  call  concat      BOSL2/transforms.scad:1569:36
 1,260,111  call  norm              640,056  call  is_list     BOSL2/vectors.scad:49:5
```

**Roughly 60% of the model's 21.3M function calls are BOSL2 type assertions**
(`is_undef`, `is_num`, `is_list`, `is_finite`, `is_nan`, `is_vector`,
`is_bool` ≈ 12.8M). They are not spread thin; they funnel through one
function:

```scad
// vectors.scad:49
function is_vector(v, length, zero, all_nonzero=false, eps=EPSILON) =
    is_list(v) && len(v)>0 && []==[for(vi=v) if(!is_finite(vi)) 0]
    && ...
// utility.scad:196
function is_finite(x) = is_num(x) && !is_nan(0*x);
```

`is_vector` walks the vector, and each element costs `is_finite` → `is_num` +
`is_nan`. One `is_vector([x,y,z])` is therefore ~14 script-level events. There
are 640k of them, called mostly from `linalg.scad:51`, `vectors.scad:224` and
`transforms.scad:1542` — so `is_vector` and its fan-out account for something
like 9M of the ~30M events in the run, and none of it computes any geometry.

Totals by library file:

```
8,764,492  BOSL2/utility.scad     2,428,970  BOSL2/vnf.scad
7,399,236  BOSL2/vectors.scad     2,416,184  BOSL2/linalg.scad
4,346,638  BOSL2/transforms.scad    993,621  BOSL2/affine.scad
```

The script's own files barely register: the busiest non-BOSL2 site is 91,782
loop iterations in `lib/globals.scad:29` (a `reverse()` that shadows BOSL2's
own), 0.3% of the total. **Nothing the author writes is the problem; the
library's argument validation is.**

That reframes the remaining interpreter work. The leads below make the
evaluator faster at running type predicates. The larger prize is not running
them: a way for a library to compile out its own validation, or for the
evaluator to recognise and fold these predicates. Neither is in scope here,
but it is the reason this model spends 6.5s evaluating a 24KB script.

## Reproducing (additions)

```bash
$BIN --profile -o /tmp/out.stl u-bot.scad                    # console report
$BIN --profile-file /tmp/prof.tsv -o /tmp/out.stl u-bot.scad # every site

# Who calls a given function, busiest first
awk -F'\t' '$3=="is_vector"' /tmp/prof.tsv | sort -t$'\t' -k1,1nr | head

# Events attributed to each file
awk -F'\t' 'NR>1 {tot[$4]+=$1} END {for (f in tot) print tot[f], f}' \
  /tmp/prof.tsv | sort -rn | head
```
