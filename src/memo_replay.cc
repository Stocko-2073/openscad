#include "memo_replay.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/BuiltinContext.h"
#include "core/Context.h"
#include "core/EvalMemo.h"
#include "core/EvaluationSession.h"
#include "core/RenderVariables.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/Tree.h"
#include "core/node.h"
#include "geometry/GeometryCache.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/cgal/CGALCache.h"
#include "openscad.h"
#include "utils/printutils.h"
#ifdef ENABLE_MANIFOLD
#include "geometry/manifold/ManifoldGeometry.h"
#endif

namespace fs = std::filesystem;

namespace {

struct Run {
  std::vector<std::pair<std::string, size_t>> reasons;
  std::shared_ptr<AbstractNode> root;
  std::string tree;  // structural dump, every node and group name
  std::vector<Message> messages;
  double ms = 0;
  size_t nodes = 0;
  memo::Stats stats;
};

void dumpTree(const AbstractNode& node, std::ostringstream& out, size_t& count)
{
  ++count;
  out << node.verbose_name() << ':' << node.toString();
  if (!node.children.empty()) {
    out << '{';
    for (const auto& child : node.children) dumpTree(*child, out, count);
    out << '}';
  }
  out << ';';
}

Run evaluate(SourceFile *file, const fs::path& dir, memo::MemoTable *table, uint64_t generation)
{
  Run run;
  resetSuppressedMessages();
  std::vector<Message> captured;
  g_message_capture.push_back(&captured);
  const auto start = std::chrono::steady_clock::now();
  {
    EvaluationSession session{dir.string()};
    ContextHandle<BuiltinContext> builtin{Context::create<BuiltinContext>(&session)};
    RenderVariables variables{};
    variables.time = 0;
    variables.applyToContext(builtin);
    AbstractNode::resetIndexCounter();
    std::optional<memo::EvalMemoSession> memo;
    if (table) {
      memo.emplace(*table, generation);
      session.setMemo(&*memo);
    }
    if (memo) memo->prepare(*file);
    std::shared_ptr<const FileContext> fileContext;
    run.root = file->instantiate(*builtin, &fileContext);
    fileContext.reset();
    if (memo) {
      run.stats = memo->stats();
      run.reasons.assign(memo->reasons().begin(), memo->reasons().end());
      std::sort(run.reasons.begin(), run.reasons.end(),
                [](const auto& x, const auto& y) { return x.second > y.second; });
      session.setMemo(nullptr);
      memo.reset();
    }
  }
  run.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  g_message_capture.pop_back();
  run.messages = std::move(captured);
  if (run.root) {
    std::ostringstream out;
    dumpTree(*run.root, out, run.nodes);
    run.tree = out.str();
  }
  return run;
}

std::string describe(const Message& m) { return m.str(); }

// "identical", "locations only", or the first difference.
std::string compareMessages(const std::vector<Message>& memo, const std::vector<Message>& fresh, bool& ok,
                            bool& locationsOnly)
{
  ok = true;
  locationsOnly = false;
  const size_t n = std::min(memo.size(), fresh.size());
  bool textDiffers = memo.size() != fresh.size();
  size_t first = n;
  for (size_t i = 0; i < n; ++i) {
    if (memo[i].group != fresh[i].group || memo[i].msg != fresh[i].msg) {
      textDiffers = true;
      first = std::min(first, i);
      break;
    }
  }
  if (textDiffers) {
    ok = false;
    std::ostringstream out;
    out << "messages differ (" << memo.size() << " memo vs " << fresh.size() << " fresh)";
    if (first < n) {
      out << ", first at #" << first << ":\n      memo:  " << describe(memo[first])
          << "\n      fresh: " << describe(fresh[first]);
    }
    return out.str();
  }
  for (size_t i = 0; i < n; ++i) {
    if (describe(memo[i]) != describe(fresh[i])) {
      locationsOnly = true;
      return "messages identical except locations";
    }
  }
  return "messages identical";
}

std::string firstTreeDifference(const std::string& a, const std::string& b)
{
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  const size_t from = i > 120 ? i - 120 : 0;
  std::ostringstream out;
  out << "trees differ at byte " << i << " (sizes " << a.size() << " vs " << b.size() << ")"
      << "\n      memo:  ..." << a.substr(from, 240) << "\n      fresh: ..." << b.substr(from, 240);
  return out.str();
}

}  // namespace

/*
 * The GUI's geometry step for a tree, with the geometry caches left as the
 * previous step left them, so the time is what a refresh would take.
 */
double geometryMs(const std::shared_ptr<AbstractNode>& root, const fs::path& dir)
{
  if (!root) return 0;
  const auto start = std::chrono::steady_clock::now();
  Tree tree(root, dir.string());
  GeometryEvaluator evaluator(tree);
  auto geometry = evaluator.evaluateGeometry(*tree.root(), true);
#ifdef ENABLE_MANIFOLD
  if (auto manifold = std::dynamic_pointer_cast<const ManifoldGeometry>(geometry)) {
    (void)manifold->getManifold().Status();  // forces evaluation, as CGALWorker does
  }
#endif
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int memo_replay(const std::vector<std::string>& files, const std::string& commands, bool verify,
                bool geometry)
{
  memo::MemoTable table;
  // The memo table points into every syntax tree it has seen; keep them all.
  std::vector<std::unique_ptr<SourceFile>> keep;
  const fs::path original = fs::current_path();
  memo::Stats total;
  int failures = 0;
  if (geometry) {
    // As large as the GUI's preferences allow, so nothing is evicted between
    // steps that a long GUI session would have kept.
    GeometryCache::instance()->setMaxSizeMB(5000);
    CGALCache::instance()->setMaxSizeMB(5000);
  }
  uint64_t generation = 0;

  for (const auto& name : files) {
    ++generation;
    const fs::path path = fs::absolute(fs::path(name));
    std::ifstream in(path);
    if (!in.is_open()) {
      std::cerr << "memo-replay: can't open " << path << "\n";
      return 1;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text += "\n\x03\n" + commands;
    fs::current_path(path.parent_path());

    SourceFile *parsed = nullptr;
    if (!parse(parsed, text, path.generic_string(), path.generic_string(), false)) {
      delete parsed;
      std::cerr << "memo-replay: can't parse " << path << "\n";
      fs::current_path(original);
      return 1;
    }
    keep.emplace_back(parsed);
    parsed->handleDependencies();

    Run memoRun = evaluate(parsed, path.parent_path(), &table, generation);
    total.add(memoRun.stats);
    const memo::Stats& s = memoRun.stats;
    const size_t uncacheable =
      s.unhashableArg + s.unhashableEnv + s.childrenLocalDef + s.childrenNoKey + s.unhashableChildrenVar;
    std::cout << "step " << generation << " " << path.filename().generic_string() << ": eval "
              << static_cast<long>(memoRun.ms) << " ms, " << memoRun.nodes << " nodes | boundaries "
              << s.boundaries << ": hit " << s.hits << ", miss " << s.misses << " (stored " << s.stored
              << ", impure " << s.impure << ", $-unhashable " << s.unhashableDollar << ", $-differ "
              << s.staleDollar << "), uncacheable " << uncacheable << " [arg " << s.unhashableArg
              << ", env " << s.unhashableEnv
              << ", child-local " << s.childrenLocalDef << ", child-nokey " << s.childrenNoKey
              << ", child-var " << s.unhashableChildrenVar << "] | cloned " << s.nodesCloned
              << " nodes, replayed " << s.messagesReplayed << " msgs, keys "
              << std::chrono::duration_cast<std::chrono::milliseconds>(s.keyTime).count()
              << " ms (closures "
              << std::chrono::duration_cast<std::chrono::milliseconds>(s.closureTime).count()
              << " ms) | table " << table.size() << "\n";

    for (size_t i = 0; i < memoRun.reasons.size(); ++i) {
      std::cout << "        " << memoRun.reasons[i].second << "  " << memoRun.reasons[i].first << "\n";
    }
    if (geometry) {
      std::cout << "        geometry " << static_cast<long>(geometryMs(memoRun.root, path.parent_path()))
                << " ms (caches kept across steps)\n";
    }
    if (verify) {
      Run fresh = evaluate(parsed, path.parent_path(), nullptr, 0);
      bool messagesOk = false;
      bool locationsOnly = false;
      const std::string messages =
        compareMessages(memoRun.messages, fresh.messages, messagesOk, locationsOnly);
      const bool treeOk = memoRun.tree == fresh.tree;
      std::cout << "        fresh " << static_cast<long>(fresh.ms) << " ms, " << fresh.nodes
                << " nodes | tree " << (treeOk ? "identical" : "DIFFERS") << ", " << messages << "\n";
      if (!treeOk) std::cout << "      " << firstTreeDifference(memoRun.tree, fresh.tree) << "\n";
      if (!treeOk || !messagesOk) ++failures;
    }
    fs::current_path(original);
  }

  std::cout << "total: hits " << total.hits << ", misses " << total.misses << ", cloned "
            << total.nodesCloned << " nodes";
  if (verify) std::cout << ", verify failures " << failures;
  std::cout << "\n";
  return failures ? 1 : 0;
}
