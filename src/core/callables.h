#pragma once

#include <cstdint>
#include <memory>
#include <variant>

#include "core/Value.h"

class Context;
class UserFunction;
class BuiltinFunction;
class AbstractModule;

struct CallableUserFunction {
  std::shared_ptr<const Context> defining_context;
  const UserFunction *function;
};
using CallableFunction =
  std::variant<const BuiltinFunction *, CallableUserFunction, Value, const Value *>;

/*
 * One call site's remembered function resolution. See
 * FunctionCall::evaluate_function_expression for what makes it valid, and
 * EvaluationSession::functionLookupCache for where it lives.
 *
 * `site` is the call site the entry was filled for. Site numbers are recycled
 * when an AST is discarded, so an entry can outlive the site it belonged to
 * within one session; checking it costs a compare against a word already in
 * the same cache line.
 *
 * Exactly one of the two targets is set. A zero serial is an empty entry:
 * scope serials start at one.
 */
struct FunctionLookupCache {
  const void *site = nullptr;
  uint64_t scope_serial = 0;
  const BuiltinFunction *builtin = nullptr;
  const UserFunction *function = nullptr;
};

struct InstantiableModule {
  std::shared_ptr<const Context> defining_context;
  const AbstractModule *module;
};
