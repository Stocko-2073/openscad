#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/AST.h"
#include "core/Arguments.h"
#include "core/Children.h"
#include "core/Context.h"
#include "core/Identifier.h"
#include "core/SourceFile.h"
#include "core/callables.h"

class UserModule;

class ScopeContext : public Context
{
public:
  void init() override;
  boost::optional<CallableFunction> lookup_local_function(const Identifier& name,
                                                          const Location& loc) const override;
  boost::optional<InstantiableModule> lookup_local_module(const Identifier& name,
                                                          const Location& loc) const override;
  [[nodiscard]] const LocalScope& localScope() const { return *scope; }

protected:
  ScopeContext(const std::shared_ptr<const Context>& parent, std::shared_ptr<const LocalScope> scope)
    : Context(parent), scope(std::move(scope))
  {
    function_bits |= this->scope->functionBits();
    // This frame, not the parent's, is now the nearest scope on the chain.
    // See Context::scopeOwner().
    scope_owner = this;
    scope_serial = session()->nextScopeSerial();
  }

private:
  const std::shared_ptr<const LocalScope> scope;

  friend class Context;
};

class UserModuleContext : public ScopeContext
{
public:
  const Children *user_module_children() const override { return &children; }
  std::vector<const std::shared_ptr<const Context> *> list_referenced_contexts() const override;

  /*
   * The incremental-evaluation key of this instantiation's children block, set
   * when the instantiation was a memo boundary. A boundary nested inside whose
   * own children call children() folds it into its key. See core/EvalMemo.h.
   */
  void setChildrenKey(const uint64_t key[3])
  {
    children_key[0] = key[0];
    children_key[1] = key[1];
    children_key[2] = key[2];
    has_children_key = true;
  }
  // The key as two hash words and a word of flags; false if there is none.
  [[nodiscard]] const uint64_t *childrenKey() const { return has_children_key ? children_key : nullptr; }

protected:
  UserModuleContext(const std::shared_ptr<const Context>& parent, const UserModule *module,
                    const Location& loc, Arguments arguments, Children children);

private:
  Children children;
  uint64_t children_key[3] = {0, 0, 0};
  bool has_children_key = false;

  friend class Context;
};

class FileContext : public ScopeContext
{
public:
  boost::optional<CallableFunction> lookup_local_function(const Identifier& name,
                                                          const Location& loc) const override;
  boost::optional<InstantiableModule> lookup_local_module(const Identifier& name,
                                                          const Location& loc) const override;
  [[nodiscard]] const SourceFile *sourceFile() const { return source_file; }

protected:
  FileContext(const std::shared_ptr<const Context>& parent, const SourceFile *source_file)
    : ScopeContext(parent, source_file->scope), source_file(source_file)
  {
    if (!source_file->usedlibs.empty()) {
      // The used files are resolved at lookup time and may not be compiled
      // yet, so their names cannot be folded into the filter. Accept every
      // name instead of risking a false negative.
      function_bits = ~uint64_t(0);
    }
  }

private:
  const SourceFile *source_file;

  friend class Context;
};
