# TODO

## Animate

### Frames first played while a render runs start from empty memo tables

The frame tasks fork their memo tables from the document's, but the cache forks
that only when no render holds the `GuiLocker`: one that is evaluating writes
to it. Pressing play while the design renders, before any frame has had a
table, gives the tasks empty ones, and ten cold evaluations side by side are
slow: twenty u-bot frames take ~9.5 s instead of ~5 s. The cache could fork
the document's table once the render ends.

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
of the last render.

### A changed `use`d file leaves the frames as they were

When a used file changes on disk, `handleDependencies()` has the GUI render
again, but `rootFile` stays the same object, so the cache is not seeded anew
and goes on playing frames made with the old file.

## Build

### With the Xcode 26.6 SDK, AMF and SVG import crash

`xcode-select` points at Xcode 26.6, so a fresh configure builds with its SDK,
and that build crashes importing AMF or SVG (for AMF, in mimalloc's `free()`
called from `AmfImporter::processNode()`). `build-release` still has the Command
Line Tools SDK in its cache.
