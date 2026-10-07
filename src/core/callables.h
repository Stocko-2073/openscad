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
 * A call site's cached function resolution; see FunctionCall::evaluate_function_expression.
 * `site` guards against recycled site numbers. A zero scope_serial marks an empty entry;
 * otherwise exactly one of builtin and function is set.
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
