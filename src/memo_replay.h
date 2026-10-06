#pragma once

#include <string>
#include <vector>

/*
 * Command-line harness for incremental evaluation (core/EvalMemo.h).
 *
 * Evaluates `files` in order with one memo table, as the GUI would on a
 * series of saves, and reports per step the evaluation time, hits and misses.
 * With `verify`, each step is also evaluated from scratch and its node tree and
 * messages compared with the memoized one. Returns nonzero on any mismatch.
 * With `geometry`, each step's geometry evaluation is timed too, with the
 * geometry caches kept across steps as the GUI keeps them.
 *
 * With `keep` of zero or more, entries the last `keep` steps did not use are
 * evicted after each step, as the GUI evicts them after each render
 * (MemoTable::evict()); with a negative `keep`, nothing is.
 *
 * `commands` is appended to every file as -D assignments are.
 */
int memo_replay(const std::vector<std::string>& files, const std::string& commands, bool verify,
                bool geometry, int keep);
