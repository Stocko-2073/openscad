# `--interference-check` — headless interference report

Status: implemented, 2026-09-10.

Exposes the preview's static interference check (commits `04e186749`,
`55981f4be`, `a65b44ad3`) on the command line, and extends it with a
per-primitive attribution so that a machine reader can tell *which*
source lines produced the overlapping material.

## Motivation

The GUI check flags overlapping top-level parts and recolors them red,
which is enough for a person looking at the preview. An AI agent driving
`openscad` from a shell has neither the preview nor the right-click
picker menu, so it needs the same two things in text form:

1. whether any parts interfere, and by how much;
2. for each interference, the chain of source locations that leads to
   the overlapping primitives — the same chain the picker menu shows when
   "Restrict viewer right-click menu to current file" is on, so that only
   lines the agent can actually edit (in the file it was given) appear.

## CLI surface

```
openscad model.scad --interference-check -o model.stl
openscad model.scad --interference-file report.json -o model.stl
```

* `--interference-check` runs the check during any export and writes one
  JSON document to stdout.
* `--interference-file <path>` writes it to a file instead; `-` means
  stdout. It implies `--interference-check`.
* The check runs for every export format (including `echo`, `csg`,
  `ast`), since it evaluates the part geometry itself.
* Exit status stays `0` when collisions are found. Non-zero still means
  "the run failed", as everywhere else on the command line. Readers gate
  on `summary.has_interference`.
* Errors (exit `1`): combining a stdout report with `-o -` or
  `--summary-file -`; combining with `--animate`; a build without the
  Manifold backend.
* The Warning/Echo lines the GUI console shows are still logged (to
  stderr, or into the `.echo` export). They are logged *after* the JSON is
  written, so `--hardwarnings` cannot lose the report.
* The check is timed as the `Interference check` phase in
  `--summary time`.

## Algorithm

Shared module `src/geometry/InterferenceCheck.{h,cc}` (namespace
`interference`), built only with `ENABLE_MANIFOLD`.

1. **Parts.** Each direct child of the root node (after the `!` root
   modifier) is a part, numbered from 1 in child order. `%` children,
   empty or 2D geometry, and anything Manifold rejects are recorded with
   a `status` and excluded from the pair test.
2. **Pairs.** Bounding boxes cull non-touching pairs; the rest are
   intersected with Manifold, and the pair interferes when the
   intersection volume exceeds `1e-5` (flush mating faces stay below).
3. **Primitives** (CLI only; the GUI turns this off). For each colliding
   part, `CSGTreeEvaluator::buildCSGTree` produces the CSG term with
   world matrices. A leaf adds material unless it sits on the right side
   of an odd number of `difference` operations. Each positive leaf is
   transformed to world space and intersected with the pair's overlap
   region; leaves contributing more than the epsilon are reported.
4. **Chains.** `AbstractNode::getNodeByID` from the part node to the
   primitive gives the ancestor path. Steps are kept only when they have
   a source location, are not under a library path
   (`get_library_for_path`), and are in the input file — the three rules
   the picker menu applies. Names use the picker's formatting
   (`verbose_name` with the `module ` prefix stripped, falling back to
   the instantiation name). Chains are emitted outermost first.

The GUI's `MainWindow::runInterferenceCheck` now calls the same `run()`
and `logReport()` and keeps only the recolor step, so console output is
unchanged.

## JSON document

Pretty-printed, insertion-ordered, trailing newline. Volumes are rounded
to 1e-6.

```json
{
  "schema_version": 1,
  "generator": {"name": "openscad", "version": "2026.09.10"},
  "input": {"path": "/abs/model.scad", "directory": "/abs"},
  "settings": {
    "volume_epsilon": 1e-05,
    "chain_filter": "current-file-only",
    "chain_order": "outermost-first"
  },
  "summary": {
    "parts_total": 2, "parts_checked": 2, "pairs_tested": 1,
    "collisions": 1, "has_interference": true
  },
  "parts": [
    {"number": 1, "status": "checked", "name": "cube",
     "description": "cube(size = [10, 10, 10], center = false)",
     "location": {"file": "model.scad", "path": "/abs/model.scad",
                  "line": 3, "column": 1, "end_line": 3, "end_column": 9},
     "bbox": {"min": [0, 0, 0], "max": [10, 10, 10]}},
    {"number": 2, "status": "checked", "name": "a", "description": "group()",
     "location": {"file": "model.scad", "path": "/abs/model.scad",
                  "line": 10, "column": 1, "end_line": 10, "end_column": 4},
     "bbox": {"min": [5, 0, 0], "max": [15, 10, 10]}}
  ],
  "collisions": [
    {"parts": [1, 2], "volume": 500.0,
     "primitives": [
       {"part": 1, "node_index": 2, "name": "cube",
        "description": "cube(size = [10, 10, 10], center = false)",
        "location": {"file": "model.scad", "path": "/abs/model.scad",
                     "line": 3, "column": 1, "end_line": 3, "end_column": 9},
        "volume": 500.0,
        "chain": [
          {"node_index": 2, "name": "cube", "label": "cube (model.scad:3)",
           "location": {"file": "model.scad", "path": "/abs/model.scad",
                        "line": 3, "column": 1, "end_line": 3, "end_column": 9}}
        ]},
       {"part": 2, "node_index": 6, "name": "cube", "description": "...",
        "location": {"file": "model.scad", "line": 7, "...": "..."},
        "volume": 500.0,
        "chain": [
          {"node_index": 3, "name": "a", "label": "a (model.scad:10)", "location": {"line": 10, "...": "..."}},
          {"node_index": 4, "name": "translate", "label": "translate (model.scad:4)", "location": {"line": 4, "...": "..."}},
          {"node_index": 5, "name": "b", "label": "b (model.scad:4)", "location": {"line": 4, "...": "..."}},
          {"node_index": 6, "name": "cube", "label": "cube (model.scad:7)", "location": {"line": 7, "...": "..."}}
        ]}
     ]}
  ]
}
```

Field notes:

* `parts[].status` ∈ `checked | skipped-null | skipped-background |
  skipped-empty | skipped-2d | skipped-not-manifold`. Only `checked`
  parts carry a `bbox`.
* `location` is `null` for nodes without a source reference. `file` is
  relative to the input file's directory; `path` is absolute; lines and
  columns are 1-based.
* `primitives[].location` is the primitive's own location, which may be
  in a library file even though the `chain` only lists steps from the
  input file. When the primitive itself is in another file, the last
  chain step is the outermost call in the input file that led to it.
* `node_index` is the node's index in this run's tree; it is stable only
  within one invocation.

## Limitations

* Opaque leaves — `render()`, `hull()`, `minkowski()`, `import()`,
  extrusions, `physics()` — are single CSG leaves; primitives inside them
  are not visible, and the chain ends at that call.
* Per-primitive attribution costs one Manifold construction and one
  boolean per positive leaf whose bounding box touches the overlap, for
  colliding parts only. Terms and leaf manifolds are memoized across
  pairs.
* The check uses Manifold regardless of `--backend`. With
  `--backend=cgal` the part geometry arrives as Nef polyhedra and is
  converted first, which is slow.
* Non-manifold input meshes (bad `polyhedron`, dirty STL) go through
  Manifold's repair path and may add warnings.

## Tests

`tests/data/scad/interference/*.scad` with expectations under
`tests/regression/interference-echo/` (logged lines, via the `.echo`
export) and `tests/regression/interference-json/` (the report, via
`tests/interference_json_test.py`, which replaces absolute paths and the
build version with placeholders before the structural compare). Both
groups are registered only when `ENABLE_MANIFOLD_TESTS` is set.
