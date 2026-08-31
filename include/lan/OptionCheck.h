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
  // Proven fields of this immutable value, not a structural Record type.
  std::map<std::string, OptionType> fields;
  bool completeFields = false;
};

struct OptionTypeQueries {
  std::function<OptionType(const Expr&)> expression;
  std::function<OptionType(const Function&, std::size_t)> parameter;
  std::function<void(const BinaryExpr&, bool)> coalesceKind;
};

// Run after base-type unification and before lowering. No runtime operations
// are performed, including for unused IO actions and unreachable branches.
void checkOptions(const Program& program, const OptionTypeQueries& queries);

}  // namespace lan
