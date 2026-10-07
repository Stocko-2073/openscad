#pragma once

#include <algorithm>
#include <boost/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/AST.h"
#include "core/Identifier.h"
#include "core/ContextMemoryManager.h"  // FIXME: don't use as value type so we don't need to include header
#include "core/callables.h"

class Value;
class ContextFrame;
namespace memo {
class EvalMemoSession;
}

class EvaluationSession
{
public:
  EvaluationSession(std::string documentRoot) : document_root(std::move(documentRoot)) {}

  size_t push_frame(ContextFrame *frame);
  void replace_frame(size_t index, ContextFrame *frame);
  void pop_frame(size_t index);

  [[nodiscard]] boost::optional<const Value&> try_lookup_special_variable(const Identifier& name) const;
  [[nodiscard]] const Value& lookup_special_variable(const Identifier& name, const Location& loc) const;
  [[nodiscard]] boost::optional<CallableFunction> lookup_special_function(const Identifier& name,
                                                                          const Location& loc) const;
  [[nodiscard]] boost::optional<InstantiableModule> lookup_special_module(const Identifier& name,
                                                                          const Location& loc) const;

  // Starts at one: zero means "no scope on this chain" and never matches.
  [[nodiscard]] uint64_t nextScopeSerial() { return ++scope_serial_counter; }

  // Per session, not on the AST: animation prefetch evaluates a script on several threads.
  [[nodiscard]] FunctionLookupCache& functionLookupCache(size_t site)
  {
    if (site >= function_lookup_cache.size()) {
      function_lookup_cache.resize(std::max(site + 1, function_lookup_cache.size() * 2));
    }
    return function_lookup_cache[site];
  }

  [[nodiscard]] const std::string& documentRoot() const { return document_root; }

  // Null unless incremental evaluation is on for this session.
  [[nodiscard]] memo::EvalMemoSession *memo() const { return memo_session; }
  void setMemo(memo::EvalMemoSession *memo) { memo_session = memo; }
  [[nodiscard]] const std::vector<ContextFrame *>& frames() const { return stack; }
  ContextMemoryManager& contextMemoryManager() { return context_memory_manager; }
  HeapSizeAccounting& accounting() { return context_memory_manager.accounting(); }

private:
  std::string document_root;
  std::vector<ContextFrame *> stack;
  ContextMemoryManager context_memory_manager;
  uint64_t scope_serial_counter = 0;
  std::vector<FunctionLookupCache> function_lookup_cache;
  memo::EvalMemoSession *memo_session = nullptr;
};
