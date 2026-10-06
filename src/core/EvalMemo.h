#pragma once

/*
 * Incremental evaluation: reuse the node subtree that a module instantiation
 * produced in an earlier evaluation when nothing it can depend on has changed.
 *
 * Boundaries are calls to a file-scope user module whose call site is in a
 * non-library file -- the calls the user wrote. BOSL2's own internal calls are
 * never boundaries, so a reused call skips all the library work beneath it.
 *
 * What a call can depend on is split in two.
 *
 * The key, computed before the call runs, covers what is lexical:
 *  - the module's code and everything it can reach by name, plus the values of
 *    the file-scope variables that code references (Closure, envHash());
 *  - the argument values;
 *  - the children block: its syntax, the caller-context values of the names it
 *    references, and the enclosing module's children when it calls children();
 *  - the directories the code and the call site live in, which import() and
 *    surface() resolve relative paths against.
 *
 * $ variables are dynamic, so instead the call records which ones it reads
 * from outside itself, with their values, and an entry is reused only where
 * those reads would see the same values. A read that only feeds the same
 * variable back -- BOSL2's `$transform = $transform * m` in every transform --
 * counts only if something under the call reads that variable for real.
 * Calls to parent_module() likewise record the module-name stack.
 *
 * Anything that cannot be hashed (objects) makes the call ineligible, and it
 * runs as usual. While a stored call runs it records the messages it prints,
 * which are replayed on reuse, and whether it touched anything impure (rands(),
 * file reads at evaluation time), which keeps it out of the table.
 *
 * Reuse clones the stored subtree with fresh node indices. The clone keeps
 * pointing into the syntax tree that produced it, so whoever owns the table
 * must keep those SourceFiles alive.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/Identifier.h"
#include "utils/printutils.h"

class AbstractNode;
class Arguments;
class Assignment;
class Context;
class EvaluationSession;
class Expression;
class FileContext;
class FunctionType;
class LocalScope;
class ModuleInstantiation;
class SourceFile;
class UserFunction;
class UserModule;
class Value;
using AssignmentList = std::vector<std::shared_ptr<Assignment>>;

namespace memo {

struct Hash128 {
  uint64_t a{0};
  uint64_t b{0};
  bool operator==(const Hash128& o) const { return a == o.a && b == o.b; }
  bool operator!=(const Hash128& o) const { return !(*this == o); }
  bool operator<(const Hash128& o) const { return a < o.a || (a == o.a && b < o.b); }
};

struct Hash128Hash {
  size_t operator()(const Hash128& h) const noexcept { return static_cast<size_t>(h.a ^ (h.b << 1)); }
};

// Order-sensitive 128-bit hash of a word sequence. Not cryptographic; two
// lanes of murmur3's finalizer, which is a bijection per word.
class Hasher
{
public:
  void u64(uint64_t w)
  {
    s0 = fmix(s0 ^ w) + 0x9E3779B97F4A7C15ULL;
    s1 = fmix(s1 + (w ^ 0xC2B2AE3D27D4EB4FULL)) ^ (s0 >> 17);
    ++n;
  }
  void f64(double d)
  {
    uint64_t w;
    std::memcpy(&w, &d, sizeof w);
    u64(w);
  }
  void bytes(const void *data, size_t size);
  void str(std::string_view s)
  {
    u64(s.size());
    bytes(s.data(), s.size());
  }
  // Interned names never move within a process, and the table never outlives one.
  void id(const Identifier& name) { u64(name.index() + 1); }
  void h(const Hash128& x)
  {
    u64(x.a);
    u64(x.b);
  }
  [[nodiscard]] Hash128 finish() const;

private:
  static uint64_t fmix(uint64_t k)
  {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
  }
  uint64_t s0{0x243F6A8885A308D3ULL};
  uint64_t s1{0x13198A2E03707344ULL};
  uint64_t n{0};
};

/*
 * Exact structural hash of syntax: literals by bit pattern (the AST printer
 * rounds numbers to six digits, so printed text cannot be used), no source
 * locations, and the ! # % tags that the printer omits. Also collects every
 * name the syntax refers to, ignoring shadowing, which over-approximates what
 * it can depend on.
 */
class ASTHasher : public Hasher
{
public:
  void expr(const Expression *e);
  void assignments(const AssignmentList& list);
  void scope(const LocalScope& scope);
  void inst(const ModuleInstantiation& inst);
  void userModule(const UserModule& module);
  void userFunction(const UserFunction& function);
  void literal(const Value& value);

  void refVar(const Identifier& name) { vars.push_back(name); }
  void refFunction(const Identifier& name) { functions.push_back(name); }
  void refModule(const Identifier& name) { modules.push_back(name); }

  std::vector<Identifier> vars;
  std::vector<Identifier> functions;
  std::vector<Identifier> modules;

  // When set, marks every read of this $ name as an accumulator read; see
  // annotate(). Not inside function literals, whose reads happen when they
  // are called.
  const Identifier *accumulatorOf = nullptr;
};

/*
 * Prepares a freshly parsed file for incremental evaluation. The parser calls
 * it on every file it parses successfully, the main file and each `use`d
 * one, before anything can evaluate it: it writes to the syntax tree, which
 * the GUI's animation prefetch evaluates on several threads at once.
 *
 * Marks accumulator reads: in `$x = <expr>`, each read of $x inside <expr>
 * (Lookup::accumulator), which a recording call treats as pending rather
 * than as a dependency; see Recorder.
 */
void annotate(SourceFile& file);

class EvalMemoSession;

// Values of the hashed vectors, kept alive so their addresses cannot be reused
// while the cache is keyed on them. Per evaluation. Function values hash only
// with a session to resolve what they capture.
class ValueHashCache
{
public:
  explicit ValueHashCache(EvalMemoSession *session = nullptr) : session(session) {}
  bool hash(Hasher& h, const Value& value);

private:
  bool hashValue(Hasher& h, const Value& value);
  EvalMemoSession *session;
  // Recursion depth; nesting deeper than kMaxDepth counts as unhashable, so a
  // pathologically nested value cannot exhaust the stack here.
  int depth = 0;
  std::unordered_map<const void *, std::pair<std::shared_ptr<const void>, Hash128>> vectors;
};

struct Stats {
  size_t calls = 0;       // user-module instantiations seen while memo was on
  size_t boundaries = 0;  // of those, at a user call site of a file-scope module
  size_t hits = 0;
  size_t misses = 0;
  size_t stored = 0;
  size_t impure = 0;
  size_t cloneFailed = 0;
  size_t nodesCloned = 0;
  size_t messagesReplayed = 0;
  size_t staleDollar = 0;  // key matched, but a $ variable it reads differs
  // Why a boundary was not eligible
  size_t unhashableArg = 0;
  size_t unhashableEnv = 0;
  size_t childrenLocalDef = 0;
  size_t childrenNoKey = 0;
  size_t unhashableChildrenVar = 0;
  size_t unhashableDollar = 0;  // a $ variable it read could not be hashed
  std::chrono::nanoseconds keyTime{0};
  std::chrono::nanoseconds closureTime{0};
  void add(const Stats& other);
};

// A $ variable a call read from outside itself, and what it saw.
struct DollarRead {
  Identifier name;
  Hash128 value;
  bool absent = false;
};

struct Entry {
  std::shared_ptr<AbstractNode> root;
  std::vector<Message> messages;
  std::vector<DollarRead> reads;        // must match for reuse
  std::vector<Identifier> accumulated;  // read only to feed themselves; see Recorder
  std::vector<Identifier> realNames;    // $ names read for real anywhere inside
  bool readsModuleStack = false;
  Hash128 moduleStack;
  size_t nodes = 0;
  uint64_t lastUsed = 0;  // generation
};

// Survives across evaluations; owned by whoever keeps the SourceFiles alive.
class MemoTable
{
public:
  std::vector<Entry> *find(const Hash128& key);
  void store(const Hash128& key, Entry entry);
  [[nodiscard]] size_t size() const { return count; }
  void clear()
  {
    entries.clear();
    count = 0;
  }
  // Drop entries not used in the last `keep` generations.
  size_t evict(uint64_t generation, uint64_t keep);

private:
  std::unordered_map<Hash128, std::vector<Entry>, Hash128Hash> entries;
  size_t count = 0;
};

struct DefInfo;
struct Closure;
class Recorder;

// What UserModule::instantiate does with a call; see EvalMemoSession::enter().
enum class Call {
  Plain,      // not a boundary, or not one that can be keyed: run it as usual
  Reused,     // the result is ready: takeReused()
  Recording,  // run it with childrenKey(), then leave(), or abandon() if it throws
};

// One evaluation's view of the table. Must be destroyed before the
// EvaluationSession it is attached to (it holds Values from that session).
class EvalMemoSession
{
public:
  EvalMemoSession(MemoTable& table, uint64_t generation);
  ~EvalMemoSession();
  EvalMemoSession(const EvalMemoSession&) = delete;
  EvalMemoSession& operator=(const EvalMemoSession&) = delete;

  /*
   * The boundary protocol, for UserModule::instantiate once it has evaluated
   * the arguments (with the module's name pushed, which parent_module()
   * sees). enter() decides what happens to the call. The module body runs
   * from instantiate's own frame on every path and the memo keeps its state
   * on the heap, so a boundary costs no more stack than any other call: deep
   * recursion fails at the same depth with the memo as without.
   */
  Call enter(const UserModule& module, const std::shared_ptr<const Context>& defining_context,
             const ModuleInstantiation *inst, const std::shared_ptr<const Context>& context,
             const Arguments& arguments);
  // After Reused: the result.
  std::shared_ptr<AbstractNode> takeReused() { return std::move(reused); }
  // The recording call's children key, for its UserModuleContext.
  [[nodiscard]] const uint64_t *childrenKey() const;
  // The recording call returned `node`.
  void leave(const std::shared_ptr<AbstractNode>& node);
  // The recording call threw.
  void abandon();

  // Called by builtins whose result depends on more than their arguments.
  static void noteImpure(EvaluationSession *session);
  // Called by parent_module(), which reads the module-name stack.
  static void noteModuleStackRead(EvaluationSession *session);
  // Called by EvaluationSession for every $ lookup: `index` is the stack
  // position of the frame that answered, or SIZE_MAX if none did.
  void noteDollarRead(const Identifier& name, size_t index, const Value *value);
  // Set around a read that only feeds the same $ variable; see Lookup::evaluate.
  bool accumulatorRead = false;

  [[nodiscard]] const Stats& stats() const { return stats_; }
  // With OPENSCAD_MEMO_DEBUG set: why boundaries were not reused, by reason and name.
  [[nodiscard]] const std::unordered_map<std::string, size_t>& reasons() const { return reasons_; }

private:
  std::shared_ptr<AbstractNode> reuse(EvaluationSession& session, Entry& entry,
                                      const ModuleInstantiation *inst);
  void store(const std::shared_ptr<AbstractNode>& node, Recorder& recorder);
  // Starts recording a call whose $ reads come from frames below `base`.
  Recorder& pushRecorder(size_t base);
  // Ends the innermost recording and hands what it saw to the one around it.
  std::unique_ptr<Recorder> popRecorder();
  bool isUserFile(const ModuleInstantiation *inst);
  // childrenKey receives the children block's key: two hash words and flags.
  bool computeKey(const UserModule& module, const FileContext& definingFile,
                  const ModuleInstantiation *inst, const std::shared_ptr<const Context>& context,
                  const Arguments& arguments, Hash128& key, uint64_t childrenKey[3]);
  bool childrenHash(const ModuleInstantiation *inst, const std::shared_ptr<const Context>& context,
                    Hasher& h);
  bool envHash(const void *def, bool isModule, const FileContext& file, Hasher& out);
  // A function or module name as code in `context` would resolve it.
  bool resolveRef(const Context *context, const Identifier& name, bool isModule, Hasher& h);
  // A function value: its syntax and the values of the names it captures.
  bool hashFunction(Hasher& h, const FunctionType& function);
  friend class ValueHashCache;
  // Whether `entry`'s $ reads and module stack would read the same now.
  bool matches(EvaluationSession& session, const Entry& entry);
  // Re-reads `entry`'s $ dependencies so the enclosing call records them.
  void replayReads(EvaluationSession& session, const Entry& entry);
  static Hash128 moduleStackHash();
  const DefInfo& defInfo(const void *def, bool isModule);
  const Closure& closure(const void *def, bool isModule, const SourceFile& file);
  Hash128 fileHash(const SourceFile& file);
  std::shared_ptr<AbstractNode> cloneTree(const AbstractNode& node, size_t& count);
  void replay(const std::vector<Message>& messages);

  MemoTable& table;
  uint64_t generation;
  Stats stats_;
  ValueHashCache values;
  bool debug = false;
  std::unordered_map<std::string, size_t> reasons_;
  void reason(const std::string& why)
  {
    if (debug) ++reasons_[why];
  }
  Hash128 configHash;
  std::unordered_map<const void *, bool> userFiles;
  std::vector<std::string> libraryDirs;
  std::unordered_map<const void *, std::unique_ptr<DefInfo>> defInfos;
  std::unordered_map<const void *, std::unique_ptr<Closure>> closures;
  std::unordered_map<const void *, Hash128> fileHashes;
  struct ScopeInfo;
  const ScopeInfo& scopeInfo(const LocalScope& scope);
  std::unordered_map<const void *, std::unique_ptr<ScopeInfo>> scopeInfos;
  struct FunctionInfo {
    Hash128 syntax;
    std::vector<Identifier> vars;
    std::vector<Identifier> functions;
  };
  const FunctionInfo& functionInfo(const FunctionType& function);
  // Keyed by the literal's expression, which outlives every value made from it.
  std::unordered_map<const void *, std::unique_ptr<FunctionInfo>> functionInfos;
  // envHash per (definition, file context): file-scope values are fixed for
  // the life of a FileContext. Keyed on the context's scope serial, not its
  // address: a `use`d file gets a fresh FileContext per lookup, and freed
  // addresses come back.
  struct EnvKey {
    const void *def;
    uint64_t serial;
    bool operator==(const EnvKey& o) const { return def == o.def && serial == o.serial; }
  };
  struct EnvKeyHash {
    size_t operator()(const EnvKey& k) const noexcept
    {
      return std::hash<const void *>{}(k.def) * 31 + std::hash<uint64_t>{}(k.serial);
    }
  };
  struct EnvResult {
    bool ok;
    Hash128 hash;
  };
  std::unordered_map<EnvKey, EnvResult, EnvKeyHash> envs;
  // The calls being recorded, innermost last, and finished ones to reuse:
  // on the heap so that the frames of the recursion stay small.
  std::vector<std::unique_ptr<Recorder>> recorders;
  std::vector<std::unique_ptr<Recorder>> spareRecorders;
  std::shared_ptr<AbstractNode> reused;  // see takeReused()
  int suspendRecording = 0;  // validation lookups are not reads of the enclosing call
  std::vector<const FunctionType *> hashingFunctions;  // cycle guard for hashFunction()
  friend class Recorder;
};

}  // namespace memo
