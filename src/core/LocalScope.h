#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "core/Assignment.h"
#include "core/Identifier.h"
#include "core/IdentifierMap.h"

class AbstractNode;
class Context;
class ModuleInstantiation;
class SourceFile;
class UserModule;

class LocalScope
{
public:
  size_t numElements() const { return assignments.size() + moduleInstantiations.size(); }
  void print(std::ostream& stream, const std::string& indent, const bool inlined = false) const;
  std::shared_ptr<AbstractNode> instantiateModules(const std::shared_ptr<const Context>& context,
                                                   const std::shared_ptr<AbstractNode>& target) const;
  std::shared_ptr<AbstractNode> instantiateModules(const std::shared_ptr<const Context>& context,
                                                   const std::shared_ptr<AbstractNode>& target,
                                                   const std::vector<size_t>& indices) const;
  void addModuleInst(const std::shared_ptr<class ModuleInstantiation>& modinst);
  void addModule(const std::shared_ptr<class UserModule>& module);
  void addFunction(const std::shared_ptr<class UserFunction>& function);
  void addAssignment(const std::shared_ptr<class Assignment>& assignment);
  bool hasChildren() const { return !(moduleInstantiations.empty()); }

  /**
   * @brief Search corresponding environment of name for type
   *
   * FYI can only find `function x()` not `x = function ()`
   */
  template <typename T>
  std::optional<T> lookup(const Identifier& name) const;

  /**
   * @brief Union of Identifier::bit() over the functions defined here
   *
   * Lets a context frame reject a function lookup without probing the table.
   * See ContextFrame::may_hold_function().
   */
  uint64_t functionBits() const { return function_bits; }

  // Definitions in source order, as written. For core/EvalMemo, which hashes them.
  const std::vector<std::pair<std::string, std::shared_ptr<UserModule>>>& moduleDefinitions() const
  {
    return astModules;
  }
  const std::vector<std::pair<std::string, std::shared_ptr<UserFunction>>>& functionDefinitions() const
  {
    return astFunctions;
  }

  AssignmentList assignments;
  std::vector<std::shared_ptr<ModuleInstantiation>> moduleInstantiations;

  /*
   * What this scope belongs to, set once after parsing (memo::annotate()), as
   * are the matching back-links of the statements in it and the modules it
   * defines. Incremental evaluation follows them to say where a statement
   * is in a way that a later parse of the same text can answer.
   */
  struct Origin {
    enum class Kind : uint8_t { Unknown, File, ModuleBody, Children, Else };
    Kind kind = Kind::Unknown;
    const SourceFile *file = nullptr;                // File
    const UserModule *module = nullptr;              // ModuleBody
    const ModuleInstantiation *statement = nullptr;  // Children, Else
  };
  Origin origin;

private:
  // Modules and functions are stored twice; once for lookup and once for AST serialization
  // FIXME: Should we split this class into an ASTNode and a run-time support class?
  IdentifierMap<std::shared_ptr<UserFunction>> functions;
  IdentifierMap<std::shared_ptr<UserModule>> modules;
  uint64_t function_bits{0};

  // All below only used for printing:
  std::vector<std::pair<std::string, std::shared_ptr<UserModule>>> astModules;
  std::vector<std::pair<std::string, std::shared_ptr<UserFunction>>> astFunctions;
};

template <>
std::optional<UserFunction *> LocalScope::lookup(const Identifier& name) const;

template <>
std::optional<UserModule *> LocalScope::lookup(const Identifier& name) const;
