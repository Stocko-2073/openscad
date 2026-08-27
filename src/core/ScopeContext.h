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

protected:
  ScopeContext(const std::shared_ptr<const Context>& parent, std::shared_ptr<const LocalScope> scope)
    : Context(parent), scope(std::move(scope))
  {
    function_bits |= this->scope->functionBits();
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

protected:
  UserModuleContext(const std::shared_ptr<const Context>& parent, const UserModule *module,
                    const Location& loc, Arguments arguments, Children children);

private:
  Children children;

  friend class Context;
};

class FileContext : public ScopeContext
{
public:
  boost::optional<CallableFunction> lookup_local_function(const Identifier& name,
                                                          const Location& loc) const override;
  boost::optional<InstantiableModule> lookup_local_module(const Identifier& name,
                                                          const Location& loc) const override;

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
