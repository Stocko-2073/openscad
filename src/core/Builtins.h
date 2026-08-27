#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/Assignment.h"
#include "core/Identifier.h"
#include "core/IdentifierMap.h"

class AbstractModule;
class BuiltinFunction;

void initialize_rng();

class Builtins
{
public:
  static Builtins *instance(bool erase = false);
  static void init(const std::string& name, AbstractModule *module);
  static void init(const std::string& name, AbstractModule *module,
                   const std::vector<std::string>& calltipList);
  static void init(const std::string& name, BuiltinFunction *function,
                   const std::vector<std::string>& calltipList);
  void initialize();
  const std::string& isDeprecated(const Identifier& name) const;

  const auto& getAssignments() const { return this->assignments; }
  const auto& getFunctions() const { return this->functions; }
  const auto& getModules() const { return this->modules; }

  static std::unordered_map<std::string, const std::vector<std::string>> keywordList;

private:
  Builtins();
  virtual ~Builtins() = default;

  static void initKeywordList();

  AssignmentList assignments;
  // Probed for every builtin call in a script, so these are open-addressed on
  // the interned name's index rather than hashed.
  IdentifierMap<BuiltinFunction *> functions;
  IdentifierMap<AbstractModule *> modules;

  std::unordered_map<Identifier, std::string> deprecations;
};
