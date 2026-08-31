#pragma once

#include "lan/AST.h"
#include <functional>
#include <set>

namespace lan {

// Lexically free names only; a shadowing binding reads its old value first.
inline std::set<std::string> freeVariables(const Expr& expression) {
  std::set<std::string> free;
  using Names = std::set<std::string>;
  std::function<void(const Expr&, Names)> visit = [&](const Expr& expr, Names bound) {
    auto child = [&](const Expr& value) { visit(value, bound); };
    if (auto* value = dynamic_cast<const VariableExpr*>(&expr)) {
      if (!bound.count(value->name)) free.insert(value->name);
    } else if (auto* value = dynamic_cast<const LetExpr*>(&expr)) {
      child(*value->value);
      bound.insert(value->name);
      visit(*value->body, bound);
    } else if (auto* value = dynamic_cast<const IOBindExpr*>(&expr)) {
      child(*value->action);
      if (!value->name.empty()) bound.insert(value->name);
      visit(*value->body, bound);
    } else if (auto* value = dynamic_cast<const IOExpr*>(&expr)) {
      child(*value->body);
    } else if (auto* value = dynamic_cast<const IOResultExpr*>(&expr)) {
      child(*value->value);
    } else if (auto* value = dynamic_cast<const CollectionExpr*>(&expr)) {
      child(*value->collection);
      bound.insert("$lan_current_element");
      if (!value->binding.empty()) bound.insert(value->binding);
      visit(*value->body, bound);
    } else if (auto* value = dynamic_cast<const ElementBindingExpr*>(&expr)) {
      bound.insert(value->name);
      visit(*value->body, bound);
    } else if (auto* value = dynamic_cast<const ListExpr*>(&expr)) {
      for (const auto& element : value->elements) child(*element);
    } else if (auto* value = dynamic_cast<const RecordExpr*>(&expr)) {
      for (const auto& field : value->fields) {
        if (field.key) child(*field.key);
        child(*field.value);
      }
    } else if (auto* value = dynamic_cast<const UnaryExpr*>(&expr)) {
      child(*value->operand);
    } else if (auto* value = dynamic_cast<const PropagateExpr*>(&expr)) {
      child(*value->operand);
    } else if (auto* value = dynamic_cast<const FieldExpr*>(&expr)) {
      child(*value->record);
      if (value->key) child(*value->key);
    } else if (auto* value = dynamic_cast<const RecordUpdateExpr*>(&expr)) {
      child(*value->record);
      if (value->key) child(*value->key);
      child(*value->value);
    } else if (auto* value = dynamic_cast<const NestedRecordUpdateExpr*>(&expr)) {
      child(*value->record);
      for (const auto& part : value->path) if (part.key) child(*part.key);
      child(*value->value);
    } else if (auto* value = dynamic_cast<const BinaryExpr*>(&expr)) {
      child(*value->lhs);
      child(*value->rhs);
    } else if (auto* value = dynamic_cast<const IfExpr*>(&expr)) {
      child(*value->condition);
      child(*value->thenValue);
      child(*value->elseValue);
    } else if (auto* value = dynamic_cast<const CallExpr*>(&expr)) {
      for (const auto& argument : value->args) child(*argument);
    }
  };
  visit(expression, {});
  return free;
}

}  // namespace lan
