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

---

## 2026-08-27 (later) — Could the type predicates move into C++?

Discussion only; nothing implemented. The profiler's finding — that ~60% of
the model's function calls are BOSL2 type assertions — raises two questions.
Can that work move into the interpreter, and can it be done without patching
BOSL2? Written up here as a handoff, because the answer is yes to both but
only via a specific mechanism.

### Most of the type checking is already in C++

Splitting the profile's calls against OpenSCAD's builtin table:

```
builtin calls (already C++) : 13,795,463   64.6%
user calls (BOSL2 .scad)    :  7,552,777   35.4%
loop iterations             :  8,916,824
module instantiations       :    262,419
```

`is_num`, `is_list`, `is_undef`, `is_bool`, `is_string`, `is_function` and
`len` are builtins already, and BOSL2 does not redefine any of them. So there
is nothing to port for the bulk of the calls; what costs is the *call*, not the
check.

Only three predicates are .scad, and they are 67% of all user function calls:

```scad
function is_nan(x) = (x!=x);                                   // 2,057,701 calls
function is_finite(x) = is_num(x) && !is_nan(0*x);             // 2,350,057
function is_vector(v, length, zero, all_nonzero=false, eps=EPSILON) =
    is_list(v) && len(v)>0 && []==[for(vi=v) if(!is_finite(vi)) 0] && …  // 640,056
```

The prize is their fan-out, not their bodies. `is_nan` has **exactly one call
site in the entire model** — inside `is_finite` — so a native `is_finite`
deletes the whole chain:

```
2,350,057  is_finite user calls  -> builtin calls (body contexts gone)
2,057,701  is_nan user calls     -> gone entirely
~2,350,000 is_num builtin calls  -> gone
~4,400,000 BinaryOps (0*x, x!=x) -> gone
─────────
  ~6.8M of 30.5M events and ~4.4M of 18M contexts, from one small builtin
```

`is_vector` accounts for most of the remaining ~10M (its comprehension is
2,004,006 iterations and 2,004,006 of the `is_finite` calls), but see below —
it is the wrong shape.

### Adding builtins alone does nothing for vanilla BOSL2

`Context::lookup_function` walks the context chain and `BuiltinContext` is at
the bottom, so a user `function is_finite(x) = …` in an included file is found
first, every time. A builtin of the same name is never reached: dead code.

### The proposal: priority dispatch, guarded by the definition

Idea from this session: a priority builtin table consulted *before* the chain
walk. Mechanically that is a one-liner in `Context::lookup_function`.

Name-only priority is not acceptable — it inverts scoping, which nothing in
the language can undo. No script could say "no, I mean mine", every script
defining a same-named function would silently change behaviour, and other
libraries define these names (`is_vector` especially) differently.

**The fix is to guard the substitution on the definition rather than the
name.** The priority table finds a candidate; the walk still resolves the user
definition; the native implementation is used only if the user definition's
parameter list and body match the one being replaced; the decision is then
cached on the call site. This never changes any script's meaning — the native
version runs only where the .scad version provably *is* what was
reimplemented — and it works against vanilla BOSL2. The priority context stops
being a scoping change and becomes a dispatch accelerator.

The inline cache is where much of the win lands: after the first call, that
site skips lookup entirely.

### The semantics are the risk, and they are not intuitive

Probed empirically rather than reasoned about (`doc/journals` reproduction
below). Both candidates surprised:

```
is_num(nan)     = false     # OpenSCAD's is_num already excludes NaN
is_nan([nan])   = true      # vector != propagates NaN element-wise
is_nan([[nan]]) = true      # and recursively
[nan]==[nan]    = false
is_finite([nan])= false     # the is_num guard fires first
```

So:

- A native `is_nan` written as `type == NUMBER && std::isnan(x)` **would be
  wrong**: it returns false where BOSL2 returns true, for any vector
  containing a NaN at any depth. The correct native form is
  `Value(arg != arg)`, reusing OpenSCAD's own comparison operator — exact by
  construction rather than by reimplementation.
- A native `is_finite` as `type == NUMBER && std::isfinite(x)` is exact. It was
  checked against all 12 value types (number, ±inf, nan, undef, bool, string,
  vector, empty vector, range, function) plus the vector-of-NaN case; the
  `is_num` guard excludes everything non-numeric.

That asymmetry is the strongest argument for the body-matching guard: it
protects against exactly this class of mistake.

### Why `is_vector` is the wrong shape

Body-matching would make it *safe*, but the native side has to carry a lot of
library semantics:

- five parameters, and `eps=EPSILON` where `EPSILON` is a BOSL2 *variable*
  evaluated in the defining context. A builtin has no such context, so its
  value (1e-9) would have to be hard-coded into the interpreter.
- an internal `assert(is_num(length))` whose message would have to be
  reproduced exactly.
- a call out to `all_nonzero`, another user function with its own default
  `eps=EPSILON`.

### Plan for next session

1. **Measure the ceiling first.** Copy BOSL2, stub `is_finite`, `is_nan` and
   `is_vector` to trivially cheap definitions, and time `u-bot.scad` against
   `OPENSCADPATH` pointing at the copy. If script evaluation drops ~25% the
   mechanism pays for itself; if it drops ~8% because the events are cheaper
   than the count suggests, the general fast paths (leads 1-3 below) win
   instead. This is ~20 minutes and decides the rest.
2. If it pays: implement guarded priority dispatch behind an opt-in flag,
   with `is_finite` only. One builtin, ~6.8M events, and the smallest possible
   semantic surface.
3. Add `is_nan` as `Value(arg != arg)` only if step 1 shows the direct
   `is_nan` calls matter — in this model there are none outside `is_finite`.
4. Leave `is_vector` alone until the general call-path work is done; revisit
   only if it is still the top item afterwards.

Whatever happens, the general leads are unaffected and compound with this:
cheap calling convention for single-argument builtins (13.8M calls), a stack
fast path for user functions whose bodies create nothing that outlives the
call (7.5M calls), and loop-context reuse (8.9M iterations).

## Reproducing (additions)

```bash
# builtin vs user split of the profile
grep -oE 'Builtins::init\("[a-z_0-9]+"' src/core/builtin_functions.cc \
  | sed 's/.*"\(.*\)"/\1/' | sort > /tmp/builtins.txt
# then classify column 3 of the --profile-file TSV against that list

# All call sites of one function, busiest first
awk -F'\t' '$3=="is_nan"' /tmp/prof.tsv | sort -t$'\t' -k1,1nr

# Predicate semantics across every value type: define BOSL2's is_nan/is_finite
# in a scratch .scad and echo them over
#   [1, -0.5, 0/0, 1/0, -1/0, undef, true, "abc", [1,2], [], [0:2], function(y) y]
# plus [nan], [[nan]], [1,nan]. Do this before writing any native version.
```

---

## 2026-08-27 (later still) — Working the general call path

Measured the ceiling from the previous entry's plan, found it did not clear
the bar the entry set, and worked leads 1 and 3 instead. Script evaluation on
`u-bot.scad` went from 6534ms to 5341ms, **-18.3%**; total render 10.0s -> 9.1s.

### First: the benchmark was lying

Every number in this journal before today was taken from two or three
consecutive runs. This machine needs **four warm-up passes** to settle: a cold
run reads ~4% slow, which is the size of most of the effects being measured.
A 13-run series made it obvious — 5502, 5596, 5516, 5426, 5590, then 5322,
5284, 5294, 5346, 5275, 5280, 5285, 5280.

`doc/journals/bench.sh` in the reproduction below discards four runs and
reports min/median over the rest. Median across 9 warmed runs is repeatable to
about ±0.5%. **Earlier entries' percentages should be treated as ±4%**, and
anything reported there under ~5% is not distinguishable from warm-up drift.

### Step 1 of the plan: the predicate ceiling, measured

Stubbed BOSL2's predicates in a scratch copy pointed at by `OPENSCADPATH`.
Geometry stayed byte-identical (md5 `4426e8e3…`) at every stage:

```
baseline                                          6425 ms   --
is_finite(x) = is_num(x)                          6065 ms   -5.6%
  + is_vector without the finite comprehension    5380 ms  -16.3%
  + is_vector stripped to a minimal body          5282 ms  -17.8%
```

(unwarmed, so ±4%; the stages are consistent with each other.)

Stubs still pay for the user-function call itself, so a native `is_finite` and
`is_vector` would go further — projecting ~21% off script evaluation. The
previous entry set the bar at "~25% and the mechanism pays for itself, ~8% and
the general fast paths win instead". 21% lands between them, and guarded
priority dispatch only ever helps scripts that use BOSL2, so the general leads
went first. **The proposal is not dead**; it now has a measured ceiling.

### `Arguments` on the stack — ~1%

20.9M `Arguments` are built instantiating this model, of which **79% carry a
single argument and 98% carry two or fewer**; only **1.2%** of calls use a
named argument at all. Switching the base from `std::vector<Argument>` to
`boost::container::small_vector<Argument, 2>` — the trade `ValueMap` already
makes — keeps 98% of them off the heap.

Worth ~1%, which is less than the allocation count suggests: 20.9M alloc/free
pairs for ~90ms is ~4ns each, because mimalloc (`97d87e05b`) had already made
them nearly free. **Kept, because builtin calls still build one**, but this is
the entry's evidence that raw allocation count is a poor proxy for cost here.

### Binding positional arguments straight into the callee — ~11.5%

This is lead 1, and it is where the time was. A user-function call did this:

```
Arguments{...}                  evaluate each argument into {optional<Identifier>, Value}
Parameters::parse(...)            match against parameters into a second ContextFrame
  ...                             evaluate defaults for whatever was not supplied
  Parameters{...}                 push that frame on the special-variable stack
body_context->apply_variables(...) move the frame's ValueMap into the body context
```

Two containers and a spare `ContextFrame`, every value moved three times, and
a special-variable stack push/pop that the function-call path never reads —
`Parameters` is constructed and immediately destructured.

When no argument is named, argument *i* is simply parameter *i*, and none of
it is needed: `bind_positional_arguments()` evaluates each argument straight
into the body context, then fills defaults. `FunctionCall::allPositionalArgs`
is fixed by the parser, so the test costs nothing at runtime.

It falls back to the general path, unchanged, for three cases:

- **more arguments than parameters**, which has to warn;
- **a config-variable parameter.** The body context is already on the
  special-variable stack, so binding a `$`-name into it early would let a
  later default expression see it. `function f($fn, x = $fn) = ...; f(5)`
  must give `x` the *outer* `$fn`, and it still does;
- **a parameter named `this`**, which `builtin_object` fills from the defining
  context in preference to anything supplied.

### Reusing the loop context — 7.4%

Lead 3. A `for` binds its variable in a fresh child context per iteration —
8.9M of them — and almost none outlive the iteration that made them.
`LoopContext` keeps one and rebinds it, allocating again only when the
previous iteration's context was captured.

The test for "was it captured" is `use_count() == 1`, which is complete
because a child context holds its parent by `shared_ptr`, so anything that
outlives the iteration — a closure, an object, a surviving child — raises the
count. `ContextMemoryManager::addContext` already makes exactly this test to
decide whether a context can be dropped instead of handed to the collector.
Allocation is lazy so a loop over an empty list still builds nothing.

Measured by A/B on warmed builds with everything else held constant: median
5709ms with reuse off, 5284ms on.

The decisive correctness case, which reuse would break if the guard were
wrong:

```scad
fns = [for (i = [1, 2, 3]) function() i];
echo([for (f = fns) f()]);      // [1, 2, 3], not [3, 3, 3]
```

That and the nested, range, string and partial-capture variants all hold.

### Where the time is now

Script evaluation is 5341ms of 9.1s. Flat self-time over the script-eval
window, 3040 active samples:

```
 9.6%  FunctionCall::evaluate 291   (simplify_function_body inlined; spread
                                     evenly over the whole body, no hotspot)
 6.6%  operator new 101 + operator delete 99
 4.7%  Context::try_lookup_variable 143
 3.9%  Value::clone 118
 5.0%  function lookup    FileContext 60, LocalScope::lookup 51, lookup_function 41
 3.3%  BinaryOp::evaluate 100
 3.2%  multvecmat 96                (genuine arithmetic)
 2.3%  ValueMap::insert_or_assign 70
 1.6%  collectGarbage 45
 1.5%  Arguments::Arguments 45      (builtin calls only now)
 0.9%  _tlv_get_addr 28             (thread-local access, StackCheck)
 0.7%  std::function trampoline 22  (builtin dispatch)
```

`Parameters::parse` and `ContextFrame::ContextFrame(&&)` have left the profile
entirely, which is the fast path doing its job.

### Checked and dropped

- **Replacing `BuiltinFunction`'s `std::function` with a tagged function
  pointer.** The trampoline is 22 samples, 0.7%. Not worth the churn.
- **Reordering the `typeid` chain in `simplify_function_body`.** `FunctionCall`
  is tested last behind four types that essentially never occur, which looked
  like a free win. It is only worth ~4 pointer comparisons: no `strcmp`
  appears in the script-eval profile, so libc++ is taking the unique-RTTI fast
  path. Left alone as not worth the restructuring risk for <1%.

### A latent crash, found by accident

Stubbing `is_finite(x) = true` sent BOSL2's `_edges` into runaway recursion
building a deeply nested list, and OpenSCAD **segfaulted on the stack guard
while destroying it** — no error, just SIGSEGV.

`VectorType::VectorObjectDeleter::operator()` (`src/core/Value.cc:674`)
already contains an iterative unwinder written to avoid exactly this, but the
whole loop body is gated on `v->embed_excess`:

```cpp
while (true) {
  if (v && v->embed_excess) {      // <-- gate
    for (Value& val : v->vec) { ...collect children into purge... }
  }
  if (purge.empty()) break;        // plain nested vector: always breaks here
  ...
}
delete orig;                       // recurses one frame per nesting level
```

`embed_excess` is non-zero only when `EmbeddedVectorType`s have been appended,
i.e. list-comprehension flattening (`Value.cc:644`). A plainly nested `VECTOR`
has `embed_excess == 0`, the unwinder is skipped, and `delete orig` unwinds
naturally: `~Value` -> variant destroy dispatch -> `__on_zero_shared` ->
deleter -> repeat, one C++ frame per level until the 8MB stack runs out.

Evaluation *built* the structure fine — `StackCheck` and the 1,000,000-depth
`RecursionException` both let it through. Only destruction dies. So a script
that survives the recursion limits can still take the process down with no
diagnostic. Pre-existing, unrelated to this branch, and not fixed here.

## Reproducing (additions)

```bash
# Warmed benchmark: four discarded passes, then min/median over N runs.
doc/journals/bench.sh 9 "label"

# Is a change actually paying? Build both ways and compare warmed medians;
# a single unwarmed pair will not resolve anything under ~5%.

# Ceiling for a library predicate, without patching the installed library:
cp -R ~/Documents/OpenSCAD/libraries/BOSL2 /tmp/libs/ && edit /tmp/libs/BOSL2/...
OPENSCADPATH=/tmp/libs $BIN --summary time --summary-file - -o /tmp/out.stl u-bot.scad
# OPENSCADPATH takes precedence over the user library dir; confirm with an
# echo() in the copy before trusting the numbers.

# Argument-shape census: temporary counters in Arguments::Arguments over
# argument_expressions.size() and whether any argument is named. Not committed.

# Script-eval-only profile: sample for ~6s from launch, so the window is
# parsing (0.15s) plus script evaluation, and read "Sort by top of stack".
# Do NOT use sample's call tree here -- BOSL2 recursion is deep enough that
# stacks get truncated and subtree attribution silently under-counts.
$BIN -o /tmp/prof.stl u-bot.scad & sample $! 6 1 -f /tmp/sample.txt; wait
```

## Open leads (revised again)

1. **`FunctionCall::evaluate` — 9.6%, spread evenly.** No hotspot; it is the
   aggregate of the `typeid` chain, `boost::optional<CallableFunction>`, and
   the `variant<SimplifiedExpression, Value>` built and destroyed per
   simplification step, where `SimplifiedExpression` carries an
   `optional<ContextHandle<Context>>`. Cutting it means restructuring the
   step protocol, not tuning a line.

2. **Function lookup — ~5%, and an inline cache would take most of it.**
   `FileContext::lookup_local_function` + `LocalScope::lookup` +
   `Context::lookup_function`. A call site's target almost never changes;
   caching the resolution on the `FunctionCall` node with a validity check is
   the same mechanism the guarded-dispatch proposal needs anyway.

3. **`Value` copy and destroy — ~4% in `clone` plus the variant dispatchers.**
   Unchanged as an analysis. Every variable read clones. The atomic
   refcounting cannot simply be made non-atomic: geometry evaluation is
   multi-threaded and the GUI prefetches on a worker thread.

4. **Native `is_finite` / `is_vector` behind guarded priority dispatch.**
   Ceiling now measured at ~21% of script evaluation for this model, 0% for
   models that do not use BOSL2. Lead 2 above builds the inline cache it needs.

5. **`collectGarbage` — 1.6%**, down from 2%. Unchanged.

6. **Early termination and lexical-path prune for `!`** — unchanged.

7. **`Parameters`' string-keyed accessors** — unchanged.

### Beyond script evaluation

Script evaluation is now 58% of total render (5341ms of 9.1s), down from 64%.
Geometry evaluation is 34% (3.1s) and still untouched.

---

## 2026-08-27 (evening) — The inline function-lookup cache

Lead 2 from the previous entry. Script evaluation on `u-bot.scad` 5249ms ->
4877ms, **-7.1%**; total render 8.70s -> 8.26s (warmed medians of 3; the
previous entry's 9.1s for the same commit was measured before `bench.sh`
existed and reads about 5% high). Exported STL byte-identical
(md5 `4426e8e3…`), regression suite 2721/2724 with the same three pre-existing
`export-svg*_spec-paths-arcs01` content diffs.

`8514e596f perf: cache each call site's function resolution`

### What makes a cached resolution valid

The lead said "a call site's target almost never changes; cache the resolution
with a validity check". The whole problem is the validity check: the walk
starts at whatever context the call is being evaluated in, and there are 18M
of those.

What a site resolves to is fixed by exactly two things.

1. **The ordered list of `LocalScope`s on the chain.** Immutable once parsed,
   and the only frames carrying one are the scope contexts (`FileContext`,
   `UserModuleContext`). So if the *nearest enclosing scope context is the same
   object*, the whole chain below it is the same objects too — a context's
   parent is fixed when it is built — and the frames above it, being ordinary
   contexts, define no functions at all.

2. **Variables holding function values.** The dynamic part, and the earlier
   census said how dynamic: of 21.3M lookups, **9,316** resolve to a function
   value.

So `Context` now carries the nearest enclosing scope context and a serial
unique to it, both inherited from the parent for free and overridden by
`ScopeContext`'s constructor. A serial rather than the pointer, because
contexts are created and destroyed tens of millions of times and addresses get
reused — an ABA compare would hand back a resolution from a dead chain.

And `Identifier` carries a flag set the first time anything binds that name to
a function value, from `ContextFrame::set_variable`, where the existing
`function_bits` filter is already maintained. Global to the process and never
cleared. That sounds crude, and it is exactly as crude as it needs to be: on
this model it fires for the 9,316 lookups that genuinely resolve to a function
value and nothing else.

The guard is then a serial compare and a flag test:

```cpp
const uint64_t serial = context->scopeSerial();
if (serial == 0 || name.isConfigVariable() || name.hasFunctionValue()) {
  return context->lookup_function(name, location());   // walk
}
```

### Only two of the four answers can be cached

`CallableFunction` has four alternatives, and two of them cannot be stored on
a call site:

- a **builtin** is a global pointer: cacheable;
- a **user function defined by the scope owner itself** is cacheable, because
  the guard has just identified the scope owner, so the defining context is
  recoverable as `context->scopeOwner()->get_shared_ptr()`. Storing the
  `shared_ptr` instead would pin a context for the life of the AST and defeat
  the context memory manager;
- a user function from a scope **further down** the chain, or one reached
  through `use` (which builds a fresh `FileContext` that is not on the chain at
  all), and a **function value**: not cached, fall through to the walk.

Caching the deeper scope hit would mean storing how far down it was, and the
depth is only lexically fixed if nothing ever interposes a context — a
property that holds today but is not enforced anywhere. The census below says
it is worth 1.1% of lookups, so it was left alone.

### The cache cannot live on the AST node

Which is where an inline cache belongs, and where the first version put it.
`UserModule.h` and `ScriptProfile.h` both already record why it cannot: the
GUI's animation prefetch evaluates **the same AST on several worker threads at
once**. `profileCount` tolerates that because a lost count is a lost count.
A three-word cache does not: thread A writes `{serial=100, builtin=X}` while
thread B writes `{serial=200, user=Y}`, and a reader sees
`{serial=200, builtin=X}` and calls the wrong function.

So the entries live on the `EvaluationSession` — one per render — in a vector
keyed by a call site number assigned to each `FunctionCall` as it is parsed.
Concurrent evaluations have separate sessions and cannot see each other's
entries at all.

Site numbers are handed back in `~FunctionCall`, because the GUI reparses on
every edit and a session sizes its cache to the highest number it sees;
without recycling the numbering would climb for as long as the process runs.
On this model the vector settles at 23,312 entries (746KB). An entry also
records which site filled it, so a number recycled mid-session cannot serve a
stale answer to its successor.

### The census

Temporary counters in `evaluate_function_expression`:

```
lookups 21,347,022
  bypassed: function-value name 9,316   config name 0   no scope on chain 0
  hit:      builtin 13,708,363   user function 7,265,664      = 98.3%
  miss:     363,679  ->  filled builtin 133,521  filled user 1,202
                         uncacheable    228,956  failed 0
```

The lookup total matches the earlier entry's census exactly (21,347,022), which
is a good sign that nothing about the call path changed.

Two things worth keeping:

- **The bypass count is exactly the "hit variable" count from the 2026-08-26
  census.** The global monotonic flag costs nothing on this model because BOSL2
  keeps functions in variables under names it does not also call directly.
- **133,521 builtin fills against 1,202 user fills**, on 13.7M and 7.3M hits
  respectively. Fills are per scope-context, so this is the shape of the
  script: user-function call sites are reached almost entirely from the root
  file scope, one serial for the whole render, while builtin call sites also
  sit inside module bodies, and every module instantiation is a new
  `UserModuleContext` with a new serial that invalidates them.
- **228,956 uncacheable** — 1.1% of lookups — is the deeper-scope case
  described above. Not worth the depth bookkeeping.

### Where the time is now

Function lookup has left the profile. `FileContext::lookup_local_function`,
`LocalScope::lookup`, `Context::lookup_function` and
`ContextFrame::lookup_local_function` are all below the reporting threshold;
what is left is `evaluate_function_expression` itself at 48 samples, which is
the guard plus the `shared_from_this()` on the 7.3M user-function hits.

Flat self-time, 2929 main-thread samples over a 4.7s window from launch
(parsing 0.15s, then script evaluation):

```
11.4%  FunctionCall::evaluate 334
 6.9%  Value variant dispatch  ~203 across move/destroy alternatives
 6.8%  raw allocation          operator new 96, operator delete 104
 6.5%  variable lookup         try_lookup_variable 159, lookup_variable 30
 4.4%  Value::clone 129
 3.4%  BinaryOp::evaluate 99
 3.1%  ValueMap::insert_or_assign 90
 3.1%  multvecmat 90            (genuine arithmetic)
 2.6%  collectGarbage 77
 2.0%  doForEach 59
 1.7%  Arguments::Arguments 49  (builtin calls only)
 1.6%  evaluate_function_expression 48
 1.4%  VectorObjectDeleter 40
 1.4%  ContextFrame::clear 40
```

Note the window matters. A 6s sample at this speed reaches ~1s into geometry
evaluation, and `_platform_strcmp` (77), `__class_type_info::search_below_dst`
(45) and `std::type_info::operator==` (39) appear — all from geometry's
`dynamic_cast`s on the node tree, none from `simplify_function_body`'s `typeid`
chain. The 2026-08-27 entry's reading that script evaluation takes libc++'s
unique-RTTI fast path still holds; a wide window just makes it look otherwise.

### Peak memory

1.70GB (three runs: 1.70, 1.72, 1.73), against 1.68GB for the same three runs
without the change (1.68, 1.75, 1.66). Unchanged — the spread between runs is
larger than any difference. **The 1.55GB recorded in the 2026-08-26 entry does
not reproduce**; a single reading of this number is worth about ±5%.

## Reproducing (additions)

```bash
# Inline-cache census: temporary counters in
# FunctionCall::evaluate_function_expression around each exit (bypassed by
# which guard, hit builtin/user, miss and what the fill decided), dumped from a
# static destructor. Not committed; see the numbers above.

# Cache footprint: temporary fprintf in
# EvaluationSession::functionLookupCache when the vector grows.

# Semantics: the cases that must not change, in one scratch script --
#   a function VALUE shadowing a builtin (sin = function(x) 999)
#   one call site whose target varies with the enclosing module instantiation
#   a function defined in a module scope, called from a file-scope helper
#     (lexical: the helper must NOT see it)
#   closures captured per loop iteration
#   a function value passed as a parameter and called by name
#   let-bound function value vs. a same-named scope function
#   `use <lib.scad>` where the used file shadows a builtin
#   nested module scopes three deep, each instantiated twice
#   tail and non-tail recursion
#   a $-named function, and an unknown function (must still warn)
# Diff the echo output against a build of HEAD~1; identical is the bar.

# Peak memory needs at least three runs each way; a single pair is noise.
```

## Open leads (revised again)

1. **`FunctionCall::evaluate` — 11.4%, spread evenly.** Unchanged as an
   analysis and now the largest single item by a wide margin: the `typeid`
   chain, the `boost::optional<CallableFunction>`, and the
   `variant<SimplifiedExpression, Value>` built and destroyed per
   simplification step, where `SimplifiedExpression` carries an
   `optional<ContextHandle<Context>>`. Cutting it means restructuring the step
   protocol. With lookup gone, this is where the next real win is.

2. **`Value` copy and destroy — ~11% together** (variant dispatch ~6.9%,
   `clone` 4.4%). Two separable halves, as before: libc++'s `std::variant`
   dispatching through a function table rather than a switch, and the atomic
   refcounting. The variant half is the tractable one — a hand-rolled tag and
   switch in `Value`'s move and destroy paths — and it is now the second
   largest item. The atomics cannot simply be relaxed: geometry evaluation is
   multi-threaded and the GUI prefetches on worker threads.

3. **Variable lookup — 6.5%.** `try_lookup_variable` is now the third item.
   The frames are small and the file scope is indexed, so what is left is the
   walk. The scope-serial machinery this entry added is the guard a *variable*
   inline cache would need too, but a variable's value changes per call where a
   function's target does not, so the cacheable thing is the frame and offset,
   not the value. Worth a census before any code.

4. **Native `is_finite` / `is_vector` behind guarded priority dispatch.**
   Ceiling measured at ~21% of script evaluation for this model, 0% for models
   that do not use BOSL2. The inline cache lead 4 was waiting on now exists,
   and the guarded-dispatch design drops straight into
   `evaluate_function_expression`: the priority table supplies a candidate, the
   walk still resolves the user definition, the bodies are compared once, and
   the answer goes in the same cache entry.

5. **`collectGarbage` — 2.6%**, up from 1.6% as a share. Scans the weak-pointer
   list of live contexts; the cadence may no longer suit a population half the
   size it was tuned for.

6. **Deeper-scope function hits — 1.1% of lookups**, uncached. Needs a stable
   depth or a scope-identity walk. Small.

7. **Early termination and lexical-path prune for `!`** — unchanged.

8. **`Parameters`' string-keyed accessors** — unchanged.

### Beyond script evaluation

Script evaluation is 58% of total render (4.83s of 8.26s), geometry evaluation
35% (2.87s) and export 5% (0.40s). Both shares are measured warm and are the
first ones in this journal that can be compared with each other -- the earlier
entries' totals are unwarmed and read a few percent high across the board.
Geometry is still entirely untouched; it was 18% of an 11.5s render when this
effort started.
