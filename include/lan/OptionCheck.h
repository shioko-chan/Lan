#pragma once

#include "lan/AST.h"
#include <functional>
#include <map>
#include <memory>

namespace lan {

// Source-level type qualification, independent of the presence facts used to
// choose an LLVM ABI. A container's optionality is not its element's optionality.
struct OptionType {
  Type kind = Type::Unknown;
  bool optional = false;
  bool missingOnly = false;
  std::shared_ptr<const OptionType> element;
  std::shared_ptr<const OptionType> error;
  unsigned resultStates = 3;  // 1 = ok, 2 = err; payload facts only apply to that arm.
  // Proven fields of this immutable value, not a structural Record type.
  std::map<std::string, OptionType> fields;
  bool completeFields = false;
};

struct OptionTypeQueries {
  std::function<OptionType(const Expr&)> expression;
  std::function<OptionType(const Function&, std::size_t)> parameter;
  std::function<void(const BinaryExpr&, bool)> coalesceKind;
  std::function<bool(const DoExpr&)> resultDo;
};

// Run after base-type unification and before lowering. No runtime operations
// are performed, including for unused IO actions and unreachable branches.
void checkOptions(const Program& program, const OptionTypeQueries& queries);

}  // namespace lan
