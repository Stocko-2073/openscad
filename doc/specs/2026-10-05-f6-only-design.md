# F6 is the only view: preview mode removed

Status: implemented, 2026-10-06.

The OpenCSG preview (F5) is gone. Every view of a design is a render (F6): the
mesh that gets exported. `$preview` is always false.

## Motivation

The preview existed because F6 used to be slow. With Manifold it isn't. On the
release build, from the command line with the output discarded:

| design | F6 | preview |
|---|---|---|
| BOSL2 logo, boolean_geometry, spring_handle | 0.5–0.9 s | 0.6–0.9 s |
| BOSL2 jigsaw_puzzle | 1.5 s | 2.7 s |
| BOSL2 fractal_tree (script evaluation bound) | 6.7 s | 6.7 s |
| minkowski, convex / nonconvex | 0.09 / 0.13 s | 0.18 / 0.23 s |
| hull of 200 spheres | 0.57 s | 0.68 s |
| plate with 2025 holes | 1.1 s | 1.3 s |
| 40 lines of extruded text | 0.28 s | 0.39 s |
| union of 1000 overlapping `$fn=64` spheres | 10.5 s | 2.0 s |

On u-bot, a real design that sets `$fs = $preview ? 2 : 0.25`, F6 took 9.9 s and
the preview 6.3 s, and F6 with the coarse facets also took 6.3 s: the whole gap
was the `$preview` flag, not the pipeline.

Meanwhile every feature had to be built twice, once on the CSG products and once
on the F6 mesh (the picker, the interference check, animation), and the two
views disagreed wherever a design read `$preview`.

## What changed

### The 3D view

* F5 and F6 both render (Design > Render). F4 is Reload and Render. The View
  menu's Preview (F9) and Thrown Together (F12), Design > Display CSG Products,
  the preview toolbar buttons and the "3D Preview (OpenCSG)" preferences are
  gone.
* Everything that previewed now renders: auto-reload, the Customizer's
  Automatic Render, number scrubbing in the editor, the reload AppleEvent, a new
  window or tab, the interference toggle, and a compile that waited for iCloud
  downloads.
* A render asked for while another one runs is queued, and requests made
  meanwhile make one render, so live updates end on the latest value
  (`MainWindow::actionRender()`, `renderWhenUnlocked()`).
* The view keeps showing the last result until the next one is done, instead of
  going black for the length of the render. A compile with nothing to render,
  such as after a parse error, still clears it.

### `#` and `%`

F6 renders a `#` subtree as ordinary geometry and leaves a `%` subtree out, so
neither showed. `overlay::collect()` (`core/ModifierOverlays`) walks the tree the
way GeometryEvaluator places it and returns each `#` and `%` subtree as a
world-space mesh:

* It looks into transforms, booleans, groups, `color()`, `render()`, `hull()`
  and the other whole-object operations, and into `physics()` at its simulated
  pose. It doesn't look into the children of extrusions, `projection()`,
  `offset()` or `roof()`: those are outlines the node reshapes.
* A `#` subtree is drawn whole, so `#` inside it adds nothing. A `%` inside
  either kind is left out of that subtree's geometry, so it is drawn on its own.
* The root's own `%` is ignored, since F6 renders the root anyway.
* A `%` list (lazy union) is evaluated child by child, because GeometryEvaluator
  skips a `%` list given to it whole.

The render worker collects them after the geometry (so `#` subtrees come from
the cache), and `VBORenderer` draws them after it: translucent, in the highlight
and background colours, tested against the geometry's depth without writing any,
and pulled forward with a polygon offset so a `#` operand lying on the surface
tints it. They are not picked or measured, and count towards View All. A design
of only `%` subtrees still shows them. Command-line PNGs draw them too.

### Interference

The check runs on the render worker after the geometry, when the toggle is on.
F6 unions the parts, so the overlap is inside the result: each collision keeps
its overlap manifold, and the view draws it red through the model (no depth
test). The console report is unchanged.

The command line's per-primitive attribution no longer builds a CSG tree.
`pick::collectLeaves()` records which leaves an odd number of `difference()`s
subtract (with lists flattened and `%` operands skipped, as GeometryEvaluator
combines them) and the attribution keeps the others. Unlike the CSG tree, it
counts `#` operands and names primitives inside `render()`.

### Animation

The prefetch workers evaluate each frame's geometry and overlays, as F6 does,
and the view draws them as a render. Dump Pictures saves each picture when that
step's render is done, and playback waits for it. The right-click menu stays
closed while a frame is shown.

### The picker

Only its F6 path is left: `pick::findLeaf()` and the depth that chose a crossing
of a preview `render()` leaf are gone, and the mouse selector reads back only the
depth.

### The command line

* A PNG is always rendered. `--render=force` keeps its meaning.
* `--preview`, `--csglimit` and `.term` (CSG term) export are gone.
* `$preview` is false for every output.

### Removed code

`CSGTreeEvaluator`, `CSGNode` and the CSG products, `CSGTreeNormalizer`,
`CsgInfo`, the OpenCSG and Thrown Together renderers and shader, the OpenCSG
capability check and warning dialog, and the OpenCSG dependency (the submodule,
`USE_BUILTIN_OPENCSG`, `FindOpenCSG.cmake`, and the dependency scripts' entries).
GLAD is the default OpenGL loader. The `opencsg-face-*` colour-scheme keys stay:
F6 colours faces with them.

## What is gone for good

* Thrown Together, and with it the purple inverted faces BOSL2's `debug_vnf()`
  relies on.
* Display CSG Products and the OpenCSG element limit.
* `$preview` as a draft switch: designs that use it to render coarser while
  editing now always get the final branch. BOSL2's `ruler()` draws only when
  `$preview` is true, so it draws nothing.
* Preview's speed on very large unions of curved primitives.

## Tests

* `[overlay]` unit tests cover the collector: a `#` cutter, `%` under
  transforms, nested modifiers, `%` lists with and without lazy union,
  `render()`/`hull()`/extrusions, 2D shapes, mirroring, the root's `%`, and
  `physics()`.
* `[animate]` unit tests run a frame task: geometry at the frame's time with
  `$preview` false, and its overlays.
* `[pick]` tests cover the subtracted flag, including `%` and lazy-union
  operands. The interference echo and JSON output is unchanged.
* The preview, Thrown Together, lazyunion-preview, physics-preview and CSG term
  suites are gone. The PNG option tests (camera, image size, colour schemes,
  view options, edges) are regenerated from renders; `preview-*` groups are
  renamed `render-*` (and `reimport-*` for the export/re-import ones). Render
  tests of designs with `#` or `%` show the overlays, and the imports, prune test
  and modifier tests that only the preview suites covered render with Manifold.
