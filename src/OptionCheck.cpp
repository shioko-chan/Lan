#include "lan/OptionCheck.h"

#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lan {
namespace {

using Value = OptionType;
using Scope = std::unordered_map<std::string, Value>;

Value child(const Value& value) {
  if (value.element) return *value.element;
  Value unknown;
  unknown.kind = Type::Dynamic;
  unknown.optional = true;
  return unknown;
}

Value withChild(Value value, Value element) {
  value.element = std::make_shared<const Value>(std::move(element));
  return value;
}

Value present(Value value) {
  value.optional = false;
  value.missingOnly = false;
  return value;
}

Value plainParameter(Value value) {
  value = present(std::move(value));
  if (value.element) value = withChild(value, plainParameter(child(value)));
  return value;
}

Value joined(const Value& left, const Value& right) {
  if (left.missingOnly && !right.missingOnly) {
    auto value = right;
    value.optional = true;
    return value;
  }
  if (right.missingOnly && !left.missingOnly) return joined(right, left);
  Value result = left;
  result.optional = left.optional || right.optional;
  result.missingOnly = left.missingOnly && right.missingOnly;
  result.completeFields = left.completeFields && right.completeFields &&
      left.fields.size() == right.fields.size();
  if (left.element && right.element)
    result = withChild(result, joined(child(left), child(right)));
  for (auto field = result.fields.begin(); field != result.fields.end();) {
    auto found = right.fields.find(field->first);
    if (found == right.fields.end()) {
      result.completeFields = false;
      field = result.fields.erase(field);
    } else {
      field->second = joined(field->second, found->second);
      ++field;
    }
  }
  return result;
}

void encode(const Value& value, std::string& key) {
  key += std::to_string(static_cast<int>(value.kind));
  key += value.optional ? '?' : '!';
  key += value.missingOnly ? '0' : '1';
  key += value.completeFields ? 'c' : 'o';
  if (value.element) { key += '['; encode(*value.element, key); key += ']'; }
  key += '{';
  for (const auto& [name, field] : value.fields) {
    key += std::to_string(name.size()) + ':' + name;
    encode(field, key);
  }
  key += '}';
}

unsigned rank(const Value& value) {
  return value.kind == Type::List ? 1 + rank(child(value)) : 0;
}

// Runtime broadcasting aligns trailing dimensions. Optional elements are an
// explicit part of the collection operator contract, unlike scalar operators.
Value broadcast(const Value& left, const Value& right, Value result,
                bool coalesce) {
  if (result.kind == Type::List) {
    unsigned l = rank(left), r = rank(right);
    result = withChild(result, broadcast(l >= r ? child(left) : left,
                                         r >= l ? child(right) : right,
                                         child(result), coalesce));
  }
  result.optional = coalesce ? left.optional && right.optional
                             : left.optional || right.optional;
  return result;
}

class Checker {
 public:
  Checker(const Program& program, const OptionTypeQueries& queries)
      : program_(program), queries_(queries) {
    for (const auto& function : program.functions)
      functions_.emplace(function->name, function.get());
  }

  void run() {
    Scope scope;
    bool escapes = false;
    auto result = eval(*program_.result, scope, escapes);
    // The runner may display missing data, but cannot execute a missing action.
    if (result.kind == Type::IO) result.optional |= escapes;
    require(result, *program_.result, result.kind == Type::IO
        ? "程序入口执行的 IO action" : "程序最终结果（可用末尾 ? 显式输出缺失）");
    // Uncalled functions are checked too; parameters default to T, not Option<T>.
    for (const auto& function : program_.functions) {
      if (checked_.count(function->name)) continue;
      std::vector<std::optional<Value>> args(function->params.size());
      for (std::size_t i = 0; i < args.size(); ++i)
        if (!function->params[i].defaultValue)
          args[i] = plainParameter(queries_.parameter(*function, i));
      functionValue(*function, args);
    }
  }

 private:
  const Program& program_;
  const OptionTypeQueries& queries_;
  std::unordered_map<std::string, const Function*> functions_;
  std::unordered_map<std::string, std::unordered_map<std::string, Value>> cache_;
  std::unordered_set<std::string> checked_;
  std::vector<std::string> calls_;

  void require(const Value& value, const Expr& expr, const std::string& usage) {
    if (!value.optional) return;
    std::string message = "Option 类型错误：" + usage +
        "需要确定存在的 T，但得到 Option<T>（可能为 missing）；请使用 ?、??、require 或 is_missing 分支处理";
    message += "\n位置：" + program_.sourcePath + ':' +
        std::to_string(expr.line) + ':' + std::to_string(expr.column);
    if (!calls_.empty()) {
      message += "\n分析调用链：程序";
      for (const auto& call : calls_) message += " -> " + call;
    }
    throw std::runtime_error(message);
  }

  Value boundary(const Expr& expr, Scope& scope) {
    bool escapes = false;
    auto value = eval(expr, scope, escapes);
    value.optional |= escapes;
    return value;
  }

  // Only immutable local bindings are narrowed. Repeated dynamic lookups are
  // not equated, and lexical shadowing naturally discards the previous fact.
  void refine(const Expr& test, Scope& scope, bool truth) {
    if (auto* unary = dynamic_cast<const UnaryExpr*>(&test);
        unary && unary->op == '!') {
      refine(*unary->operand, scope, !truth);
    } else if (auto* call = dynamic_cast<const CallExpr*>(&test);
               call && call->callee == "is_missing") {
      auto* variable = dynamic_cast<const VariableExpr*>(call->args[0].get());
      if (!variable || !scope.count(variable->name)) return;
      auto& value = scope.at(variable->name);
      value.optional = truth;
      value.missingOnly = truth;
    }
  }

  Value functionValue(const Function& function,
                      const std::vector<std::optional<Value>>& supplied) {
    std::string key;
    for (const auto& argument : supplied) {
      if (argument) encode(*argument, key);
      else key += "default;";
    }
    auto cached = cache_[function.name].find(key);
    if (cached != cache_[function.name].end()) return cached->second;
    checked_.insert(function.name);
    calls_.push_back(function.name);
    Scope local;
    bool escapes = false;
    for (std::size_t i = 0; i < function.params.size(); ++i) {
      const auto& param = function.params[i];
      // Check defaults even when this caller supplies the parameter.
      std::optional<Value> fallback;
      if (param.defaultValue) {
        bool defaultEscape = false;
        fallback = eval(*param.defaultValue, local, defaultEscape);
        if (!supplied[i]) escapes |= defaultEscape;
      }
      local[param.name] = supplied[i] ? *supplied[i] : *fallback;
    }
    auto result = boundary(*function.body, local);
    result.optional |= escapes;
    calls_.pop_back();
    cache_[function.name].emplace(std::move(key), result);
    return result;
  }

  Value eval(const Expr& expr, Scope& scope, bool& escapes) {
    Value result = queries_.expression(expr);
    if (dynamic_cast<const MissingExpr*>(&expr)) {
      result.optional = result.missingOnly = true;
    } else if (auto* variable = dynamic_cast<const VariableExpr*>(&expr)) {
      auto found = scope.find(variable->name);
      if (found != scope.end()) result = found->second;
    } else if (auto* propagate = dynamic_cast<const PropagateExpr*>(&expr)) {
      result = eval(*propagate->operand, scope, escapes);
      escapes |= result.optional;
      result = present(result);
    } else if (auto* let = dynamic_cast<const LetExpr*>(&expr)) {
      auto value = boundary(*let->value, scope);
      auto local = scope;
      local[let->name] = value;
      result = eval(*let->body, local, escapes);
    } else if (auto* conditional = dynamic_cast<const IfExpr*>(&expr)) {
      auto condition = eval(*conditional->condition, scope, escapes);
      require(condition, *conditional->condition, "if 条件");
      auto yes = scope, no = scope;
      refine(*conditional->condition, yes, true);
      refine(*conditional->condition, no, false);
      auto lhs = eval(*conditional->thenValue, yes, escapes);
      auto rhs = eval(*conditional->elseValue, no, escapes);
      result = joined(lhs, rhs);
    } else if (auto* list = dynamic_cast<const ListExpr*>(&expr)) {
      std::optional<Value> element;
      for (const auto& item : list->elements) {
        auto value = eval(*item, scope, escapes);
        element = element ? joined(*element, value) : value;
      }
      // An empty List contains no missing elements.
      result = withChild(result, element ? *element : present(child(result)));
    } else if (auto* record = dynamic_cast<const RecordExpr*>(&expr)) {
      result.completeFields = true;
      for (const auto& field : record->fields) {
        if (field.key) {
          auto key = eval(*field.key, scope, escapes);
          require(key, *field.key, "动态字段名");
        }
        auto value = eval(*field.value, scope, escapes);
        if (!field.key) result.fields[field.name] = value;
        else if (auto* text = dynamic_cast<const TextExpr*>(field.key.get()))
          result.fields[text->value] = value;
        else { result.fields.clear(); result.completeFields = false; }
      }
    } else if (auto* field = dynamic_cast<const FieldExpr*>(&expr)) {
      auto object = eval(*field->record, scope, escapes);
      require(object, *field->record, "字段/下标读取的对象");
      std::string name = field->name;
      if (field->key) {
        auto key = eval(*field->key, scope, escapes);
        require(key, *field->key, "字段名/下标");
        if (auto* text = dynamic_cast<const TextExpr*>(field->key.get()))
          name = text->value;
      }
      if (object.kind == Type::List) result = child(object);
      result.optional = true;  // Lookup can fail even for List<T>.
      if (object.kind != Type::List && !name.empty()) {
        auto found = object.fields.find(name);
        if (found != object.fields.end()) result = found->second;
      }
    } else if (auto* update = dynamic_cast<const RecordUpdateExpr*>(&expr)) {
      result = eval(*update->record, scope, escapes);
      require(result, *update->record, "Record 更新的对象");
      if (update->key) {
        auto key = eval(*update->key, scope, escapes);
        require(key, *update->key, "动态字段名");
      }
      auto value = eval(*update->value, scope, escapes);
      if (!update->key) result.fields[update->name] = value;
      else if (auto* text = dynamic_cast<const TextExpr*>(update->key.get()))
        result.fields[text->value] = value;
      else { result.fields.clear(); result.completeFields = false; }
    } else if (auto* update = dynamic_cast<const NestedRecordUpdateExpr*>(&expr)) {
      result = eval(*update->record, scope, escapes);
      require(result, *update->record, "嵌套 Record 更新的根对象");
      for (const auto& part : update->path)
        if (part.key) require(eval(*part.key, scope, escapes), *part.key, "动态字段名");
      auto value = eval(*update->value, scope, escapes);
      Value* current = &result;
      for (std::size_t i = 0; i < update->path.size(); ++i) {
        const auto& part = update->path[i];
        if (part.key) {
          current->fields.clear(); current->completeFields = false; break;
        }
        if (i + 1 == update->path.size()) current->fields[part.name] = value;
        else {
          auto& nested = current->fields[part.name];
          // This syntax explicitly asserts that each intermediate is a Record;
          // the runtime continues to validate that assertion.
          nested = present(nested);
          nested.kind = Type::Record;
          current = &nested;
        }
      }
    } else if (auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
      auto operand = eval(*unary->operand, scope, escapes);
      require(operand, *unary->operand, "一元运算");
      result = operand;
    } else if (auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
      bool vector = result.kind == Type::List;
      auto lhs = binary->op == "??" ? boundary(*binary->lhs, scope)
                                    : eval(*binary->lhs, scope, escapes);
      if (binary->op == "??" && vector && lhs.optional) {
        vector = false;
      }
      if (binary->op == "??") queries_.coalesceKind(*binary, vector);
      auto rhs = eval(*binary->rhs, scope, escapes);
      if (binary->op == "??" && !vector) {
        if (rank(lhs) != rank(rhs))
          throw std::runtime_error("Option 类型错误：?? 的整个值默认项必须与左侧 T 类型一致；List 元素默认值请在解包后使用 fill_missing");
        result = lhs.missingOnly ? rhs : !lhs.optional ? lhs : joined(present(lhs), rhs);
        result.optional = lhs.optional && rhs.optional;
      } else {
        require(lhs, *binary->lhs, "“" + binary->op + "”左侧");
        require(rhs, *binary->rhs, "“" + binary->op + "”右侧");
        if (vector) result = broadcast(lhs, rhs, result, binary->op == "??");
      }
    } else if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
      std::vector<Value> args;
      for (const auto& arg : call->args) args.push_back(eval(*arg, scope, escapes));
      auto definition = functions_.find(call->callee);
      if (definition != functions_.end()) {
        const auto& function = *definition->second;
        std::vector<std::optional<Value>> supplied(function.params.size());
        std::size_t positional = 0;
        for (std::size_t i = 0; i < args.size(); ++i) {
          std::size_t index = positional++;
          if (!call->argNames[i].empty()) {
            index = 0;
            while (function.params[index].name != call->argNames[i]) ++index;
          }
          supplied[index] = args[i];
        }
        result = functionValue(function, supplied);
      } else result = builtin(*call, args, result);
    } else if (auto* collection = dynamic_cast<const CollectionExpr*>(&expr)) {
      auto input = eval(*collection->collection, scope, escapes);
      require(input, *collection->collection, "集合操作的输入");
      Value element = child(input);
      if (input.kind == Type::Table) {
        element = {};
        element.kind = Type::Record;
        for (const auto& [name, column] : input.fields)
          element.fields[name] = child(column);
      }
      auto local = scope;
      local["$lan_current_element"] = element;
      if (!collection->binding.empty()) local[collection->binding] = element;
      auto body = boundary(*collection->body, local);
      switch (collection->operation) {
        case CollectionOperation::Map: result = withChild(result, body); break;
        case CollectionOperation::Find:
          result = element; result.optional = true; break;
        case CollectionOperation::Any:
        case CollectionOperation::All: result.optional = body.optional; break;
        case CollectionOperation::Filter: result = input; break;
        case CollectionOperation::Select:
        case CollectionOperation::Add:
        case CollectionOperation::Replace:
          require(body, *collection->body, "Table 列更新的行结果");
          result.fields = collection->operation == CollectionOperation::Select ||
              (collection->operation == CollectionOperation::Replace && !body.completeFields)
                  ? std::map<std::string, Value>{} : input.fields;
          result.completeFields = body.completeFields &&
              (collection->operation == CollectionOperation::Select || input.completeFields);
          for (auto& [name, cell] : body.fields) {
            Value column; column.kind = Type::List;
            result.fields[name] = withChild(column, cell);
          }
          break;
      }
    } else if (auto* io = dynamic_cast<const IOExpr*>(&expr)) {
      auto local = scope;
      result = withChild(result, boundary(*io->body, local));
    } else if (auto* bind = dynamic_cast<const IOBindExpr*>(&expr)) {
      auto action = boundary(*bind->action, scope);
      require(action, *bind->action, "执行的 IO action");
      auto local = scope;
      if (!bind->name.empty()) local[bind->name] = child(action);
      result = eval(*bind->body, local, escapes);
    } else if (auto* final = dynamic_cast<const IOResultExpr*>(&expr)) {
      result = boundary(*final->value, scope);
      if (result.kind == Type::IO) {
        require(result, *final->value, "执行的 IO action");
        result = child(result);
      }
    }
    return result;
  }

  Value builtin(const CallExpr& call, const std::vector<Value>& args, Value result) {
    const auto& name = call.callee;
    if (name == "is_missing" || name == "same") return result;
    if (name == "require") return present(args[0]);
    for (std::size_t i = 0; i < args.size(); ++i)
      require(args[i], *call.args[i], name + " 的参数");

    if (name == "first" || name == "last") {
      result = child(args[0]); result.optional = true;
    } else if (name == "parse_number" || name == "parse_time" ||
               name == "regex_find" || name == "average" ||
               name == "min" || name == "max") {
      result.optional = true;
    } else if (name == "split" || name == "regex_find_all" || name == "regex_split" ||
               name == "fields") {
      result = withChild(result, present(child(result)));
    } else if (name == "reverse" || name == "sort" || name == "distinct" ||
               name == "take" || name == "drop" || name == "sort_by") {
      result = args[0];
    } else if (name == "concat") {
      result = joined(args[0], args[1]);
    } else if (name == "fill_missing") {
      auto element = joined(present(child(args[0])), args[1]);
      result = withChild(result, element);
    } else if (name == "flatten") {
      auto inner = child(args[0]);
      require(inner, *call.args[0], "flatten 的子列表");
      result = withChild(result, child(inner));
    } else if (name == "join") {
      require(child(args[0]), *call.args[0], "join 的列表元素");
    } else if (name == "table") {
      if (args[0].kind == Type::List) {
        auto row = child(args[0]);
        require(row, *call.args[0], "table 的行");
        result.completeFields = row.completeFields;
        for (const auto& [field, cell] : row.fields) {
          Value column; column.kind = Type::List;
          result.fields[field] = withChild(column, cell);
        }
      } else {
        result.fields = args[0].fields;
        result.completeFields = args[0].completeFields;
        for (auto& [field, column] : result.fields) {
          require(column, *call.args[0], "table 的列“" + field + "”");
          column = present(column);
        }
      }
    } else if (name == "merge") {
      // Only fields known on the right are guaranteed not to be overwritten
      // by an unknown dynamic key from the right Record.
      if (args[1].completeFields) result.fields = args[0].fields;
      for (const auto& [field, value] : args[1].fields) result.fields[field] = value;
      result.completeFields = args[0].completeFields && args[1].completeFields;
    } else if (name == "remove_field") {
      result.fields = args[0].fields;
      result.completeFields = args[0].completeFields;
      if (auto* key = dynamic_cast<const TextExpr*>(call.args[1].get()))
        result.fields.erase(key->value);
      else { result.fields.clear(); result.completeFields = false; }
    } else if (name == "select_columns") {
      require(child(args[1]), *call.args[1], "select_columns 的列名元素");
      // Selected names may be dynamic. Only a literal selection proves fields.
      if (auto* names = dynamic_cast<const ListExpr*>(call.args[1].get())) {
        result.completeFields = true;
        for (const auto& element : names->elements) {
          auto* name = dynamic_cast<const TextExpr*>(element.get());
          if (!name) { result.completeFields = false; continue; }
          auto found = args[0].fields.find(name->value);
          if (found != args[0].fields.end()) result.fields.emplace(*found);
          else result.completeFields = false;
        }
      }
    } else if (result.kind == Type::IO) {
      auto output = present(child(result));
      if (name == "choose") {
        output = child(args[0]); output.optional = true;
      } else if (name == "shuffle") output = args[0];
      else if (name == "arguments")
        output = withChild(output, present(child(output)));
      result = withChild(result, output);
    }
    return result;
  }
};

}  // namespace

void checkOptions(const Program& program, const OptionTypeQueries& queries) {
  Checker(program, queries).run();
}

}  // namespace lan
