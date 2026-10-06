/*
 * Exact structural hashing of syntax, for incremental evaluation. See
 * core/EvalMemo.h.
 *
 * Every Expression subclass hashes a distinct tag, its own fields, and its
 * subexpressions, so two trees hash alike only if they are the same tree.
 * Locations are left out: moving code must not change a key.
 */

#include <cstdint>
#include <memory>

#include "core/Assignment.h"
#include "core/EvalMemo.h"
#include "core/Expression.h"
#include "core/LocalScope.h"
#include "core/ModuleInstantiation.h"
#include "core/SourceFile.h"
#include "core/UserModule.h"
#include "core/Value.h"
#include "core/function.h"

namespace {

// One per syntax kind, so that different kinds with the same fields differ.
enum Tag : uint64_t {
  kNull = 0x6E756C6C00000000ULL,
  kUnary = 0x4D10000000000001ULL,
  kBinary,
  kTernary,
  kArrayLookup,
  kLiteral,
  kRange,
  kVector,
  kLookup,
  kMemberLookup,
  kFunctionCall,
  kFunctionDefinition,
  kAssert,
  kEcho,
  kLet,
  kLcIf,
  kLcFor,
  kLcForC,
  kLcEach,
  kLcLet,
  kAssignments,
  kScope,
  kInst,
  kElse,
  kModule,
  kFunction,
  kUnhashableLiteral,
};

}  // namespace

namespace memo {

void ASTHasher::expr(const Expression *e)
{
  if (!e) {
    u64(kNull);
    return;
  }
  e->hashInto(*this);
}

void ASTHasher::assignments(const AssignmentList& list)
{
  u64(kAssignments);
  u64(list.size());
  for (const auto& assignment : list) {
    if (!assignment) {
      u64(kNull);
      continue;
    }
    id(assignment->getName());
    expr(assignment->getExpr().get());
  }
}

void ASTHasher::scope(const LocalScope& scope)
{
  u64(kScope);
  u64(scope.functionDefinitions().size());
  for (const auto& [name, function] : scope.functionDefinitions()) userFunction(*function);
  u64(scope.moduleDefinitions().size());
  for (const auto& [name, module] : scope.moduleDefinitions()) userModule(*module);
  assignments(scope.assignments);
  u64(scope.moduleInstantiations.size());
  for (const auto& inst : scope.moduleInstantiations) this->inst(*inst);
}

void ASTHasher::inst(const ModuleInstantiation& inst)
{
  u64(kInst);
  u64((inst.tag_root ? 1 : 0) | (inst.tag_highlight ? 2 : 0) | (inst.tag_background ? 4 : 0));
  id(inst.name());
  refModule(inst.name());
  assignments(inst.arguments);
  scope(*inst.scope);
  if (const auto *ifelse = dynamic_cast<const IfElseModuleInstantiation *>(&inst)) {
    u64(kElse);
    if (const auto& else_scope = ifelse->getElseScope()) scope(*else_scope);
    else u64(kNull);
  }
}

void ASTHasher::userModule(const UserModule& module)
{
  u64(kModule);
  str(module.name);
  assignments(module.parameters);
  scope(*module.body);
}

void ASTHasher::userFunction(const UserFunction& function)
{
  u64(kFunction);
  str(function.name);
  assignments(function.parameters);
  expr(function.expr.get());
}

void ASTHasher::literal(const Value& value)
{
  ValueHashCache none;
  if (!none.hash(*this, value)) {
    // Literals are numbers, strings, booleans and undef; nothing else parses
    // to one. Fall back on the printed form rather than give up.
    u64(kUnhashableLiteral);
    str(value.toString());
  }
}

namespace {

void annotateScope(LocalScope& scope, const LocalScope::Origin& origin)
{
  using Kind = LocalScope::Origin::Kind;
  scope.origin = origin;
  for (const auto& assignment : scope.assignments) {
    if (!assignment || !assignment->getExpr() || !assignment->getName().isConfigVariable()) continue;
    // Hashing walks the expression; the hash itself is not needed.
    ASTHasher h;
    h.accumulatorOf = &assignment->getName();
    h.expr(assignment->getExpr().get());
  }
  for (const auto& [name, module] : scope.moduleDefinitions()) {
    module->parent_scope = &scope;
    annotateScope(*module->body, {Kind::ModuleBody, nullptr, module.get(), nullptr});
  }
  for (size_t i = 0; i < scope.moduleInstantiations.size(); ++i) {
    ModuleInstantiation& inst = *scope.moduleInstantiations[i];
    inst.parent_scope = &scope;
    inst.parent_index = static_cast<uint32_t>(i);
    annotateScope(*inst.scope, {Kind::Children, nullptr, nullptr, &inst});
    if (const auto *ifelse = dynamic_cast<const IfElseModuleInstantiation *>(&inst)) {
      if (const auto& else_scope = ifelse->getElseScope()) {
        annotateScope(*else_scope, {Kind::Else, nullptr, nullptr, &inst});
      }
    }
  }
}

}  // namespace

void annotate(SourceFile& file)
{
  annotateScope(*file.scope, {LocalScope::Origin::Kind::File, &file, nullptr, nullptr});
}

}  // namespace memo

void UnaryOp::hashInto(memo::ASTHasher& h) const
{
  h.u64(kUnary);
  h.u64(static_cast<uint64_t>(op));
  h.expr(expr.get());
}

void BinaryOp::hashInto(memo::ASTHasher& h) const
{
  h.u64(kBinary);
  h.u64(static_cast<uint64_t>(op));
  h.expr(left.get());
  h.expr(right.get());
}

void TernaryOp::hashInto(memo::ASTHasher& h) const
{
  h.u64(kTernary);
  h.expr(cond.get());
  h.expr(ifexpr.get());
  h.expr(elseexpr.get());
}

void ArrayLookup::hashInto(memo::ASTHasher& h) const
{
  h.u64(kArrayLookup);
  h.expr(array.get());
  h.expr(index.get());
}

void Literal::hashInto(memo::ASTHasher& h) const
{
  h.u64(kLiteral);
  h.literal(value);
}

void Range::hashInto(memo::ASTHasher& h) const
{
  h.u64(kRange);
  h.expr(begin.get());
  h.expr(step.get());
  h.expr(end.get());
}

void Vector::hashInto(memo::ASTHasher& h) const
{
  h.u64(kVector);
  h.u64(children.size());
  for (const auto& child : children) h.expr(child.get());
}

void Lookup::hashInto(memo::ASTHasher& h) const
{
  h.u64(kLookup);
  h.id(name);
  h.refVar(name);
  if (h.accumulatorOf && name == *h.accumulatorOf) accumulator = true;
}

void MemberLookup::hashInto(memo::ASTHasher& h) const
{
  h.u64(kMemberLookup);
  h.expr(expr.get());
  h.str(member);
}

void FunctionCall::hashInto(memo::ASTHasher& h) const
{
  h.u64(kFunctionCall);
  h.u64(isLookup ? 1 : 0);
  if (isLookup) {
    h.id(name);
    // A name in call position can also resolve to a variable holding a function.
    h.refFunction(name);
    h.refVar(name);
  } else {
    h.expr(expr.get());
  }
  h.assignments(arguments);
}

void FunctionDefinition::hashInto(memo::ASTHasher& h) const
{
  h.u64(kFunctionDefinition);
  const Identifier *accumulatorOf = h.accumulatorOf;
  h.accumulatorOf = nullptr;
  h.assignments(parameters);
  h.expr(expr.get());
  h.accumulatorOf = accumulatorOf;
}

void Assert::hashInto(memo::ASTHasher& h) const
{
  h.u64(kAssert);
  h.assignments(arguments);
  h.expr(expr.get());
}

void Echo::hashInto(memo::ASTHasher& h) const
{
  h.u64(kEcho);
  h.assignments(arguments);
  h.expr(expr.get());
}

void Let::hashInto(memo::ASTHasher& h) const
{
  h.u64(kLet);
  h.assignments(arguments);
  h.expr(expr.get());
}

void LcIf::hashInto(memo::ASTHasher& h) const
{
  h.u64(kLcIf);
  h.expr(cond.get());
  h.expr(ifexpr.get());
  h.expr(elseexpr.get());
}

void LcFor::hashInto(memo::ASTHasher& h) const
{
  h.u64(kLcFor);
  h.assignments(arguments);
  h.expr(expr.get());
}

void LcForC::hashInto(memo::ASTHasher& h) const
{
  h.u64(kLcForC);
  h.assignments(arguments);
  h.assignments(incr_arguments);
  h.expr(cond.get());
  h.expr(expr.get());
}

void LcEach::hashInto(memo::ASTHasher& h) const
{
  h.u64(kLcEach);
  h.expr(expr.get());
}

void LcLet::hashInto(memo::ASTHasher& h) const
{
  h.u64(kLcLet);
  h.assignments(arguments);
  h.expr(expr.get());
}
