#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/Children.h"
#include "core/Context.h"
#include "core/EvalMemo.h"
#include "core/LocalScope.h"
#include "core/ModuleInstantiation.h"
#include "core/ScopeContext.h"
#include "core/SourceFile.h"
#include "core/SourceFileCache.h"
#include "core/UserModule.h"
#include "core/node.h"
#include "utils/StackCheck.h"

namespace memo {

namespace {

// What a path word does at the index in its upper bits.
enum PathOp : uint32_t {
  kPathChildren = 0,
  kPathElse = 1,
  kPathDefinition = 2,  // go into the body of the module Entry::names[index]; the next
                        // word is how many definitions of that name follow it
  kPathStatement = 3,   // that statement: the end of the path
};

uint32_t pathWord(uint32_t index, PathOp op)
{
  return (index << 2) | op;
}

constexpr uint32_t kNotFound = UINT32_MAX;

constexpr size_t kMaxEnclosing = 256;

// Catches a path that leads to a different statement; the anchor's hash covers the rest.
uint64_t fingerprint(const ModuleInstantiation& statement)
{
  Hasher h;
  h.id(statement.name());
  h.u64((statement.tag_root ? 1 : 0) | (statement.tag_highlight ? 2 : 0) |
        (statement.tag_background ? 4 : 0));
  h.u64(statement.arguments.size());
  h.u64(statement.scope->moduleInstantiations.size());
  return h.finish().a;
}

uint32_t laterDefinitions(const UserModule& module, const LocalScope& scope)
{
  const auto& definitions = scope.moduleDefinitions();
  uint32_t later = 0;
  for (auto it = definitions.rbegin(); it != definitions.rend(); ++it) {
    if (it->second.get() == &module) return later;
    if (it->first == module.name) ++later;
  }
  return kNotFound;
}

const UserModule *findDefinition(const LocalScope& scope, const Identifier& name, uint32_t later)
{
  if (later == 0) {
    // The last definition of a name is the one lookups find.
    const auto found = scope.lookup<UserModule *>(name);
    return found ? *found : nullptr;
  }
  const auto& definitions = scope.moduleDefinitions();
  for (auto it = definitions.rbegin(); it != definitions.rend(); ++it) {
    if (it->first != name.str()) continue;
    if (later-- == 0) return it->second.get();
  }
  return nullptr;
}

// The children blocks of the user-module calls enclosing `context`, nearest first, which are what
// children() reaches from there. False if there are more than kMaxEnclosing.
bool enclosingChildren(const Context& context, std::vector<const LocalScope *>& out)
{
  const Context *c = &context;
  while (c) {
    const UserModuleContext *module = nullptr;
    for (; c; c = c->getParent().get()) {
      if ((module = dynamic_cast<const UserModuleContext *>(c))) break;
    }
    if (!module) return true;
    if (out.size() == kMaxEnclosing) return false;
    const Children *children = module->user_module_children();
    out.push_back(children->getScope().get());
    c = children->getContext().get();
  }
  return true;
}

struct TreeCopy {
  uint64_t generation;
  std::vector<std::pair<Entry *, std::shared_ptr<AbstractNode>>> copies;

  std::shared_ptr<AbstractNode> entry(const AbstractNode& root, Entry& entry,
                                      const std::vector<const ModuleInstantiation *>& statements,
                                      const ModuleInstantiation *statement)
  {
    std::shared_ptr<AbstractNode> result = root.copy();
    if (!result) return nullptr;
    result->modinst = statement;
    size_t next = 0;
    result->children.reserve(root.children.size());
    for (const auto& child : root.children) {
      auto child_copy = coded(*child, entry, statements, next);
      if (!child_copy) return nullptr;
      result->children.push_back(std::move(child_copy));
    }
    if (next != entry.nodeCodes.size()) return nullptr;
    result->takeDigest(root);
    copies.emplace_back(&entry, result);
    return result;
  }

  std::shared_ptr<AbstractNode> coded(const AbstractNode& node, Entry& entry,
                                      const std::vector<const ModuleInstantiation *>& statements,
                                      size_t& next)
  {
    // Near the stack limit, evaluate instead, which then fails as a fresh evaluation would.
    if (StackCheck::inst().check()) return nullptr;
    if (next >= entry.nodeCodes.size()) return nullptr;
    const uint32_t code = entry.nodeCodes[next++];
    if (code & Entry::kNested) {
      const uint32_t index = code & ~Entry::kNested;
      if (index >= entry.nested.size()) return nullptr;
      Entry::Nested& nested = entry.nested[index];
      std::vector<const ModuleInstantiation *> inner(nested.sites.size());
      for (size_t i = 0; i < inner.size(); ++i) {
        if (nested.sites[i] >= statements.size()) return nullptr;
        inner[i] = statements[nested.sites[i]];
      }
      if (nested.site >= statements.size()) return nullptr;
      return this->entry(node, *nested.entry, inner, statements[nested.site]);
    }
    if (code >= statements.size()) return nullptr;
    std::shared_ptr<AbstractNode> result = node.copy();
    if (!result) return nullptr;
    result->modinst = statements[code];
    result->children.reserve(node.children.size());
    for (const auto& child : node.children) {
      auto child_copy = coded(*child, entry, statements, next);
      if (!child_copy) return nullptr;
      result->children.push_back(std::move(child_copy));
    }
    result->takeDigest(node);
    return result;
  }
};

}  // namespace

struct EvalMemoSession::Located {
  Located(EvalMemoSession& session, const ModuleInstantiation *inst, const Context& context,
          Entry& entry)
    : session(session), inst(inst), context(context), entry(entry)
  {
  }

  // A scope's anchor in `entry` (-1 if none) and the path to it from there, as words[begin, end).
  struct ScopePath {
    int32_t anchor = -1;
    uint32_t begin = 0;
    uint32_t end = 0;
  };

  EvalMemoSession& session;
  const ModuleInstantiation *inst;
  const Context& context;
  Entry& entry;

  std::unordered_map<const LocalScope *, ScopePath> scopes;
  std::vector<uint32_t> words;
  std::unordered_map<const void *, int32_t> anchors;  // by module, or by children block
  std::vector<const LocalScope *> anchorScopes;       // children blocks, by anchor
  std::unordered_map<const ModuleInstantiation *, uint32_t> sites;

  bool enclosingKnown = false;
  bool enclosingComplete = true;
  std::vector<const LocalScope *> enclosing;

  std::vector<const ModuleInstantiation *> statements;

  std::unordered_map<const AbstractNode *, const NestedResult *> nestedRoots;
  std::unordered_set<const ModuleInstantiation *> nestedCalls;
  std::vector<bool> siteCallsNested;

  const ModuleInstantiation *lastStatement = nullptr;
  uint32_t lastSite = 0;

  // 0 if `scope` is no enclosing call's children block, else its depth, or -1 if that cannot be
  // told: recursion can put a block at two depths, and the list stops at kMaxEnclosing.
  int enclosingDepth(const LocalScope *scope)
  {
    if (!enclosingKnown) {
      enclosingKnown = true;
      enclosingComplete = enclosingChildren(context, enclosing);
    }
    int depth = 0;
    for (size_t k = 0; k < enclosing.size(); ++k) {
      if (enclosing[k] != scope) continue;
      if (depth) return -1;
      depth = static_cast<int>(k + 1);
    }
    if (!depth && !enclosingComplete) return -1;
    return depth;
  }

  int32_t childrenAnchor(const LocalScope *scope, uint32_t depth)
  {
    auto [it, inserted] = anchors.try_emplace(scope, 0);
    if (inserted) {
      Anchor anchor;
      anchor.kind = depth ? Anchor::Kind::EnclosingChildren : Anchor::Kind::OwnChildren;
      anchor.depth = depth;
      anchor.hash = session.scopeHash(*scope);
      it->second = static_cast<int32_t>(entry.anchors.size());
      entry.anchors.push_back(anchor);
      anchorScopes.push_back(scope);
    }
    return it->second;
  }

  int32_t definitionAnchor(const UserModule& module, const SourceFile& file)
  {
    auto it = anchors.find(&module);
    if (it != anchors.end()) return it->second;
    const Anchor *anchor = session.definitionAnchor(module, file);
    if (!anchor) return -1;
    const auto index = static_cast<int32_t>(entry.anchors.size());
    entry.anchors.push_back(*anchor);
    anchorScopes.push_back(nullptr);
    anchors.emplace(&module, index);
    return index;
  }

  ScopePath extend(const ScopePath& parent, uint32_t step, uint32_t operand, bool hasOperand)
  {
    ScopePath path;
    path.anchor = parent.anchor;
    words.reserve(words.size() + (parent.end - parent.begin) + 2);
    path.begin = static_cast<uint32_t>(words.size());
    for (uint32_t i = parent.begin; i < parent.end; ++i) words.push_back(words[i]);
    words.push_back(step);
    if (hasOperand) words.push_back(operand);
    path.end = static_cast<uint32_t>(words.size());
    return path;
  }

  bool scopePath(const LocalScope *scope, ScopePath& out)
  {
    auto it = scopes.find(scope);
    if (it != scopes.end()) {
      out = it->second;
      return out.anchor >= 0;
    }
    ScopePath path;
    compute(scope, path);
    scopes.emplace(scope, path);
    out = path;
    return path.anchor >= 0;
  }

  void compute(const LocalScope *scope, ScopePath& path)
  {
    using Kind = LocalScope::Origin::Kind;
    const LocalScope::Origin& origin = scope->origin;
    switch (origin.kind) {
    case Kind::Children:
    case Kind::Else: {
      const ModuleInstantiation *owner = origin.statement;
      if (!owner || !owner->parent_scope) return;
      if (origin.kind == Kind::Children) {
        // The innermost anchor wins: a block inside the reused call's own
        // children belongs to them even where they sit in a module body.
        const int depth = enclosingDepth(scope);
        if (depth < 0) return;
        if (owner == inst) {
          if (depth == 0) path.anchor = childrenAnchor(scope, 0);
          return;
        }
        if (depth > 0) {
          path.anchor = childrenAnchor(scope, depth);
          return;
        }
      }
      ScopePath parent;
      if (!scopePath(owner->parent_scope, parent)) return;
      path = extend(parent, pathWord(owner->parent_index, origin.kind == Kind::Children ? kPathChildren : kPathElse),
                    0, false);
      return;
    }
    case Kind::ModuleBody: {
      const UserModule *module = origin.module;
      const LocalScope *outer = module ? module->parent_scope : nullptr;
      if (!outer) return;
      if (outer->origin.kind == Kind::File) {
        if (outer->origin.file) path.anchor = definitionAnchor(*module, *outer->origin.file);
        return;
      }
      const uint32_t later = laterDefinitions(*module, *outer);
      if (later == kNotFound) return;
      ScopePath parent;
      if (!scopePath(outer, parent)) return;
      const auto name = static_cast<uint32_t>(entry.names.size());
      entry.names.emplace_back(module->name);
      path = extend(parent, pathWord(name, kPathDefinition), later, true);
      return;
    }
    default:
      // A file's statements never run under a call, and the parser makes no unknown scopes.
      return;
    }
  }

  bool site(const ModuleInstantiation *statement, uint32_t& index)
  {
    auto it = sites.find(statement);
    if (it != sites.end()) {
      index = it->second;
      return true;
    }
    ScopePath path;
    if (!statement || !statement->parent_scope || !scopePath(statement->parent_scope, path)) {
      return false;
    }
    Site s;
    s.anchor = static_cast<uint32_t>(path.anchor);
    s.begin = static_cast<uint32_t>(entry.paths.size());
    entry.paths.insert(entry.paths.end(), words.begin() + path.begin, words.begin() + path.end);
    entry.paths.push_back(pathWord(statement->parent_index, kPathStatement));
    s.end = static_cast<uint32_t>(entry.paths.size());
    s.check = fingerprint(*statement);
    index = static_cast<uint32_t>(entry.sites.size());
    entry.sites.push_back(s);
    statements.push_back(statement);
    siteCallsNested.push_back(nestedCalls.count(statement) != 0);
    sites.emplace(statement, index);
    return true;
  }

  bool walk(const AbstractNode& node)
  {
    if (!node.modinst) return false;
    if (node.modinst != lastStatement) {
      if (!site(node.modinst, lastSite)) return false;
      lastStatement = node.modinst;
    }
    if (siteCallsNested[lastSite]) {
      auto it = nestedRoots.find(&node);
      if (it != nestedRoots.end()) return nest(*it->second, lastSite);
    }
    entry.nodeCodes.push_back(lastSite);
    for (const auto& child : node.children) {
      if (!walk(*child)) return false;
    }
    return true;
  }

  bool nest(const NestedResult& result, uint32_t rootSite)
  {
    Entry::Nested nested;
    nested.entry = result.entry;
    nested.site = rootSite;
    nested.sites.resize(result.statements.size());
    for (size_t i = 0; i < result.statements.size(); ++i) {
      if (!site(result.statements[i], nested.sites[i])) return false;
    }
    entry.nodeCodes.push_back(Entry::kNested | static_cast<uint32_t>(entry.nested.size()));
    entry.nested.push_back(std::move(nested));
    return true;
  }

  // A children block whose call ran again inside the subtree may have run as that inner call's
  // children rather than the anchor's, which its place alone cannot tell apart.
  bool unambiguous() const
  {
    for (size_t i = 0; i < entry.anchors.size(); ++i) {
      const LocalScope *scope = anchorScopes[i];
      if (!scope) continue;
      if (sites.count(scope->origin.statement)) return false;
    }
    return true;
  }
};

bool EvalMemoSession::locate(const AbstractNode& root, const ModuleInstantiation *inst,
                             const Context& context, const std::vector<NestedResult>& nested,
                             Entry& entry, std::vector<const ModuleInstantiation *>& statements)
{
  if (root.modinst != inst) return false;
  Located located(*this, inst, context, entry);
  for (const auto& result : nested) {
    located.nestedRoots.emplace(result.root, &result);
    located.nestedCalls.insert(result.root->modinst);
  }
  for (const auto& child : root.children) {
    if (!located.walk(*child)) return false;
  }
  if (!located.unambiguous()) return false;
  statements = std::move(located.statements);
  return true;
}

const Anchor *EvalMemoSession::definitionAnchor(const UserModule& module, const SourceFile& file)
{
  auto it = definitionAnchors.find(&module);
  if (it != definitionAnchors.end()) return &it->second;
  Anchor anchor;
  anchor.kind = Anchor::Kind::Definition;
  anchor.later = laterDefinitions(module, *file.scope);
  if (anchor.later == kNotFound) return nullptr;
  anchor.file = &file == &root ? 0 : table.fileId(file.getFullpath());
  anchor.name = Identifier(module.name);
  anchor.hash = moduleHash(module);
  return &definitionAnchors.emplace(&module, anchor).first->second;
}

const SourceFile *EvalMemoSession::usedFile(uint32_t id)
{
  if (!usedFilesKnown) {
    usedFilesKnown = true;
    std::vector<const SourceFile *> pending{&root};
    std::unordered_set<const SourceFile *> seen{&root};
    while (!pending.empty()) {
      const SourceFile *file = pending.back();
      pending.pop_back();
      for (const auto& path : file->usedlibs) {
        const SourceFile *used = SourceFileCache::instance()->lookup(path);
        if (!used || !seen.insert(used).second) continue;
        usedFiles.emplace(table.fileId(used->getFullpath()), used);
        pending.push_back(used);
      }
    }
  }
  auto it = usedFiles.find(id);
  return it == usedFiles.end() ? nullptr : it->second;
}

const LocalScope *EvalMemoSession::definitionBody(const Anchor& anchor)
{
  const DefinitionKey key{anchor.file, anchor.later, anchor.name.index()};
  auto it = definitionBodies.find(key);
  if (it == definitionBodies.end()) {
    FoundDefinition found{nullptr, {}};
    const SourceFile *file = anchor.file == 0 ? &root : usedFile(anchor.file);
    if (file) {
      if (const UserModule *module = findDefinition(*file->scope, anchor.name, anchor.later)) {
        found = {module->body.get(), moduleHash(*module)};
      }
    }
    it = definitionBodies.emplace(key, found).first;
  }
  // Entries from different versions of the code can share a key here.
  return it->second.body && it->second.hash == anchor.hash ? it->second.body : nullptr;
}

bool EvalMemoSession::relocate(const Entry& entry, const ModuleInstantiation *inst,
                               const Context& context,
                               std::vector<const ModuleInstantiation *>& statements)
{
  std::vector<const LocalScope *> scopes(entry.anchors.size());
  std::vector<const LocalScope *> enclosing;
  bool enclosingKnown = false;
  for (size_t i = 0; i < entry.anchors.size(); ++i) {
    const Anchor& anchor = entry.anchors[i];
    const LocalScope *scope = nullptr;
    switch (anchor.kind) {
    case Anchor::Kind::Definition: scope = definitionBody(anchor); break;
    case Anchor::Kind::OwnChildren: scope = inst->scope.get(); break;
    case Anchor::Kind::EnclosingChildren:
      if (!enclosingKnown) {
        enclosingKnown = true;
        enclosingChildren(context, enclosing);
      }
      if (anchor.depth >= 1 && anchor.depth <= enclosing.size()) scope = enclosing[anchor.depth - 1];
      break;
    }
    if (!scope) return false;
    if (anchor.kind != Anchor::Kind::Definition && scopeHash(*scope) != anchor.hash) return false;
    scopes[i] = scope;
  }

  statements.resize(entry.sites.size());
  for (size_t i = 0; i < entry.sites.size(); ++i) {
    const Site& site = entry.sites[i];
    if (site.anchor >= scopes.size() || site.begin >= site.end || site.end > entry.paths.size()) {
      return false;
    }
    const LocalScope *scope = scopes[site.anchor];
    const ModuleInstantiation *statement = nullptr;
    for (uint32_t w = site.begin; w < site.end; ++w) {
      const uint32_t word = entry.paths[w];
      const uint32_t index = word >> 2;
      const auto op = static_cast<PathOp>(word & 3);
      if (op == kPathDefinition) {
        if (w + 1 >= site.end || index >= entry.names.size()) return false;
        const UserModule *module = findDefinition(*scope, entry.names[index], entry.paths[++w]);
        if (!module) return false;
        scope = module->body.get();
        continue;
      }
      if (index >= scope->moduleInstantiations.size()) return false;
      const ModuleInstantiation *step = scope->moduleInstantiations[index].get();
      if (op == kPathStatement) {
        if (w + 1 != site.end) return false;
        statement = step;
      } else if (op == kPathChildren) {
        scope = step->scope.get();
      } else {
        const auto *ifelse = dynamic_cast<const IfElseModuleInstantiation *>(step);
        if (!ifelse || !ifelse->getElseScope()) return false;
        scope = ifelse->getElseScope().get();
      }
    }
    if (!statement || fingerprint(*statement) != site.check) return false;
    statements[i] = statement;
  }
  return true;
}

std::shared_ptr<AbstractNode> EvalMemoSession::copyTree(
  Entry& entry, const std::vector<const ModuleInstantiation *>& statements, const ModuleInstantiation *inst)
{
  TreeCopy copy{generation, {}};
  auto result = copy.entry(*entry.root, entry, statements, inst);
  if (!result) return nullptr;
  for (auto& [copied, root] : copy.copies) {
    replaced.push_back(std::move(copied->root));
    copied->root = std::move(root);
    copied->lastUsed = generation;
  }
  return result;
}

}  // namespace memo
