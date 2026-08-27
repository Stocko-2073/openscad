#pragma once

#include <boost/container/small_vector.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "core/Assignment.h"
#include "core/Context.h"
#include "core/Identifier.h"
#include "core/Value.h"

class EvaluationSession;

struct Argument {
  boost::optional<Identifier> name;
  Value value;

  Argument(boost::optional<Identifier> name, Value value)
    : name(std::move(name)), value(std::move(value))
  {
  }
  Argument(Argument&& other) = default;
  Argument& operator=(Argument&& other) = default;
  Argument(const Argument& other) = delete;
  Argument& operator=(const Argument& other) = delete;
  ~Argument() = default;

  const Value *operator->() const { return &value; }
  Value *operator->() { return &value; }
};

/*
 * The evaluated arguments of one call.
 *
 * Calls are overwhelmingly short. Instantiating a BOSL2-heavy model builds
 * ~20.9M of these, of which 79% carry a single argument and 98% carry two or
 * fewer, so the inline capacity keeps all but 2% of them off the heap
 * entirely -- the same trade ValueMap makes for context frames.
 */
class Arguments : public boost::container::small_vector<Argument, 2>
{
public:
  Arguments(const AssignmentList& argument_expressions, const std::shared_ptr<const Context>& context);
  Arguments(Arguments&& other) = default;
  Arguments& operator=(Arguments&& other) = default;
  Arguments(const Arguments& other) = delete;
  Arguments& operator=(const Arguments& other) = delete;
  ~Arguments() = default;

private:
  Arguments(EvaluationSession *session) : evaluation_session(session) {}

public:
  [[nodiscard]] Arguments clone() const;

  [[nodiscard]] EvaluationSession *session() const { return evaluation_session; }
  const std::string& documentRoot() const;

private:
  EvaluationSession *evaluation_session;
};

std::ostream& operator<<(std::ostream& stream, const Argument& argument);
std::ostream& operator<<(std::ostream& stream, const Arguments& arguments);
