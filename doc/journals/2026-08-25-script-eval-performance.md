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
