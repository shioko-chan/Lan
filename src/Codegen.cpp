#include "lan/Codegen.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace lan {

namespace {

const char* typeName(Type type) {
  switch (type) {
    case Type::Number: return "数字";
    case Type::Boolean: return "真假值";
    case Type::Text: return "文字";
    case Type::List: return "列表";
    case Type::Record: return "Record";
    case Type::Table: return "Table";
    case Type::Dynamic: return "动态值";
    case Type::Unknown: return "尚未确定的类型";
  }
  return "未知类型";
}

struct TypeNode {
  int parent;
  Type type = Type::Unknown;
  int listElement = -1;
};

struct FunctionType {
  std::vector<int> params;
  int result;
};

using Presence = unsigned char;
constexpr Presence kNoPresence = 0;
constexpr Presence kPresent = 1;
constexpr Presence kMissing = 2;
constexpr Presence kMaybeMissing = kPresent | kMissing;

class Inference {
 public:
  explicit Inference(const Program& program) {
    for (const auto& function : program.functions) {
      if (function->name == "main" || function->name == "length" ||
          function->name == "table" || function->name == "row_count" ||
          function->name == "sum" || function->name == "average" ||
          function->name == "min" || function->name == "max" ||
          function->name == "count" || function->name == "fill_missing" ||
          function->name == "where" || function->name == "add_column" ||
          function->name == "replace_column" ||
          function->name == "add" || function->name == "replace" ||
          function->name == "select_columns" || function->name == "take" ||
          function->name == "drop" || function->name == "sort_by" ||
          function->name == "same" || function->name == "first" ||
          function->name == "last" || function->name == "map" ||
          function->name == "filter" || function->name == "select" ||
          function->name == "find" || function->name == "any" ||
          function->name == "all" || function->name == "is_missing" ||
          function->name == "require")
        fail("“" + function->name + "”由语言保留，请换一个函数名");
      if (functions_.count(function->name))
        fail("函数“" + function->name + "”定义了不止一次");
      FunctionType signature;
      for (std::size_t i = 0; i < function->params.size(); ++i)
        signature.params.push_back(newVariable());
      signature.result = newVariable();
      functions_.emplace(function->name, std::move(signature));
      definitions_.emplace(function->name, function.get());
    }

    rejectRecursion(program);

    for (const auto& function : program.functions) {
      scopes_.clear();
      scopes_.emplace_back();
      const auto& signature = functions_.at(function->name);
      bool sawDefault = false;
      for (std::size_t i = 0; i < function->params.size(); ++i) {
        const auto& parameter = function->params[i];
        if (parameter.defaultValue) sawDefault = true;
        else if (sawDefault)
          fail("函数“" + function->name + "”中，没有默认值的参数“" +
               parameter.name + "”必须放在默认参数前面");
        if (scopes_.back().count(parameter.name))
          fail("函数“" + function->name + "”中参数“" + parameter.name + "”重复了");
        if (parameter.defaultValue)
          unify(infer(*parameter.defaultValue), signature.params[i],
                "参数“" + parameter.name + "”的默认值");
        scopes_.back().emplace(parameter.name, signature.params[i]);
      }
      unify(infer(*function->body), signature.result,
            "函数“" + function->name + "”的结果");
    }

    scopes_.clear();
    scopes_.emplace_back();
    infer(*program.result);

    for (const auto& access : dynamicAccesses_) {
      Type objectType = resolved(access.object);
      Type keyType = resolved(access.key);
      if (objectType == Type::List || keyType == Type::Number) {
        bind(access.key, Type::Number, "List 下标");
        int element = listElement(access.object, "下标读取左侧");
        unify(access.result, element, "List 下标读取结果");
      } else {
        recordLikeVariables_.push_back(access.object);
        bind(access.key, Type::Text, "动态字段名");
      }
    }

    for (const auto& constraint : collectionConstraints_) {
      Type collectionType = resolved(constraint.collection);
      if (collectionType == Type::Unknown) {
        bind(constraint.collection, Type::List, "集合操作的输入");
        collectionType = Type::List;
      }
      if (collectionType == Type::List) {
        unify(constraint.element,
              listElement(constraint.collection, "集合操作的 List 输入"),
              "集合元素");
      } else if (collectionType == Type::Table) {
        bind(constraint.element, Type::Record, "Table 的逻辑行");
      } else {
        fail("集合操作需要 List 或 Table，但这里得到的是" +
             std::string(typeName(collectionType)));
      }
    }
    for (const auto& constraint : tableInputConstraints_) {
      Type inputType = resolved(constraint.input);
      if (inputType == Type::Unknown) {
        bind(constraint.input, Type::Record, "table 的输入");
      } else if (inputType == Type::List) {
        bind(listElement(constraint.input, "table 的行列表"), Type::Record,
             "table 的每一行");
      } else if (inputType != Type::Record) {
        fail("table 需要 Record<List<T>> 或 List<Record>");
      }
    }

    resolveShapes();

    for (int variable : plusVariables_) {
      Type type = resolved(variable);
      if (type == Type::Unknown) bind(variable, Type::Number, "加法");
      else if (type != Type::Number && type != Type::Text)
        fail("“+”只能连接两个数字或两段文字，不能用于" +
             std::string(typeName(type)));
    }
    for (int variable : equalityVariables_) {
      if (resolved(variable) == Type::List || resolved(variable) == Type::Record ||
          resolved(variable) == Type::Table)
        fail("列表、Record 和 Table 暂时不能使用 == 或 != 整体比较");
    }
    for (int variable : recordLikeVariables_) {
      Type type = resolved(variable);
      if (type == Type::Unknown) bind(variable, Type::Record, "字段访问左侧");
      else if (type != Type::Record && type != Type::Table)
        fail("字段访问左侧需要 Record 或 Table，但这里得到的是" +
             std::string(typeName(type)));
    }
    for (const auto& [expression, variable] : expressions_)
      if (dynamic_cast<const FieldExpr*>(expression) && resolved(variable) == Type::Unknown)
        bind(variable, Type::Dynamic, "动态字段");
    for (int element : tableColumnElements_)
      if (resolved(element) == Type::Unknown)
        bind(element, Type::Dynamic, "Table 的动态列元素");
    for (auto& node : nodes_) {
      int root = find(node.parent);
      if (nodes_[root].type == Type::Unknown) nodes_[root].type = Type::Number;
    }
    analyzePresence(program);
  }

  Type expressionType(const Expr& expr) { return resolved(expressions_.at(&expr)); }

  Type listElementType(const ListExpr& expr) {
    return resolved(listElement(expressions_.at(&expr), "列表"));
  }

  Type expressionListElementType(const Expr& expr) {
    return resolved(listElement(expressions_.at(&expr), "List 表达式"));
  }

  unsigned expressionListRank(const Expr& expr) {
    return listRank(expressions_.at(&expr));
  }

  Type collectionElementType(const CollectionExpr& expression) {
    return resolved(collectionElements_.at(&expression));
  }

  bool tableHasFixedField(const CollectionExpr& expression,
                          const std::string& name) {
    int input = expressions_.at(expression.collection.get());
    auto shape = shapes_.find(find(input));
    return shape != shapes_.end() && shape->second.fields.count(name) != 0;
  }

  std::vector<Type> parameterTypes(const Function& function) {
    std::vector<Type> result;
    for (int variable : functions_.at(function.name).params)
      result.push_back(resolved(variable));
    return result;
  }

  Type resultType(const Function& function) {
    return resolved(functions_.at(function.name).result);
  }

  Presence expressionPresence(const Expr& expr) const { return presences_.at(&expr); }

  std::vector<Presence> parameterPresences(const Function& function) const {
    return parameterPresences_.at(function.name);
  }

  Presence resultPresence(const Function& function) const {
    return resultPresences_.at(function.name);
  }

  bool isVectorOperation(const BinaryExpr& expression) const {
    return vectorOperations_.count(&expression) != 0;
  }

 private:
  std::vector<TypeNode> nodes_;
  std::unordered_map<const Expr*, int> expressions_;
  std::unordered_map<std::string, FunctionType> functions_;
  std::unordered_map<std::string, const Function*> definitions_;
  std::vector<std::unordered_map<std::string, int>> scopes_;
  std::vector<int> plusVariables_;
  std::vector<int> equalityVariables_;
  std::vector<int> recordLikeVariables_;
  struct DynamicAccess {
    int object;
    int key;
    int result;
  };
  std::vector<DynamicAccess> dynamicAccesses_;
  std::vector<int> tableColumnElements_;
  std::unordered_set<const BinaryExpr*> vectorOperations_;
  struct CollectionConstraint {
    int collection;
    int element;
    int result;
    int body;
    const CollectionExpr* expression;
  };
  std::vector<CollectionConstraint> collectionConstraints_;
  std::unordered_map<const CollectionExpr*, int> collectionElements_;
  struct TableInputConstraint { int input; int result; };
  std::vector<TableInputConstraint> tableInputConstraints_;
  struct Shape {
    std::unordered_map<std::string, int> fields;
    bool complete = true;
  };
  std::unordered_map<int, Shape> shapes_;
  struct FixedAccess {
    int object;
    int result;
    std::string field;
  };
  std::vector<FixedAccess> fixedAccesses_;
  std::vector<std::pair<int, int>> shapeCopies_;
  std::unordered_map<const Expr*, Presence> presences_;
  std::unordered_map<std::string, std::vector<Presence>> parameterPresences_;
  std::unordered_map<std::string, Presence> resultPresences_;
  std::vector<std::unordered_map<std::string, Presence>> presenceScopes_;
  std::unordered_map<std::string, std::unordered_map<std::string, Presence>>
      presenceResultsByContext_;
  std::unordered_set<std::string> presenceAnalyzedFunctions_;

  void collectCalls(const Expr& expr, std::unordered_set<std::string>& calls) const {
    if (auto* list = dynamic_cast<const ListExpr*>(&expr)) {
      for (const auto& element : list->elements) collectCalls(*element, calls);
    } else if (auto* record = dynamic_cast<const RecordExpr*>(&expr)) {
      for (const auto& field : record->fields) {
        if (field.key) collectCalls(*field.key, calls);
        collectCalls(*field.value, calls);
      }
    } else if (auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
      collectCalls(*unary->operand, calls);
    } else if (auto* question = dynamic_cast<const PropagateExpr*>(&expr)) {
      collectCalls(*question->operand, calls);
    } else if (auto* field = dynamic_cast<const FieldExpr*>(&expr)) {
      collectCalls(*field->record, calls);
      if (field->key) collectCalls(*field->key, calls);
    } else if (auto* update = dynamic_cast<const RecordUpdateExpr*>(&expr)) {
      collectCalls(*update->value, calls);
      collectCalls(*update->record, calls);
      if (update->key) collectCalls(*update->key, calls);
    } else if (auto* update = dynamic_cast<const NestedRecordUpdateExpr*>(&expr)) {
      collectCalls(*update->record, calls);
      for (const auto& part : update->path)
        if (part.key) collectCalls(*part.key, calls);
      collectCalls(*update->value, calls);
    } else if (auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
      collectCalls(*binary->lhs, calls);
      collectCalls(*binary->rhs, calls);
    } else if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
      if (functions_.count(call->callee)) calls.insert(call->callee);
      for (const auto& argument : call->args) collectCalls(*argument, calls);
    } else if (auto* collection = dynamic_cast<const CollectionExpr*>(&expr)) {
      collectCalls(*collection->collection, calls);
      collectCalls(*collection->body, calls);
    } else if (auto* binding = dynamic_cast<const ElementBindingExpr*>(&expr)) {
      collectCalls(*binding->body, calls);
    } else if (auto* let = dynamic_cast<const LetExpr*>(&expr)) {
      collectCalls(*let->value, calls);
      collectCalls(*let->body, calls);
    } else if (auto* conditional = dynamic_cast<const IfExpr*>(&expr)) {
      collectCalls(*conditional->condition, calls);
      collectCalls(*conditional->thenValue, calls);
      collectCalls(*conditional->elseValue, calls);
    }
  }

  void rejectRecursion(const Program& program) const {
    std::unordered_map<std::string, std::unordered_set<std::string>> graph;
    for (const auto& function : program.functions) {
      auto& calls = graph[function->name];
      for (const auto& parameter : function->params)
        if (parameter.defaultValue) collectCalls(*parameter.defaultValue, calls);
      collectCalls(*function->body, calls);
    }

    enum class Visit { Unvisited, Visiting, Done };
    std::unordered_map<std::string, Visit> visits;
    std::vector<std::string> path;
    std::function<void(const std::string&)> visit = [&](const std::string& name) {
      if (visits[name] == Visit::Done) return;
      if (visits[name] == Visit::Visiting) {
        auto start = std::find(path.begin(), path.end(), name);
        std::string cycle;
        for (auto current = start; current != path.end(); ++current) {
          if (!cycle.empty()) cycle += " → ";
          cycle += *current;
        }
        if (!cycle.empty()) cycle += " → ";
        cycle += name;
        fail("Lan 禁止递归调用：" + cycle);
      }
      visits[name] = Visit::Visiting;
      path.push_back(name);
      for (const auto& called : graph.at(name)) visit(called);
      path.pop_back();
      visits[name] = Visit::Done;
    };
    for (const auto& function : program.functions) visit(function->name);
  }

  [[noreturn]] static void fail(const std::string& message) {
    throw std::runtime_error(message);
  }

  std::vector<int> argumentSlots(const CallExpr& call,
                                 const Function& definition) const {
    std::vector<int> slots(definition.params.size(), -1);
    bool sawNamed = false;
    std::size_t nextPositional = 0;
    for (std::size_t source = 0; source < call.args.size(); ++source) {
      const std::string& name = call.argNames[source];
      std::size_t slot = 0;
      if (name.empty()) {
        if (sawNamed)
          fail("函数“" + call.callee + "”的位置参数必须放在命名参数之前");
        slot = nextPositional++;
        if (slot >= definition.params.size())
          fail("函数“" + call.callee + "”收到的参数太多了");
      } else {
        sawNamed = true;
        auto found = std::find_if(
            definition.params.begin(), definition.params.end(),
            [&](const Parameter& parameter) { return parameter.name == name; });
        if (found == definition.params.end())
          fail("函数“" + call.callee + "”没有名为“" + name + "”的参数");
        slot = static_cast<std::size_t>(found - definition.params.begin());
      }
      if (slots[slot] >= 0)
        fail("函数“" + call.callee + "”的参数“" +
             definition.params[slot].name + "”被传入了不止一次");
      slots[slot] = static_cast<int>(source);
    }
    for (std::size_t slot = 0; slot < slots.size(); ++slot)
      if (slots[slot] < 0 && !definition.params[slot].defaultValue)
        fail("调用函数“" + call.callee + "”时缺少必填参数“" +
             definition.params[slot].name + "”");
    return slots;
  }

  int newVariable() {
    int id = static_cast<int>(nodes_.size());
    nodes_.push_back({id, Type::Unknown, -1});
    return id;
  }

  int find(int variable) {
    if (nodes_[variable].parent != variable)
      nodes_[variable].parent = find(nodes_[variable].parent);
    return nodes_[variable].parent;
  }

  Type resolved(int variable) { return nodes_[find(variable)].type; }

  void bind(int variable, Type type, const std::string& contextName) {
    int root = find(variable);
    Type old = nodes_[root].type;
    if (old != Type::Unknown && old != type)
      fail(contextName + "需要" + typeName(type) + "，但这里得到的是" + typeName(old));
    nodes_[root].type = type;
  }

  int listElement(int variable, const std::string& contextName) {
    bind(variable, Type::List, contextName);
    int root = find(variable);
    if (nodes_[root].listElement < 0)
      nodes_[root].listElement = newVariable();
    return nodes_[root].listElement;
  }

  unsigned listRank(int variable) {
    unsigned rank = 0;
    while (resolved(variable) == Type::List) {
      variable = listElement(variable, "嵌套 List");
      ++rank;
    }
    return rank;
  }

  int listLeaf(int variable, const std::string& contextName) {
    while (resolved(variable) == Type::List)
      variable = listElement(variable, contextName);
    return variable;
  }

  int makeResultShape(int source, int result, const std::string& contextName) {
    while (resolved(source) == Type::List) {
      source = listElement(source, contextName);
      result = listElement(result, contextName);
    }
    return result;
  }

  void mergeShapeInto(int destination, const Shape& source,
                      const std::string& contextName) {
    int root = find(destination);
    auto existing = shapes_.find(root);
    if (existing == shapes_.end()) {
      shapes_.emplace(root, source);
      return;
    }
    Shape& target = existing->second;
    bool sameFields = target.fields.size() == source.fields.size();
    for (auto field = target.fields.begin(); field != target.fields.end();) {
      auto incoming = source.fields.find(field->first);
      if (incoming == source.fields.end()) {
        sameFields = false;
        field = target.fields.erase(field);
        continue;
      }
      unify(field->second, incoming->second,
            contextName + "的字段“" + field->first + "”");
      ++field;
    }
    target.complete = target.complete && source.complete && sameFields;
  }

  void attachShape(int variable, Shape shape, const std::string& contextName) {
    mergeShapeInto(variable, shape, contextName);
  }

  void unify(int lhs, int rhs, const std::string& contextName) {
    int leftRoot = find(lhs);
    int rightRoot = find(rhs);
    if (leftRoot == rightRoot) return;
    Type leftType = nodes_[leftRoot].type;
    Type rightType = nodes_[rightRoot].type;
    int leftElement = nodes_[leftRoot].listElement;
    int rightElement = nodes_[rightRoot].listElement;
    if (leftType != Type::Unknown && rightType != Type::Unknown && leftType != rightType)
      fail(contextName + "两边的种类不同：一边是" + typeName(leftType) +
           "，另一边是" + typeName(rightType));
    nodes_[rightRoot].parent = leftRoot;
    if (leftType == Type::Unknown) nodes_[leftRoot].type = rightType;
    if (leftElement < 0) nodes_[leftRoot].listElement = rightElement;
    else if (rightElement >= 0)
      unify(leftElement, rightElement, contextName + "的列表元素");
    auto rightShape = shapes_.find(rightRoot);
    if (rightShape != shapes_.end()) {
      Shape moved = std::move(rightShape->second);
      shapes_.erase(rightShape);
      mergeShapeInto(leftRoot, moved, contextName);
    }
  }

  void resolveShapes() {
    for (const auto& constraint : tableInputConstraints_) {
      Shape converted;
      int input = constraint.input;
      if (resolved(input) == Type::List) {
        int row = listElement(input, "table 的行列表");
        if (auto found = shapes_.find(find(row)); found != shapes_.end())
          converted = found->second;
        else
          converted.complete = false;
      } else if (auto found = shapes_.find(find(input)); found != shapes_.end()) {
        converted.complete = found->second.complete;
        for (const auto& [name, column] : found->second.fields) {
          if (resolved(column) == Type::List)
            converted.fields.emplace(
                name, listElement(column, "Table 列“" + name + "”"));
          else
            converted.complete = false;
        }
      } else {
        converted.complete = false;
      }
      attachShape(constraint.result, std::move(converted), "table schema");
    }

    for (const auto& [source, destination] : shapeCopies_)
      if (auto found = shapes_.find(find(source)); found != shapes_.end())
        attachShape(destination, found->second, "Table schema 传播");

    for (const auto& constraint : collectionConstraints_) {
      if (resolved(constraint.collection) != Type::Table) continue;
      auto inputShape = shapes_.find(find(constraint.collection));
      if (inputShape != shapes_.end())
        attachShape(constraint.element, inputShape->second, "Table 逻辑行");

      const auto operation = constraint.expression->operation;
      if (operation == CollectionOperation::Select) {
        if (auto bodyShape = shapes_.find(find(constraint.body));
            bodyShape != shapes_.end())
          attachShape(constraint.result, bodyShape->second, "select schema");
      } else if (operation == CollectionOperation::Add ||
                 operation == CollectionOperation::Replace) {
        Shape output;
        if (inputShape != shapes_.end()) output = inputShape->second;
        else output.complete = false;
        if (auto bodyShape = shapes_.find(find(constraint.body));
            bodyShape != shapes_.end()) {
          output.complete = output.complete && bodyShape->second.complete;
          for (const auto& [name, value] : bodyShape->second.fields)
            output.fields[name] = value;
        } else {
          output.complete = false;
        }
        attachShape(constraint.result, std::move(output), "Table 更新 schema");
      }
    }

    for (const auto& access : fixedAccesses_) {
      auto foundShape = shapes_.find(find(access.object));
      if (foundShape == shapes_.end()) continue;
      auto foundField = foundShape->second.fields.find(access.field);
      if (foundField == foundShape->second.fields.end()) continue;
      if (resolved(access.object) == Type::Table)
        unify(listElement(access.result, "Table 列“" + access.field + "”"),
              foundField->second, "Table 列“" + access.field + "”");
      else
        unify(access.result, foundField->second,
              "Record 字段“" + access.field + "”");
    }
  }

  int remember(const Expr& expr, int variable) {
    expressions_.emplace(&expr, variable);
    return variable;
  }

  int infer(const Expr& expr) {
    int result = newVariable();
    remember(expr, result);
    if (dynamic_cast<const NumberExpr*>(&expr)) {
      bind(result, Type::Number, "数字");
      return result;
    }
    if (dynamic_cast<const BooleanExpr*>(&expr)) {
      bind(result, Type::Boolean, "是/否值");
      return result;
    }
    if (dynamic_cast<const TextExpr*>(&expr)) {
      bind(result, Type::Text, "文字");
      return result;
    }
    if (dynamic_cast<const MissingExpr*>(&expr)) return result;
    if (auto* list = dynamic_cast<const ListExpr*>(&expr)) {
      int elementType = listElement(result, "列表");
      for (const auto& element : list->elements)
        unify(elementType, infer(*element), "列表中的元素");
      return result;
    }
    if (auto* record = dynamic_cast<const RecordExpr*>(&expr)) {
      std::unordered_set<std::string> names;
      Shape shape;
      for (const auto& field : record->fields) {
        if (!field.key && !names.insert(field.name).second)
          fail("Record 字段“" + field.name + "”重复了");
        if (field.key) {
          bind(infer(*field.key), Type::Text, "动态字段名");
          shape.complete = false;
        }
        int value = infer(*field.value);
        if (!field.key) shape.fields.emplace(field.name, value);
      }
      bind(result, Type::Record, "Record");
      attachShape(result, std::move(shape), "Record schema");
      return result;
    }
    if (auto* variable = dynamic_cast<const VariableExpr*>(&expr)) {
      for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto found = scope->find(variable->name);
        if (found != scope->end()) {
          unify(result, found->second, "名称“" + variable->name + "”");
          return result;
        }
      }
      if (variable->name == "$lan_current_element")
        fail("当前元素“.”只能写在集合操作的元素表达式中");
      fail("找不到名称“" + variable->name + "”，请检查拼写或先给它一个值");
    }
    if (auto* let = dynamic_cast<const LetExpr*>(&expr)) {
      int value = infer(*let->value);
      scopes_.emplace_back();
      scopes_.back().emplace(let->name, value);
      int body = infer(*let->body);
      scopes_.pop_back();
      unify(result, body, "局部公式“" + let->name + "”");
      return result;
    }
    if (auto* conditional = dynamic_cast<const IfExpr*>(&expr)) {
      int condition = infer(*conditional->condition);
      bind(condition, Type::Boolean, "if 后面的条件");
      int thenValue = infer(*conditional->thenValue);
      int elseValue = infer(*conditional->elseValue);
      unify(thenValue, elseValue, "if 和 else 的结果");
      unify(result, thenValue, "条件的结果");
      return result;
    }
    if (auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
      int operand = infer(*unary->operand);
      Type required = unary->op == '!' ? Type::Boolean : Type::Number;
      if (resolved(operand) == Type::List) {
        bind(listLeaf(operand, "一元整列运算"), required,
             std::string("符号“") + unary->op + "”的列元素");
        bind(makeResultShape(operand, result, "一元整列运算结果"), required,
             "一元整列运算结果");
      } else {
        bind(operand, required, std::string("符号“") + unary->op + "”");
        bind(result, required, "单项计算");
      }
      return result;
    }
    if (auto* question = dynamic_cast<const PropagateExpr*>(&expr)) {
      unify(result, infer(*question->operand), "“?”的结果");
      return result;
    }
    if (auto* field = dynamic_cast<const FieldExpr*>(&expr)) {
      int object = infer(*field->record);
      if (field->key) {
        dynamicAccesses_.push_back({object, infer(*field->key), result});
      } else {
        recordLikeVariables_.push_back(object);
        fixedAccesses_.push_back({object, result, field->name});
        if (resolved(object) == Type::Table) {
          int element = listElement(result, "Table 的列");
          tableColumnElements_.push_back(element);
        }
      }
      return result;
    }
    if (auto* update = dynamic_cast<const RecordUpdateExpr*>(&expr)) {
      int record = infer(*update->record);
      bind(record, Type::Record, "字段更新左侧");
      if (update->key) bind(infer(*update->key), Type::Text, "动态字段名");
      infer(*update->value);
      bind(result, Type::Record, "字段更新结果");
      return result;
    }
    if (auto* update = dynamic_cast<const NestedRecordUpdateExpr*>(&expr)) {
      bind(infer(*update->record), Type::Record, "嵌套字段更新左侧");
      for (const auto& part : update->path)
        if (part.key) bind(infer(*part.key), Type::Text, "动态字段名");
      infer(*update->value);
      bind(result, Type::Record, "嵌套字段更新结果");
      return result;
    }
    if (auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
      int lhs = infer(*binary->lhs);
      int rhs = infer(*binary->rhs);
      const auto& op = binary->op;
      bool leftList = resolved(lhs) == Type::List;
      bool rightList = resolved(rhs) == Type::List;
      bool vectorOperation = leftList || rightList;
      if (op == "??" && !leftList) vectorOperation = false;
      if (vectorOperation) {
        vectorOperations_.insert(binary);
        int leftElement = listLeaf(lhs, "整列运算左侧");
        int rightElement = listLeaf(rhs, "整列运算右侧");
        int shape = listRank(lhs) >= listRank(rhs) ? lhs : rhs;
        int resultElement = makeResultShape(shape, result, "整列运算结果");
        if (op == "+" || op == "??") {
          unify(leftElement, rightElement, "整列运算");
          unify(resultElement, leftElement, "整列运算结果");
          if (op == "+") plusVariables_.push_back(resultElement);
        } else if (op == "and" || op == "or") {
          bind(leftElement, Type::Boolean, "整列逻辑运算左侧");
          bind(rightElement, Type::Boolean, "整列逻辑运算右侧");
          bind(resultElement, Type::Boolean, "整列逻辑运算结果");
        } else if (op == "==" || op == "!=") {
          unify(leftElement, rightElement, "整列比较");
          equalityVariables_.push_back(leftElement);
          bind(resultElement, Type::Boolean, "整列比较结果");
        } else {
          bind(leftElement, Type::Number, "整列运算左侧");
          bind(rightElement, Type::Number, "整列运算右侧");
          bind(resultElement,
               (op == "<" || op == ">" || op == "<=" || op == ">=")
                   ? Type::Boolean : Type::Number,
               "整列运算结果");
        }
        return result;
      }
      if (op == "??") {
        unify(lhs, rhs, "“??”两边");
        unify(result, lhs, "默认值的结果");
      } else if (op == "+") {
        unify(lhs, rhs, "“+”号");
        unify(result, lhs, "“+”号的结果");
        plusVariables_.push_back(result);
      } else if (op == "-" || op == "*" || op == "/") {
        bind(lhs, Type::Number, "“" + op + "”号左边");
        bind(rhs, Type::Number, "“" + op + "”号右边");
        bind(result, Type::Number, "计算结果");
      } else if (op == "and" || op == "or") {
        bind(lhs, Type::Boolean, "“" + op + "”左边");
        bind(rhs, Type::Boolean, "“" + op + "”右边");
        bind(result, Type::Boolean, "逻辑结果");
      } else if (op == "==" || op == "!=") {
        unify(lhs, rhs, "比较号“" + op + "”");
        equalityVariables_.push_back(lhs);
        bind(result, Type::Boolean, "比较结果");
      } else {
        bind(lhs, Type::Number, "比较号“" + op + "”左边");
        bind(rhs, Type::Number, "比较号“" + op + "”右边");
        bind(result, Type::Boolean, "比较结果");
      }
      return result;
    }
    if (auto* collection = dynamic_cast<const CollectionExpr*>(&expr)) {
      int input = infer(*collection->collection);
      int element = newVariable();
      collectionElements_.emplace(collection, element);
      scopes_.emplace_back();
      scopes_.back().emplace("$lan_current_element", element);
      if (!collection->binding.empty())
        scopes_.back().emplace(collection->binding, element);
      int body = infer(*collection->body);
      scopes_.pop_back();

      switch (collection->operation) {
        case CollectionOperation::Map:
          unify(listElement(result, "map 的结果"), body, "map 的元素结果");
          break;
        case CollectionOperation::Filter:
          bind(body, Type::Boolean, "filter 的条件");
          unify(result, input, "filter 保留输入容器类型");
          break;
        case CollectionOperation::Find:
          bind(body, Type::Boolean, "find 的条件");
          unify(result, element, "find 的结果");
          break;
        case CollectionOperation::Any:
        case CollectionOperation::All:
          bind(body, Type::Boolean, "any/all 的条件");
          bind(result, Type::Boolean, "any/all 的结果");
          break;
        case CollectionOperation::Select:
        case CollectionOperation::Add:
        case CollectionOperation::Replace:
          bind(input, Type::Table, "Table schema 操作的输入");
          bind(element, Type::Record, "Table 的逻辑行");
          bind(body, Type::Record, "Table schema 操作的字段表达式");
          bind(result, Type::Table, "Table schema 操作的结果");
          break;
      }
      collectionConstraints_.push_back(
          {input, element, result, body, collection});
      return result;
    }
    if (dynamic_cast<const ElementBindingExpr*>(&expr))
      fail("元素绑定“=>”只能写在集合操作中");
    if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
      bool hasNamed = std::any_of(call->argNames.begin(), call->argNames.end(),
                                  [](const std::string& name) { return !name.empty(); });
      if (hasNamed && !functions_.count(call->callee))
        fail("内置函数“" + call->callee + "”暂不接受命名参数");
      if (call->callee == "table") {
        if (call->args.size() != 1)
          fail("table 需要 1 个 Record，但这里给了 " +
               std::to_string(call->args.size()) + " 个值");
        int input = infer(*call->args[0]);
        if (resolved(input) == Type::List)
          bind(listElement(input, "table 的行列表"), Type::Record,
               "table 的每一行");
        else if (resolved(input) == Type::Record)
          bind(input, Type::Record, "table 的输入");
        else
          tableInputConstraints_.push_back({input, result});
        if (resolved(input) == Type::List || resolved(input) == Type::Record)
          tableInputConstraints_.push_back({input, result});
        bind(result, Type::Table, "table 的结果");
        return result;
      }
      if (call->callee == "is_missing") {
        if (call->args.size() != 1) fail("is_missing 需要 1 个值");
        infer(*call->args[0]);
        bind(result, Type::Boolean, "is_missing 的结果");
        return result;
      }
      if (call->callee == "require") {
        if (call->args.size() != 1) fail("require 需要 1 个值");
        unify(result, infer(*call->args[0]), "require 的结果");
        return result;
      }
      if (call->callee == "row_count") {
        if (call->args.size() != 1)
          fail("row_count 需要 1 个 Table，但这里给了 " +
               std::to_string(call->args.size()) + " 个值");
        bind(infer(*call->args[0]), Type::Table, "row_count 的输入");
        bind(result, Type::Number, "row_count 的结果");
        return result;
      }
      if (call->callee == "sum" || call->callee == "average" ||
          call->callee == "min" || call->callee == "max" ||
          call->callee == "count") {
        if (call->args.size() != 1 && call->args.size() != 2)
          fail("函数“" + call->callee + "”需要一个 List，或者一个 Table 和列名");
        if (call->args.size() == 1) {
          int element = listElement(infer(*call->args[0]),
                                    "函数“" + call->callee + "”的输入");
          if (call->callee != "count")
            bind(element, Type::Number, "函数“" + call->callee + "”的列表元素");
        } else {
          bind(infer(*call->args[0]), Type::Table,
               "函数“" + call->callee + "”的第一个输入");
          bind(infer(*call->args[1]), Type::Text,
               "函数“" + call->callee + "”的列名");
        }
        bind(result, Type::Number, "函数“" + call->callee + "”的结果");
        return result;
      }
      if (call->callee == "length") {
        if (call->args.size() != 1)
          fail("length 需要 1 个列表，但这里给了 " +
               std::to_string(call->args.size()) + " 个值");
        int list = infer(*call->args[0]);
        listElement(list, "length 的输入");
        bind(result, Type::Number, "length 的结果");
        return result;
      }
      if (call->callee == "first" || call->callee == "last") {
        if (call->args.size() != 1)
          fail("函数“" + call->callee + "”需要 1 个列表");
        int list = infer(*call->args[0]);
        unify(result, listElement(list, "函数“" + call->callee + "”的输入"),
              "函数“" + call->callee + "”的结果");
        return result;
      }
      if (call->callee == "same") {
        if (call->args.size() != 2)
          fail("same 需要两个要比较的值");
        infer(*call->args[0]);
        infer(*call->args[1]);
        bind(result, Type::Boolean, "same 的结果");
        return result;
      }
      if (call->callee == "fill_missing") {
        if (call->args.size() != 2)
          fail("fill_missing 需要一个列表和一个替代值");
        int list = infer(*call->args[0]);
        int element = listElement(list, "fill_missing 的列表");
        unify(element, infer(*call->args[1]), "fill_missing 的替代值");
        unify(result, list, "fill_missing 的结果");
        return result;
      }
      if (call->callee == "where") {
        if (call->args.size() != 2) fail("where 需要一个 Table 和一个真假值列表");
        bind(infer(*call->args[0]), Type::Table, "where 的输入");
        int mask = infer(*call->args[1]);
        bind(listElement(mask, "where 的筛选条件"), Type::Boolean,
             "where 的筛选条件");
        bind(result, Type::Table, "where 的结果");
        shapeCopies_.push_back({expressions_.at(call->args[0].get()), result});
        return result;
      }
      if (call->callee == "add_column" || call->callee == "replace_column") {
        const std::string operation = call->callee == "add_column" ? "add" : "replace";
        if (call->args.size() != 3)
          fail(operation + " 需要一个 Table、列名和列表");
        bind(infer(*call->args[0]), Type::Table, operation + " 的输入");
        bind(infer(*call->args[1]), Type::Text, operation + " 的列名");
        listElement(infer(*call->args[2]), operation + " 的新列");
        bind(result, Type::Table, operation + " 的结果");
        return result;
      }
      if (call->callee == "select_columns") {
        if (call->args.size() != 2)
          fail("select_columns 需要一个 Table 和列名列表");
        bind(infer(*call->args[0]), Type::Table, "select_columns 的输入");
        int names = infer(*call->args[1]);
        bind(listElement(names, "select_columns 的列名"), Type::Text,
             "select_columns 的列名");
        bind(result, Type::Table, "select_columns 的结果");
        return result;
      }
      if (call->callee == "take" || call->callee == "drop") {
        if (call->args.size() != 2)
          fail("函数“" + call->callee + "”需要一个 Table 和行数");
        bind(infer(*call->args[0]), Type::Table, "函数“" + call->callee + "”的输入");
        bind(infer(*call->args[1]), Type::Number, "函数“" + call->callee + "”的行数");
        bind(result, Type::Table, "函数“" + call->callee + "”的结果");
        shapeCopies_.push_back({expressions_.at(call->args[0].get()), result});
        return result;
      }
      if (call->callee == "sort_by") {
        if (call->args.size() != 2) fail("sort_by 需要一个 Table 和列名");
        bind(infer(*call->args[0]), Type::Table, "sort_by 的输入");
        bind(infer(*call->args[1]), Type::Text, "sort_by 的列名");
        bind(result, Type::Table, "sort_by 的结果");
        shapeCopies_.push_back({expressions_.at(call->args[0].get()), result});
        return result;
      }
      auto found = functions_.find(call->callee);
      if (found == functions_.end())
        fail("找不到函数“" + call->callee + "”，请检查它是否已经定义");
      const auto& definition = *definitions_.at(call->callee);
      auto slots = argumentSlots(*call, definition);
      for (std::size_t slot = 0; slot < slots.size(); ++slot)
        if (slots[slot] >= 0)
          unify(infer(*call->args[slots[slot]]), found->second.params[slot],
                "传给函数“" + call->callee + "”的参数“" +
                    definition.params[slot].name + "”");
      unify(result, found->second.result, "函数“" + call->callee + "”的结果");
      return result;
    }
    fail("编译器暂时不能理解这个表达式");
  }

  static Presence propagate(Presence lhs, Presence rhs) {
    Presence result = kNoPresence;
    if ((lhs & kPresent) && (rhs & kPresent)) result |= kPresent;
    if ((lhs & kMissing) || (rhs & kMissing)) result |= kMissing;
    return result;
  }

  static Presence coalescePresence(Presence lhs, Presence rhs) {
    Presence result = kNoPresence;
    if (lhs & kPresent) result |= kPresent;
    if (lhs & kMissing) result |= rhs;
    return result;
  }

  Presence presenceOf(const Expr& expr) {
    Presence result = kNoPresence;
    if (dynamic_cast<const NumberExpr*>(&expr) ||
        dynamic_cast<const BooleanExpr*>(&expr) ||
        dynamic_cast<const TextExpr*>(&expr) || dynamic_cast<const ListExpr*>(&expr) ||
        dynamic_cast<const RecordExpr*>(&expr)) {
      if (auto* list = dynamic_cast<const ListExpr*>(&expr))
        for (const auto& element : list->elements) presenceOf(*element);
      if (auto* record = dynamic_cast<const RecordExpr*>(&expr))
        for (const auto& field : record->fields) {
          if (field.key) presenceOf(*field.key);
          presenceOf(*field.value);
        }
      result = kPresent;
    } else if (dynamic_cast<const MissingExpr*>(&expr)) {
      result = kMissing;
    } else if (auto* variable = dynamic_cast<const VariableExpr*>(&expr)) {
      for (auto scope = presenceScopes_.rbegin(); scope != presenceScopes_.rend(); ++scope) {
        auto found = scope->find(variable->name);
        if (found != scope->end()) { result = found->second; break; }
      }
    } else if (auto* let = dynamic_cast<const LetExpr*>(&expr)) {
      Presence value = presenceOf(*let->value);
      presenceScopes_.emplace_back();
      presenceScopes_.back().emplace(let->name, value);
      result = presenceOf(*let->body);
      presenceScopes_.pop_back();
    } else if (auto* conditional = dynamic_cast<const IfExpr*>(&expr)) {
      Presence condition = presenceOf(*conditional->condition);
      const VariableExpr* refined = nullptr;
      bool presentInThen = false;
      const Expr* test = conditional->condition.get();
      if (auto* negated = dynamic_cast<const UnaryExpr*>(test);
          negated && negated->op == '!') {
        test = negated->operand.get();
        presentInThen = true;
      }
      if (auto* call = dynamic_cast<const CallExpr*>(test);
          call && call->callee == "is_missing" && call->args.size() == 1)
        refined = dynamic_cast<const VariableExpr*>(call->args[0].get());
      Presence thenPresence;
      Presence elsePresence;
      if (refined) {
        presenceScopes_.emplace_back();
        presenceScopes_.back().emplace(
            refined->name, presentInThen ? kPresent : kMissing);
        thenPresence = presenceOf(*conditional->thenValue);
        presenceScopes_.pop_back();
        presenceScopes_.emplace_back();
        presenceScopes_.back().emplace(
            refined->name, presentInThen ? kMissing : kPresent);
        elsePresence = presenceOf(*conditional->elseValue);
        presenceScopes_.pop_back();
      } else {
        thenPresence = presenceOf(*conditional->thenValue);
        elsePresence = presenceOf(*conditional->elseValue);
      }
      Presence branches = thenPresence | elsePresence;
      if (condition & kPresent) result |= branches;
      if (condition & kMissing) result |= kMissing;
    } else if (auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
      result = presenceOf(*unary->operand);
    } else if (auto* question = dynamic_cast<const PropagateExpr*>(&expr)) {
      result = presenceOf(*question->operand);
    } else if (auto* field = dynamic_cast<const FieldExpr*>(&expr)) {
      Presence record = presenceOf(*field->record);
      if (field->key) presenceOf(*field->key);
      result = record & kPresent ? kMaybeMissing : kMissing;
    } else if (auto* update = dynamic_cast<const RecordUpdateExpr*>(&expr)) {
      presenceOf(*update->value);
      Presence record = presenceOf(*update->record);
      if (update->key) presenceOf(*update->key);
      result = record;
    } else if (auto* update = dynamic_cast<const NestedRecordUpdateExpr*>(&expr)) {
      Presence record = presenceOf(*update->record);
      for (const auto& part : update->path)
        if (part.key) presenceOf(*part.key);
      presenceOf(*update->value);
      result = record;
    } else if (auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
      Presence lhs = presenceOf(*binary->lhs);
      Presence rhs = presenceOf(*binary->rhs);
      if (binary->op == "??") result = coalescePresence(lhs, rhs);
      else if (binary->op == "and" || binary->op == "or") {
        result = lhs == kPresent && rhs == kPresent ? kPresent : kMaybeMissing;
      } else {
        result = propagate(lhs, rhs);
      }
    } else if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
      if (call->callee == "length" || call->callee == "table" ||
          call->callee == "row_count") {
        result = presenceOf(*call->args[0]);
      } else if (call->callee == "is_missing") {
        presenceOf(*call->args[0]);
        result = kPresent;
      } else if (call->callee == "require") {
        presenceOf(*call->args[0]);
        result = kPresent;
      } else if (call->callee == "first" || call->callee == "last") {
        presenceOf(*call->args[0]);
        result = kMaybeMissing;
      } else if (call->callee == "same") {
        presenceOf(*call->args[0]);
        presenceOf(*call->args[1]);
        result = kPresent;
      } else if (call->callee == "sum" || call->callee == "count") {
        result = presenceOf(*call->args[0]);
        if (call->args.size() == 2)
          result = propagate(result, presenceOf(*call->args[1]));
      } else if (call->callee == "average" || call->callee == "min" ||
                 call->callee == "max") {
        presenceOf(*call->args[0]);
        if (call->args.size() == 2) presenceOf(*call->args[1]);
        result = kMaybeMissing;
      } else if (call->callee == "fill_missing" || call->callee == "where" ||
                 call->callee == "select_columns" || call->callee == "take" ||
                 call->callee == "drop" || call->callee == "sort_by") {
        result = propagate(presenceOf(*call->args[0]), presenceOf(*call->args[1]));
      } else if (call->callee == "add_column" ||
                 call->callee == "replace_column") {
        result = propagate(propagate(presenceOf(*call->args[0]),
                                    presenceOf(*call->args[1])),
                           presenceOf(*call->args[2]));
      } else {
        const auto& definition = *definitions_.at(call->callee);
        auto slots = argumentSlots(*call, definition);
        auto arguments = std::vector<Presence>(definition.params.size(), kNoPresence);
        for (std::size_t slot = 0; slot < slots.size(); ++slot)
          if (slots[slot] >= 0)
            arguments[slot] = presenceOf(*call->args[slots[slot]]);
        result = analyzeFunctionPresence(definition, std::move(arguments));
      }
    } else if (auto* collection = dynamic_cast<const CollectionExpr*>(&expr)) {
      Presence input = presenceOf(*collection->collection);
      presenceScopes_.emplace_back();
      Presence element = expressionType(*collection->collection) == Type::Table
          ? kPresent : kMaybeMissing;
      presenceScopes_.back().emplace("$lan_current_element", element);
      if (!collection->binding.empty())
        presenceScopes_.back().emplace(collection->binding, element);
      Presence body = presenceOf(*collection->body);
      presenceScopes_.pop_back();
      if (collection->operation == CollectionOperation::Find)
        result = kMaybeMissing;
      else if (collection->operation == CollectionOperation::Any ||
               collection->operation == CollectionOperation::All)
        result = body & kMissing ? kMaybeMissing : kPresent;
      else
        result = input;
    } else if (dynamic_cast<const ElementBindingExpr*>(&expr)) {
      result = kNoPresence;
    }
    presences_[&expr] |= result;
    return result;
  }

  Presence analyzeFunctionPresence(const Function& function,
                                    std::vector<Presence> arguments) {
    std::string contextKey;
    for (Presence argument : arguments)
      contextKey.push_back(static_cast<char>('0' + argument));
    for (Presence argument : arguments)
      contextKey.push_back(static_cast<char>('0' + argument));
    auto& contexts = presenceResultsByContext_[function.name];
    if (auto found = contexts.find(contextKey); found != contexts.end())
      return found->second;

    presenceAnalyzedFunctions_.insert(function.name);
    auto callerScopes = std::move(presenceScopes_);
    presenceScopes_.clear();
    presenceScopes_.emplace_back();
    for (std::size_t i = 0; i < function.params.size(); ++i) {
      if (arguments[i] == kNoPresence)
        arguments[i] = presenceOf(*function.params[i].defaultValue);
      presenceScopes_.back().emplace(function.params[i].name, arguments[i]);
    }

    auto& params = parameterPresences_.at(function.name);
    for (std::size_t i = 0; i < arguments.size(); ++i)
      params[i] |= arguments[i];
    Presence result = presenceOf(*function.body);
    resultPresences_[function.name] |= result;
    presenceScopes_ = std::move(callerScopes);
    presenceResultsByContext_[function.name].emplace(std::move(contextKey), result);
    return result;
  }

  void analyzePresence(const Program& program) {
    for (const auto& function : program.functions) {
      parameterPresences_[function->name] =
          std::vector<Presence>(function->params.size(), kNoPresence);
      resultPresences_[function->name] = kNoPresence;
    }
    presenceScopes_.clear();
    presenceScopes_.emplace_back();
    presenceOf(*program.result);

    for (const auto& function : program.functions) {
      if (presenceAnalyzedFunctions_.count(function->name)) continue;
      analyzeFunctionPresence(
          *function, std::vector<Presence>(function->params.size(), kPresent));
    }
  }
};

struct TypedValue {
  llvm::Value* value;
  Type type;
  Presence presence;
};

}  // namespace

struct CodeGenerator::Impl {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  llvm::IRBuilder<> builder{context};
  std::unique_ptr<Inference> inference;
  std::vector<std::unordered_map<std::string, TypedValue>> scopes;
  struct LazyTableRow {
    std::size_t scopeIndex;
    llvm::Value* table;
    llvm::Value* index;
    std::string binding;
  };
  std::vector<LazyTableRow> lazyTableRows;
  std::unordered_map<std::string, const Function*> declarations;
  llvm::StructType* taggedType = nullptr;
  llvm::StructType* listType = nullptr;
  llvm::StructType* optionalNumberType = nullptr;
  llvm::Function* printValueFunction = nullptr;
  llvm::Function* printListFunction = nullptr;
  llvm::BasicBlock* currentMissingTarget = nullptr;
  llvm::BasicBlock* rootMissingTarget = nullptr;

  Impl() : module(std::make_unique<llvm::Module>("lan_module", context)) {}

  void loadRecordRuntime() {
    auto buffer = llvm::MemoryBuffer::getFile(LAN_RECORD_BITCODE_PATH);
    if (!buffer)
      throw std::runtime_error("无法读取 Record 运行时：" + buffer.getError().message());
    auto runtime = llvm::parseBitcodeFile((*buffer)->getMemBufferRef(), context);
    if (!runtime)
      throw std::runtime_error("无法解析 Record 运行时：" +
                               llvm::toString(runtime.takeError()));
    if (llvm::Linker::linkModules(*module, std::move(*runtime)))
      throw std::runtime_error("无法链接 Record 运行时");
  }

  llvm::Type* llvmType(Type type) {
    switch (type) {
      case Type::Number: return builder.getDoubleTy();
      case Type::Boolean: return builder.getInt1Ty();
      case Type::Text: return llvm::PointerType::get(context, 0);
      case Type::List: return llvm::PointerType::get(context, 0);
      case Type::Record: return llvm::PointerType::get(context, 0);
      case Type::Table: return llvm::PointerType::get(context, 0);
      case Type::Dynamic: return taggedValueType();
      case Type::Unknown: break;
    }
    throw std::runtime_error("内部错误：类型尚未确定");
  }

  llvm::StructType* maybeNumberType() {
    if (!optionalNumberType) {
      optionalNumberType = llvm::StructType::create(context, "lan.maybe.number");
      optionalNumberType->setBody({builder.getInt1Ty(), builder.getDoubleTy()});
    }
    return optionalNumberType;
  }

  llvm::Type* encodedType(Type type, Presence presence) {
    if (presence == kPresent) return llvmType(type);
    if (type == Type::Number) return maybeNumberType();
    if (type == Type::Boolean) return builder.getIntNTy(2);
    return llvmType(type);
  }

  llvm::Value* missingValue(Type type) {
    if (type == Type::Dynamic) {
      llvm::Value* value = llvm::UndefValue::get(taggedValueType());
      value = builder.CreateInsertValue(value, builder.getInt32(0), 0);
      value = builder.CreateInsertValue(
          value, llvm::ConstantFP::get(builder.getDoubleTy(), 0.0), 1);
      return builder.CreateInsertValue(
          value, llvm::ConstantPointerNull::get(
                     llvm::cast<llvm::PointerType>(llvmType(Type::Text))), 2);
    }
    if (type == Type::Number) {
      llvm::Value* value = llvm::UndefValue::get(maybeNumberType());
      value = builder.CreateInsertValue(value, builder.getFalse(), 0);
      return builder.CreateInsertValue(
          value, llvm::ConstantFP::get(builder.getDoubleTy(), 0.0), 1);
    }
    if (type == Type::Boolean) return llvm::ConstantInt::get(builder.getIntNTy(2), 0);
    return llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(llvmType(type)));
  }

  llvm::Value* missingValue(Type type, Presence presence) {
    if (presence == kPresent)
      return llvm::UndefValue::get(encodedType(type, presence));
    return missingValue(type);
  }

  llvm::Value* encodePresent(llvm::Value* raw, Type type, Presence target) {
    if (type == Type::Dynamic) return raw;
    if (target == kPresent) return raw;
    if (type == Type::Number) {
      llvm::Value* value = llvm::UndefValue::get(maybeNumberType());
      value = builder.CreateInsertValue(value, builder.getTrue(), 0);
      return builder.CreateInsertValue(value, raw, 1);
    }
    if (type == Type::Boolean) {
      auto* widened = builder.CreateZExt(raw, builder.getIntNTy(2), "boolean.option");
      return builder.CreateAdd(widened, llvm::ConstantInt::get(builder.getIntNTy(2), 1),
                               "boolean.present");
    }
    return raw;
  }

  llvm::Value* presentFlag(const TypedValue& value) {
    if (value.presence == kPresent) return builder.getTrue();
    if (value.presence == kMissing) return builder.getFalse();
    if (value.type == Type::Dynamic)
      return builder.CreateICmpNE(builder.CreateExtractValue(value.value, 0),
                                  builder.getInt32(0), "dynamic.present");
    if (value.type == Type::Number)
      return builder.CreateExtractValue(value.value, 0, "number.present");
    if (value.type == Type::Boolean)
      return builder.CreateICmpNE(
          value.value, llvm::ConstantInt::get(builder.getIntNTy(2), 0), "boolean.present");
    return builder.CreateIsNotNull(value.value, "pointer.present");
  }

  llvm::Value* rawPayload(const TypedValue& value) {
    if (value.presence == kPresent) return value.value;
    if (value.type == Type::Dynamic) return value.value;
    if (value.type == Type::Number)
      return builder.CreateExtractValue(value.value, 1, "number.value");
    if (value.type == Type::Boolean)
      return builder.CreateICmpEQ(
          value.value, llvm::ConstantInt::get(builder.getIntNTy(2), 2), "boolean.value");
    return value.value;
  }

  TypedValue convertPresence(const TypedValue& value, Presence target) {
    if (value.presence == target) return value;
    if (target == kPresent) return {rawPayload(value), value.type, kPresent};
    if (value.presence == kPresent)
      return {encodePresent(value.value, value.type, target), value.type, target};
    if (value.presence == kMissing)
      return {missingValue(value.type), value.type, target};
    return {value.value, value.type, target};
  }

  void emitMissingError(const std::string& contextName) {
    auto putsFn = external("puts", builder.getInt32Ty(), {llvmType(Type::Text)});
    auto exitFn = external("exit", builder.getVoidTy(), {builder.getInt32Ty()});
    builder.CreateCall(
        putsFn, {builder.CreateGlobalString("Lan 运行错误：" + contextName + "不能使用 missing")});
    builder.CreateCall(exitFn, {builder.getInt32(1)});
    builder.CreateUnreachable();
  }

  TypedValue requirePresent(const TypedValue& value, const std::string& contextName) {
    if (value.presence == kPresent) return value;
    auto* function = builder.GetInsertBlock()->getParent();
    if (value.presence == kMissing) {
      emitMissingError(contextName);
      builder.SetInsertPoint(
          llvm::BasicBlock::Create(context, "after.missing.error", function));
      return {llvm::UndefValue::get(llvmType(value.type)), value.type, kPresent};
    }

    auto* valueBlock = llvm::BasicBlock::Create(context, "required.value", function);
    auto* errorBlock = llvm::BasicBlock::Create(context, "required.missing", function);
    builder.CreateCondBr(presentFlag(value), valueBlock, errorBlock);
    builder.SetInsertPoint(errorBlock);
    emitMissingError(contextName);
    builder.SetInsertPoint(valueBlock);
    return {rawPayload(value), value.type, kPresent};
  }

  TypedValue guarded(const std::vector<TypedValue>& inputs, Type resultType,
                     Presence resultPresence,
                     const std::function<llvm::Value*()>& compute) {
    (void)resultPresence;
    for (const auto& input : inputs)
      requirePresent(input, "普通运算");
    return {compute(), resultType, kPresent};
  }

  llvm::FunctionCallee external(const std::string& name, llvm::Type* result,
                                std::vector<llvm::Type*> params, bool variadic = false) {
    return module->getOrInsertFunction(
        name, llvm::FunctionType::get(result, std::move(params), variadic));
  }

  llvm::StructType* taggedValueType() {
    if (!taggedType) {
      taggedType = llvm::StructType::create(context, "lan.mixed.value");
      taggedType->setBody({builder.getInt32Ty(), builder.getDoubleTy(),
                           llvmType(Type::Text)});
    }
    return taggedType;
  }

  llvm::StructType* listValueType() {
    if (!listType) {
      listType = llvm::StructType::create(context, "lan.list");
      listType->setBody({builder.getInt64Ty(), llvmType(Type::Text),
                         builder.getInt32Ty(), llvmType(Type::Text)});
    }
    return listType;
  }

  int tagFor(Type type) {
    switch (type) {
      case Type::Number: return 1;
      case Type::Boolean: return 2;
      case Type::Text: return 3;
      case Type::List: return 4;
      case Type::Record: return 5;
      case Type::Table: return 5;
      case Type::Dynamic: break;
      case Type::Unknown: break;
    }
    throw std::runtime_error("内部错误：列表元素类型未知");
  }

  llvm::Value* allocate(llvm::Value* size, const std::string& name) {
    auto mallocFn = external("malloc", llvmType(Type::Text), {builder.getInt64Ty()});
    if (size->getType() != builder.getInt64Ty())
      size = builder.CreateZExtOrTrunc(size, builder.getInt64Ty());
    return builder.CreateCall(mallocFn, {size}, name);
  }

  std::array<llvm::Value*, 3> dynamicParts(const TypedValue& value) {
    if (value.type == Type::Dynamic)
      return {builder.CreateExtractValue(value.value, 0),
              builder.CreateExtractValue(value.value, 1),
              builder.CreateExtractValue(value.value, 2)};
    llvm::Value* tag = builder.getInt32(tagFor(value.type));
    if (value.presence == kMissing) tag = builder.getInt32(0);
    else if (value.presence == kMaybeMissing)
      tag = builder.CreateSelect(presentFlag(value), tag, builder.getInt32(0),
                                 "dynamic.tag");
    llvm::Value* number = llvm::ConstantFP::get(builder.getDoubleTy(), 0.0);
    llvm::Value* pointer = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(llvmType(Type::Text)));
    if (value.type == Type::Number) number = rawPayload(value);
    else if (value.type == Type::Boolean)
      number = builder.CreateUIToFP(rawPayload(value), builder.getDoubleTy(),
                                    "dynamic.boolean");
    else pointer = rawPayload(value);
    return {tag, number, pointer};
  }

  llvm::Value* emitRecord(const RecordExpr& record) {
    auto emptyFn = external("lan_record_empty", llvmType(Type::Record), {});
    auto assocFn = external("lan_record_assoc", llvmType(Type::Record),
                            {llvmType(Type::Record), llvmType(Type::Text),
                             builder.getInt32Ty(), builder.getDoubleTy(),
                             llvmType(Type::Text)});
    auto releaseFn = external("lan_record_release", builder.getVoidTy(),
                              {llvmType(Type::Record)});
    llvm::Value* root = builder.CreateCall(emptyFn, {}, "record.empty");
    for (const auto& field : record.fields) {
      auto* key = field.key
          ? requirePresent(emitExpr(*field.key), "动态字段名").value
          : builder.CreateGlobalString(field.name);
      auto value = emitExpr(*field.value);
      auto parts = dynamicParts(value);
      auto* updated = builder.CreateCall(
          assocFn, {root, key, parts[0], parts[1], parts[2]},
          "record.insert");
      builder.CreateCall(releaseFn, {root});
      root = updated;
    }
    return root;
  }

  llvm::Value* fieldKey(const std::string& name, const Expr* key) {
    if (!key) return builder.CreateGlobalString(name);
    return requirePresent(emitExpr(*key), "动态字段名").value;
  }

  void emitFieldTypeError(Type expected) {
    auto putsFn = external("puts", builder.getInt32Ty(), {llvmType(Type::Text)});
    auto exitFn = external("exit", builder.getVoidTy(), {builder.getInt32Ty()});
    builder.CreateCall(
        putsFn, {builder.CreateGlobalString("Lan 运行错误：读取到的值不是" +
                                           std::string(typeName(expected)))});
    builder.CreateCall(exitFn, {builder.getInt32(1)});
    builder.CreateUnreachable();
  }

  TypedValue decodeLookupValue(llvm::Value* found, llvm::Value* dynamic,
                               Type expected) {
    if (expected == Type::Dynamic)
      return {dynamic, Type::Dynamic, kMaybeMissing};

    auto* tag = builder.CreateExtractValue(dynamic, 0, "lookup.tag");
    auto* absent = builder.CreateOr(builder.CreateNot(found),
                                    builder.CreateICmpEQ(tag, builder.getInt32(0)),
                                    "lookup.missing");
    auto* valid = builder.CreateOr(absent,
        builder.CreateICmpEQ(tag, builder.getInt32(tagFor(expected))), "lookup.valid");
    auto* function = builder.GetInsertBlock()->getParent();
    auto* validBlock = llvm::BasicBlock::Create(context, "lookup.typed", function);
    auto* errorBlock = llvm::BasicBlock::Create(context, "lookup.type.error", function);
    builder.CreateCondBr(valid, validBlock, errorBlock);
    builder.SetInsertPoint(errorBlock);
    emitFieldTypeError(expected);
    builder.SetInsertPoint(validBlock);

    if (expected == Type::Number) {
      llvm::Value* option = llvm::UndefValue::get(maybeNumberType());
      option = builder.CreateInsertValue(option, builder.CreateNot(absent), 0);
      option = builder.CreateInsertValue(option, builder.CreateExtractValue(dynamic, 1), 1);
      return {option, expected, kMaybeMissing};
    }
    if (expected == Type::Boolean) {
      auto* truth = builder.CreateFCmpONE(
          builder.CreateExtractValue(dynamic, 1),
          llvm::ConstantFP::get(builder.getDoubleTy(), 0.0));
      auto* encoded = builder.CreateAdd(builder.CreateZExt(truth, builder.getIntNTy(2)),
                                        llvm::ConstantInt::get(builder.getIntNTy(2), 1));
      return {builder.CreateSelect(absent,
                                   llvm::ConstantInt::get(builder.getIntNTy(2), 0), encoded),
              expected, kMaybeMissing};
    }
    auto* pointer = builder.CreateExtractValue(dynamic, 2);
    auto* null = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(llvmType(expected)));
    return {builder.CreateSelect(absent, null, pointer), expected, kMaybeMissing};
  }

  TypedValue emitField(const FieldExpr& field, Type expected) {
    auto record = requirePresent(emitExpr(*field.record), "字段访问左侧");
    auto* key = fieldKey(field.name, field.key.get());
    return emitRecordLookup(record.value, key, expected);
  }

  TypedValue emitRecordLookup(llvm::Value* record, llvm::Value* key, Type expected) {
    auto* storage = builder.CreateAlloca(taggedValueType(), nullptr, "field.value");
    builder.CreateStore(missingValue(Type::Dynamic), storage);
    auto lookupFn = external("lan_record_lookup", builder.getInt32Ty(),
                             {llvmType(Type::Record), llvmType(Type::Text),
                              llvm::PointerType::get(context, 0)});
    auto* found = builder.CreateICmpNE(
        builder.CreateCall(lookupFn, {record, key, storage}), builder.getInt32(0),
        "field.found");
    auto* dynamic = builder.CreateLoad(taggedValueType(), storage, "field.dynamic");
    return decodeLookupValue(found, dynamic, expected);
  }

  TypedValue emitListIndex(const FieldExpr& access, Type expected) {
    auto list = requirePresent(emitExpr(*access.record), "List 下标读取左侧");
    auto index = requirePresent(emitExpr(*access.key), "List 下标");
    return emitListLookup(list.value, index.value, expected);
  }

  TypedValue emitListLookup(llvm::Value* list, llvm::Value* index, Type expected) {
    auto* storage = builder.CreateAlloca(taggedValueType(), nullptr, "list.item");
    builder.CreateStore(missingValue(Type::Dynamic), storage);
    auto lookupFn = external("lan_list_lookup", builder.getInt32Ty(),
        {llvmType(Type::List), builder.getDoubleTy(),
         llvm::PointerType::get(context, 0)});
    auto* found = builder.CreateICmpNE(
        builder.CreateCall(lookupFn, {list, index, storage}),
        builder.getInt32(0), "item.found");
    auto* dynamic = builder.CreateLoad(taggedValueType(), storage, "item.dynamic");
    return decodeLookupValue(found, dynamic, expected);
  }

  TypedValue emitRecordUpdate(const RecordUpdateExpr& update) {
    auto record = requirePresent(emitExpr(*update.record), "字段更新左侧");
    auto* key = fieldKey(update.name, update.key.get());
    auto value = emitExpr(*update.value);
    auto parts = dynamicParts(value);
    auto assocFn = external("lan_record_assoc", llvmType(Type::Record),
                            {llvmType(Type::Record), llvmType(Type::Text),
                             builder.getInt32Ty(), builder.getDoubleTy(),
                             llvmType(Type::Text)});
    return {builder.CreateCall(assocFn,
                               {record.value, key, parts[0], parts[1], parts[2]},
                               "record.update"),
            Type::Record, kPresent};
  }

  TypedValue emitNestedRecordUpdate(const NestedRecordUpdateExpr& update) {
    auto root = requirePresent(emitExpr(*update.record), "嵌套字段更新左侧");
    std::vector<llvm::Value*> keys;
    keys.reserve(update.path.size());
    for (const auto& part : update.path)
      keys.push_back(fieldKey(part.name, part.key.get()));

    std::vector<llvm::Value*> records{root.value};
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
      auto child = requirePresent(
          emitRecordLookup(records.back(), keys[i], Type::Record),
          "嵌套字段更新的中间字段");
      records.push_back(child.value);
    }

    TypedValue current = emitExpr(*update.value);
    auto assocFn = external("lan_record_assoc", llvmType(Type::Record),
                            {llvmType(Type::Record), llvmType(Type::Text),
                             builder.getInt32Ty(), builder.getDoubleTy(),
                             llvmType(Type::Text)});
    for (std::size_t i = keys.size(); i-- > 0;) {
      auto parts = dynamicParts(current);
      current = {builder.CreateCall(
                     assocFn,
                     {records[i], keys[i], parts[0], parts[1], parts[2]},
                     "record.nested.update"),
                 Type::Record, kPresent};
    }
    return current;
  }

  TypedValue emitListEndpoint(const CallExpr& call, bool last, Type expected) {
    auto list = requirePresent(emitExpr(*call.args[0]),
                               last ? "last 的输入" : "first 的输入");
    llvm::Value* index = llvm::ConstantFP::get(builder.getDoubleTy(), 0.0);
    if (last) {
      auto* length = builder.CreateLoad(
          builder.getInt64Ty(),
          builder.CreateStructGEP(listValueType(), list.value, 0), "list.length");
      auto* nonempty = builder.CreateICmpNE(length, builder.getInt64(0));
      auto* safeIndex = builder.CreateSelect(
          nonempty, builder.CreateSub(length, builder.getInt64(1)),
          builder.getInt64(0));
      index = builder.CreateUIToFP(safeIndex, builder.getDoubleTy(), "last.index");
    }
    return emitListLookup(list.value, index, expected);
  }

  struct ListAllocation {
    llvm::Value* list;
    llvm::Value* data;
    llvm::Value* presence;
    Type elementType;
  };

  ListAllocation allocateListStorage(llvm::Value* length,
                                    const std::string& name,
                                    Type elementType = Type::Dynamic) {
    auto* listStorage = listValueType();
    auto* list = allocate(llvm::ConstantExpr::getSizeOf(listStorage), name);
    builder.CreateStore(length, builder.CreateStructGEP(listStorage, list, 0));
    Type storageType = elementType == Type::Table ? Type::Record : elementType;
    llvm::Type* elementStorage = taggedValueType();
    if (storageType == Type::Number) elementStorage = builder.getDoubleTy();
    else if (storageType == Type::Boolean) elementStorage = builder.getInt8Ty();
    else if (storageType == Type::Text || storageType == Type::List ||
             storageType == Type::Record)
      elementStorage = llvmType(Type::Text);
    else
      storageType = Type::Dynamic;
    auto* bytes = builder.CreateMul(length,
                                    llvm::ConstantExpr::getSizeOf(elementStorage));
    auto* data = allocate(bytes, name + ".elements");
    builder.CreateStore(data, builder.CreateStructGEP(listStorage, list, 1));
    builder.CreateStore(builder.getInt32(storageType == Type::Dynamic
                                             ? 0 : tagFor(storageType)),
                        builder.CreateStructGEP(listStorage, list, 2));
    llvm::Value* presence = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(llvmType(Type::Text)));
    if (storageType == Type::Number)
      presence = allocate(length, name + ".presence");
    builder.CreateStore(
        presence,
        builder.CreateStructGEP(listStorage, list, 3));
    return {list, data, presence, storageType};
  }

  void storeListElement(const ListAllocation& storage, llvm::Value* index,
                        const TypedValue& value) {
    if (storage.elementType == Type::Number) {
      auto* slot = builder.CreateGEP(builder.getDoubleTy(), storage.data, index,
                                     "number.output");
      builder.CreateStore(rawPayload(value), slot);
      auto* present = builder.CreateGEP(builder.getInt8Ty(), storage.presence,
                                        index, "number.present");
      builder.CreateStore(builder.CreateZExt(presentFlag(value), builder.getInt8Ty()),
                          present);
      return;
    }
    if (storage.elementType == Type::Boolean) {
      auto* slot = builder.CreateGEP(builder.getInt8Ty(), storage.data, index,
                                     "boolean.output");
      auto* encoded = builder.CreateAdd(
          builder.CreateZExt(rawPayload(value), builder.getInt8Ty()),
          builder.getInt8(1));
      builder.CreateStore(builder.CreateSelect(presentFlag(value), encoded,
                                               builder.getInt8(0)), slot);
      return;
    }
    if (storage.elementType == Type::Text || storage.elementType == Type::List ||
        storage.elementType == Type::Record) {
      auto* slot = builder.CreateGEP(llvmType(Type::Text), storage.data, index,
                                     "pointer.output");
      builder.CreateStore(rawPayload(value), slot);
      return;
    }
    auto* slot = builder.CreateGEP(taggedValueType(), storage.data, index,
                                   "element.output");
    auto parts = dynamicParts(value);
    builder.CreateStore(parts[0], builder.CreateStructGEP(taggedValueType(), slot, 0));
    builder.CreateStore(parts[1], builder.CreateStructGEP(taggedValueType(), slot, 1));
    builder.CreateStore(parts[2], builder.CreateStructGEP(taggedValueType(), slot, 2));
  }

  TypedValue loadListElement(llvm::Value* list, llvm::Value* index,
                             Type expected) {
    auto* storage = builder.CreateAlloca(taggedValueType(), nullptr, "element.value");
    auto lookupFn = external("lan_list_lookup_at", builder.getInt32Ty(),
                             {llvmType(Type::List), builder.getInt64Ty(),
                              llvm::PointerType::get(context, 0)});
    auto* found = builder.CreateICmpNE(
        builder.CreateCall(lookupFn, {list, index, storage}), builder.getInt32(0));
    auto* dynamic = builder.CreateLoad(taggedValueType(), storage);
    return decodeLookupValue(found, dynamic, expected);
  }

  TypedValue emitElementBoundary(const Expr& expression) {
    Type type = inference->expressionType(expression);
    Presence presence = inference->expressionPresence(expression);
    auto* function = builder.GetInsertBlock()->getParent();
    auto* missingBlock = llvm::BasicBlock::Create(context, "element.missing", function);
    auto* previousTarget = currentMissingTarget;
    currentMissingTarget = missingBlock;
    auto value = emitExpr(expression);
    currentMissingTarget = previousTarget;
    if (missingBlock->use_empty()) {
      missingBlock->eraseFromParent();
      return value;
    }

    auto* valueBlock = builder.GetInsertBlock();
    auto* mergeBlock = llvm::BasicBlock::Create(context, "element.result", function);
    auto presentValue = convertPresence(value, presence);
    builder.CreateBr(mergeBlock);
    builder.SetInsertPoint(missingBlock);
    auto* absentValue = missingValue(type);
    builder.CreateBr(mergeBlock);
    builder.SetInsertPoint(mergeBlock);
    auto* phi = builder.CreatePHI(encodedType(type, presence), 2, "element.expression");
    phi->addIncoming(presentValue.value, valueBlock);
    phi->addIncoming(absentValue, missingBlock);
    return {phi, type, presence};
  }

  TypedValue tableRowElement(llvm::Value* table, llvm::Value* index) {
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "row.status");
    auto rowFn = external("lan_table_row", llvmType(Type::Record),
                          {llvmType(Type::Table), builder.getInt64Ty(),
                           llvm::PointerType::get(context, 0)});
    auto* row = builder.CreateCall(rowFn, {table, index, status}, "table.row");
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "无法读取 Table 行");
    return {row, Type::Record, kPresent};
  }

  const LazyTableRow* lazyTableRow(const std::string& name) const {
    for (std::size_t scopeIndex = scopes.size(); scopeIndex-- > 0;) {
      if (scopes[scopeIndex].count(name)) return nullptr;
      for (auto row = lazyTableRows.rbegin(); row != lazyTableRows.rend(); ++row) {
        if (row->scopeIndex != scopeIndex) continue;
        if (name == "$lan_current_element" ||
            (!row->binding.empty() && name == row->binding))
          return &*row;
      }
    }
    return nullptr;
  }

  TypedValue emitTableCell(const LazyTableRow& row, const FieldExpr& field,
                           Type expected) {
    auto* key = fieldKey(field.name, field.key.get());
    auto* storage = builder.CreateAlloca(taggedValueType(), nullptr, "table.cell");
    builder.CreateStore(missingValue(Type::Dynamic), storage);
    auto cellFn = external("lan_table_cell", builder.getInt32Ty(),
                           {llvmType(Type::Table), llvmType(Type::Text),
                            builder.getInt64Ty(),
                            llvm::PointerType::get(context, 0)});
    auto* found = builder.CreateICmpNE(
        builder.CreateCall(cellFn, {row.table, key, row.index, storage}),
        builder.getInt32(0), "table.cell.found");
    auto* dynamic = builder.CreateLoad(taggedValueType(), storage,
                                       "table.cell.value");
    return decodeLookupValue(found, dynamic, expected);
  }

  TypedValue emitCollectionElement(const CollectionExpr& expression,
                                   const TypedValue& receiver,
                                   llvm::Value* index) {
    if (receiver.type == Type::Table) return tableRowElement(receiver.value, index);
    return loadListElement(receiver.value, index,
                           inference->collectionElementType(expression));
  }

  TypedValue emitElementExpression(const CollectionExpr& expression,
                                   const Expr& body,
                                   const TypedValue& receiver,
                                   llvm::Value* index,
                                   const TypedValue* element = nullptr) {
    scopes.emplace_back();
    if (receiver.type == Type::Table) {
      lazyTableRows.push_back(
          {scopes.size() - 1, receiver.value, index, expression.binding});
    } else {
      scopes.back().emplace("$lan_current_element", *element);
      if (!expression.binding.empty())
        scopes.back().emplace(expression.binding, *element);
    }
    auto value = emitElementBoundary(body);
    if (receiver.type == Type::Table) lazyTableRows.pop_back();
    scopes.pop_back();
    return value;
  }

  TypedValue emitElementBody(const CollectionExpr& expression,
                             const TypedValue& receiver, llvm::Value* index,
                             const TypedValue* element = nullptr) {
    return emitElementExpression(expression, *expression.body, receiver, index,
                                 element);
  }

  llvm::Value* collectionLength(const TypedValue& receiver) {
    if (receiver.type == Type::Table) {
      auto rowCount = external("lan_table_row_count", builder.getInt64Ty(),
                               {llvmType(Type::Table)});
      return builder.CreateCall(rowCount, {receiver.value}, "collection.length");
    }
    return builder.CreateLoad(
        builder.getInt64Ty(),
        builder.CreateStructGEP(listValueType(), receiver.value, 0),
        "collection.length");
  }

  TypedValue emitMappedCollection(const CollectionExpr& expression,
                                  const TypedValue& receiver) {
    auto* length = collectionLength(receiver);
    Type mappedType = expression.operation == CollectionOperation::Filter
        ? Type::Boolean : inference->expressionType(*expression.body);
    auto mappedStorage = allocateListStorage(length, "collection.map", mappedType);
    auto* mapped = mappedStorage.list;
    auto* function = builder.GetInsertBlock()->getParent();
    auto* start = builder.GetInsertBlock();
    auto* check = llvm::BasicBlock::Create(context, "collection.check", function);
    auto* body = llvm::BasicBlock::Create(context, "collection.body", function);
    auto* done = llvm::BasicBlock::Create(context, "collection.done", function);
    auto* indexAddress = builder.CreateAlloca(builder.getInt64Ty(), nullptr,
                                              "collection.index");
    builder.CreateStore(builder.getInt64(0), indexAddress);
    builder.CreateBr(check);

    builder.SetInsertPoint(check);
    auto* index = builder.CreateLoad(builder.getInt64Ty(), indexAddress, "index");
    builder.CreateCondBr(builder.CreateICmpULT(index, length), body, done);

    builder.SetInsertPoint(body);
    TypedValue element{nullptr, Type::Unknown, kNoPresence};
    if (receiver.type != Type::Table)
      element = emitCollectionElement(expression, receiver, index);
    auto mappedValue = emitElementBody(
        expression, receiver, index,
        receiver.type == Type::Table ? nullptr : &element);
    if (expression.operation == CollectionOperation::Add ||
        expression.operation == CollectionOperation::Replace) {
      if (receiver.type == Type::Table)
        element = tableRowElement(receiver.value, index);
      auto changes = requirePresent(mappedValue, "add/replace 的字段 Record");
      auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                          "record.merge.status");
      auto mergeFn = external("lan_record_merge_checked", llvmType(Type::Record),
                              {llvmType(Type::Record), llvmType(Type::Record),
                               builder.getInt32Ty(),
                               llvm::PointerType::get(context, 0)});
      auto* merged = builder.CreateCall(
          mergeFn,
          {element.value, changes.value,
           builder.getInt32(expression.operation == CollectionOperation::Replace),
           status},
          "row.updated");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            expression.operation == CollectionOperation::Add
                                ? "add 不能添加已经存在的字段"
                                : "replace 只能替换已有字段");
      mappedValue = {merged, Type::Record, kPresent};
    }
    storeListElement(mappedStorage, index, mappedValue);
    auto* next = builder.CreateAdd(index, builder.getInt64(1), "next.index");
    builder.CreateStore(next, indexAddress);
    builder.CreateBr(check);
    builder.SetInsertPoint(done);
    (void)start;

    if (expression.operation == CollectionOperation::Map)
      return {mapped, Type::List, kPresent};
    if (expression.operation == CollectionOperation::Filter) {
      auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                          "filter.status");
      llvm::FunctionCallee filterFn;
      if (receiver.type == Type::Table)
        filterFn = external("lan_table_where", llvmType(Type::Table),
                            {llvmType(Type::Table), llvmType(Type::List),
                             llvm::PointerType::get(context, 0)});
      else
        filterFn = external("lan_list_filter", llvmType(Type::List),
                            {llvmType(Type::List), llvmType(Type::List),
                             llvm::PointerType::get(context, 0)});
      auto* filtered = builder.CreateCall(filterFn,
          {receiver.value, mapped, status}, "collection.filtered");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "filter 条件必须产生 Boolean 或 missing");
      return {filtered, receiver.type, kPresent};
    }

    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "rows.to.table.status");
    auto tableFn = external("lan_table_from_rows", llvmType(Type::Table),
                            {llvmType(Type::List),
                             llvm::PointerType::get(context, 0)});
    auto* table = builder.CreateCall(tableFn, {mapped, status}, "rows.to.table");
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "Table 的所有行必须具有相同字段和列类型");
    return {table, Type::Table, kPresent};
  }

  TypedValue emitShortCircuitCollection(const CollectionExpr& expression,
                                        const TypedValue& receiver) {
    auto* length = collectionLength(receiver);
    auto* function = builder.GetInsertBlock()->getParent();
    auto* check = llvm::BasicBlock::Create(context, "search.check", function);
    auto* body = llvm::BasicBlock::Create(context, "search.body", function);
    auto* keepGoing = llvm::BasicBlock::Create(context, "search.next", function);
    auto* matched = llvm::BasicBlock::Create(context, "search.matched", function);
    auto* done = llvm::BasicBlock::Create(context, "search.done", function);
    auto* indexAddress = builder.CreateAlloca(builder.getInt64Ty(), nullptr,
                                              "search.index");
    builder.CreateStore(builder.getInt64(0), indexAddress);

    bool isFind = expression.operation == CollectionOperation::Find;
    bool isAny = expression.operation == CollectionOperation::Any;
    Type resultType = isFind ? inference->expressionType(expression) : Type::Boolean;
    Presence resultPresence = inference->expressionPresence(expression);
    auto* resultAddress = builder.CreateAlloca(
        encodedType(resultType, resultPresence), nullptr, "search.result");
    if (isFind)
      builder.CreateStore(missingValue(resultType), resultAddress);
    else
      builder.CreateStore(
          encodePresent(builder.getInt1(!isAny), Type::Boolean, resultPresence),
          resultAddress);
    builder.CreateBr(check);

    builder.SetInsertPoint(check);
    auto* index = builder.CreateLoad(builder.getInt64Ty(), indexAddress, "index");
    builder.CreateCondBr(builder.CreateICmpULT(index, length), body, done);

    builder.SetInsertPoint(body);
    TypedValue element{nullptr, Type::Unknown, kNoPresence};
    if (receiver.type != Type::Table)
      element = emitCollectionElement(expression, receiver, index);
    auto predicate = emitElementBody(
        expression, receiver, index,
        receiver.type == Type::Table ? nullptr : &element);
    auto* predicatePresent = presentFlag(predicate);
    auto* predicateTrue = builder.CreateAnd(predicatePresent, rawPayload(predicate));
    llvm::Value* shouldStop = nullptr;
    if (isFind || isAny) shouldStop = predicateTrue;
    else shouldStop = builder.CreateAnd(predicatePresent,
                                        builder.CreateNot(rawPayload(predicate)));

    if (isFind && receiver.type != Type::Table)
      shouldStop = builder.CreateAnd(shouldStop, presentFlag(element));
    if (!isFind) {
      auto* recordMissing = llvm::BasicBlock::Create(context, "search.missing", function);
      auto* decide = llvm::BasicBlock::Create(context, "search.decide", function);
      builder.CreateCondBr(predicatePresent, decide, recordMissing);
      builder.SetInsertPoint(recordMissing);
      builder.CreateStore(missingValue(Type::Boolean, resultPresence), resultAddress);
      builder.CreateBr(keepGoing);
      builder.SetInsertPoint(decide);
    }
    builder.CreateCondBr(shouldStop, matched, keepGoing);

    builder.SetInsertPoint(matched);
    if (isFind) {
      if (receiver.type == Type::Table)
        element = tableRowElement(receiver.value, index);
      auto found = convertPresence(element, resultPresence);
      builder.CreateStore(found.value, resultAddress);
    } else {
      builder.CreateStore(
          encodePresent(builder.getInt1(isAny), Type::Boolean, resultPresence),
          resultAddress);
    }
    builder.CreateBr(done);

    builder.SetInsertPoint(keepGoing);
    auto* next = builder.CreateAdd(index, builder.getInt64(1), "next.index");
    builder.CreateStore(next, indexAddress);
    builder.CreateBr(check);

    builder.SetInsertPoint(done);
    return {builder.CreateLoad(encodedType(resultType, resultPresence), resultAddress),
            resultType, resultPresence};
  }

  TypedValue emitCollection(const CollectionExpr& expression) {
    auto receiver = requirePresent(emitExpr(*expression.collection),
                                   "集合管道的输入");
    if (receiver.type == Type::Table &&
        expression.operation == CollectionOperation::Map) {
      if (auto* field = dynamic_cast<const FieldExpr*>(expression.body.get())) {
        auto* base = dynamic_cast<const VariableExpr*>(field->record.get());
        bool current = base &&
            (base->name == "$lan_current_element" ||
             (!expression.binding.empty() && base->name == expression.binding));
        if (current && !field->key &&
            inference->tableHasFixedField(expression, field->name)) {
          auto column = emitRecordLookup(
              receiver.value, builder.CreateGlobalString(field->name), Type::List);
          return requirePresent(column, "Table 列“" + field->name + "”");
        }
      }
    }
    if (expression.operation == CollectionOperation::Find ||
        expression.operation == CollectionOperation::Any ||
        expression.operation == CollectionOperation::All)
      return emitShortCircuitCollection(expression, receiver);
    return emitMappedCollection(expression, receiver);
  }

  void emitTableError(const std::string& message) {
    auto putsFn = external("puts", builder.getInt32Ty(), {llvmType(Type::Text)});
    auto exitFn = external("exit", builder.getVoidTy(), {builder.getInt32Ty()});
    builder.CreateCall(putsFn, {builder.CreateGlobalString("Lan 运行错误：" + message)});
    builder.CreateCall(exitFn, {builder.getInt32(1)});
    builder.CreateUnreachable();
  }

  bool projectionField(const CollectionExpr& map, const Expr& expression,
                       std::string& name) {
    auto* field = dynamic_cast<const FieldExpr*>(&expression);
    if (!field || field->key) return false;
    auto* base = dynamic_cast<const VariableExpr*>(field->record.get());
    if (!base) return false;
    bool current = base->name == "$lan_current_element" ||
        (!map.binding.empty() && base->name == map.binding);
    if (!current || !inference->tableHasFixedField(map, field->name)) return false;
    name = field->name;
    return true;
  }

  bool canFuseTableProjection(const CollectionExpr& map) {
    if (map.operation != CollectionOperation::Map ||
        inference->expressionType(*map.collection) != Type::Table)
      return false;
    auto* record = dynamic_cast<const RecordExpr*>(map.body.get());
    if (!record) return false;
    for (const auto& field : record->fields) {
      std::string input;
      if (field.key || !projectionField(map, *field.value, input)) return false;
    }
    return true;
  }

  bool canFuseColumnExpression(const CollectionExpr& map,
                               const Expr& expression) {
    if (inference->expressionType(expression) == Type::Dynamic) return false;
    if (dynamic_cast<const NumberExpr*>(&expression) ||
        dynamic_cast<const BooleanExpr*>(&expression) ||
        dynamic_cast<const TextExpr*>(&expression) ||
        dynamic_cast<const MissingExpr*>(&expression))
      return true;
    if (auto* field = dynamic_cast<const FieldExpr*>(&expression)) {
      std::string ignored;
      return projectionField(map, expression, ignored);
    }
    if (auto* unary = dynamic_cast<const UnaryExpr*>(&expression))
      return canFuseColumnExpression(map, *unary->operand);
    if (auto* binary = dynamic_cast<const BinaryExpr*>(&expression))
      return canFuseColumnExpression(map, *binary->lhs) &&
             canFuseColumnExpression(map, *binary->rhs);
    return false;
  }

  bool canFuseComputedTable(const CollectionExpr& map) {
    if (map.operation != CollectionOperation::Map ||
        inference->expressionType(*map.collection) != Type::Table)
      return false;
    auto* record = dynamic_cast<const RecordExpr*>(map.body.get());
    if (!record) return false;
    return std::all_of(record->fields.begin(), record->fields.end(),
                       [&](const RecordField& field) {
      return !field.key && canFuseColumnExpression(map, *field.value);
    });
  }

  TypedValue emitFusedTableProjection(const CollectionExpr& map) {
    auto source = requirePresent(emitExpr(*map.collection),
                                 "Table 投影的输入");
    auto* record = static_cast<const RecordExpr*>(map.body.get());
    auto emptyFn = external("lan_record_empty", llvmType(Type::Record), {});
    auto assocFn = external("lan_record_assoc", llvmType(Type::Record),
                            {llvmType(Type::Record), llvmType(Type::Text),
                             builder.getInt32Ty(), builder.getDoubleTy(),
                             llvmType(Type::Text)});
    auto releaseFn = external("lan_record_release", builder.getVoidTy(),
                              {llvmType(Type::Record)});
    llvm::Value* result = builder.CreateCall(emptyFn, {}, "projection.empty");
    for (const auto& output : record->fields) {
      std::string input;
      projectionField(map, *output.value, input);
      auto column = requirePresent(
          emitRecordLookup(source.value, builder.CreateGlobalString(input), Type::List),
          "Table 列“" + input + "”");
      auto* updated = builder.CreateCall(
          assocFn,
          {result, builder.CreateGlobalString(output.name),
           builder.getInt32(tagFor(Type::List)),
           llvm::ConstantFP::get(builder.getDoubleTy(), 0.0), column.value},
          "projection.column");
      builder.CreateCall(releaseFn, {result});
      result = updated;
    }
    return {result, Type::Table, kPresent};
  }

  TypedValue emitFusedComputedTable(const CollectionExpr& map) {
    auto source = requirePresent(emitExpr(*map.collection),
                                 "Table 计算投影的输入");
    auto* record = static_cast<const RecordExpr*>(map.body.get());
    auto* length = collectionLength(source);
    std::vector<ListAllocation> columns;
    columns.reserve(record->fields.size());
    for (std::size_t field = 0; field < record->fields.size(); ++field) {
      columns.push_back(allocateListStorage(
          length, "projection.computed",
          inference->expressionType(*record->fields[field].value)));
    }

    auto* function = builder.GetInsertBlock()->getParent();
    auto* check = llvm::BasicBlock::Create(context, "projection.check", function);
    auto* body = llvm::BasicBlock::Create(context, "projection.body", function);
    auto* done = llvm::BasicBlock::Create(context, "projection.done", function);
    auto* indexAddress = builder.CreateAlloca(builder.getInt64Ty(), nullptr,
                                              "projection.index");
    builder.CreateStore(builder.getInt64(0), indexAddress);
    builder.CreateBr(check);
    builder.SetInsertPoint(check);
    auto* index = builder.CreateLoad(builder.getInt64Ty(), indexAddress,
                                     "projection.row");
    builder.CreateCondBr(builder.CreateICmpULT(index, length), body, done);

    builder.SetInsertPoint(body);
    for (std::size_t field = 0; field < record->fields.size(); ++field) {
      auto value = emitElementExpression(map, *record->fields[field].value,
                                         source, index);
      storeListElement(columns[field], index, value);
    }
    auto* next = builder.CreateAdd(index, builder.getInt64(1),
                                   "projection.next");
    builder.CreateStore(next, indexAddress);
    builder.CreateBr(check);
    builder.SetInsertPoint(done);

    auto emptyFn = external("lan_record_empty", llvmType(Type::Record), {});
    auto assocFn = external("lan_record_assoc", llvmType(Type::Record),
                            {llvmType(Type::Record), llvmType(Type::Text),
                             builder.getInt32Ty(), builder.getDoubleTy(),
                             llvmType(Type::Text)});
    auto releaseFn = external("lan_record_release", builder.getVoidTy(),
                              {llvmType(Type::Record)});
    llvm::Value* result = builder.CreateCall(emptyFn, {}, "projection.result");
    for (std::size_t field = 0; field < record->fields.size(); ++field) {
      auto* updated = builder.CreateCall(
          assocFn,
          {result, builder.CreateGlobalString(record->fields[field].name),
           builder.getInt32(tagFor(Type::List)),
           llvm::ConstantFP::get(builder.getDoubleTy(), 0.0), columns[field].list},
          "projection.result.column");
      builder.CreateCall(releaseFn, {result});
      result = updated;
    }
    return {result, Type::Table, kPresent};
  }

  TypedValue emitTableValidation(const CallExpr& call) {
    if (auto* map = dynamic_cast<const CollectionExpr*>(call.args[0].get());
        map && canFuseTableProjection(*map))
      return emitFusedTableProjection(*map);
    if (auto* map = dynamic_cast<const CollectionExpr*>(call.args[0].get());
        map && canFuseComputedTable(*map))
      return emitFusedComputedTable(*map);
    auto input = requirePresent(emitExpr(*call.args[0]), "table 的输入");
    if (input.type == Type::List) {
      auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                          "table.rows.status");
      auto convertFn = external("lan_table_from_rows", llvmType(Type::Table),
                                {llvmType(Type::List),
                                 llvm::PointerType::get(context, 0)});
      auto* table = builder.CreateCall(convertFn, {input.value, status},
                                       "table.from.rows");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "table 的所有行必须具有相同字段和列类型");
      return {table, Type::Table, kPresent};
    }
    auto record = input;
    auto* rows = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "table.rows");
    auto validateFn = external("lan_table_validate", builder.getInt32Ty(),
                               {llvmType(Type::Record),
                                llvm::PointerType::get(context, 0)});
    auto* valid = builder.CreateICmpNE(
        builder.CreateCall(validateFn, {record.value, rows}), builder.getInt32(0),
        "table.valid");
    auto* function = builder.GetInsertBlock()->getParent();
    auto* validBlock = llvm::BasicBlock::Create(context, "table.validated", function);
    auto* errorBlock = llvm::BasicBlock::Create(context, "table.invalid", function);
    builder.CreateCondBr(valid, validBlock, errorBlock);
    builder.SetInsertPoint(errorBlock);
    emitTableError("table 的所有字段必须是长度相同的列表");
    builder.SetInsertPoint(validBlock);
    return {record.value, Type::Table, kPresent};
  }

  TypedValue emitTableRowCount(const CallExpr& call) {
    auto table = requirePresent(emitExpr(*call.args[0]), "row_count 的输入");
    auto countFn = external("lan_table_row_count", builder.getInt64Ty(),
                            {llvmType(Type::Table)});
    auto* rows = builder.CreateCall(countFn, {table.value}, "table.row.count");
    return {builder.CreateUIToFP(rows, builder.getDoubleTy(), "table.row.number"),
            Type::Number, kPresent};
  }

  TypedValue emitTableAggregate(const CallExpr& call) {
    auto table = requirePresent(emitExpr(*call.args[0]),
                                "函数“" + call.callee + "”的 Table");
    auto key = requirePresent(emitExpr(*call.args[1]),
                              "函数“" + call.callee + "”的列名");
    int operation = call.callee == "sum" ? 0 : call.callee == "average" ? 1
                  : call.callee == "min" ? 2 : call.callee == "max" ? 3 : 4;
    auto* output = builder.CreateAlloca(builder.getDoubleTy(), nullptr,
                                        "table.aggregate.value");
    builder.CreateStore(llvm::ConstantFP::get(builder.getDoubleTy(), 0.0), output);
    auto aggregateFn = external("lan_table_aggregate", builder.getInt32Ty(),
                                {llvmType(Type::Table), llvmType(Type::Text),
                                 builder.getInt32Ty(),
                                 llvm::PointerType::get(context, 0)});
    auto* status = builder.CreateCall(
        aggregateFn, {table.value, key.value, builder.getInt32(operation), output},
        "table.aggregate.status");
    auto* valid = builder.CreateICmpSGE(status, builder.getInt32(0));
    auto* function = builder.GetInsertBlock()->getParent();
    auto* validBlock = llvm::BasicBlock::Create(context, "aggregate.valid", function);
    auto* errorBlock = llvm::BasicBlock::Create(context, "aggregate.invalid", function);
    builder.CreateCondBr(valid, validBlock, errorBlock);
    builder.SetInsertPoint(errorBlock);
    emitTableError("函数“" + call.callee + "”找不到该列，或列的类型不符合要求");
    builder.SetInsertPoint(validBlock);
    auto* value = builder.CreateLoad(builder.getDoubleTy(), output,
                                     "table.aggregate.result");
    if (call.callee == "sum" || call.callee == "count")
      return {value, Type::Number, kPresent};
    llvm::Value* option = llvm::UndefValue::get(maybeNumberType());
    option = builder.CreateInsertValue(
        option, builder.CreateICmpEQ(status, builder.getInt32(1)), 0);
    option = builder.CreateInsertValue(option, value, 1);
    return {option, Type::Number, kMaybeMissing};
  }

  TypedValue emitListAggregate(const CallExpr& call) {
    auto list = requirePresent(emitExpr(*call.args[0]),
                               "函数“" + call.callee + "”的 List");
    int operation = call.callee == "sum" ? 0 : call.callee == "average" ? 1
                  : call.callee == "min" ? 2 : call.callee == "max" ? 3 : 4;
    auto* output = builder.CreateAlloca(builder.getDoubleTy(), nullptr,
                                        "list.aggregate.value");
    builder.CreateStore(llvm::ConstantFP::get(builder.getDoubleTy(), 0.0), output);
    auto aggregateFn = external("lan_list_aggregate", builder.getInt32Ty(),
        {llvmType(Type::List), builder.getInt32Ty(),
         llvm::PointerType::get(context, 0)});
    auto* status = builder.CreateCall(
        aggregateFn, {list.value, builder.getInt32(operation), output},
        "list.aggregate.status");
    auto* valid = builder.CreateICmpSGE(status, builder.getInt32(0));
    auto* function = builder.GetInsertBlock()->getParent();
    auto* validBlock = llvm::BasicBlock::Create(context, "aggregate.valid", function);
    auto* errorBlock = llvm::BasicBlock::Create(context, "aggregate.invalid", function);
    builder.CreateCondBr(valid, validBlock, errorBlock);
    builder.SetInsertPoint(errorBlock);
    emitTableError("函数“" + call.callee + "”需要数字列表");
    builder.SetInsertPoint(validBlock);
    auto* value = builder.CreateLoad(builder.getDoubleTy(), output,
                                     "list.aggregate.result");
    if (call.callee == "sum" || call.callee == "count")
      return {value, Type::Number, kPresent};
    llvm::Value* option = llvm::UndefValue::get(maybeNumberType());
    option = builder.CreateInsertValue(
        option, builder.CreateICmpEQ(status, builder.getInt32(1)), 0);
    option = builder.CreateInsertValue(option, value, 1);
    return {option, Type::Number, kMaybeMissing};
  }

  TypedValue emitSame(const CallExpr& call) {
    auto left = emitExpr(*call.args[0]);
    auto right = emitExpr(*call.args[1]);
    auto leftParts = dynamicParts(left);
    auto rightParts = dynamicParts(right);
    auto sameFn = external("lan_value_same", builder.getInt32Ty(),
        {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text),
         builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
    auto* equal = builder.CreateCall(
        sameFn,
        {leftParts[0], leftParts[1], leftParts[2],
         rightParts[0], rightParts[1], rightParts[2]},
        "structurally.same");
    return {builder.CreateICmpNE(equal, builder.getInt32(0), "same.result"),
            Type::Boolean, kPresent};
  }

  void requireRuntimeSuccess(llvm::Value* status, const std::string& message) {
    auto* function = builder.GetInsertBlock()->getParent();
    auto* validBlock = llvm::BasicBlock::Create(context, "operation.valid", function);
    auto* errorBlock = llvm::BasicBlock::Create(context, "operation.invalid", function);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                         validBlock, errorBlock);
    builder.SetInsertPoint(errorBlock);
    emitTableError(message);
    builder.SetInsertPoint(validBlock);
  }

  TypedValue emitListBinary(const BinaryExpr& binary) {
    Type rightType = inference->expressionType(*binary.rhs);
    auto left = requirePresent(emitExpr(*binary.lhs), "整列运算左侧");
    auto emittedRight = emitExpr(*binary.rhs);
    auto right = binary.op == "??" && rightType != Type::List
        ? emittedRight : requirePresent(emittedRight, "整列运算右侧");
    auto leftParts = dynamicParts(left);
    auto rightParts = dynamicParts(right);
    int operation = binary.op == "+" ? 0 : binary.op == "-" ? 1
                  : binary.op == "*" ? 2 : binary.op == "/" ? 3
                  : binary.op == "==" ? 4 : binary.op == "!=" ? 5
                  : binary.op == "<" ? 6 : binary.op == ">" ? 7
                  : binary.op == "<=" ? 8 : binary.op == ">=" ? 9
                  : binary.op == "and" ? 10 : binary.op == "or" ? 11 : 12;
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "list.operation.status");
    unsigned leftRank = inference->expressionListRank(*binary.lhs);
    unsigned rightRank = inference->expressionListRank(*binary.rhs);
    bool compactNumberPair = leftRank == 1 && rightRank == 1 &&
        inference->expressionListElementType(*binary.lhs) == Type::Number &&
        inference->expressionListElementType(*binary.rhs) == Type::Number &&
        operation != 10 && operation != 11;
    if (compactNumberPair) {
      auto operationFn = external(
          "lan_number_list_binary", llvmType(Type::List),
          {llvmType(Type::List), llvmType(Type::List), builder.getInt32Ty(),
           llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(
          operationFn, {left.value, right.value, builder.getInt32(operation), status},
          "number.list.operation");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "整列数字运算要求 Number 元素");
      return {result, Type::List, kPresent};
    }
    auto operationFn = external(
        "lan_list_binary", llvmType(Type::List),
        {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text),
         builder.getInt32Ty(), builder.getInt32Ty(), builder.getDoubleTy(),
         llvmType(Type::Text), builder.getInt32Ty(),
         builder.getInt32Ty(), llvm::PointerType::get(context, 0)});
    auto* result = builder.CreateCall(
        operationFn,
        {leftParts[0], leftParts[1], leftParts[2],
         builder.getInt32(leftRank),
         rightParts[0], rightParts[1], rightParts[2],
         builder.getInt32(rightRank),
         builder.getInt32(operation), status},
        "list.operation");
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "整列运算要求元素类型正确");
    return {result, Type::List, kPresent};
  }

  TypedValue emitListUnary(const UnaryExpr& unary) {
    auto list = requirePresent(emitExpr(*unary.operand), "一元整列运算");
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "list.unary.status");
    auto operationFn = external("lan_list_unary", llvmType(Type::List),
        {llvmType(Type::List), builder.getInt32Ty(), builder.getInt32Ty(),
         llvm::PointerType::get(context, 0)});
    auto* result = builder.CreateCall(
        operationFn, {list.value,
                      builder.getInt32(inference->expressionListRank(*unary.operand)),
                      builder.getInt32(unary.op == '!' ? 1 : 0), status},
        "list.unary");
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "一元整列运算的元素类型不正确");
    return {result, Type::List, kPresent};
  }

  TypedValue emitFillMissing(const CallExpr& call) {
    auto list = requirePresent(emitExpr(*call.args[0]), "fill_missing 的列表");
    auto replacement = requirePresent(emitExpr(*call.args[1]),
                                      "fill_missing 的替代值");
    auto parts = dynamicParts(replacement);
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "fill.missing.status");
    auto function = external(
        "lan_list_fill_missing", llvmType(Type::List),
        {llvmType(Type::List), builder.getInt32Ty(), builder.getDoubleTy(),
         llvmType(Type::Text), llvm::PointerType::get(context, 0)});
    auto* result = builder.CreateCall(function,
        {list.value, parts[0], parts[1], parts[2], status}, "filled.column");
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "fill_missing 的替代值必须与列元素类型相同");
    return {result, Type::List, kPresent};
  }

  TypedValue emitTableTransform(const CallExpr& call) {
    const std::string displayName = call.callee == "add_column" ? "add"
                                  : call.callee == "replace_column" ? "replace"
                                  : call.callee;
    auto table = requirePresent(emitExpr(*call.args[0]),
                                "操作“" + displayName + "”的 Table");
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "table.operation.status");
    llvm::Value* result = nullptr;
    if (call.callee == "where") {
      auto mask = requirePresent(emitExpr(*call.args[1]), "where 的筛选条件");
      auto function = external("lan_table_where", llvmType(Type::Table),
          {llvmType(Type::Table), llvmType(Type::List),
           llvm::PointerType::get(context, 0)});
      result = builder.CreateCall(function, {table.value, mask.value, status},
                                  "filtered.table");
    } else if (call.callee == "add_column" ||
               call.callee == "replace_column") {
      const std::string operation = call.callee == "add_column" ? "add" : "replace";
      auto name = requirePresent(emitExpr(*call.args[1]), operation + " 的列名");
      auto column = requirePresent(emitExpr(*call.args[2]), operation + " 的新列");
      auto function = external("lan_table_update_column", llvmType(Type::Table),
          {llvmType(Type::Table), llvmType(Type::Text), llvmType(Type::List),
           builder.getInt32Ty(),
           llvm::PointerType::get(context, 0)});
      result = builder.CreateCall(function,
          {table.value, name.value, column.value,
           builder.getInt32(call.callee == "replace_column"), status},
          "table.updated.column");
    } else if (call.callee == "select_columns") {
      auto names = requirePresent(emitExpr(*call.args[1]),
                                  "select_columns 的列名列表");
      auto function = external("lan_table_select_columns", llvmType(Type::Table),
          {llvmType(Type::Table), llvmType(Type::List),
           llvm::PointerType::get(context, 0)});
      result = builder.CreateCall(function, {table.value, names.value, status},
                                  "selected.table");
    } else if (call.callee == "take" || call.callee == "drop") {
      auto amount = requirePresent(emitExpr(*call.args[1]),
                                   "函数“" + call.callee + "”的行数");
      auto function = external("lan_table_slice", llvmType(Type::Table),
          {llvmType(Type::Table), builder.getDoubleTy(), builder.getInt32Ty(),
           llvm::PointerType::get(context, 0)});
      result = builder.CreateCall(function,
          {table.value, amount.value, builder.getInt32(call.callee == "drop"), status},
          "sliced.table");
    } else {
      auto name = requirePresent(emitExpr(*call.args[1]), "sort_by 的列名");
      auto function = external("lan_table_sort_by", llvmType(Type::Table),
          {llvmType(Type::Table), llvmType(Type::Text),
           llvm::PointerType::get(context, 0)});
      result = builder.CreateCall(function, {table.value, name.value, status},
                                  "sorted.table");
    }
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "操作“" + displayName + "”的参数或列不符合要求");
    return {result, Type::Table, kPresent};
  }

  llvm::Value* emitList(const ListExpr& list) {
    Type elementType = inference->listElementType(list);
    auto storage = allocateListStorage(builder.getInt64(list.elements.size()),
                                       "list", elementType);

    llvm::AllocaInst* observedTag = nullptr;
    if (elementType == Type::Dynamic) {
      observedTag = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                         "list.element.type");
      builder.CreateStore(builder.getInt32(0), observedTag);
    }

    for (std::size_t i = 0; i < list.elements.size(); ++i) {
      auto element = emitExpr(*list.elements[i]);
      if (observedTag) {
        auto parts = dynamicParts(element);
        auto* previous = builder.CreateLoad(builder.getInt32Ty(), observedTag,
                                            "list.previous.type");
        auto* isMissing = builder.CreateICmpEQ(parts[0], builder.getInt32(0));
        auto* hasNoType = builder.CreateICmpEQ(previous, builder.getInt32(0));
        auto* sameType = builder.CreateICmpEQ(previous, parts[0]);
        auto* valid = builder.CreateOr(isMissing,
            builder.CreateOr(hasNoType, sameType), "list.same.type");
        auto* function = builder.GetInsertBlock()->getParent();
        auto* validBlock = llvm::BasicBlock::Create(context, "list.type.valid", function);
        auto* errorBlock = llvm::BasicBlock::Create(context, "list.type.error", function);
        builder.CreateCondBr(valid, validBlock, errorBlock);
        builder.SetInsertPoint(errorBlock);
        auto putsFn = external("puts", builder.getInt32Ty(), {llvmType(Type::Text)});
        auto exitFn = external("exit", builder.getVoidTy(), {builder.getInt32Ty()});
        builder.CreateCall(putsFn, {builder.CreateGlobalString(
            "Lan 运行错误：列表中的元素必须属于同一种类型")});
        builder.CreateCall(exitFn, {builder.getInt32(1)});
        builder.CreateUnreachable();
        builder.SetInsertPoint(validBlock);
        auto* discovered = builder.CreateSelect(hasNoType, parts[0], previous);
        builder.CreateStore(discovered, observedTag);
      }
      storeListElement(storage, builder.getInt64(i), element);
    }
    return storage.list;
  }

  void declarePrintHelpers() {
    auto* voidType = builder.getVoidTy();
    auto* pointer = llvmType(Type::Text);
    printValueFunction = llvm::Function::Create(
        llvm::FunctionType::get(voidType,
                                {builder.getInt32Ty(), builder.getDoubleTy(), pointer}, false),
        llvm::Function::InternalLinkage, "lan.print.value", module.get());
    printListFunction = llvm::Function::Create(
        llvm::FunctionType::get(voidType, {pointer}, false),
        llvm::Function::InternalLinkage, "lan.print.list", module.get());
  }

  void emitPrintValueHelper() {
    auto* entry = llvm::BasicBlock::Create(context, "start", printValueFunction);
    auto* numberBlock = llvm::BasicBlock::Create(context, "number", printValueFunction);
    auto* booleanBlock = llvm::BasicBlock::Create(context, "boolean", printValueFunction);
    auto* textBlock = llvm::BasicBlock::Create(context, "text", printValueFunction);
    auto* listBlock = llvm::BasicBlock::Create(context, "list", printValueFunction);
    auto* recordBlock = llvm::BasicBlock::Create(context, "record", printValueFunction);
    auto* pointer = llvmType(Type::Text);
    auto printfFn = external("printf", builder.getInt32Ty(), {pointer}, true);

    auto argument = printValueFunction->arg_begin();
    llvm::Value* tag = &*argument++;
    llvm::Value* number = &*argument++;
    llvm::Value* payload = &*argument;
    builder.SetInsertPoint(entry);
    auto* missingBlock = llvm::BasicBlock::Create(context, "missing", printValueFunction);
    auto* choices = builder.CreateSwitch(tag, missingBlock, 6);
    choices->addCase(builder.getInt32(0), missingBlock);
    choices->addCase(builder.getInt32(1), numberBlock);
    choices->addCase(builder.getInt32(2), booleanBlock);
    choices->addCase(builder.getInt32(3), textBlock);
    choices->addCase(builder.getInt32(4), listBlock);
    choices->addCase(builder.getInt32(5), recordBlock);

    builder.SetInsertPoint(missingBlock);
    builder.CreateCall(printfFn, {builder.CreateGlobalString("missing")});
    builder.CreateRetVoid();

    builder.SetInsertPoint(numberBlock);
    builder.CreateCall(printfFn, {builder.CreateGlobalString("%g"), number});
    builder.CreateRetVoid();

    builder.SetInsertPoint(booleanBlock);
    auto* truth = builder.CreateFCmpONE(number,
                                        llvm::ConstantFP::get(builder.getDoubleTy(), 0.0));
    auto* booleanText = builder.CreateSelect(truth, builder.CreateGlobalString("true"),
                                             builder.CreateGlobalString("false"));
    builder.CreateCall(printfFn, {builder.CreateGlobalString("%s"), booleanText});
    builder.CreateRetVoid();

    builder.SetInsertPoint(textBlock);
    builder.CreateCall(printfFn, {builder.CreateGlobalString("\"%s\""), payload});
    builder.CreateRetVoid();

    builder.SetInsertPoint(listBlock);
    builder.CreateCall(printListFunction, {payload});
    builder.CreateRetVoid();

    builder.SetInsertPoint(recordBlock);
    auto recordPrint = external("lan_record_print", builder.getVoidTy(), {pointer});
    builder.CreateCall(recordPrint, {payload});
    builder.CreateRetVoid();
  }

  void emitPrintListHelper() {
    auto* listStorage = listValueType();
    auto* elementStorage = taggedValueType();
    auto* pointer = llvmType(Type::Text);
    auto printfFn = external("printf", builder.getInt32Ty(), {pointer}, true);
    auto* entry = llvm::BasicBlock::Create(context, "start", printListFunction);
    auto* check = llvm::BasicBlock::Create(context, "check", printListFunction);
    auto* element = llvm::BasicBlock::Create(context, "element", printListFunction);
    auto* separator = llvm::BasicBlock::Create(context, "separator", printListFunction);
    auto* display = llvm::BasicBlock::Create(context, "display", printListFunction);
    auto* done = llvm::BasicBlock::Create(context, "done", printListFunction);

    llvm::Value* list = &*printListFunction->arg_begin();
    builder.SetInsertPoint(entry);
    builder.CreateCall(printfFn, {builder.CreateGlobalString("[")});
    auto* length = builder.CreateLoad(
        builder.getInt64Ty(), builder.CreateStructGEP(listStorage, list, 0), "length");
    builder.CreateBr(check);

    builder.SetInsertPoint(check);
    auto* index = builder.CreatePHI(builder.getInt64Ty(), 2, "index");
    index->addIncoming(builder.getInt64(0), entry);
    auto* hasElement = builder.CreateICmpULT(index, length, "has.element");
    builder.CreateCondBr(hasElement, element, done);

    builder.SetInsertPoint(element);
    auto* isFirst = builder.CreateICmpEQ(index, builder.getInt64(0), "is.first");
    builder.CreateCondBr(isFirst, display, separator);

    builder.SetInsertPoint(separator);
    builder.CreateCall(printfFn, {builder.CreateGlobalString(", ")});
    builder.CreateBr(display);

    builder.SetInsertPoint(display);
    auto* storage = builder.CreateAlloca(elementStorage, nullptr, "value");
    auto lookupFn = external("lan_list_lookup_at", builder.getInt32Ty(),
                             {llvmType(Type::List), builder.getInt64Ty(),
                              llvm::PointerType::get(context, 0)});
    builder.CreateCall(lookupFn, {list, index, storage});
    auto* dynamic = builder.CreateLoad(elementStorage, storage);
    auto* tag = builder.CreateExtractValue(dynamic, 0);
    auto* number = builder.CreateExtractValue(dynamic, 1);
    auto* payload = builder.CreateExtractValue(dynamic, 2);
    builder.CreateCall(printValueFunction, {tag, number, payload});
    auto* next = builder.CreateAdd(index, builder.getInt64(1), "next");
    builder.CreateBr(check);
    index->addIncoming(next, display);

    builder.SetInsertPoint(done);
    builder.CreateCall(printfFn, {builder.CreateGlobalString("]")});
    builder.CreateRetVoid();
  }

  void emitPrintHelpers() {
    emitPrintValueHelper();
    emitPrintListHelper();
  }

  llvm::Value* concatenate(llvm::Value* lhs, llvm::Value* rhs) {
    auto* pointer = llvmType(Type::Text);
    auto strlenFn = external("strlen", builder.getInt64Ty(), {pointer});
    auto mallocFn = external("malloc", pointer, {builder.getInt64Ty()});
    auto strcpyFn = external("strcpy", pointer, {pointer, pointer});
    auto strcatFn = external("strcat", pointer, {pointer, pointer});
    auto* leftLength = builder.CreateCall(strlenFn, {lhs}, "text.left.length");
    auto* rightLength = builder.CreateCall(strlenFn, {rhs}, "text.right.length");
    auto* total = builder.CreateAdd(leftLength, rightLength, "text.length");
    total = builder.CreateAdd(total, builder.getInt64(1), "text.space");
    auto* buffer = builder.CreateCall(mallocFn, {total}, "text.buffer");
    builder.CreateCall(strcpyFn, {buffer, lhs});
    builder.CreateCall(strcatFn, {buffer, rhs});
    return buffer;
  }

  llvm::BasicBlock* missingTarget() {
    if (currentMissingTarget) return currentMissingTarget;
    auto* function = builder.GetInsertBlock()->getParent();
    rootMissingTarget = llvm::BasicBlock::Create(context, "explicit.missing", function);
    currentMissingTarget = rootMissingTarget;
    return currentMissingTarget;
  }

  TypedValue emitPropagate(const PropagateExpr& propagate) {
    auto value = emitExpr(*propagate.operand);
    if (value.presence == kPresent) return value;
    auto* function = builder.GetInsertBlock()->getParent();
    auto* target = missingTarget();
    if (value.presence == kMissing) {
      builder.CreateBr(target);
      builder.SetInsertPoint(
          llvm::BasicBlock::Create(context, "after.explicit.missing", function));
      return {llvm::UndefValue::get(llvmType(value.type)), value.type, kPresent};
    }
    auto* valueBlock = llvm::BasicBlock::Create(context, "explicit.value", function);
    builder.CreateCondBr(presentFlag(value), valueBlock, target);
    builder.SetInsertPoint(valueBlock);
    return {rawPayload(value), value.type, kPresent};
  }

  TypedValue emitIf(const IfExpr& conditional) {
    auto condition = requirePresent(emitExpr(*conditional.condition), "if 条件");
    Type resultType = inference->expressionType(conditional);
    Presence resultPresence = inference->expressionPresence(conditional);
    auto* function = builder.GetInsertBlock()->getParent();
    auto* thenBlock = llvm::BasicBlock::Create(context, "if.yes", function);
    auto* elseBlock = llvm::BasicBlock::Create(context, "if.no", function);
    auto* mergeBlock = llvm::BasicBlock::Create(context, "if.result", function);
    builder.CreateCondBr(rawPayload(condition), thenBlock, elseBlock);

    builder.SetInsertPoint(thenBlock);
    auto thenValue = convertPresence(emitExpr(*conditional.thenValue), resultPresence);
    thenBlock = builder.GetInsertBlock();
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(elseBlock);
    auto elseValue = convertPresence(emitExpr(*conditional.elseValue), resultPresence);
    elseBlock = builder.GetInsertBlock();
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(mergeBlock);
    auto* phi = builder.CreatePHI(encodedType(resultType, resultPresence), 2, "choice");
    phi->addIncoming(thenValue.value, thenBlock);
    phi->addIncoming(elseValue.value, elseBlock);
    return {phi, resultType, resultPresence};
  }

  TypedValue emitLogical(const BinaryExpr& binary) {
    auto lhs = emitExpr(*binary.lhs);
    auto* function = builder.GetInsertBlock()->getParent();
    auto* leftPresentBlock = llvm::BasicBlock::Create(context, "logic.left.value", function);
    auto* rhsBlock = llvm::BasicBlock::Create(context, "logic.next", function);
    auto* missingRhsBlock = llvm::BasicBlock::Create(context, "logic.missing.next", function);
    auto* shortcutBlock = llvm::BasicBlock::Create(context, "logic.shortcut", function);
    auto* mergeBlock = llvm::BasicBlock::Create(context, "logic.result", function);
    const bool isAnd = binary.op == "and";
    Presence resultPresence = inference->expressionPresence(binary);
    auto* resultAddress = builder.CreateAlloca(
        encodedType(Type::Boolean, resultPresence), nullptr, "logic.storage");

    if (lhs.presence == kPresent) builder.CreateBr(leftPresentBlock);
    else if (lhs.presence == kMissing) builder.CreateBr(missingRhsBlock);
    else builder.CreateCondBr(presentFlag(lhs), leftPresentBlock, missingRhsBlock);

    builder.SetInsertPoint(leftPresentBlock);
    if (isAnd) builder.CreateCondBr(rawPayload(lhs), rhsBlock, shortcutBlock);
    else builder.CreateCondBr(rawPayload(lhs), shortcutBlock, rhsBlock);

    builder.SetInsertPoint(rhsBlock);
    auto rhs = convertPresence(emitExpr(*binary.rhs), resultPresence);
    builder.CreateStore(rhs.value, resultAddress);
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(shortcutBlock);
    builder.CreateStore(
        encodePresent(builder.getInt1(!isAnd), Type::Boolean, resultPresence),
        resultAddress);
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(missingRhsBlock);
    auto missingRhs = emitExpr(*binary.rhs);
    auto* rhsPresent = presentFlag(missingRhs);
    auto* rhsDecides = builder.CreateAnd(
        rhsPresent, isAnd ? builder.CreateNot(rawPayload(missingRhs))
                          : rawPayload(missingRhs));
    auto* decidedBlock = llvm::BasicBlock::Create(context, "logic.decided", function);
    auto* remainsMissingBlock = llvm::BasicBlock::Create(context, "logic.missing", function);
    builder.CreateCondBr(rhsDecides, decidedBlock, remainsMissingBlock);
    builder.SetInsertPoint(decidedBlock);
    builder.CreateStore(
        encodePresent(builder.getInt1(!isAnd), Type::Boolean, resultPresence),
        resultAddress);
    builder.CreateBr(mergeBlock);
    builder.SetInsertPoint(remainsMissingBlock);
    builder.CreateStore(missingValue(Type::Boolean, resultPresence), resultAddress);
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(mergeBlock);
    return {builder.CreateLoad(encodedType(Type::Boolean, resultPresence),
                               resultAddress, "logic"),
            Type::Boolean, resultPresence};
  }

  TypedValue emitCoalesce(const BinaryExpr& binary) {
    auto* function = builder.GetInsertBlock()->getParent();
    auto* fallbackBlock = llvm::BasicBlock::Create(context, "default.fallback", function);
    auto* previousTarget = currentMissingTarget;
    currentMissingTarget = fallbackBlock;
    auto lhs = emitExpr(*binary.lhs);
    currentMissingTarget = previousTarget;
    Presence resultPresence = inference->expressionPresence(binary);
    if (lhs.presence == kPresent) {
      if (fallbackBlock->use_empty()) {
        fallbackBlock->eraseFromParent();
        return convertPresence(lhs, resultPresence);
      }
      auto* valueBlock = builder.GetInsertBlock();
      auto value = convertPresence(lhs, resultPresence);
      auto* mergeBlock = llvm::BasicBlock::Create(context, "default.result", function);
      builder.CreateBr(mergeBlock);
      builder.SetInsertPoint(fallbackBlock);
      auto fallback = convertPresence(emitExpr(*binary.rhs), resultPresence);
      fallbackBlock = builder.GetInsertBlock();
      builder.CreateBr(mergeBlock);
      builder.SetInsertPoint(mergeBlock);
      auto* phi = builder.CreatePHI(encodedType(lhs.type, resultPresence), 2, "default");
      phi->addIncoming(value.value, valueBlock);
      phi->addIncoming(fallback.value, fallbackBlock);
      return {phi, lhs.type, resultPresence};
    }
    if (lhs.presence == kMissing) {
      builder.CreateBr(fallbackBlock);
      builder.SetInsertPoint(fallbackBlock);
      return convertPresence(emitExpr(*binary.rhs), resultPresence);
    }
    auto* valueBlock = llvm::BasicBlock::Create(context, "default.value", function);
    auto* mergeBlock = llvm::BasicBlock::Create(context, "default.result", function);
    builder.CreateCondBr(presentFlag(lhs), valueBlock, fallbackBlock);

    builder.SetInsertPoint(valueBlock);
    auto value = convertPresence({rawPayload(lhs), lhs.type, kPresent}, resultPresence);
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(fallbackBlock);
    auto fallback = convertPresence(emitExpr(*binary.rhs), resultPresence);
    fallbackBlock = builder.GetInsertBlock();
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(mergeBlock);
    auto* phi = builder.CreatePHI(encodedType(lhs.type, resultPresence), 2, "default");
    phi->addIncoming(value.value, valueBlock);
    phi->addIncoming(fallback.value, fallbackBlock);
    return {phi, lhs.type, resultPresence};
  }

  TypedValue emitStrictBinary(
      const BinaryExpr& binary, Type resultType,
      const std::function<llvm::Value*(const TypedValue&, const TypedValue&)>& compute) {
    auto lhs = requirePresent(emitExpr(*binary.lhs), "“" + binary.op + "”左侧");
    auto rhs = requirePresent(emitExpr(*binary.rhs), "“" + binary.op + "”右侧");
    return {compute(lhs, rhs), resultType, kPresent};
  }

  std::vector<int> emittedArgumentSlots(const CallExpr& call,
                                        const Function& definition) const {
    std::vector<int> slots(definition.params.size(), -1);
    std::size_t positional = 0;
    for (std::size_t source = 0; source < call.args.size(); ++source) {
      if (call.argNames[source].empty()) {
        slots[positional++] = static_cast<int>(source);
      } else {
        auto found = std::find_if(
            definition.params.begin(), definition.params.end(),
            [&](const Parameter& parameter) {
              return parameter.name == call.argNames[source];
            });
        slots[static_cast<std::size_t>(found - definition.params.begin())] =
            static_cast<int>(source);
      }
    }
    return slots;
  }

  TypedValue emitExpr(const Expr& expr) {
    Type type = inference->expressionType(expr);
    Presence presence = inference->expressionPresence(expr);
    if (auto* number = dynamic_cast<const NumberExpr*>(&expr))
      return {llvm::ConstantFP::get(builder.getDoubleTy(), number->value), type, kPresent};
    if (auto* boolean = dynamic_cast<const BooleanExpr*>(&expr))
      return {builder.getInt1(boolean->value), type, kPresent};
    if (auto* text = dynamic_cast<const TextExpr*>(&expr))
      return {builder.CreateGlobalString(text->value), type, kPresent};
    if (dynamic_cast<const MissingExpr*>(&expr))
      return {missingValue(type), type, kMissing};
    if (auto* list = dynamic_cast<const ListExpr*>(&expr))
      return {emitList(*list), Type::List, kPresent};
    if (auto* record = dynamic_cast<const RecordExpr*>(&expr))
      return {emitRecord(*record), Type::Record, kPresent};
    if (auto* variable = dynamic_cast<const VariableExpr*>(&expr)) {
      for (std::size_t scopeIndex = scopes.size(); scopeIndex-- > 0;) {
        auto found = scopes[scopeIndex].find(variable->name);
        if (found != scopes[scopeIndex].end())
          return convertPresence(found->second, presence);
        for (auto row = lazyTableRows.rbegin(); row != lazyTableRows.rend(); ++row) {
          if (row->scopeIndex != scopeIndex) continue;
          if (variable->name == "$lan_current_element" ||
              (!row->binding.empty() && variable->name == row->binding))
            return convertPresence(tableRowElement(row->table, row->index), presence);
        }
      }
    }
    if (auto* let = dynamic_cast<const LetExpr*>(&expr)) {
      auto value = emitExpr(*let->value);
      scopes.emplace_back();
      scopes.back().emplace(let->name, value);
      auto result = emitExpr(*let->body);
      scopes.pop_back();
      return convertPresence(result, presence);
    }
    if (auto* conditional = dynamic_cast<const IfExpr*>(&expr)) return emitIf(*conditional);
    if (auto* propagate = dynamic_cast<const PropagateExpr*>(&expr))
      return emitPropagate(*propagate);
    if (auto* field = dynamic_cast<const FieldExpr*>(&expr)) {
      if (inference->expressionType(*field->record) == Type::List)
        return emitListIndex(*field, type);
      if (auto* base = dynamic_cast<const VariableExpr*>(field->record.get()))
        if (auto* row = lazyTableRow(base->name))
          return emitTableCell(*row, *field, type);
      return emitField(*field, type);
    }
    if (auto* update = dynamic_cast<const RecordUpdateExpr*>(&expr))
      return emitRecordUpdate(*update);
    if (auto* update = dynamic_cast<const NestedRecordUpdateExpr*>(&expr))
      return emitNestedRecordUpdate(*update);
    if (auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
      if (type == Type::List) return emitListUnary(*unary);
      auto operand = emitExpr(*unary->operand);
      if (unary->op == '!') {
        if (operand.presence == kPresent)
          return {builder.CreateNot(operand.value, "not"), Type::Boolean, kPresent};
        Presence resultPresence = inference->expressionPresence(expr);
        auto* inverted = builder.CreateNot(rawPayload(operand), "not.value");
        auto* present = encodePresent(inverted, Type::Boolean, resultPresence);
        return {builder.CreateSelect(presentFlag(operand), present,
                                     missingValue(Type::Boolean), "not.option"),
                Type::Boolean, resultPresence};
      }
      return guarded({operand}, type, presence, [&]() {
        auto* raw = rawPayload(operand);
        return builder.CreateFNeg(raw, "negative");
      });
    }
    if (auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
      if (inference->isVectorOperation(*binary))
        return emitListBinary(*binary);
      if (binary->op == "??") return emitCoalesce(*binary);
      if (binary->op == "and" || binary->op == "or") return emitLogical(*binary);
      const auto& op = binary->op;
      if (op == "==" || op == "!=") {
        return emitStrictBinary(
            *binary, Type::Boolean,
            [&](const TypedValue& lhs, const TypedValue& rhs) {
          llvm::Value* equal = nullptr;
          auto* left = rawPayload(lhs);
          auto* right = rawPayload(rhs);
          if (lhs.type == Type::Text) {
            auto strcmpFn = external("strcmp", builder.getInt32Ty(),
                                     {llvmType(Type::Text), llvmType(Type::Text)});
            auto* compared = builder.CreateCall(strcmpFn, {left, right}, "text.compare");
            equal = builder.CreateICmpEQ(compared, builder.getInt32(0), "text.equal");
          } else if (lhs.type == Type::Number) {
            equal = builder.CreateFCmpOEQ(left, right, "equal");
          } else {
            equal = builder.CreateICmpEQ(left, right, "equal");
          }
          return op == "==" ? equal : builder.CreateNot(equal, "not.equal");
        });
      }
      Type resultType = (op == "<" || op == ">" || op == "<=" || op == ">=")
          ? Type::Boolean : type;
      return emitStrictBinary(
          *binary, resultType,
          [&](const TypedValue& lhs, const TypedValue& rhs) {
        auto* left = rawPayload(lhs);
        auto* right = rawPayload(rhs);
        if (op == "+" && lhs.type == Type::Text) return concatenate(left, right);
        if (op == "+") return builder.CreateFAdd(left, right, "sum");
        if (op == "-") return builder.CreateFSub(left, right, "difference");
        if (op == "*") return builder.CreateFMul(left, right, "product");
        if (op == "/") return builder.CreateFDiv(left, right, "quotient");
        if (op == "<") return builder.CreateFCmpOLT(left, right, "less");
        if (op == ">") return builder.CreateFCmpOGT(left, right, "greater");
        if (op == "<=") return builder.CreateFCmpOLE(left, right, "at.most");
        return builder.CreateFCmpOGE(left, right, "at.least");
      });
    }
    if (auto* collection = dynamic_cast<const CollectionExpr*>(&expr))
      return emitCollection(*collection);
    if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
      if (call->callee == "is_missing") {
        auto value = emitExpr(*call->args[0]);
        return {builder.CreateNot(presentFlag(value), "is.missing"),
                Type::Boolean, kPresent};
      }
      if (call->callee == "require")
        return requirePresent(emitExpr(*call->args[0]), "require 的输入");
      if (call->callee == "same") return emitSame(*call);
      if (call->callee == "table") return emitTableValidation(*call);
      if (call->callee == "row_count") return emitTableRowCount(*call);
      if (call->callee == "sum" || call->callee == "average" ||
          call->callee == "min" || call->callee == "max" ||
          call->callee == "count")
        return call->args.size() == 1 ? emitListAggregate(*call)
                                      : emitTableAggregate(*call);
      if (call->callee == "fill_missing") return emitFillMissing(*call);
      if (call->callee == "where" || call->callee == "add_column" ||
          call->callee == "replace_column" ||
          call->callee == "select_columns" || call->callee == "take" ||
          call->callee == "drop" || call->callee == "sort_by")
        return emitTableTransform(*call);
      if (call->callee == "length") {
        auto list = emitExpr(*call->args[0]);
        return guarded({list}, Type::Number, presence, [&]() {
          auto* length = builder.CreateLoad(
              builder.getInt64Ty(),
              builder.CreateStructGEP(listValueType(), rawPayload(list), 0), "list.length");
          return builder.CreateUIToFP(length, builder.getDoubleTy(), "length.number");
        });
      }
      if (call->callee == "first") return emitListEndpoint(*call, false, type);
      if (call->callee == "last") return emitListEndpoint(*call, true, type);
      std::vector<llvm::Value*> args;
      const auto& target = *declarations.at(call->callee);
      auto targetPresences = inference->parameterPresences(target);
      std::vector<TypedValue> supplied;
      for (const auto& argument : call->args) supplied.push_back(emitExpr(*argument));
      auto slots = emittedArgumentSlots(*call, target);
      scopes.emplace_back();
      for (std::size_t i = 0; i < target.params.size(); ++i) {
        auto argument = slots[i] >= 0 ? supplied[slots[i]]
            : emitExpr(*target.params[i].defaultValue);
        argument = convertPresence(argument, targetPresences[i]);
        scopes.back().emplace(target.params[i].name, argument);
        args.push_back(argument.value);
      }
      scopes.pop_back();
      auto returnedPresence = inference->resultPresence(target);
      TypedValue returned{builder.CreateCall(module->getFunction(call->callee), args,
                                             call->callee + ".result"),
                          type, returnedPresence};
      return convertPresence(returned, presence);
    }
    throw std::runtime_error("内部错误：无法生成表达式");
  }

  void declareFunction(const Function& function) {
    std::vector<llvm::Type*> params;
    auto types = inference->parameterTypes(function);
    auto presences = inference->parameterPresences(function);
    for (std::size_t i = 0; i < types.size(); ++i)
      params.push_back(encodedType(types[i], presences[i]));
    auto* signature = llvm::FunctionType::get(
        encodedType(inference->resultType(function), inference->resultPresence(function)),
        params, false);
    auto* generated = llvm::Function::Create(signature, llvm::Function::InternalLinkage,
                                              function.name, module.get());
    std::size_t index = 0;
    for (auto& arg : generated->args()) arg.setName(function.params[index++].name);
    declarations.emplace(function.name, &function);
  }

  void emitFunction(const Function& function) {
    scopes.clear();
    scopes.emplace_back();
    auto* generated = module->getFunction(function.name);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "start", generated));
    currentMissingTarget = nullptr;
    rootMissingTarget = nullptr;
    auto types = inference->parameterTypes(function);
    auto presences = inference->parameterPresences(function);
    std::size_t index = 0;
    for (auto& arg : generated->args()) {
      scopes.back().emplace(function.params[index].name,
                            TypedValue{&arg, types[index], presences[index]});
      ++index;
    }
    auto result = emitExpr(*function.body);
    result = convertPresence(result, inference->resultPresence(function));
    builder.CreateRet(result.value);
    if (rootMissingTarget) {
      builder.SetInsertPoint(rootMissingTarget);
      builder.CreateRet(missingValue(inference->resultType(function)));
    }
    currentMissingTarget = nullptr;
    rootMissingTarget = nullptr;
    if (llvm::verifyFunction(*generated, &llvm::errs()))
      throw std::runtime_error("LLVM 无法验证函数“" + function.name + "”");
  }

  void displayPresent(const TypedValue& result, llvm::FunctionCallee printfFn) {
    auto* raw = rawPayload(result);
    if (result.type == Type::Number) {
      builder.CreateCall(printfFn, {builder.CreateGlobalString("%g\n"), raw});
    } else if (result.type == Type::Text) {
      builder.CreateCall(printfFn, {builder.CreateGlobalString("%s\n"), raw});
    } else if (result.type == Type::Boolean) {
      auto* yes = builder.CreateGlobalString("true");
      auto* no = builder.CreateGlobalString("false");
      auto* word = builder.CreateSelect(raw, yes, no, "boolean.text");
      builder.CreateCall(printfFn, {builder.CreateGlobalString("%s\n"), word});
    } else if (result.type == Type::List) {
      builder.CreateCall(printListFunction, {raw});
      builder.CreateCall(printfFn, {builder.CreateGlobalString("\n")});
    } else if (result.type == Type::Record || result.type == Type::Table) {
      auto recordPrint = external("lan_record_print", builder.getVoidTy(),
                                  {llvmType(Type::Record)});
      builder.CreateCall(recordPrint, {raw});
      builder.CreateCall(printfFn, {builder.CreateGlobalString("\n")});
    } else {
      auto dynamicPrint = external("lan_value_print", builder.getVoidTy(),
                                   {builder.getInt32Ty(), builder.getDoubleTy(),
                                    llvmType(Type::Text)});
      builder.CreateCall(dynamicPrint,
                         {builder.CreateExtractValue(result.value, 0),
                          builder.CreateExtractValue(result.value, 1),
                          builder.CreateExtractValue(result.value, 2)});
      builder.CreateCall(printfFn, {builder.CreateGlobalString("\n")});
    }
  }

  void emitProgramResult(const Program& program) {
    auto* signature = llvm::FunctionType::get(builder.getInt32Ty(), {}, false);
    auto* mainFunction = llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                                "main", module.get());
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "start", mainFunction));
    scopes.clear();
    scopes.emplace_back();
    currentMissingTarget = nullptr;
    rootMissingTarget = nullptr;
    auto* pointer = llvmType(Type::Text);
    auto printfFn = external("printf", builder.getInt32Ty(), {pointer}, true);
    auto result = requirePresent(emitExpr(*program.result), "程序最终结果");
    displayPresent(result, printfFn);
    builder.CreateRet(builder.getInt32(0));
    if (rootMissingTarget) {
      builder.SetInsertPoint(rootMissingTarget);
      builder.CreateCall(printfFn, {builder.CreateGlobalString("missing\n")});
      builder.CreateRet(builder.getInt32(0));
    }
    currentMissingTarget = nullptr;
    rootMissingTarget = nullptr;
  }
};

CodeGenerator::CodeGenerator() : impl_(std::make_unique<Impl>()) {}
CodeGenerator::~CodeGenerator() = default;

bool CodeGenerator::generate(const Program& program, std::string& error) {
  try {
    if (!program.result) throw std::runtime_error("程序最后需要有一个要计算或显示的内容");
    impl_->inference = std::make_unique<Inference>(program);
    impl_->loadRecordRuntime();
    impl_->declarePrintHelpers();
    for (const auto& function : program.functions) impl_->declareFunction(*function);
    impl_->emitPrintHelpers();
    for (const auto& function : program.functions) impl_->emitFunction(*function);
    impl_->emitProgramResult(program);
    if (llvm::verifyModule(*impl_->module, &llvm::errs()))
      throw std::runtime_error("LLVM 无法验证生成的程序");
    llvm::LoopAnalysisManager loopAnalyses;
    llvm::FunctionAnalysisManager functionAnalyses;
    llvm::CGSCCAnalysisManager cgsccAnalyses;
    llvm::ModuleAnalysisManager moduleAnalyses;
    llvm::PassBuilder passes;
    passes.registerModuleAnalyses(moduleAnalyses);
    passes.registerCGSCCAnalyses(cgsccAnalyses);
    passes.registerFunctionAnalyses(functionAnalyses);
    passes.registerLoopAnalyses(loopAnalyses);
    passes.crossRegisterProxies(loopAnalyses, functionAnalyses, cgsccAnalyses,
                                moduleAnalyses);
    auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    pipeline.run(*impl_->module, moduleAnalyses);
    if (llvm::verifyModule(*impl_->module, &llvm::errs()))
      throw std::runtime_error("LLVM 优化后的程序未通过验证");
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool CodeGenerator::writeIR(const std::string& path, std::string& error) const {
  std::error_code code;
  llvm::raw_fd_ostream output(path, code, llvm::sys::fs::OF_None);
  if (code) { error = code.message(); return false; }
  impl_->module->print(output, nullptr);
  return true;
}

std::string CodeGenerator::getIR() const {
  std::string result;
  llvm::raw_string_ostream output(result);
  impl_->module->print(output, nullptr);
  return result;
}

}  // namespace lan
