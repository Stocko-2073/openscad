# TODO

## Animate

Left over from finding why playback re-rendered every frame.

### The first pass renders every frame from scratch

Since the preview went, a frame is a full F6, and the prefetch workers evaluate
without the module memo (`core/EvalMemo.h`), whose tables serve one evaluation
at a time. On u-bot each frame spends ~22 s evaluating the script, ten workers
at once, and 3-6 s on its geometry: ~30 s for ten frames, ~60 s for twenty.
Each worker could take a table of its own from a pool: `$t` is a recorded `$`
read, so at a new time only the calls that read it would run. Each table keeps
up to a tree's worth of nodes.

### An unchanged parse throws the frames away

F6, typing a time and a step button that misses the cache all render, and a
render parses the document again. The cache tells sources apart by their
`SourceFile` object, so it is seeded anew and every frame is rendered again
though nothing changed. It could compare what the frames depend on instead:
the text, the Customizer's values and the files included and used.

### The script's camera does not move during playback

A design that assigns `$vpr`, `$vpt`, `$vpd` or `$vpf` moves the view when the
GUI thread evaluates it (`MainWindow::instantiateRoot()`), but `FrameTask`
drops each frame's file context, so cached frames are all shown with the camera
of the last render. This dates from the frame cache (f88bbaec8).

### A changed `use`d file leaves the frames as they were

When a used file changes on disk, `handleDependencies()` has the GUI render
again, but `rootFile` stays the same object, so the cache is not seeded anew
and goes on playing frames made with the old file.

## Build

### With the Xcode 26.6 SDK, AMF and SVG import crash

`xcode-select` points at Xcode 26.6, so a fresh configure builds with its SDK,
and that build crashes importing AMF or SVG (for AMF, in mimalloc's `free()`
called from `AmfImporter::processNode()`): the 205 regression tests that import
AMF or SVG fail, at 36a9d71ae too. `build-release` still has the Command Line
Tools SDK in its cache.
