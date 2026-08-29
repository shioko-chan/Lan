#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lan {

// Table is a compile-time refinement. Its LLVM/runtime representation is the
// same Record pointer returned by table(record).
enum class Type { Unknown, Number, Boolean, Text, List, Record, Table, Dynamic };

struct Expr {
  virtual ~Expr() = default;
};

struct NumberExpr final : Expr {
  explicit NumberExpr(double value) : value(value) {}
  double value;
};

struct BooleanExpr final : Expr {
  explicit BooleanExpr(bool value) : value(value) {}
  bool value;
};

struct TextExpr final : Expr {
  explicit TextExpr(std::string value) : value(std::move(value)) {}
  std::string value;
};

struct MissingExpr final : Expr {};

struct ListExpr final : Expr {
  explicit ListExpr(std::vector<Expr*>* elements) {
    for (auto* element : *elements) this->elements.emplace_back(element);
    delete elements;
  }
  std::vector<std::unique_ptr<Expr>> elements;
};

struct RecordField {
  RecordField(std::string name, Expr* value)
      : name(std::move(name)), value(value) {}
  RecordField(Expr* key, Expr* value) : key(key), value(value) {}
  RecordField(RecordField&&) = default;
  RecordField& operator=(RecordField&&) = default;
  RecordField(const RecordField&) = delete;
  RecordField& operator=(const RecordField&) = delete;
  std::string name;
  std::unique_ptr<Expr> key;
  std::unique_ptr<Expr> value;
};

struct RecordExpr final : Expr {
  explicit RecordExpr(std::vector<RecordField>* fields)
      : fields(std::move(*fields)) { delete fields; }
  std::vector<RecordField> fields;
};

struct VariableExpr final : Expr {
  explicit VariableExpr(std::string name) : name(std::move(name)) {}
  std::string name;
};

struct UnaryExpr final : Expr {
  UnaryExpr(char op, Expr* operand) : op(op), operand(operand) {}
  char op;
  std::unique_ptr<Expr> operand;
};

struct PropagateExpr final : Expr {
  explicit PropagateExpr(Expr* operand) : operand(operand) {}
  std::unique_ptr<Expr> operand;
};

struct FieldExpr final : Expr {
  FieldExpr(Expr* record, std::string name)
      : record(record), name(std::move(name)) {}
  FieldExpr(Expr* record, Expr* key) : record(record), key(key) {}
  std::unique_ptr<Expr> record;
  std::string name;
  std::unique_ptr<Expr> key;
};

struct RecordUpdateExpr final : Expr {
  RecordUpdateExpr(Expr* record, std::string name, Expr* value)
      : record(record), name(std::move(name)), value(value) {}
  RecordUpdateExpr(Expr* record, Expr* key, Expr* value)
      : record(record), key(key), value(value) {}
  std::unique_ptr<Expr> record;
  std::string name;
  std::unique_ptr<Expr> key;
  std::unique_ptr<Expr> value;
};

struct RecordPathPart {
  explicit RecordPathPart(std::string name) : name(std::move(name)) {}
  explicit RecordPathPart(Expr* key) : key(key) {}
  RecordPathPart(RecordPathPart&&) = default;
  RecordPathPart& operator=(RecordPathPart&&) = default;
  RecordPathPart(const RecordPathPart&) = delete;
  RecordPathPart& operator=(const RecordPathPart&) = delete;
  std::string name;
  std::unique_ptr<Expr> key;
};

struct NestedRecordUpdateExpr final : Expr {
  NestedRecordUpdateExpr(Expr* record, std::vector<RecordPathPart>&& path,
                         Expr* value)
      : record(record), path(std::move(path)), value(value) {}
  std::unique_ptr<Expr> record;
  std::vector<RecordPathPart> path;
  std::unique_ptr<Expr> value;
};

struct BinaryExpr final : Expr {
  BinaryExpr(std::string op, Expr* lhs, Expr* rhs)
      : op(std::move(op)), lhs(lhs), rhs(rhs) {}
  std::string op;
  std::unique_ptr<Expr> lhs;
  std::unique_ptr<Expr> rhs;
};

struct CallArgument {
  CallArgument(Expr* value, std::string name = {})
      : name(std::move(name)), value(value) {}
  CallArgument(CallArgument&&) = default;
  CallArgument& operator=(CallArgument&&) = default;
  CallArgument(const CallArgument&) = delete;
  CallArgument& operator=(const CallArgument&) = delete;
  std::string name;
  std::unique_ptr<Expr> value;
};

struct CallExpr final : Expr {
  CallExpr(std::string callee, std::vector<Expr*>* args,
           std::vector<std::string>* argNames = nullptr)
      : callee(std::move(callee)) {
    for (auto* arg : *args) this->args.emplace_back(arg);
    if (argNames) this->argNames = std::move(*argNames);
    else this->argNames.resize(args->size());
    delete args;
    delete argNames;
  }
  CallExpr(std::string callee, std::vector<CallArgument>* args)
      : callee(std::move(callee)) {
    for (auto& arg : *args) {
      argNames.push_back(std::move(arg.name));
      this->args.emplace_back(arg.value.release());
    }
    delete args;
  }
  std::string callee;
  std::vector<std::unique_ptr<Expr>> args;
  // Empty means positional; otherwise it is the parameter name supplied by
  // the caller. This parallel array always has args.size() entries.
  std::vector<std::string> argNames;
};

struct LetExpr final : Expr {
  LetExpr(std::string name, Expr* value, Expr* body)
      : name(std::move(name)), value(value), body(body) {}
  std::string name;
  std::unique_ptr<Expr> value;
  std::unique_ptr<Expr> body;
};

struct IfExpr final : Expr {
  IfExpr(Expr* condition, Expr* thenValue, Expr* elseValue)
      : condition(condition), thenValue(thenValue), elseValue(elseValue) {}
  std::unique_ptr<Expr> condition;
  std::unique_ptr<Expr> thenValue;
  std::unique_ptr<Expr> elseValue;
};

struct Parameter {
  Parameter(std::string name, Expr* defaultValue = nullptr)
      : name(std::move(name)), defaultValue(defaultValue) {}
  Parameter(Parameter&&) = default;
  Parameter& operator=(Parameter&&) = default;
  Parameter(const Parameter&) = delete;
  Parameter& operator=(const Parameter&) = delete;
  std::string name;
  std::unique_ptr<Expr> defaultValue;
};

struct Function {
  Function(std::string name, std::vector<Parameter>* params, Expr* body)
      : name(std::move(name)), params(std::move(*params)), body(body) {
    delete params;
  }
  std::string name;
  std::vector<Parameter> params;
  std::unique_ptr<Expr> body;
};

struct Program {
  std::vector<std::unique_ptr<Function>> functions;
  std::unique_ptr<Expr> result;
};

}  // namespace lan
