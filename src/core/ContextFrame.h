#pragma once

#include <boost/optional.hpp>
#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

#include "core/AST.h"
#include "core/Identifier.h"
#include "core/Value.h"
#include "core/ValueMap.h"
#include "core/callables.h"

class EvaluationSession;

class ContextFrame
{
public:
  ContextFrame(EvaluationSession *session);
  virtual ~ContextFrame() = default;

  ContextFrame(ContextFrame&& other) = default;

  // Not virtual: this runs ~150M times instantiating a large model, so it is
  // defined here to be inlined into the context-chain walk.
  boost::optional<const Value&> lookup_local_variable(const Identifier& name) const
  {
    const ValueMap& variables = name.isConfigVariable() ? config_variables : lexical_variables;
    auto result = variables.find(name);
    if (result != variables.end()) {
      return result->second;
    }
    return boost::none;
  }

  virtual boost::optional<CallableFunction> lookup_local_function(const Identifier& name,
                                                                  const Location& loc) const;
  virtual boost::optional<InstantiableModule> lookup_local_module(const Identifier& name,
                                                                  const Location& loc) const;

  virtual std::vector<const Value *> list_embedded_values() const;
  virtual size_t clear();

  virtual bool set_variable(const Identifier& name, Value&& value);

  void apply_variables(const ValueMap& variables);
  void apply_lexical_variables(const ContextFrame& other);
  void apply_config_variables(const ContextFrame& other);
  void apply_variables(const ContextFrame& other)
  {
    apply_lexical_variables(other);
    apply_config_variables(other);
  }

  void apply_variables(ValueMap&& variables);
  void apply_lexical_variables(ContextFrame&& other);
  void apply_config_variables(ContextFrame&& other);
  void apply_variables(ContextFrame&& other);

  EvaluationSession *session() const { return evaluation_session; }
  const std::string& documentRoot() const;

protected:
  ValueMap lexical_variables;
  ValueMap config_variables;
  EvaluationSession *evaluation_session;

public:
#ifdef DEBUG
  virtual std::string dumpFrame() const;
#endif
};

/*
 * A ContextFrameHandle stores a reference to a ContextFrame, and keeps it on
 * the special variable stack for the lifetime of the handle.
 */
class ContextFrameHandle
{
public:
  ContextFrameHandle(ContextFrame *frame);
  ~ContextFrameHandle() { release(); }

  ContextFrameHandle(const ContextFrameHandle&) = delete;
  ContextFrameHandle& operator=(const ContextFrameHandle&) = delete;
  ContextFrameHandle& operator=(ContextFrameHandle&&) = delete;

  ContextFrameHandle(ContextFrameHandle&& other) noexcept
    : session(other.session), frame_index(other.frame_index)
  {
    other.session = nullptr;
  }

  ContextFrameHandle& operator=(ContextFrame *frame);

  // Valid only if handle is on the top of the stack.
  void release();

protected:
  EvaluationSession *session;
  size_t frame_index;
};
