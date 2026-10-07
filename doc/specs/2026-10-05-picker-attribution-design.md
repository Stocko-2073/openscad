# Picker names the primitive inside render() and in the F6 view

Status: implemented, 2026-10-05.

Update, 2026-10-06: the preview is gone (see `2026-10-05-f6-only-design.md`),
so only the F6 path below remains. `pick::findLeaf()` and the depth-chosen
crossing are removed, and the select pass reads back only the depth.

Update, 2026-10-07: `hull()` and the other whole nodes no longer depend on
the geometry cache keeping them. Renders that reuse earlier ones never look
inside a part they find in the cache, so once the cache is full it drops
what is inside first: on u-bot, with the caches at 5,000 MB, 23 edits of a
number after an F6 left 359 of its 370 whole nodes uncached (44, all empty,
never were), and right-clicking those parts showed no menu until the next
F6. Each render now holds their geometry for the picker (see Memo).

Right-clicking the 3D view lists the chain of nodes under the cursor,
primitive first, then each parent up to the top level. Hovering an entry
highlights its source in the editor. The chain now reaches the primitive
under the cursor even when it is inside a `render()` in preview, or part of
the F6 result.

## Motivation

The picker's ID pass draws each preview leaf in a color made from its node
index and reads back the color under the cursor. Two kinds of mesh hide
their primitives from it:

* **render() in preview.** `CSGTreeEvaluator` evaluates a `render()` subtree
  into one leaf carrying the render node's index, so the menu stopped at
  `render`.
* **F6.** The result is one mesh with no per-node colors. `rightClick()`
  returned early because F6 never builds `rootProduct`, so there was no menu
  at all.

Both meshes still write correct depth, which is all the new code needs.

## Algorithm

`core/PickAttribution` holds the GL-free part; `MainWindow::pickPrimitives()`
drives it.

1. **Pick.** The select pass reads the ID and the depth from the same pixel.
   It also fixes an off-by-one row: it now reads `height - 1 - y`.
   `QGLView::pickObject()` unprojects that pixel's center to a world-space
   ray, from the near clipping plane (t = 0) to the far plane (t = 1), and
   the drawn depth to `depthT` on that ray.
2. **Surface.** The surface is the mesh the click landed on:
   * Preview: the render() leaf from the CSG products, placed by its
     matrix.
   * F6: the 3D meshes of `rootGeom`. Faces that are neither triangles nor
     part of a convex mesh are tessellated, so concave faces aren't
     fan-split.
3. **Crossing.** Choose where the ray crosses the surface:
   * Preview: the crossing nearest `depthT`, because a subtracted
     `render()` shows its far side.
   * F6: the first crossing, since the view shows exactly this surface.

   This gives the hit point P and the hit face's outward normal N.
4. **Candidates.** `collectLeaves()` walks the subtree the way
   GeometryEvaluator builds it:
   * It looks through groups, modules, booleans, `color()` and `render()`,
     and accumulates transforms.
   * Primitives, extrusions, imports, `hull()`, `minkowski()`, `resize()`,
     `fill()` and physics nodes stay whole. Only 3D ones are kept.
   * `%` subtrees below the start node are left out, as F6 leaves them out.
     The start node's own `%` or `#` is ignored, so a `%render()` drawn in
     preview can still be looked into.
   * Geometry comes from `GeometryEvaluator::evaluateGeometry()`. For
     `hull()` and the other whole nodes (`pick::isWhole()`) it comes from
     what the window holds for the result, or else from the cache, so a
     click never re-runs one.
5. **Match.** A primitive matches when one of its faces passes within a
   tolerance of P and is parallel to N.
   * If no face is parallel, a face merely touching P counts, ranked by how
     parallel it is.
   * The strict tolerance is tried first, the lossy one only when strict
     finds nothing:

     | Tolerance | Distance | Parallel | Covers |
     |---|---|---|---|
     | strict | `max(5e-8, 1e-9·max(M, D))` | \|cos\| ≥ 1 − 1e-6 | Manifold copies input vertices into its result and collapses edges only within 1e-12 of the coordinates |
     | lossy | `max(strict, 4e-6 + 2.5e-7·M)` | angle ≤ 1e-2 rad | CGAL snaps to a 2^-20 grid and returns float coordinates |

     M is the largest coordinate of P and of the surface's bounding box; D
     is that box's diagonal.
   * A face cut by `difference()` lies on the cutting primitive's face, so
     it is attributed to that primitive.
6. **Order.** Faces are wound counter-clockwise from outside throughout
   (primitives, polyhedron after its reversal, Manifold output), so the
   sign of n·N tells whether a primitive adds material at P or cuts it
   away. Material comes first, then source (node index) order. Where a
   face is both added and cut away, the primitive adding it made it.
7. **Menu.** The first match's chain is the menu, built by
   `addPickerMenuSteps()` exactly as before. Each further match gets an
   "Also here: …" submenu, up to 8. Hovering a submenu's title highlights
   its primitive. A submenu is left out when "Restrict viewer right-click
   menu to current file" leaves it empty, or when it would show the same
   first entry as one already listed. When nothing matches, the menu is
   what it was before: the render node's chain in preview, and no menu in
   F6.
8. **Memo.** The candidates and the F6 surface are built on the first
   right-click, under a wait cursor, and kept until the next compile, F6,
   tab close or animation frame. A failed build isn't retried on every
   click. Cheap per-triangle culls in plain doubles keep clicks fast. On a
   150k-triangle F6 result in an unoptimized build, the first right-click
   took about 0.1 s and later ones about 30 ms.

   The whole nodes' geometry is held by each render, after its result,
   on the render thread (`pick::holdWholeGeometry()`): for each whole node
   that `collectLeaves()` would reach, by digest, what the last render held,
   else the cache's entry, else the node evaluated again (an edit undone
   after the cache dropped its part). The window keeps it with the result
   and hands it to the next render; F6 and closing the tab drop it. The
   walk goes only where the digests say a whole node is below
   (`Tree::hasWholeBelow()`): on u-bot, 18k of 470k nodes, 1.1 ms a render.

The pick runs on the GUI thread, but never while `GuiLocker` is held, so
never during a compile or render.

## Limitations

* `hull()`, `minkowski()`, `resize()`, `fill()`, physics nodes,
  extrusions, imports and `text()` are named as a whole, as in preview.
* 2D designs keep the old behavior in both views.
* Nothing is attributed while an animation frame is shown: its products
  come from a separately instantiated tree, whose node indices don't match
  `rootNode`.
* The pick has logical-pixel resolution, as before.
* Coincident faces are all listed: material first, then in source order.

## Tests

* `OpenSCADUnitTests "[pick]"`, GL-free. The tests parse and instantiate
  `.scad` text, evaluate it as F6 does, or build the CSG products as preview
  does, and cast rays. They cover:
  * ray/triangle intersection, closest point on a triangle, and choosing a
    crossing by depth;
  * cut faces naming the cutter;
  * material before carver;
  * a face 0.001 below the surface left out;
  * `#` primitives included, `%` and 2D ones left out;
  * `hull()` and `minkowski()` named as a whole, also once the cache has
    dropped them if held; what is held taken over from the last render,
    else from the cache, `%` and nested whole nodes left out;
  * nested modules inside a translated and rotated `render()`, in both
    views, including the chain through the module call;
  * a subtracted `render()` resolved by depth;
  * `%render()` and `#render()`;
  * mirrored and far-off geometry;
  * a concave polyhedron's notch;
  * a 2D result;
  * the CGAL backend (`[cgal]`).
* `OpenSCADUnitTests "[digest]"`: which nodes have a whole node below,
  carried by copies.
* GUI test `TestEvalMemo::pickerNamesWhatTheCacheDropped`: a render that
  finds a part in the cache, whose `hull()` the cache dropped, leaves it
  named by a right-click.
* The GUI wiring (select pass depth, menu, memo resets) is checked by hand:
  * F5 with `render()`;
  * F6 with Manifold and with CGAL;
  * orthographic and perspective;
  * Retina and non-Retina screens;
  * the current-file-only preference;
  * animation;
  * a hidden editor dock.
