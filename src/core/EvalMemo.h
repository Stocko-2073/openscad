#pragma once

/*
 * Incremental evaluation: a boundary, a call to a file-scope user module from a non-library
 * file, reuses the subtree it produced in an earlier evaluation if nothing it depends on changed.
 *
 *  - The key covers what is lexical: the code the module reaches by name, the file-scope values
 *    it reads, the arguments, and the children block with the caller's values that block reads.
 *  - $ variables read from outside the call, and parent_module()'s reads of the module-name stack
 *    below it, are dynamic: the call records them and is reused only where they read the same.
 *  - An accumulator read, of $x only to assign $x (`$transform = $transform * m`), counts only if
 *    something under the call reads $x for real.
 *  - A call that touches anything impure (rands(), file reads during evaluation) is not stored.
 *  - Nodes point at their statements (modinst) for their locations and ! # % tags, so reused
 *    copies must point into the current parse: an entry records the way to each (Anchor, Site).
 */

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
#include "utils/Hash128.h"
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

using Hash128 = ::Hash128;
using Hash128Hash = ::Hash128Hash;

class Hasher : public Hasher128
{
public:
  // Interned names never move within a process, and the table never outlives one.
  void id(const Identifier& name) { u64(name.index() + 1); }
};

// Exact structural hash of syntax, without locations. Not the printed text, which rounds numbers
// and omits the ! # % tags. Collects referenced names ignoring shadowing, an over-approximation.
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

  // annotate() marks reads of this $ name as accumulator reads, except inside function literals,
  // whose reads happen when they are called.
  const Identifier *accumulatorOf = nullptr;
};

// Sets LocalScope::origin and the parent links, and marks accumulator reads. Writes to the tree,
// so the parser runs it before anything evaluates the file, possibly on several threads at once.
void annotate(SourceFile& file);

class EvalMemoSession;

// Keeps the vectors it hashed alive, as it caches their hashes by address. Function values hash
// only with a session, to resolve what they capture.
class ValueHashCache
{
public:
  explicit ValueHashCache(EvalMemoSession *session = nullptr) : session(session) {}
  bool hash(Hasher& h, const Value& value);

private:
  bool hashValue(Hasher& h, const Value& value);
  EvalMemoSession *session;
  int depth = 0;
  std::unordered_map<const void *, std::pair<std::shared_ptr<const void>, Hash128>> vectors;
};

struct Stats {
  // Boundaries a fresh evaluation would reach, and how many of them reused calls stood for.
  size_t userCalls = 0;
  size_t userCallsReused = 0;
};

struct DollarRead {
  Identifier name;
  Hash128 value;
  bool absent = false;
};

// Where a Site's path starts, in terms a later parse can answer. Children blocks are anchored at
// the call that runs them, so editing a caller does not invalidate what is beneath it.
struct Anchor {
  enum class Kind : uint8_t {
    Definition,         // the body of a file-scope module definition
    OwnChildren,        // the children block of the reused call itself
    EnclosingChildren,  // that of the depth-th enclosing user-module call, nearest first
  };
  Kind kind = Kind::Definition;
  uint32_t file = 0;   // Definition: 0 for the main file, else MemoTable::fileId()
  uint32_t later = 0;  // Definition: definitions of the same name after it in that file
  uint32_t depth = 0;  // EnclosingChildren
  Identifier name;     // Definition
  Hash128 hash;        // the scope's structural hash, which it must still have
};

// A statement, as a path from an anchor: the PathOp words Entry::paths[begin, end).
struct Site {
  uint32_t anchor = 0;
  uint32_t begin = 0;
  uint32_t end = 0;
  uint64_t check = 0;
};

struct Entry {
  // Never read a stored node's modinst: the parse it points into may be gone.
  std::shared_ptr<AbstractNode> root;
  std::vector<Message> messages;
  std::vector<DollarRead> reads;
  std::vector<Identifier> accumulated;  // read only to feed themselves; see Recorder
  std::vector<Identifier> realNames;    // $ names read for real anywhere inside
  // What parent_module() read below the call's own name on the module-name stack: the
  // `outerModules` names right below it, or the whole stack and its size. `moduleStack` hashes it.
  uint32_t outerModules = 0;
  bool wholeModuleStack = false;
  Hash128 moduleStack;
  size_t calls = 0;       // boundaries its evaluation reaches, its own call included
  uint64_t lastUsed = 0;  // generation

  // Where the statements of the nodes below the root are (the root's is the call site). nodeCodes,
  // in preorder: a node's site, or kNested | its index in `nested` for a nested entry's subtree.
  std::vector<Anchor> anchors;
  std::vector<Site> sites;
  std::vector<uint32_t> paths;
  std::vector<Identifier> names;  // module definitions nested inside others, on paths
  std::vector<uint32_t> nodeCodes;
  static constexpr uint32_t kNested = 0x80000000u;

  // Entries stored or reused while this call ran. Reusing this one hands them its copies too.
  struct Nested {
    std::shared_ptr<Entry> entry;
    uint32_t site = 0;             // its root's, which is its call site
    std::vector<uint32_t> sites;   // the site here of each of its sites
  };
  std::vector<Nested> nested;
};

// Kept across evaluations; holds on to no parse. One evaluation at a time: nothing is synchronized.
class MemoTable
{
public:
  [[nodiscard]] size_t size() const { return count; }
  void clear()
  {
    entries.clear();
    count = 0;
  }
  // Drops the entries unused for more than `keep` evaluations; returns how many.
  size_t evict(uint64_t keep);
  // A copy to evaluate with on another thread, sharing only the stored nodes, which nothing changes
  // once made. Not while this table is in use.
  [[nodiscard]] std::unique_ptr<MemoTable> fork() const;

private:
  friend class EvalMemoSession;
  std::vector<std::shared_ptr<Entry>> *find(const Hash128& key);
  void store(const Hash128& key, std::shared_ptr<Entry> entry);
  uint32_t fileId(const std::string& path);

  std::unordered_map<Hash128, std::vector<std::shared_ptr<Entry>>, Hash128Hash> entries;
  size_t count = 0;
  uint64_t generation_ = 0;
  std::unordered_map<std::string, uint32_t> fileIds;
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

// One evaluation's use of the table, attached with EvaluationSession::setMemo(). Destroy it before
// that session, whose Values it holds. `root` must be the file that evaluation instantiates.
class EvalMemoSession
{
public:
  EvalMemoSession(MemoTable& table, const SourceFile& root);
  ~EvalMemoSession();
  EvalMemoSession(const EvalMemoSession&) = delete;
  EvalMemoSession& operator=(const EvalMemoSession&) = delete;

  // Called once the module's name is pushed and the arguments evaluated. The body runs in the
  // caller's frame and the memo's state lives on the heap: recursion fails as deep as without it.
  Call enter(const UserModule& module, const std::shared_ptr<const Context>& defining_context,
             const ModuleInstantiation *inst, const std::shared_ptr<const Context>& context,
             const Arguments& arguments);
  std::shared_ptr<AbstractNode> takeReused() { return std::move(reused); }
  [[nodiscard]] const uint64_t *childrenKey() const;
  void leave(const std::shared_ptr<AbstractNode>& node);
  void abandon();

  // Called by builtins whose result depends on more than their arguments.
  static void noteImpure(EvaluationSession *session);
  // parent_module() read the module-name stack at `index`, or its size if kWholeModuleStack.
  static void noteModuleStackRead(EvaluationSession *session, size_t index);
  static constexpr size_t kWholeModuleStack = SIZE_MAX;
  // For every $ lookup: `index` is the frame that answered, or SIZE_MAX if none did.
  void noteDollarRead(const Identifier& name, size_t index, const Value *value);
  // Set around a read that only feeds the same $ variable; see Lookup::evaluate.
  bool accumulatorRead = false;

  [[nodiscard]] const Stats& stats() const { return stats_; }
  // The trees reused entries held before this evaluation's copies. Freeing a large tree takes a
  // while, so a caller in a hurry holds these until its result is on screen.
  std::vector<std::shared_ptr<AbstractNode>> takeReplaced() { return std::move(replaced); }

private:
  std::shared_ptr<AbstractNode> reuse(EvaluationSession& session, Entry& entry,
                                      const ModuleInstantiation *inst, const Context& context,
                                      std::vector<const ModuleInstantiation *>& statements);
  void store(const std::shared_ptr<AbstractNode>& node, Recorder& recorder);

  // A boundary stored or reused inside the recording call; `statements` parallels entry->sites.
  struct NestedResult {
    const AbstractNode *root;
    std::shared_ptr<Entry> entry;
    std::vector<const ModuleInstantiation *> statements;
  };

  // In EvalMemoSites.cc. `statements` gets the statement of each of the entry's sites.
  struct Located;
  bool locate(const AbstractNode& root, const ModuleInstantiation *inst, const Context& context,
              const std::vector<NestedResult>& nested, Entry& entry,
              std::vector<const ModuleInstantiation *>& statements);
  bool relocate(const Entry& entry, const ModuleInstantiation *inst, const Context& context,
                std::vector<const ModuleInstantiation *>& statements);
  const Anchor *definitionAnchor(const UserModule& module, const SourceFile& file);
  const LocalScope *definitionBody(const Anchor& anchor);
  const SourceFile *usedFile(uint32_t id);
  // Also gives the copies to the entry and those nested in it. Null if it cannot, changing nothing.
  std::shared_ptr<AbstractNode> copyTree(Entry& entry,
                                         const std::vector<const ModuleInstantiation *>& statements,
                                         const ModuleInstantiation *inst);
  Hash128 scopeHash(const LocalScope& scope);
  Hash128 moduleHash(const UserModule& module);
  Recorder& pushRecorder(size_t base);
  std::unique_ptr<Recorder> popRecorder();
  bool isUserFile(const ModuleInstantiation *inst);
  bool computeKey(const UserModule& module, const FileContext& definingFile,
                  const ModuleInstantiation *inst, const std::shared_ptr<const Context>& context,
                  const Arguments& arguments, Hash128& key, uint64_t childrenKey[3]);
  bool childrenHash(const ModuleInstantiation *inst, const std::shared_ptr<const Context>& context,
                    Hasher& h);
  bool envHash(const void *def, bool isModule, const FileContext& file, Hasher& out);
  bool resolveRef(const Context *context, const Identifier& name, bool isModule, Hasher& h);
  bool hashFunction(Hasher& h, const FunctionType& function);
  friend class ValueHashCache;
  bool matches(EvaluationSession& session, const Entry& entry);
  // Re-reads `entry`'s $ dependencies so the enclosing call records them.
  void replayReads(EvaluationSession& session, const Entry& entry);
  static Hash128 moduleStackHash(size_t from, size_t to);
  static bool moduleStackRange(const Entry& entry, size_t own, size_t& from, size_t& to);
  const DefInfo& defInfo(const void *def, bool isModule);
  const Closure& closure(const void *def, bool isModule, const SourceFile& file);
  Hash128 fileHash(const SourceFile& file);
  void replay(const std::vector<Message>& messages);

  MemoTable& table;
  const SourceFile& root;
  uint64_t generation;
  Stats stats_;
  ValueHashCache values;
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
  // envHash per definition and FileContext, whose file-scope values are fixed. Keyed on the scope
  // serial, not the address, which recurs: a `use`d file gets a fresh FileContext per lookup.
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
  std::vector<std::unique_ptr<Recorder>> recorders;
  std::vector<std::unique_ptr<Recorder>> spareRecorders;
  std::shared_ptr<AbstractNode> reused;
  std::vector<std::shared_ptr<AbstractNode>> replaced;
  std::unordered_map<const UserModule *, Anchor> definitionAnchors;
  struct DefinitionKey {
    uint32_t file;
    uint32_t later;
    size_t name;
    bool operator==(const DefinitionKey& o) const
    {
      return file == o.file && later == o.later && name == o.name;
    }
  };
  struct DefinitionKeyHash {
    size_t operator()(const DefinitionKey& k) const noexcept
    {
      return (k.name * 31 + k.file) * 31 + k.later;
    }
  };
  struct FoundDefinition {
    const LocalScope *body;
    Hash128 hash;
  };
  std::unordered_map<DefinitionKey, FoundDefinition, DefinitionKeyHash> definitionBodies;
  std::unordered_map<uint32_t, const SourceFile *> usedFiles;
  bool usedFilesKnown = false;
  int suspendRecording = 0;  // validation lookups are not reads of the enclosing call
  std::vector<const FunctionType *> hashingFunctions;
  friend class Recorder;
};

}  // namespace memo
