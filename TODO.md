# TODO

## Memory that is not given back after freeing

Measured at the end of incremental evaluation's Phase 3
(`doc/journals/2026-10-06-incremental-eval.md`, "Known gaps"): at the end of
the 13-step u-bot replay the process footprint is about 2.8 GB, and about
**1.6 GB of it stays** once the geometry caches, the memo table and the node
tree have all been freed. Not investigated yet.

Why it matters: a GUI session renders the same model hundreds of times. Peak
footprint was 3.15 GB for the harness run and 3.92 GB for the GUI test run, and
whatever is not returned accumulates or at least never shrinks.

Numbers to start from (same journal entry): the geometry caches held 4,873
entries counted as 1,555 MB (Manifold sizes are estimates, `NumVert()*250`) and
gave back only 522 MB when emptied.

Suspects, none confirmed:

- the allocator keeping freed pages: mimalloc overrides malloc on macOS
  (`947e88331`) and purges lazily;
- fragmentation from the ~470k small nodes of each tree, freed in a different
  order than they were made;
- Manifold/TBB keeping their own pools;
- a real leak.

How to look:

- Reproduce with `--memo-replay <13-step sequence> --memo-geometry` on the
  scratch copy of u-bot, then free everything at the end and compare
  `footprint <pid>` / `vmmap --summary <pid>` before and after.
- `MIMALLOC_SHOW_STATS=1` and `MIMALLOC_PURGE_DELAY=0` show whether mimalloc is
  holding the pages; calling `mi_collect(true)` after Design → Flush Caches
  would be the cheap fix if it is.
- `leaks <pid>` or a heap profile to rule out a leak.
