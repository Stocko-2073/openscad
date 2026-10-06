/*
 * Incremental evaluation. See core/EvalMemo.h for what a key covers and why.
 */

#include "core/EvalMemo.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Feature.h"
#include "core/Arguments.h"
#include "core/Context.h"
#include "core/EvaluationSession.h"
#include "core/Expression.h"
#include "core/LocalScope.h"
#include "core/ModuleInstantiation.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/UserModule.h"
#include "core/Value.h"
#include "core/function.h"
#include "core/node.h"
#include "core/parsersettings.h"
#include "utils/compiler_specific.h"
#include "utils/printutils.h"

namespace fs = std::filesystem;

namespace memo {

namespace {

enum Tag : uint64_t {
  kUndef = 0x7A10000000000001ULL,
  kBool,
  kNumber,
  kString,
  kVector,
  kRange,
  kClosure,
  kKey,
  kAbsent,
  kNoChildren,
  kEnclosingChildren,
  kNoEnclosingChildren,
  kUsedFile,
  kModuleStack,
  kFunctionValue,
  kRecursiveFunction,
};

// Bumped whenever the key's composition changes.
constexpr uint64_t kKeyVersion = 3;

// Vectors shorter than this are rehashed rather than looked up.
constexpr size_t kCacheVectorsFrom = 4;

// Variants kept per key: the same call evaluated under different $ values,
// such as BOSL2 diff()'s keep, remove and intersect passes.
constexpr size_t kMaxEntriesPerKey = 16;

constexpr size_t kNoFrame = SIZE_MAX;
constexpr size_t kPromoted = SIZE_MAX - 1;

void sortUnique(std::vector<Identifier>& names)
{
  std::sort(names.begin(), names.end(),
            [](const Identifier& x, const Identifier& y) { return x.index() < y.index(); });
  names.erase(std::unique(names.begin(), names.end()), names.end());
}

const Identifier& childrenModule()
{
  static const Identifier name{"children"};
  return name;
}

// The `use`d file of `file` that defines `name`, if any, in lookup order.
const SourceFile *usedFileDefining(const SourceFile& file, const Identifier& name, bool isModule)
{
  for (const auto& path : file.usedlibs) {
    const SourceFile *used = SourceFileCache::instance()->lookup(path);
    if (!used) continue;
    const bool defined = isModule ? used->scope->lookup<UserModule *>(name).has_value()
                                  : used->scope->lookup<UserFunction *>(name).has_value();
    if (defined) return used;
  }
  return nullptr;
}

}  // namespace

void Hasher::bytes(const void *data, size_t size)
{
  const auto *p = static_cast<const unsigned char *>(data);
  while (size >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    u64(w);
    p += 8;
    size -= 8;
  }
  if (size) {
    uint64_t w = 0;
    std::memcpy(&w, p, size);
    u64(w ^ (uint64_t(size) << 56));
  }
}

Hash128 Hasher::finish() const
{
  Hash128 r;
  r.a = fmix(s0 ^ fmix(n + 0x632BE59BD9B4E019ULL));
  r.b = fmix(s1 ^ (n * 0x94D049BB133111EBULL) ^ r.a);
  return r;
}

constexpr int kMaxDepth = 200;

bool ValueHashCache::hash(Hasher& h, const Value& value)
{
  if (depth >= kMaxDepth) return false;
  ++depth;
  const bool ok = hashValue(h, value);
  --depth;
  return ok;
}

bool ValueHashCache::hashValue(Hasher& h, const Value& value)
{
  switch (value.type()) {
  case Value::Type::UNDEFINED: h.u64(kUndef); return true;
  case Value::Type::BOOL:      h.u64(kBool + (value.toBool() ? 0x100 : 0)); return true;
  case Value::Type::NUMBER:
    h.u64(kNumber);
    h.f64(value.toDouble());
    return true;
  case Value::Type::STRING:
    h.u64(kString);
    h.str(value.toStrUtf8Wrapper().toString());
    return true;
  case Value::Type::VECTOR:
  case Value::Type::EMBEDDED_VECTOR: {
    const Value::VectorType& vec = value.type() == Value::Type::VECTOR
                                     ? value.toVector()
                                     : static_cast<const Value::VectorType&>(value.toEmbeddedVector());
    const size_t size = vec.size();
    const void *address = vec.ptr.get();
    if (size >= kCacheVectorsFrom && address) {
      auto it = vectors.find(address);
      if (it != vectors.end()) {
        h.u64(kVector);
        h.h(it->second.second);
        return true;
      }
    }
    Hasher sub;
    sub.u64(size);
    for (const auto& element : vec) {
      if (!hash(sub, element)) return false;
    }
    const Hash128 result = sub.finish();
    if (size >= kCacheVectorsFrom && address) {
      vectors.emplace(address, std::make_pair(std::shared_ptr<const void>(vec.ptr), result));
    }
    h.u64(kVector);
    h.h(result);
    return true;
  }
  case Value::Type::RANGE: {
    const RangeType& range = value.toRange();
    h.u64(kRange);
    h.f64(range.begin_value());
    h.f64(range.step_value());
    h.f64(range.end_value());
    return true;
  }
  case Value::Type::FUNCTION: return session && session->hashFunction(h, value.toFunction());
  case Value::Type::OBJECT:
  case Value::Type::SOLUTION: return false;
  }
  return false;
}

void Stats::add(const Stats& o)
{
  calls += o.calls;
  boundaries += o.boundaries;
  hits += o.hits;
  misses += o.misses;
  stored += o.stored;
  impure += o.impure;
  unlocatable += o.unlocatable;
  relocateFailed += o.relocateFailed;
  cloneFailed += o.cloneFailed;
  nodesCloned += o.nodesCloned;
  nodesLocated += o.nodesLocated;
  messagesReplayed += o.messagesReplayed;
  staleDollar += o.staleDollar;
  unhashableArg += o.unhashableArg;
  unhashableEnv += o.unhashableEnv;
  childrenLocalDef += o.childrenLocalDef;
  childrenNoKey += o.childrenNoKey;
  unhashableChildrenVar += o.unhashableChildrenVar;
  unhashableDollar += o.unhashableDollar;
  keyTime += o.keyTime;
  closureTime += o.closureTime;
  locateTime += o.locateTime;
  reuseTime += o.reuseTime;
}

std::vector<std::shared_ptr<Entry>> *MemoTable::find(const Hash128& key)
{
  auto it = entries.find(key);
  return it == entries.end() ? nullptr : &it->second;
}

void MemoTable::store(const Hash128& key, std::shared_ptr<Entry> entry)
{
  auto& variants = entries[key];
  variants.push_back(std::move(entry));
  ++count;
  if (variants.size() > kMaxEntriesPerKey) {
    variants.erase(variants.begin());
    --count;
  }
}

uint32_t MemoTable::fileId(const std::string& path)
{
  auto [it, inserted] = fileIds.try_emplace(path, 0);
  if (inserted) it->second = static_cast<uint32_t>(fileIds.size());
  return it->second;
}

size_t MemoTable::evict(uint64_t keep)
{
  size_t dropped = 0;
  for (auto it = entries.begin(); it != entries.end();) {
    auto& variants = it->second;
    const size_t before = variants.size();
    variants.erase(std::remove_if(variants.begin(), variants.end(),
                                  [&](const std::shared_ptr<Entry>& e) {
                                    return e->lastUsed + keep < generation_;
                                  }),
                   variants.end());
    dropped += before - variants.size();
    if (variants.empty()) it = entries.erase(it);
    else ++it;
  }
  count -= dropped;
  return dropped;
}

struct DefInfo {
  Hash128 own;
  std::vector<Identifier> vars;
  std::vector<Identifier> functions;
  std::vector<Identifier> modules;
};

struct Closure {
  Hash128 code;
  std::vector<Identifier> freeVars;  // ordinary (non-$) names referenced anywhere in it
};

// A children block's syntax hash and the names it refers to; immutable, so
// computed once per evaluation however often the call runs.
struct EvalMemoSession::ScopeInfo {
  Hash128 syntax;
  std::vector<Identifier> vars;
  std::vector<Identifier> functions;
  std::vector<Identifier> modules;
};

/*
 * Brackets one candidate boundary's evaluation and records what it depends on
 * beyond its key: the $ variables it reads from frames below `base` (the
 * special-variable stack at the call), whether it read the module-name stack,
 * what it printed, and whether it touched anything impure. Hands all of that on
 * to the enclosing boundary when it ends, however it ends.
 *
 * A read made only to compute the same variable again -- `$transform =
 * $transform * m` -- is kept apart as pending: the value it saw cannot reach
 * the output unless something reads that variable for real, so it becomes a
 * dependency only if some real read of the name happens anywhere inside.
 */
class Recorder
{
public:
  struct Read {
    Identifier name;
    size_t index;
    Hash128 value;
    bool absent;
  };

  explicit Recorder(EvalMemoSession& session) : session(session) {}
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // Starts recording. A recorder is reused once it has finished.
  void begin(size_t frames)
  {
    base = frames;
    impure = false;
    unhashable = false;
    readsModuleStack = false;
    reads.clear();
    pending.clear();
    realNames.clear();
    messages.clear();
    nested.clear();
    g_message_capture.push_back(&messages);
  }

  static bool contains(const std::vector<Read>& reads, const Identifier& name)
  {
    for (const auto& r : reads) {
      if (r.name == name) return true;
    }
    return false;
  }
  bool hasRealName(const Identifier& name) const
  {
    for (const auto& n : realNames) {
      if (n == name) return true;
    }
    return false;
  }
  void addRealName(const Identifier& name)
  {
    if (!hasRealName(name)) realNames.push_back(name);
  }
  void add(std::vector<Read>& list, const Identifier& name, size_t index, const Value *value)
  {
    if (contains(list, name)) return;
    Read read{name, index, {}, value == nullptr};
    if (value) {
      Hasher h;
      if (!session.values.hash(h, *value)) {
        unhashable = true;
        return;
      }
      read.value = h.finish();
    }
    list.push_back(read);
  }
  static void merge(std::vector<Read>& list, const Read& read)
  {
    if (!contains(list, read.name)) list.push_back(read);
  }

  // Stops recording and hands what the call saw to `parent`, the recording
  // call around it, if there is one.
  void finish(Recorder *parent)
  {
    g_message_capture.pop_back();
    if (!g_message_capture.empty()) {
      auto *outer = g_message_capture.back();
      outer->insert(outer->end(), messages.begin(), messages.end());
    }
    // A pending read becomes a dependency once its name was read for real.
    for (auto& p : pending) {
      if (hasRealName(p.name)) {
        merge(reads, p);
        p.index = kPromoted;
      }
    }
    if (!parent) return;
    parent->impure |= impure;
    parent->unhashable |= unhashable;
    parent->readsModuleStack |= readsModuleStack;
    for (const auto& r : reads) {
      if (r.index == kNoFrame || r.index == kPromoted || r.index < parent->base) merge(parent->reads, r);
    }
    for (const auto& p : pending) {
      if (p.index == kPromoted) continue;
      if (p.index == kNoFrame || p.index < parent->base) merge(parent->pending, p);
    }
    for (const auto& n : realNames) parent->addRealName(n);
  }

  // The call: its key, the children key for its module context, its site,
  // and the context it was made from (the caller's).
  Hash128 key;
  uint64_t childrenKey[3] = {0, 0, 0};
  const ModuleInstantiation *inst = nullptr;
  const Context *context = nullptr;
  // Results of the boundaries inside it that were stored or reused.
  std::vector<EvalMemoSession::NestedResult> nested;

  size_t base = 0;  // $ reads from frames below this one are the call's dependencies
  bool impure = false;
  bool unhashable = false;
  bool readsModuleStack = false;
  std::vector<Read> reads;
  std::vector<Read> pending;
  std::vector<Identifier> realNames;
  std::vector<Message> messages;

private:
  EvalMemoSession& session;
};

EvalMemoSession::EvalMemoSession(MemoTable& table, const SourceFile& root)
  : table(table),
    root(root),
    generation(++table.generation_),
    values(this),
    debug(std::getenv("OPENSCAD_MEMO_DEBUG") != nullptr)
{
  Hasher h;
  for (auto it = Feature::begin(); it != Feature::end(); ++it) {
    if ((*it)->is_enabled()) h.str((*it)->get_name());
  }
  configHash = h.finish();
  for (const auto& dir : get_library_path()) {
    std::error_code ec;
    std::string normalized = fs::weakly_canonical(fs::path(dir), ec).generic_string();
    if (ec || normalized.empty()) normalized = fs::path(dir).lexically_normal().generic_string();
    if (normalized.empty()) continue;
    if (normalized.back() != '/') normalized += '/';
    libraryDirs.push_back(std::move(normalized));
  }
}

EvalMemoSession::~EvalMemoSession()
{
  // Only if an evaluation was torn down mid-call: g_message_capture must not
  // keep pointing into recorders that are gone.
  while (!recorders.empty()) spareRecorders.push_back(popRecorder());
}

Recorder& EvalMemoSession::pushRecorder(size_t base)
{
  if (spareRecorders.empty()) {
    recorders.push_back(std::make_unique<Recorder>(*this));
  } else {
    recorders.push_back(std::move(spareRecorders.back()));
    spareRecorders.pop_back();
  }
  Recorder& recorder = *recorders.back();
  recorder.begin(base);
  return recorder;
}

std::unique_ptr<Recorder> EvalMemoSession::popRecorder()
{
  std::unique_ptr<Recorder> recorder = std::move(recorders.back());
  recorders.pop_back();
  recorder->finish(recorders.empty() ? nullptr : recorders.back().get());
  return recorder;
}

void EvalMemoSession::noteImpure(EvaluationSession *session)
{
  if (!session) return;
  EvalMemoSession *memo = session->memo();
  if (memo && !memo->recorders.empty()) {
    memo->recorders.back()->impure = true;
    memo->reason("impure builtin under a boundary");
  }
}

void EvalMemoSession::noteModuleStackRead(EvaluationSession *session)
{
  if (!session) return;
  EvalMemoSession *memo = session->memo();
  if (memo && !memo->recorders.empty()) memo->recorders.back()->readsModuleStack = true;
}

void EvalMemoSession::noteDollarRead(const Identifier& name, size_t index, const Value *value)
{
  if (recorders.empty() || suspendRecording) return;
  Recorder& r = *recorders.back();
  const bool outside = index == kNoFrame || index < r.base;
  // An unset variable is reported, so even an accumulator's read of one shows
  // in the output.
  if (accumulatorRead && value) {
    if (outside) r.add(r.pending, name, index, value);
    return;
  }
  r.addRealName(name);
  if (outside) r.add(r.reads, name, index, value);
}

bool EvalMemoSession::isUserFile(const ModuleInstantiation *inst)
{
  const fs::path& path = inst->location().filePath();
  const void *key = &path;
  auto it = userFiles.find(key);
  if (it != userFiles.end()) return it->second;
  bool user = true;
  if (!path.empty()) {
    std::error_code ec;
    std::string normalized = fs::weakly_canonical(path, ec).generic_string();
    if (ec || normalized.empty()) normalized = path.lexically_normal().generic_string();
    for (const auto& dir : libraryDirs) {
      if (normalized.compare(0, dir.size(), dir) == 0) {
        user = false;
        break;
      }
    }
  }
  userFiles.emplace(key, user);
  return user;
}

const DefInfo& EvalMemoSession::defInfo(const void *def, bool isModule)
{
  auto it = defInfos.find(def);
  if (it != defInfos.end()) return *it->second;
  ASTHasher h;
  if (isModule) h.userModule(*static_cast<const UserModule *>(def));
  else h.userFunction(*static_cast<const UserFunction *>(def));
  // Relative paths in import() and surface() resolve against the directory of
  // the file the code is in, so the same text elsewhere is different code.
  const Location& location = isModule ? static_cast<const UserModule *>(def)->location()
                                      : static_cast<const UserFunction *>(def)->location();
  h.str(location.filePath().parent_path().generic_string());
  auto info = std::make_unique<DefInfo>();
  info->own = h.finish();
  sortUnique(h.vars);
  sortUnique(h.functions);
  sortUnique(h.modules);
  info->vars = std::move(h.vars);
  info->functions = std::move(h.functions);
  info->modules = std::move(h.modules);
  return *defInfos.emplace(def, std::move(info)).first->second;
}

Hash128 EvalMemoSession::fileHash(const SourceFile& file)
{
  auto it = fileHashes.find(&file);
  if (it != fileHashes.end()) return it->second;
  // Placeholder first, so a `use` cycle terminates.
  fileHashes.emplace(&file, Hash128{});
  ASTHasher h;
  h.u64(kUsedFile);
  h.str(file.getFullpath());
  h.scope(*file.scope);
  h.u64(file.usedlibs.size());
  for (const auto& path : file.usedlibs) {
    h.str(path);
    if (const SourceFile *used = SourceFileCache::instance()->lookup(path)) h.h(fileHash(*used));
    else h.u64(kAbsent);
  }
  const Hash128 result = h.finish();
  fileHashes[&file] = result;
  return result;
}

const Closure& EvalMemoSession::closure(const void *def, bool isModule, const SourceFile& file)
{
  auto it = closures.find(def);
  if (it != closures.end()) return *it->second;

  auto result = std::make_unique<Closure>();
  std::vector<std::pair<const void *, bool>> stack{{def, isModule}};
  std::unordered_set<const void *> visited{def};
  std::vector<Hash128> hashes;
  std::vector<const SourceFile *> usedFiles;
  while (!stack.empty()) {
    const auto [current, currentIsModule] = stack.back();
    stack.pop_back();
    const DefInfo& info = defInfo(current, currentIsModule);
    hashes.push_back(info.own);
    for (const auto& name : info.vars) {
      if (!name.isConfigVariable()) result->freeVars.push_back(name);
    }
    for (const auto& name : info.functions) {
      if (const auto defined = file.scope->lookup<UserFunction *>(name)) {
        if (visited.insert(*defined).second) stack.emplace_back(*defined, false);
      } else if (const SourceFile *used = usedFileDefining(file, name, false)) {
        usedFiles.push_back(used);
      }
    }
    for (const auto& name : info.modules) {
      if (const auto defined = file.scope->lookup<UserModule *>(name)) {
        if (visited.insert(*defined).second) stack.emplace_back(*defined, true);
      } else if (const SourceFile *used = usedFileDefining(file, name, true)) {
        usedFiles.push_back(used);
      }
    }
  }
  std::sort(hashes.begin(), hashes.end());
  std::vector<Hash128> usedHashes;
  usedHashes.reserve(usedFiles.size());
  for (const SourceFile *used : usedFiles) usedHashes.push_back(fileHash(*used));
  std::sort(usedHashes.begin(), usedHashes.end());
  usedHashes.erase(std::unique(usedHashes.begin(), usedHashes.end()), usedHashes.end());

  Hasher h;
  h.u64(kClosure);
  h.u64(hashes.size());
  for (const auto& x : hashes) h.h(x);
  h.u64(usedHashes.size());
  for (const auto& x : usedHashes) h.h(x);
  result->code = h.finish();
  sortUnique(result->freeVars);
  return *closures.emplace(def, std::move(result)).first->second;
}

bool EvalMemoSession::envHash(const void *def, bool isModule, const FileContext& file, Hasher& out)
{
  const EnvKey key{def, file.scopeSerial()};
  auto it = envs.find(key);
  if (it == envs.end()) {
    const auto start = std::chrono::steady_clock::now();
    EnvResult result{true, {}};
    const Closure& c = closure(def, isModule, *file.sourceFile());
    Hasher h;
    h.h(c.code);
    for (const auto& name : c.freeVars) {
      // Only names this file scope binds; anything else is a local, a
      // parameter, or a builtin constant.
      const auto value = file.lookup_local_variable(name);
      if (!value) continue;
      h.id(name);
      if (!values.hash(h, *value)) {
        result.ok = false;
        reason("env " + name.str() + " (" + value->typeName() + ") in closure of " +
               (isModule ? static_cast<const UserModule *>(def)->name
                         : static_cast<const UserFunction *>(def)->name));
        break;
      }
    }
    result.hash = h.finish();
    it = envs.emplace(key, result).first;
    stats_.closureTime += std::chrono::steady_clock::now() - start;
  }
  if (!it->second.ok) return false;
  out.h(it->second.hash);
  return true;
}

/*
 * Functions and modules, resolved through the scopes the evaluator would walk
 * from `context`. A definition local to a module body would need that body's
 * variables too; not handled, so the caller is not reused.
 */
bool EvalMemoSession::resolveRef(const Context *context, const Identifier& name, bool isModule, Hasher& h)
{
  for (const Context *c = context; c; c = c->getParent().get()) {
    const auto *scope = dynamic_cast<const ScopeContext *>(c);
    if (!scope) continue;
    const auto *file = dynamic_cast<const FileContext *>(c);
    const void *defined = nullptr;
    if (isModule) {
      if (auto m = scope->localScope().lookup<UserModule *>(name)) defined = *m;
    } else {
      if (auto f = scope->localScope().lookup<UserFunction *>(name)) defined = *f;
    }
    if (defined) {
      if (!file) {
        ++stats_.childrenLocalDef;
        reason(std::string("local definition ") + name.str());
        return false;
      }
      return envHash(defined, isModule, *file, h);
    }
    if (file) {
      if (const SourceFile *used = usedFileDefining(*file->sourceFile(), name, isModule)) {
        h.h(fileHash(*used));
      }
      return true;  // otherwise a builtin
    }
  }
  return true;
}

bool EvalMemoSession::hashFunction(Hasher& h, const FunctionType& function)
{
  // A function that captures itself, directly or through others.
  for (size_t i = 0; i < hashingFunctions.size(); ++i) {
    if (hashingFunctions[i] == &function) {
      h.u64(kRecursiveFunction);
      h.u64(hashingFunctions.size() - i);
      return true;
    }
  }
  const auto& info = functionInfo(function);
  hashingFunctions.push_back(&function);
  struct Pop {
    std::vector<const FunctionType *>& v;
    ~Pop() { v.pop_back(); }
  } pop{hashingFunctions};

  h.u64(kFunctionValue);
  h.h(info.syntax);
  const Context *context = function.getContext().get();
  for (const auto& name : info.vars) {
    // $ names are looked up where the function is called, and recorded there.
    if (name.isConfigVariable()) continue;
    h.id(name);
    const auto value = context ? context->try_lookup_variable(name) : boost::none;
    if (!value) {
      h.u64(kAbsent);
    } else if (!values.hash(h, *value)) {
      return false;
    }
  }
  for (const auto& name : info.functions) {
    if (!resolveRef(context, name, false, h)) return false;
  }
  return true;
}

const EvalMemoSession::FunctionInfo& EvalMemoSession::functionInfo(const FunctionType& function)
{
  const void *key = function.getExpr().get();
  auto it = functionInfos.find(key);
  if (it != functionInfos.end()) return *it->second;
  ASTHasher h;
  if (function.getParameters()) h.assignments(*function.getParameters());
  h.expr(function.getExpr().get());
  auto info = std::make_unique<FunctionInfo>();
  info->syntax = h.finish();
  sortUnique(h.vars);
  sortUnique(h.functions);
  info->vars = std::move(h.vars);
  info->functions = std::move(h.functions);
  return *functionInfos.emplace(key, std::move(info)).first->second;
}

Hash128 EvalMemoSession::scopeHash(const LocalScope& scope)
{
  return scopeInfo(scope).syntax;
}

Hash128 EvalMemoSession::moduleHash(const UserModule& module)
{
  return defInfo(&module, true).own;
}

const EvalMemoSession::ScopeInfo& EvalMemoSession::scopeInfo(const LocalScope& scope)
{
  auto it = scopeInfos.find(&scope);
  if (it != scopeInfos.end()) return *it->second;
  ASTHasher h;
  h.scope(scope);
  auto info = std::make_unique<ScopeInfo>();
  info->syntax = h.finish();
  sortUnique(h.vars);
  sortUnique(h.functions);
  sortUnique(h.modules);
  info->vars = std::move(h.vars);
  info->functions = std::move(h.functions);
  info->modules = std::move(h.modules);
  return *scopeInfos.emplace(&scope, std::move(info)).first->second;
}

bool EvalMemoSession::childrenHash(const ModuleInstantiation *inst,
                                   const std::shared_ptr<const Context>& context, Hasher& h)
{
  const LocalScope& children = *inst->scope;
  if (children.numElements() == 0 && children.moduleDefinitions().empty() &&
      children.functionDefinitions().empty()) {
    h.u64(kNoChildren);
    return true;
  }
  const ScopeInfo& syntax = scopeInfo(children);
  h.h(syntax.syntax);

  // Variables, as the caller's context sees them now. $ names are read when
  // the children run, inside the call, and recorded there.
  for (const auto& name : syntax.vars) {
    if (name.isConfigVariable()) continue;
    const auto value = context->try_lookup_variable(name);
    h.id(name);
    if (!value) {
      h.u64(kAbsent);
    } else if (!values.hash(h, *value)) {
      ++stats_.unhashableChildrenVar;
      reason("children var " + name.str() + " (" + value->typeName() + ")");
      return false;
    }
  }
  for (const auto& name : syntax.functions) {
    if (!resolveRef(context.get(), name, false, h)) return false;
  }
  for (const auto& name : syntax.modules) {
    if (name == childrenModule()) {
      // children() here means the enclosing module's children.
      bool found = false;
      for (const Context *c = context.get(); c; c = c->getParent().get()) {
        const auto *module = dynamic_cast<const UserModuleContext *>(c);
        if (!module) continue;
        const uint64_t *key = module->childrenKey();
        if (!key) {
          ++stats_.childrenNoKey;
          reason("children() of a module call without a key, in call of " + inst->name().str());
          return false;
        }
        h.u64(kEnclosingChildren);
        h.u64(key[0]);
        h.u64(key[1]);
        found = true;
        break;
      }
      if (!found) h.u64(kNoEnclosingChildren);
      continue;
    }
    if (!resolveRef(context.get(), name, true, h)) return false;
  }
  return true;
}

bool EvalMemoSession::computeKey(const UserModule& module, const FileContext& definingFile,
                                 const ModuleInstantiation *inst,
                                 const std::shared_ptr<const Context>& context,
                                 const Arguments& arguments, Hash128& key, uint64_t childrenKey[3])
{
  Hasher h;
  h.u64(kKey + kKeyVersion);
  h.h(configHash);
  // The children block's import()s resolve against the call site's directory.
  h.str(inst->location().filePath().parent_path().generic_string());

  if (!envHash(&module, true, definingFile, h)) {
    ++stats_.unhashableEnv;
    return false;
  }

  h.u64(arguments.size());
  for (const auto& argument : arguments) {
    if (argument.name) h.id(*argument.name);
    else h.u64(0);
    if (!values.hash(h, argument.value)) {
      ++stats_.unhashableArg;
      reason("argument (" + argument.value.typeName() + ") to " + inst->name().str());
      return false;
    }
  }

  Hasher children;
  if (!childrenHash(inst, context, children)) return false;
  const Hash128 ck = children.finish();
  childrenKey[0] = ck.a;
  childrenKey[1] = ck.b;
  childrenKey[2] = 0;
  h.h(ck);
  key = h.finish();
  return true;
}

Hash128 EvalMemoSession::moduleStackHash()
{
  Hasher h;
  const int size = UserModule::stack_size();
  h.u64(kModuleStack);
  h.u64(size);
  for (int i = 0; i < size; ++i) h.str(UserModule::stack_element(i));
  return h.finish();
}

bool EvalMemoSession::matches(EvaluationSession& session, const Entry& entry)
{
  ++suspendRecording;
  bool ok = true;
  for (const auto& read : entry.reads) {
    const auto value = session.try_lookup_special_variable(read.name);
    if (!value || read.absent) {
      ok = !value && read.absent;
    } else {
      Hasher h;
      ok = values.hash(h, *value) && h.finish() == read.value;
    }
    if (!ok) break;
  }
  if (ok && entry.readsModuleStack) ok = moduleStackHash() == entry.moduleStack;
  --suspendRecording;
  return ok;
}

void EvalMemoSession::replayReads(EvaluationSession& session, const Entry& entry)
{
  if (recorders.empty()) return;
  for (const auto& read : entry.reads) (void)session.try_lookup_special_variable(read.name);
  accumulatorRead = true;
  for (const auto& name : entry.accumulated) (void)session.try_lookup_special_variable(name);
  accumulatorRead = false;
  Recorder& r = *recorders.back();
  for (const auto& name : entry.realNames) r.addRealName(name);
  r.readsModuleStack |= entry.readsModuleStack;
}

void EvalMemoSession::replay(const std::vector<Message>& messages)
{
  for (const auto& message : messages) {
    if (message.group == message_group::Deprecated) {
      // As make_message_obj() does: printed the first time only, but always
      // handed to whoever records, who may replay it where it is the first.
      const std::string seen = message.msg + message.loc.toRelativeString(message.docPath);
      if (!printedDeprecations.insert(seen).second) {
        if (!g_message_capture.empty()) {
          Message repeat = message;
          repeat.repeat = true;
          g_message_capture.back()->push_back(std::move(repeat));
        }
        continue;
      }
      if (message.repeat) {
        Message first = message;
        first.repeat = false;
        ++stats_.messagesReplayed;
        PRINT(first);
        continue;
      }
    }
    ++stats_.messagesReplayed;
    PRINT(message);
  }
}

NOINLINE Call EvalMemoSession::enter(const UserModule& module,
                                     const std::shared_ptr<const Context>& defining_context,
                                     const ModuleInstantiation *inst,
                                     const std::shared_ptr<const Context>& context,
                                     const Arguments& arguments)
{
  ++stats_.calls;
  const auto *definingFile = dynamic_cast<const FileContext *>(defining_context.get());
  if (!definingFile || !isUserFile(inst)) return Call::Plain;
  ++stats_.boundaries;
  EvaluationSession& session = *context->session();

  const auto start = std::chrono::steady_clock::now();
  Hash128 key;
  uint64_t childrenKey[3] = {0, 0, 0};
  const bool eligible = computeKey(module, *definingFile, inst, context, arguments, key, childrenKey);
  stats_.keyTime += std::chrono::steady_clock::now() - start;
  if (!eligible) return Call::Plain;

  if (auto *variants = table.find(key)) {
    bool matched = false;
    for (const auto& entry : *variants) {
      if (!matches(session, *entry)) continue;
      matched = true;
      std::vector<const ModuleInstantiation *> statements;
      reused = reuse(session, *entry, inst, *context, statements);
      if (!reused) break;  // evaluate instead; reuse() counted why
      if (!recorders.empty()) {
        recorders.back()->nested.push_back({reused.get(), entry, std::move(statements)});
      }
      return Call::Reused;
    }
    if (!matched) {
      ++stats_.staleDollar;
      if (debug) reason("$ variables differ for " + inst->name().str());
    }
  }

  ++stats_.misses;
  if (debug) {
    reason("miss " + inst->name().str() + " at " +
           inst->location().filePath().filename().generic_string() + ":" +
           std::to_string(inst->location().firstLine()));
  }
  Recorder& recorder = pushRecorder(session.frames().size());
  recorder.key = key;
  std::copy(childrenKey, childrenKey + 3, recorder.childrenKey);
  recorder.inst = inst;
  recorder.context = context.get();
  return Call::Recording;
}

const uint64_t *EvalMemoSession::childrenKey() const
{
  return recorders.back()->childrenKey;
}

NOINLINE void EvalMemoSession::leave(const std::shared_ptr<AbstractNode>& node)
{
  std::unique_ptr<Recorder> recorder = popRecorder();
  store(node, *recorder);
  spareRecorders.push_back(std::move(recorder));
}

NOINLINE void EvalMemoSession::abandon()
{
  spareRecorders.push_back(popRecorder());
  // Whatever caught the exception made the enclosing call's result depend on
  // it, and an error can depend on what no key covers, such as stack depth.
  if (!recorders.empty()) recorders.back()->impure = true;
}

NOINLINE std::shared_ptr<AbstractNode> EvalMemoSession::reuse(
  EvaluationSession& session, Entry& entry, const ModuleInstantiation *inst, const Context& context,
  std::vector<const ModuleInstantiation *>& statements)
{
  const auto start = std::chrono::steady_clock::now();
  struct Timer {
    std::chrono::steady_clock::time_point start;
    std::chrono::nanoseconds& total;
    ~Timer() { total += std::chrono::steady_clock::now() - start; }
  } timer{start, stats_.reuseTime};
  if (!relocate(entry, inst, context, statements)) {
    ++stats_.relocateFailed;
    if (debug) reason("statements not found in this parse, under " + inst->name().str());
    return nullptr;
  }
  // From now on the entry, and those nested in it, share the copy's nodes,
  // which point into this parse.
  auto copy = copyTree(entry, statements, inst);
  if (!copy) {
    ++stats_.cloneFailed;
    return nullptr;
  }
  ++stats_.hits;
  stats_.nodesCloned += entry.nodes;
  replayReads(session, entry);
  replay(entry.messages);
  return copy;
}

NOINLINE void EvalMemoSession::store(const std::shared_ptr<AbstractNode>& node, Recorder& recorder)
{
  const ModuleInstantiation *inst = recorder.inst;
  if (recorder.impure || recorder.unhashable || !node) {
    if (recorder.unhashable) {
      ++stats_.unhashableDollar;
      reason("unhashable $ read under " + inst->name().str());
    } else {
      ++stats_.impure;
    }
    return;
  }
  auto entry = std::make_shared<Entry>();
  std::vector<const ModuleInstantiation *> statements;
  const auto start = std::chrono::steady_clock::now();
  const bool located = locate(*node, inst, *recorder.context, recorder.nested, *entry, statements);
  stats_.locateTime += std::chrono::steady_clock::now() - start;
  if (!located) {
    ++stats_.unlocatable;
    reason("a statement under " + inst->name().str() + " has no place to record");
    // Whatever was stored inside still belongs to the caller's result.
    if (!recorders.empty()) {
      auto& outer = recorders.back()->nested;
      std::move(recorder.nested.begin(), recorder.nested.end(), std::back_inserter(outer));
    }
    return;
  }
  entry->messages = std::move(recorder.messages);
  entry->reads.reserve(recorder.reads.size());
  for (const auto& r : recorder.reads) entry->reads.push_back(DollarRead{r.name, r.value, r.absent});
  for (const auto& p : recorder.pending) {
    if (p.index != kPromoted) entry->accumulated.push_back(p.name);
  }
  entry->realNames = std::move(recorder.realNames);
  entry->readsModuleStack = recorder.readsModuleStack;
  if (entry->readsModuleStack) entry->moduleStack = moduleStackHash();
  entry->root = node;
  entry->lastUsed = generation;
  table.store(recorder.key, entry);
  ++stats_.stored;
  stats_.nodesLocated += entry->nodeCodes.size() + 1;
  // leave() popped this call's recorder: the top one is the caller's.
  if (!recorders.empty()) recorders.back()->nested.push_back({node.get(), entry, std::move(statements)});
}

}  // namespace memo
