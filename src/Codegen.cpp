#include "lan/Codegen.h"
#include "lan/RuntimeAssets.h"
#include "lan/FreeVariables.h"
#include "lan/OptionCheck.h"
#include "lan/Match.h"

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
#include <optional>
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
    case Type::Regex: return "Regex";
    case Type::Instant: return "Instant";
    case Type::Unit: return "Unit";
    case Type::IO: return "IO";
    case Type::Result: return "Result";
    case Type::Dynamic: return "动态值";
    case Type::Unknown: return "尚未确定的类型";
  }
  return "未知类型";
}

struct TypeNode {
  int parent;
  Type type = Type::Unknown;
  int listElement = -1;
  int ioResult = -1;
  int success = -1;
  int error = -1;
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

bool isRegexBuiltin(const std::string& name) {
  return name == "regex" || name == "regex_contains" ||
      name == "regex_matches" || name == "regex_find" ||
      name == "regex_find_all" || name == "regex_split";
}

bool isIOBuiltin(const std::string& name) {
  return name == "read_stdin" || name == "write_stdout" || name == "arguments" ||
      name == "now" ||
      name == "random" || name == "random_between" ||
      name == "random_integer" || name == "choose" || name == "shuffle";
}

bool isTimeBuiltin(const std::string& name) {
  return name == "parse_time" || name == "format_time" ||
      name == "time_from_unix" || name == "unix_seconds" ||
      name == "add_seconds" || name == "seconds_between";
}

class Inference {
 public:
  explicit Inference(const Program& program) {
    for (const auto& function : program.functions) {
      if (isRegexBuiltin(function->name) || isIOBuiltin(function->name) ||
          isTimeBuiltin(function->name) ||
          function->name == "main" || function->name == "length" ||
          function->name == "table" || function->name == "row_count" ||
          function->name == "sum" || function->name == "average" ||
          function->name == "min" || function->name == "max" ||
          function->name == "count" || function->name == "fill_missing" ||
          function->name == "select_columns" || function->name == "take" ||
          function->name == "drop" || function->name == "sort" ||
          function->name == "distinct" ||
          function->name == "sort_by" ||
          function->name == "same" || function->name == "first" ||
          function->name == "last" || function->name == "is_missing" ||
          function->name == "require" || function->name == "ok" ||
          function->name == "err" || function->name == "trim" ||
          function->name == "lowercase" || function->name == "uppercase" ||
          function->name == "starts_with" || function->name == "ends_with" ||
          function->name == "text_contains" ||
          function->name == "split" || function->name == "join" ||
          function->name == "abs" || function->name == "floor" ||
          function->name == "ceil" || function->name == "round" ||
          function->name == "clamp" || function->name == "sign" ||
          function->name == "sqrt" || function->name == "pow" ||
          function->name == "exp" || function->name == "log" ||
          function->name == "hypot" || function->name == "sin" ||
          function->name == "cos" || function->name == "tan" ||
          function->name == "asin" || function->name == "acos" ||
          function->name == "atan" || function->name == "atan2" ||
          function->name == "radians" || function->name == "degrees" ||
          function->name == "to_text" ||
          function->name == "parse_number" || function->name == "concat" ||
          function->name == "flatten" || function->name == "reverse" ||
          function->name == "fields" ||
          function->name == "has_field" || function->name == "merge" ||
          function->name == "remove_field")
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

    for (const auto* function : inferenceOrder_) {
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

    resolveOutputs();

    for (const auto& constraint : lengthConstraints_) {
      Type input = resolved(constraint.input);
      if (input == Type::Unknown) {
        bind(constraint.input, Type::List, "length 的输入");
        listElement(constraint.input, "length 的输入");
      } else if (input != Type::List && input != Type::Text) {
        fail("length 需要 List 或 Text，但这里得到的是" +
             std::string(typeName(input)));
      }
    }
    for (const auto& constraint : sliceConstraints_) {
      Type input = resolved(constraint.input);
      if (input == Type::Unknown) {
        bind(constraint.input, Type::List, "take/drop 的输入");
        listElement(constraint.input, "take/drop 的输入");
        input = Type::List;
      }
      if (input != Type::List && input != Type::Table)
        fail("take/drop 需要 List 或 Table，但这里得到的是" +
             std::string(typeName(input)));
      unify(constraint.result, constraint.input, "take/drop 的结果");
      if (input == Type::Table)
        shapeCopies_.push_back({constraint.input, constraint.result});
    }

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
    resolveDoConstraints(false);
    resolveOutputs(false);
    resolveDoConstraints(true);
    resolveOutputs(true);
    resolveShapes();

    for (int variable : dataOnlyVariables_)
      if (resolved(variable) == Type::IO)
        fail("IO 不能放入容器、比较或转换为文字");
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (resolved(i) == Type::List &&
          resolved(listElement(i, "List 元素")) == Type::IO)
        fail("IO 不能作为 List 或 Table 列元素");
      if (resolved(i) == Type::IO && resolved(ioResult(i)) == Type::IO)
        fail("IO 结果不能再是 IO；请在 io 块最后一步执行该 action");
      if (resolved(i) == Type::Result &&
          (resolved(resultPayload(i, false)) == Type::IO ||
           resolved(resultPayload(i, true)) == Type::IO))
        fail("Result 载荷不能是 IO；请使用 IO<Result<T, E>>");
    }

    for (int variable : plusVariables_) {
      Type type = resolved(variable);
      if (type == Type::Unknown) bind(variable, Type::Number, "加法");
      else if (type != Type::Number && type != Type::Text)
        fail("“+”只能连接两个数字或两段文字，不能用于" +
             std::string(typeName(type)));
    }
    for (int variable : equalityVariables_) {
      if (resolved(variable) == Type::IO) fail("IO 不允许比较");
      if (resolved(variable) == Type::List || resolved(variable) == Type::Record ||
          resolved(variable) == Type::Table || resolved(variable) == Type::Regex ||
          resolved(variable) == Type::Result)
        fail("列表、Record、Table、Regex 和 Result 不能使用 == 或 != 整体比较，请使用 same");
    }
    for (int variable : orderedVariables_) {
      Type type = resolved(variable);
      if (type == Type::Unknown) bind(variable, Type::Number, "顺序比较");
      else if (type != Type::Number && type != Type::Instant)
        fail("顺序比较只能用于 Number 或 Instant，不能用于" +
             std::string(typeName(type)));
    }
    for (int variable : recordLikeVariables_) {
      Type type = resolved(variable);
      if (type == Type::Unknown) bind(variable, Type::Record, "字段访问左侧");
      else if (type != Type::Record && type != Type::Table)
        fail("字段访问左侧需要 Record 或 Table，但这里得到的是" +
             std::string(typeName(type)));
    }
    for (int element : sortableListElements_) {
      Type type = resolved(element);
      if (type == Type::Unknown) bind(element, Type::Number, "sort 的列表元素");
      else if (type != Type::Number && type != Type::Text &&
               type != Type::Instant)
        fail("sort 只能用于 Number、Text 或 Instant List，不能用于" +
             std::string(typeName(type)) + " List");
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
    checkOptions(program, {
        [&](const Expr& expr) { return optionShape(expressions_.at(&expr)); },
        [&](const Function& fn, std::size_t i) {
          return optionShape(functions_.at(fn.name).params[i]);
        },
        [&](const BinaryExpr& expr, bool vector) {
          auto [found, inserted] = coalesceKinds_.emplace(&expr, vector);
          if (!inserted && found->second != vector)
            fail("Option 类型错误：同一 ?? 不能在不同调用中同时表示整值默认和逐元素填补；逐元素填补请显式使用 fill_missing");
          if (vector) vectorOperations_.insert(&expr);
          else vectorOperations_.erase(&expr);
        },
        [&](const DoExpr& expr) { return doKinds_.at(&expr); }});
    analyzePresence(program);
  }

  Type expressionType(const Expr& expr) { return resolved(expressions_.at(&expr)); }

  Type ioResultType(const Expr& expr) {
    return resolved(ioResult(expressions_.at(&expr)));
  }

  Type resultPayloadType(const Expr& expr, bool error) {
    return resolved(resultPayload(expressions_.at(&expr), error));
  }

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

  bool isResultDo(const DoExpr& expression) const {
    return doKinds_.at(&expression);
  }

 private:
  std::vector<TypeNode> nodes_;
  std::size_t typeRevision_ = 0;
  std::unordered_map<const Expr*, int> expressions_;
  std::unordered_map<std::string, FunctionType> functions_;
  std::unordered_map<std::string, const Function*> definitions_;
  std::vector<std::unordered_map<std::string, int>> scopes_;
  std::vector<int> plusVariables_;
  std::vector<int> equalityVariables_;
  std::vector<int> orderedVariables_;
  std::vector<int> recordLikeVariables_;
  struct DynamicAccess {
    int object;
    int key;
    int result;
  };
  std::vector<DynamicAccess> dynamicAccesses_;
  std::vector<int> tableColumnElements_;
  std::vector<int> sortableListElements_;
  std::unordered_set<const BinaryExpr*> vectorOperations_;
  std::unordered_map<const BinaryExpr*, bool> coalesceKinds_;
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
  struct UnaryConstraint { int input; int result; };
  std::vector<UnaryConstraint> lengthConstraints_;
  std::vector<UnaryConstraint> sliceConstraints_;
  std::unordered_map<const Expr*, Presence> presences_;
  std::unordered_map<std::string, std::vector<Presence>> parameterPresences_;
  std::unordered_map<std::string, Presence> resultPresences_;
  std::vector<std::unordered_map<std::string, Presence>> presenceScopes_;
  std::vector<bool> presenceDoKinds_;
  std::unordered_map<std::string, std::unordered_map<std::string, Presence>>
      presenceResultsByContext_;
  std::unordered_set<std::string> presenceAnalyzedFunctions_;
  bool presenceMayEscape_ = false;
  std::vector<int> dataOnlyVariables_;
  struct OutputConstraint { int input; int result; Type wrapper; };
  std::vector<OutputConstraint> outputConstraints_;
  std::unordered_set<std::size_t> resolvedOutputs_;
  std::vector<const Function*> inferenceOrder_;
  struct DoConstraint {
    const DoExpr* expression;
    int output;
    int finalValue;
    std::vector<int> inputs;
  };
  std::vector<DoConstraint> doConstraints_;
  std::vector<std::vector<int>> doInputStack_;
  std::unordered_map<const DoExpr*, bool> doKinds_;

  OptionType optionShape(int variable) {
    OptionType shape;
    shape.kind = resolved(variable);
    if (shape.kind == Type::List || shape.kind == Type::IO) {
      auto element = optionShape(shape.kind == Type::List
          ? listElement(variable, "Option 元素类型") : ioResult(variable));
      element.optional = true;
      shape.element = std::make_shared<const OptionType>(std::move(element));
    }
    if (shape.kind == Type::Result) {
      auto success = optionShape(resultPayload(variable, false));
      auto error = optionShape(resultPayload(variable, true));
      success.optional = error.optional = true;
      shape.element = std::make_shared<const OptionType>(std::move(success));
      shape.error = std::make_shared<const OptionType>(std::move(error));
    }
    return shape;
  }

  void resolveOutputs(bool final = false) {
    // Field lookup and wrapper elimination constrain one another, e.g.
    // require(record.result).field. Resolve known constraints before choosing
    // the ordinary-T interpretation of an otherwise unknown require input.
    std::size_t revision;
    do {
      revision = typeRevision_;
      resolveFixedFields();
      for (std::size_t i = 0; i < outputConstraints_.size(); ++i) {
        if (resolvedOutputs_.count(i)) continue;
        const auto& constraint = outputConstraints_[i];
        if (resolved(constraint.input) == Type::Unknown) continue;
        int payload = constraint.input;
        if (resolved(payload) == constraint.wrapper)
          payload = constraint.wrapper == Type::IO ? ioResult(payload) : resultPayload(payload, false);
        unify(constraint.result, payload, "解包的结果");
        resolvedOutputs_.insert(i);
      }
    } while (revision != typeRevision_);
    if (!final) return;
    std::vector<unsigned char> visited(outputConstraints_.size());
    std::function<void(std::size_t)> visit = [&](std::size_t index) {
      if (visited[index] == 2 || resolvedOutputs_.count(index)) return;
      if (visited[index] == 1) fail("结果存在循环类型约束");
      visited[index] = 1;
      int input = outputConstraints_[index].input;
      for (std::size_t other = 0; other < outputConstraints_.size(); ++other)
        if (other != index && find(outputConstraints_[other].result) == find(input))
          visit(other);
      if (final || resolved(input) != Type::Unknown) {
        Type wrapper = outputConstraints_[index].wrapper;
        int payload = input;
        if (resolved(input) == wrapper)
          payload = wrapper == Type::IO ? ioResult(input) : resultPayload(input, false);
        unify(outputConstraints_[index].result, payload, "解包的结果");
        resolvedOutputs_.insert(index);
      }
      visited[index] = 2;
    };
    for (std::size_t i = 0; i < outputConstraints_.size(); ++i) visit(i);
  }

  void resolveDoConstraints(bool final) {
    for (const auto& constraint : doConstraints_) {
      if (constraint.inputs.empty())
        fail("do 块至少需要一个 <- 解包步骤；普通顺序计算请直接使用块");
      Type first = resolved(constraint.inputs.front());
      if (first == Type::Unknown && !final) continue;
      bool resultDo = first == Type::Result;
      doKinds_[constraint.expression] = resultDo;
      for (int input : constraint.inputs) {
        if (resolved(input) == Type::IO)
          fail("do 的 <- 只解包 Option 或 Result；执行 IO 请使用 io 块");
        if (resultDo && resolved(input) == Type::Unknown)
          bind(input, Type::Result, "Result do 的 <- 输入");
        bool isResult = resolved(input) == Type::Result;
        if ((resolved(input) != Type::Unknown && isResult != resultDo))
          fail("同一个 do 块不能混用 Option 与 Result 的 <- 解包");
      }
      if (resultDo) {
        bind(constraint.output, Type::Result, "Result do 的结果");
        unify(resultPayload(constraint.output, false), constraint.finalValue,
              "Result do 的成功值");
        int error = resultPayload(constraint.inputs.front(), true);
        for (std::size_t i = 1; i < constraint.inputs.size(); ++i)
          unify(error, resultPayload(constraint.inputs[i], true),
                "Result do 的错误类型");
        unify(resultPayload(constraint.output, true), error,
              "Result do 的错误类型");
      } else {
        unify(constraint.output, constraint.finalValue, "Option do 的最终值");
      }
    }
  }

  void collectCalls(const Expr& expr, std::unordered_set<std::string>& calls) const {
    if (auto* io = dynamic_cast<const IOExpr*>(&expr)) {
      collectCalls(*io->body, calls);
    } else if (auto* bind = dynamic_cast<const IOBindExpr*>(&expr)) {
      collectCalls(*bind->action, calls);
      collectCalls(*bind->body, calls);
    } else if (auto* result = dynamic_cast<const IOResultExpr*>(&expr)) {
      collectCalls(*result->value, calls);
    } else if (auto* block = dynamic_cast<const DoExpr*>(&expr)) {
      collectCalls(*block->body, calls);
    } else if (auto* bind = dynamic_cast<const DoBindExpr*>(&expr)) {
      collectCalls(*bind->input, calls);
      collectCalls(*bind->body, calls);
    } else if (auto* result = dynamic_cast<const DoResultExpr*>(&expr)) {
      collectCalls(*result->value, calls);
    } else if (auto* list = dynamic_cast<const ListExpr*>(&expr)) {
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
    } else if (auto* match = dynamic_cast<const MatchExpr*>(&expr)) {
      collectCalls(*match->subject, calls);
      for (const auto& arm : match->arms) collectCalls(*arm.body, calls);
    }
  }

  void rejectRecursion(const Program& program) {
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
      inferenceOrder_.push_back(definitions_.at(name));
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
    if (old != type) ++typeRevision_;
    nodes_[root].type = type;
  }

  int listElement(int variable, const std::string& contextName) {
    bind(variable, Type::List, contextName);
    int root = find(variable);
    if (nodes_[root].listElement < 0)
      nodes_[root].listElement = newVariable();
    return nodes_[root].listElement;
  }

  int ioResult(int variable) {
    bind(variable, Type::IO, "action");
    int root = find(variable);
    if (nodes_[root].ioResult < 0)
      nodes_[root].ioResult = newVariable();
    return nodes_[root].ioResult;
  }

  int resultPayload(int variable, bool error) {
    bind(variable, Type::Result, "Result 构造/模式");
    int root = find(variable);
    int payload = error ? nodes_[root].error : nodes_[root].success;
    if (payload < 0) {
      payload = newVariable();
      if (error) nodes_[root].error = payload;
      else nodes_[root].success = payload;
    }
    return payload;
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
    auto contains = [&](int root, int target) {
      std::vector<int> pending{root};
      std::unordered_set<int> seen;
      while (!pending.empty()) {
        int current = find(pending.back());
        pending.pop_back();
        if (current == target) return true;
        if (!seen.insert(current).second) continue;
        if (nodes_[current].listElement >= 0) pending.push_back(nodes_[current].listElement);
        if (nodes_[current].ioResult >= 0) pending.push_back(nodes_[current].ioResult);
        if (nodes_[current].success >= 0) pending.push_back(nodes_[current].success);
        if (nodes_[current].error >= 0) pending.push_back(nodes_[current].error);
      }
      return false;
    };
    if (contains(leftRoot, rightRoot) || contains(rightRoot, leftRoot))
      fail(contextName + "产生无限嵌套的类型");
    Type leftType = nodes_[leftRoot].type;
    Type rightType = nodes_[rightRoot].type;
    int leftElement = nodes_[leftRoot].listElement;
    int rightElement = nodes_[rightRoot].listElement;
    int leftIO = nodes_[leftRoot].ioResult;
    int rightIO = nodes_[rightRoot].ioResult;
    int leftSuccess = nodes_[leftRoot].success, rightSuccess = nodes_[rightRoot].success;
    int leftError = nodes_[leftRoot].error, rightError = nodes_[rightRoot].error;
    if (leftType != Type::Unknown && rightType != Type::Unknown && leftType != rightType)
      fail(contextName + "两边的种类不同：一边是" + typeName(leftType) +
           "，另一边是" + typeName(rightType));
    nodes_[rightRoot].parent = leftRoot;
    ++typeRevision_;
    if (leftType == Type::Unknown) nodes_[leftRoot].type = rightType;
    if (leftElement < 0) nodes_[leftRoot].listElement = rightElement;
    else if (rightElement >= 0)
      unify(leftElement, rightElement, contextName + "的列表元素");
    if (leftIO < 0) nodes_[leftRoot].ioResult = rightIO;
    else if (rightIO >= 0) unify(leftIO, rightIO, contextName + "的 IO 结果");
    if (leftSuccess < 0) nodes_[leftRoot].success = rightSuccess;
    else if (rightSuccess >= 0) unify(leftSuccess, rightSuccess, contextName + "的 ok 载荷");
    if (leftError < 0) nodes_[leftRoot].error = rightError;
    else if (rightError >= 0) unify(leftError, rightError, contextName + "的 err 载荷");
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

    resolveFixedFields();
  }

  void resolveFixedFields() {
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
    if (auto* io = dynamic_cast<const IOExpr*>(&expr)) {
      unify(ioResult(result), infer(*io->body), "IO 结果");
      return result;
    }
    if (auto* execution = dynamic_cast<const IOBindExpr*>(&expr)) {
      int value = ioResult(infer(*execution->action));
      scopes_.emplace_back();
      if (!execution->name.empty()) scopes_.back().emplace(execution->name, value);
      unify(result, infer(*execution->body), "IO 后续步骤");
      scopes_.pop_back();
      return result;
    }
    if (auto* final = dynamic_cast<const IOResultExpr*>(&expr)) {
      int input = infer(*final->value);
      if (resolved(input) == Type::Unknown) outputConstraints_.push_back({input, result, Type::IO});
      else unify(result, resolved(input) == Type::IO ? ioResult(input) : input,
                 "io 的最终结果");
      return result;
    }
    if (auto* block = dynamic_cast<const DoExpr*>(&expr)) {
      doInputStack_.emplace_back();
      int finalValue = infer(*block->body);
      auto inputs = std::move(doInputStack_.back());
      doInputStack_.pop_back();
      doConstraints_.push_back({block, result, finalValue, std::move(inputs)});
      return result;
    }
    if (auto* bind = dynamic_cast<const DoBindExpr*>(&expr)) {
      if (doInputStack_.empty()) fail("内部错误：do 绑定不在 do 块内");
      int input = infer(*bind->input);
      int payload = newVariable();
      outputConstraints_.push_back({input, payload, Type::Result});
      doInputStack_.back().push_back(input);
      scopes_.emplace_back();
      scopes_.back().emplace(bind->name, payload);
      unify(result, infer(*bind->body), "do 后续步骤");
      scopes_.pop_back();
      return result;
    }
    if (auto* final = dynamic_cast<const DoResultExpr*>(&expr)) {
      unify(result, infer(*final->value), "do 最终值");
      return result;
    }
    if (dynamic_cast<const NumberExpr*>(&expr)) {
      bind(result, Type::Number, "数字");
      return result;
    }
    if (dynamic_cast<const BooleanExpr*>(&expr)) {
      bind(result, Type::Boolean, "是/否值");
      return result;
    }
    if (dynamic_cast<const UnitExpr*>(&expr)) {
      bind(result, Type::Unit, "单元值 ()");
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
        dataOnlyVariables_.push_back(value);
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
      if (variable->name == "pi" || variable->name == "e" ||
          variable->name == "tau") {
        bind(result, Type::Number, "数学常量“" + variable->name + "”");
        return result;
      }
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
    if (auto* match = dynamic_cast<const MatchExpr*>(&expr)) {
      int subject = infer(*match->subject);
      for (const auto& arm : match->arms) {
        int bound = subject;
        if (resultPattern(arm.pattern))
          bound = resultPayload(subject, arm.pattern.kind == MatchPattern::Kind::Err);
        if (arm.pattern.literal)
          unify(subject, infer(*arm.pattern.literal), "match 字面量模式");
        scopes_.emplace_back();
        if (bindsName(arm.pattern)) scopes_.back().emplace(arm.pattern.name, bound);
        unify(result, infer(*arm.body), "match 分支结果");
        scopes_.pop_back();
      }
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
        int key = infer(*field->key);
        if (resolved(object) == Type::List) {
          bind(key, Type::Number, "List 下标");
          unify(result, listElement(object, "下标读取左侧"),
                "List 下标读取结果");
        } else {
          dynamicAccesses_.push_back({object, key, result});
        }
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
      dataOnlyVariables_.push_back(infer(*update->value));
      bind(result, Type::Record, "字段更新结果");
      return result;
    }
    if (auto* update = dynamic_cast<const NestedRecordUpdateExpr*>(&expr)) {
      bind(infer(*update->record), Type::Record, "嵌套字段更新左侧");
      for (const auto& part : update->path)
        if (part.key) bind(infer(*part.key), Type::Text, "动态字段名");
      dataOnlyVariables_.push_back(infer(*update->value));
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
        } else if (op == "<" || op == ">" || op == "<=" || op == ">=") {
          unify(leftElement, rightElement, "整列顺序比较");
          orderedVariables_.push_back(leftElement);
          bind(resultElement, Type::Boolean, "整列比较结果");
        } else {
          bind(leftElement, Type::Number, "整列运算左侧");
          bind(rightElement, Type::Number, "整列运算右侧");
          bind(resultElement, Type::Number, "整列运算结果");
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
      } else if (op == "-" || op == "*" || op == "/" || op == "%") {
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
        unify(lhs, rhs, "比较号“" + op + "”两边");
        orderedVariables_.push_back(lhs);
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
      if (isIOBuiltin(call->callee)) {
        if (call->callee == "now") {
          if (!call->args.empty()) fail("now 不接受参数");
          bind(ioResult(result), Type::Instant, "now 的结果");
          return result;
        }
        if (call->callee == "random") {
          if (!call->args.empty()) fail("random 不接受参数");
          bind(ioResult(result), Type::Number, "random 的结果");
          return result;
        }
        if (call->callee == "random_between" || call->callee == "random_integer") {
          if (call->args.size() != 2)
            fail("函数“" + call->callee + "”需要 minimum 和 maximum");
          bind(infer(*call->args[0]), Type::Number, call->callee + " 的 minimum");
          bind(infer(*call->args[1]), Type::Number, call->callee + " 的 maximum");
          bind(ioResult(result), Type::Number, call->callee + " 的结果");
          return result;
        }
        if (call->callee == "choose" || call->callee == "shuffle") {
          if (call->args.size() != 1)
            fail("函数“" + call->callee + "”需要 1 个 List");
          int input = infer(*call->args[0]);
          int element = listElement(input, call->callee + " 的输入");
          int payload = ioResult(result);
          if (call->callee == "choose") unify(payload, element, "choose 的结果");
          else unify(payload, input, "shuffle 的结果");
          return result;
        }
        bool write = call->callee == "write_stdout";
        if (call->args.size() != (write ? 1u : 0u))
          fail(call->callee + (write ? " 需要 1 个 Text 参数" : " 不接受参数"));
        if (write) bind(infer(*call->args[0]), Type::Text, "write_stdout 的输入");
        int payload = ioResult(result);
        if (call->callee == "arguments")
          bind(listElement(payload, "arguments 的结果"), Type::Text, "命令行参数");
        else bind(payload, write ? Type::Unit : Type::Text, "IO 执行结果");
        return result;
      }
      if (isTimeBuiltin(call->callee)) {
        if (call->callee == "parse_time") {
          if (call->args.size() != 1) fail("parse_time 需要 1 个 Text");
          bind(infer(*call->args[0]), Type::Text, "parse_time 的输入");
          bind(result, Type::Instant, "parse_time 的结果");
          return result;
        }
        if (call->callee == "format_time" || call->callee == "unix_seconds") {
          if (call->args.size() != 1)
            fail("函数“" + call->callee + "”需要 1 个 Instant");
          bind(infer(*call->args[0]), Type::Instant,
               "函数“" + call->callee + "”的输入");
          bind(result, call->callee == "format_time" ? Type::Text : Type::Number,
               "函数“" + call->callee + "”的结果");
          return result;
        }
        if (call->callee == "time_from_unix") {
          if (call->args.size() != 1) fail("time_from_unix 需要 1 个 Number");
          bind(infer(*call->args[0]), Type::Number, "time_from_unix 的输入");
          bind(result, Type::Instant, "time_from_unix 的结果");
          return result;
        }
        if (call->args.size() != 2)
          fail("函数“" + call->callee + "”需要 2 个参数");
        bind(infer(*call->args[0]), Type::Instant,
             "函数“" + call->callee + "”的第一个参数");
        bind(infer(*call->args[1]),
             call->callee == "add_seconds" ? Type::Number : Type::Instant,
             "函数“" + call->callee + "”的第二个参数");
        bind(result, call->callee == "add_seconds" ? Type::Instant : Type::Number,
             "函数“" + call->callee + "”的结果");
        return result;
      }
      if (isRegexBuiltin(call->callee)) {
        size_t arity = call->callee == "regex" ? 1 : 2;
        if (call->args.size() != arity)
          fail("函数“" + call->callee + "”需要 " + std::to_string(arity) + " 个参数");
        bind(infer(*call->args[0]), Type::Text, call->callee + " 的 Text");
        if (arity == 1) bind(result, Type::Regex, "regex 的结果");
        else {
          bind(infer(*call->args[1]), Type::Regex, call->callee + " 的模式");
          if (call->callee == "regex_find_all" || call->callee == "regex_split")
            bind(listElement(result, call->callee + " 的结果"), Type::Text,
                 call->callee + " 的结果元素");
          else bind(result, call->callee == "regex_find" ? Type::Text : Type::Boolean,
                    call->callee + " 的结果");
        }
        return result;
      }
      if (call->callee == "table") {
        if (call->args.size() != 1)
          fail("table 需要 1 个 Record 或 List<Record>，但这里给了 " +
               std::to_string(call->args.size()) + " 个值");
        int input = infer(*call->args[0]);
        if (auto* literal = dynamic_cast<const RecordExpr*>(call->args[0].get())) {
          std::optional<std::size_t> knownLength;
          for (const auto& field : literal->fields) {
            int column = expressions_.at(field.value.get());
            bind(column, Type::List,
                 "Table 字面量的列“" +
                     (field.key ? std::string("(动态字段)") : field.name) + "”");
            listElement(column, "Table 字面量的列");
            if (auto* list = dynamic_cast<const ListExpr*>(field.value.get())) {
              if (knownLength && *knownLength != list->elements.size())
                fail("Table 字面量中静态已知的列长度不一致");
              knownLength = list->elements.size();
            }
          }
        }
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
        int input = infer(*call->args[0]);
        outputConstraints_.push_back({input, result, Type::Result});
        resolveOutputs();
        return result;
      }
      if (call->callee == "ok" || call->callee == "err") {
        if (call->args.size() != 1) fail(call->callee + " 需要 1 个值");
        int payload = infer(*call->args[0]);
        unify(resultPayload(result, call->callee == "err"), payload,
              call->callee + " 的载荷");
        resultPayload(result, call->callee != "err");
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
          fail("length 需要 1 个 List 或 Text，但这里给了 " +
               std::to_string(call->args.size()) + " 个值");
        int input = infer(*call->args[0]);
        lengthConstraints_.push_back({input, result});
        bind(result, Type::Number, "length 的结果");
        return result;
      }
      if (call->callee == "trim" || call->callee == "lowercase" ||
          call->callee == "uppercase") {
        if (call->args.size() != 1)
          fail("函数“" + call->callee + "”需要 1 个 Text");
        bind(infer(*call->args[0]), Type::Text,
             "函数“" + call->callee + "”的输入");
        bind(result, Type::Text, "函数“" + call->callee + "”的结果");
        return result;
      }
      if (call->callee == "starts_with" || call->callee == "ends_with" ||
          call->callee == "text_contains") {
        if (call->args.size() != 2)
          fail("函数“" + call->callee + "”需要 Text 和要匹配的 Text");
        bind(infer(*call->args[0]), Type::Text,
             "函数“" + call->callee + "”的 Text");
        bind(infer(*call->args[1]), Type::Text,
             "函数“" + call->callee + "”的匹配内容");
        bind(result, Type::Boolean, "函数“" + call->callee + "”的结果");
        return result;
      }
      if (call->callee == "split") {
        if (call->args.size() != 2) fail("split 需要 Text 和分隔符");
        bind(infer(*call->args[0]), Type::Text, "split 的 Text");
        bind(infer(*call->args[1]), Type::Text, "split 的分隔符");
        bind(listElement(result, "split 的结果"), Type::Text,
             "split 的结果元素");
        return result;
      }
      if (call->callee == "join") {
        if (call->args.size() != 2) fail("join 需要 Text List 和分隔符");
        int parts = infer(*call->args[0]);
        bind(listElement(parts, "join 的输入"), Type::Text, "join 的元素");
        bind(infer(*call->args[1]), Type::Text, "join 的分隔符");
        bind(result, Type::Text, "join 的结果");
        return result;
      }
      if (call->callee == "abs" || call->callee == "floor" ||
          call->callee == "ceil" || call->callee == "sign" ||
          call->callee == "sqrt" || call->callee == "exp" ||
          call->callee == "sin" || call->callee == "cos" ||
          call->callee == "tan" || call->callee == "asin" ||
          call->callee == "acos" || call->callee == "atan" ||
          call->callee == "radians" || call->callee == "degrees") {
        if (call->args.size() != 1)
          fail("函数“" + call->callee + "”需要 1 个 Number");
        bind(infer(*call->args[0]), Type::Number,
             "函数“" + call->callee + "”的输入");
        bind(result, Type::Number, "函数“" + call->callee + "”的结果");
        return result;
      }
      if (call->callee == "pow" || call->callee == "hypot" ||
          call->callee == "atan2") {
        if (call->args.size() != 2)
          fail("函数“" + call->callee + "”需要 2 个 Number");
        bind(infer(*call->args[0]), Type::Number,
             "函数“" + call->callee + "”的第一个输入");
        bind(infer(*call->args[1]), Type::Number,
             "函数“" + call->callee + "”的第二个输入");
        bind(result, Type::Number, "函数“" + call->callee + "”的结果");
        return result;
      }
      if (call->callee == "log") {
        if (call->args.size() != 1 && call->args.size() != 2)
          fail("log 需要 Number 和可选的底数");
        bind(infer(*call->args[0]), Type::Number, "log 的输入");
        if (call->args.size() == 2)
          bind(infer(*call->args[1]), Type::Number, "log 的底数");
        bind(result, Type::Number, "log 的结果");
        return result;
      }
      if (call->callee == "round") {
        if (call->args.size() != 1 && call->args.size() != 2)
          fail("round 需要 Number 和可选的小数位数");
        bind(infer(*call->args[0]), Type::Number, "round 的输入");
        if (call->args.size() == 2)
          bind(infer(*call->args[1]), Type::Number, "round 的小数位数");
        bind(result, Type::Number, "round 的结果");
        return result;
      }
      if (call->callee == "clamp") {
        if (call->args.size() != 3)
          fail("clamp 需要 Number、最小值和最大值");
        for (const auto& argument : call->args)
          bind(infer(*argument), Type::Number, "clamp 的参数");
        bind(result, Type::Number, "clamp 的结果");
        return result;
      }
      if (call->callee == "to_text") {
        if (call->args.size() != 1) fail("to_text 需要 1 个值");
        dataOnlyVariables_.push_back(infer(*call->args[0]));
        bind(result, Type::Text, "to_text 的结果");
        return result;
      }
      if (call->callee == "parse_number") {
        if (call->args.size() != 1) fail("parse_number 需要 1 个 Text");
        bind(infer(*call->args[0]), Type::Text, "parse_number 的输入");
        bind(result, Type::Number, "parse_number 的结果");
        return result;
      }
      if (call->callee == "concat") {
        if (call->args.size() != 2) fail("concat 需要两个 List");
        int left = infer(*call->args[0]);
        int right = infer(*call->args[1]);
        unify(listElement(left, "concat 的左侧"),
              listElement(right, "concat 的右侧"), "concat 的元素");
        unify(result, left, "concat 的结果");
        return result;
      }
      if (call->callee == "flatten") {
        if (call->args.size() != 1)
          fail("flatten 需要 1 个 List<List<T>>");
        int input = infer(*call->args[0]);
        int inner = listElement(input, "flatten 的输入");
        int element = listElement(inner, "flatten 的输入元素");
        unify(listElement(result, "flatten 的结果"), element,
              "flatten 的结果元素");
        return result;
      }
      if (call->callee == "reverse" || call->callee == "distinct") {
        if (call->args.size() != 1)
          fail("函数“" + call->callee + "”需要 1 个 List");
        int input = infer(*call->args[0]);
        listElement(input, call->callee + " 的输入");
        unify(result, input, call->callee + " 的结果");
        return result;
      }
      if (call->callee == "sort") {
        if (call->args.size() != 1) fail("sort 需要 1 个 List");
        int input = infer(*call->args[0]);
        sortableListElements_.push_back(listElement(input, "sort 的输入"));
        unify(result, input, "sort 的结果");
        return result;
      }
      if (call->callee == "fields") {
        if (call->args.size() != 1) fail("fields 需要 1 个 Record");
        bind(infer(*call->args[0]), Type::Record, "fields 的输入");
        bind(listElement(result, "fields 的结果"), Type::Text,
             "fields 的结果元素");
        return result;
      }
      if (call->callee == "has_field") {
        if (call->args.size() != 2) fail("has_field 需要 Record 和字段名");
        bind(infer(*call->args[0]), Type::Record, "has_field 的 Record");
        bind(infer(*call->args[1]), Type::Text, "has_field 的字段名");
        bind(result, Type::Boolean, "has_field 的结果");
        return result;
      }
      if (call->callee == "merge") {
        if (call->args.size() != 2) fail("merge 需要两个 Record");
        bind(infer(*call->args[0]), Type::Record, "merge 的左侧");
        bind(infer(*call->args[1]), Type::Record, "merge 的右侧");
        bind(result, Type::Record, "merge 的结果");
        return result;
      }
      if (call->callee == "remove_field") {
        if (call->args.size() != 2)
          fail("remove_field 需要 Record 和字段名");
        bind(infer(*call->args[0]), Type::Record, "remove_field 的 Record");
        bind(infer(*call->args[1]), Type::Text, "remove_field 的字段名");
        bind(result, Type::Record, "remove_field 的结果");
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
        dataOnlyVariables_.push_back(infer(*call->args[0]));
        dataOnlyVariables_.push_back(infer(*call->args[1]));
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
          fail("函数“" + call->callee + "”需要一个 List/Table 和数量");
        int input = infer(*call->args[0]);
        bind(infer(*call->args[1]), Type::Number, "函数“" + call->callee + "”的行数");
        sliceConstraints_.push_back({input, result});
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
      resolveOutputs();
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
    bool outerEscape = presenceMayEscape_;
    presenceMayEscape_ = false;
    Presence result = kNoPresence;
    if (auto* io = dynamic_cast<const IOExpr*>(&expr)) {
      presenceOf(*io->body);
      presenceMayEscape_ = false;
      result = kPresent;
    } else if (auto* execution = dynamic_cast<const IOBindExpr*>(&expr)) {
      presenceOf(*execution->action);
      presenceMayEscape_ = false;
      presenceScopes_.emplace_back();
      if (!execution->name.empty())
        presenceScopes_.back().emplace(execution->name, kMaybeMissing);
      result = presenceOf(*execution->body);
      presenceScopes_.pop_back();
    } else if (auto* final = dynamic_cast<const IOResultExpr*>(&expr)) {
      result = presenceOf(*final->value);
      if (expressionType(*final->value) == Type::IO) result = kMaybeMissing;
      presenceMayEscape_ = false;
    } else if (auto* block = dynamic_cast<const DoExpr*>(&expr)) {
      bool resultDo = isResultDo(*block);
      presenceDoKinds_.push_back(resultDo);
      Presence body = presenceOf(*block->body);
      presenceDoKinds_.pop_back();
      result = resultDo ? kPresent : body;
      presenceMayEscape_ = false;
    } else if (auto* bind = dynamic_cast<const DoBindExpr*>(&expr)) {
      Presence input = presenceOf(*bind->input);
      presenceMayEscape_ = false;
      presenceScopes_.emplace_back();
      bool resultDo = presenceDoKinds_.back();
      presenceScopes_.back().emplace(bind->name,
          resultDo ? kMaybeMissing : kPresent);
      Presence body = presenceOf(*bind->body);
      presenceScopes_.pop_back();
      result = resultDo ? body
                        : static_cast<Presence>((input & kMissing) |
                            ((input & kPresent) ? body : kNoPresence));
    } else if (auto* final = dynamic_cast<const DoResultExpr*>(&expr)) {
      result = presenceOf(*final->value);
      presenceMayEscape_ = false;
    } else if (dynamic_cast<const NumberExpr*>(&expr) ||
        dynamic_cast<const BooleanExpr*>(&expr) ||
        dynamic_cast<const UnitExpr*>(&expr) ||
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
      if (result == kNoPresence &&
          (variable->name == "pi" || variable->name == "e" ||
           variable->name == "tau"))
        result = kPresent;
    } else if (auto* let = dynamic_cast<const LetExpr*>(&expr)) {
      Presence value = presenceOf(*let->value);
      presenceMayEscape_ = false;
      presenceScopes_.emplace_back();
      presenceScopes_.back().emplace(let->name, value);
      result = presenceOf(*let->body);
      presenceScopes_.pop_back();
    } else if (auto* match = dynamic_cast<const MatchExpr*>(&expr)) {
      Presence input = presenceOf(*match->subject);
      MatchCoverage covered;
      using Kind = MatchPattern::Kind;
      for (const auto& arm : match->arms) {
        Presence narrowed = input;
        if (arm.pattern.kind == Kind::Missing) narrowed = kMissing;
        else if (resultPattern(arm.pattern) || arm.pattern.kind == Kind::Present ||
                 arm.pattern.kind == Kind::Literal)
          narrowed = kPresent;
        else {
          if (covered.missing) narrowed &= ~kMissing;
          if (covered.present) narrowed &= ~kPresent;
          if (!narrowed) narrowed = kMaybeMissing;
        }
        if (arm.pattern.literal) presenceOf(*arm.pattern.literal);
        presenceScopes_.emplace_back();
        if (auto* variable = dynamic_cast<const VariableExpr*>(match->subject.get()))
          presenceScopes_.back()[variable->name] = narrowed;
        if (bindsName(arm.pattern)) presenceScopes_.back()[arm.pattern.name] =
            resultPattern(arm.pattern) ? kMaybeMissing : narrowed;
        result |= presenceOf(*arm.body);
        presenceScopes_.pop_back();
        covered.add(arm.pattern);
      }
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
      if (result & kMissing) presenceMayEscape_ = true;
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
      // Scalar ?? catches ? from its left expression; a vector coalesce is
      // an ordinary list operation and does not create that control boundary.
      if (binary->op == "??" && !vectorOperations_.count(binary))
        presenceMayEscape_ = false;
      Presence rhs = presenceOf(*binary->rhs);
      if (binary->op == "??") result = coalescePresence(lhs, rhs);
      else if (binary->op == "and" || binary->op == "or") {
        result = lhs == kPresent && rhs == kPresent ? kPresent : kMaybeMissing;
      } else {
        result = propagate(lhs, rhs);
      }
    } else if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
      if (isIOBuiltin(call->callee)) {
        for (const auto& argument : call->args) presenceOf(*argument);
        result = kPresent;
      } else if (isTimeBuiltin(call->callee)) {
        result = presenceOf(*call->args[0]);
        for (std::size_t i = 1; i < call->args.size(); ++i)
          result = propagate(result, presenceOf(*call->args[i]));
        if (call->callee == "parse_time") result = kMaybeMissing;
      } else if (isRegexBuiltin(call->callee)) {
        result = kPresent;
        for (const auto& argument : call->args)
          result = propagate(result, presenceOf(*argument));
        if (call->callee == "regex_find") result = kMaybeMissing;
      } else if (call->callee == "length" || call->callee == "table" ||
          call->callee == "row_count") {
        result = presenceOf(*call->args[0]);
      } else if (call->callee == "is_missing") {
        presenceOf(*call->args[0]);
        result = kPresent;
      } else if (call->callee == "ok" || call->callee == "err") {
        presenceOf(*call->args[0]);
        result = kPresent;
      } else if (call->callee == "require") {
        presenceOf(*call->args[0]);
        result = expressionType(*call->args[0]) == Type::Result ? kMaybeMissing : kPresent;
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
      } else if (call->callee == "parse_number") {
        presenceOf(*call->args[0]);
        result = kMaybeMissing;
      } else if (call->callee == "trim" || call->callee == "lowercase" ||
                 call->callee == "uppercase" || call->callee == "abs" ||
                 call->callee == "floor" || call->callee == "ceil" ||
                 call->callee == "sign" || call->callee == "sqrt" ||
                 call->callee == "exp" || call->callee == "sin" ||
                 call->callee == "cos" || call->callee == "tan" ||
                 call->callee == "asin" || call->callee == "acos" ||
                 call->callee == "atan" || call->callee == "radians" ||
                 call->callee == "degrees" ||
                 call->callee == "to_text" || call->callee == "flatten" ||
                 call->callee == "reverse" || call->callee == "distinct" ||
                 call->callee == "sort" ||
                 call->callee == "fields") {
        result = presenceOf(*call->args[0]);
      } else if (call->callee == "starts_with" ||
                 call->callee == "ends_with" ||
                 call->callee == "text_contains" ||
                 call->callee == "split" ||
                 call->callee == "join" || call->callee == "concat" ||
                 call->callee == "has_field" || call->callee == "merge" ||
                 call->callee == "remove_field") {
        result = propagate(presenceOf(*call->args[0]),
                           presenceOf(*call->args[1]));
      } else if (call->callee == "round") {
        result = presenceOf(*call->args[0]);
        if (call->args.size() == 2)
          result = propagate(result, presenceOf(*call->args[1]));
      } else if (call->callee == "clamp") {
        result = propagate(presenceOf(*call->args[0]),
                           presenceOf(*call->args[1]));
        result = propagate(result, presenceOf(*call->args[2]));
      } else if (call->callee == "pow" || call->callee == "hypot" ||
                 call->callee == "atan2") {
        result = propagate(presenceOf(*call->args[0]),
                           presenceOf(*call->args[1]));
      } else if (call->callee == "log") {
        result = presenceOf(*call->args[0]);
        if (call->args.size() == 2)
          result = propagate(result, presenceOf(*call->args[1]));
      } else if (call->callee == "fill_missing" ||
                 call->callee == "select_columns" || call->callee == "take" ||
                 call->callee == "drop" || call->callee == "sort_by") {
        result = propagate(presenceOf(*call->args[0]), presenceOf(*call->args[1]));
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
      bool inputEscape = presenceMayEscape_;
      presenceMayEscape_ = false;
      presenceScopes_.emplace_back();
      Presence element = expressionType(*collection->collection) == Type::Table
          ? kPresent : kMaybeMissing;
      presenceScopes_.back().emplace("$lan_current_element", element);
      if (!collection->binding.empty())
        presenceScopes_.back().emplace(collection->binding, element);
      Presence body = presenceOf(*collection->body);
      presenceMayEscape_ = inputEscape;
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
    // A missing field is data, but a field containing ? may abort construction
    // of the enclosing Record. Keep this control effect separate from values.
    if (presenceMayEscape_) result |= kMissing;
    presenceMayEscape_ = presenceMayEscape_ || outerEscape;
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
    bool callerEscape = presenceMayEscape_;
    presenceMayEscape_ = false;
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
    presenceMayEscape_ = callerEscape;
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
  struct DiagnosticCollection {
    llvm::Value* index;
    bool table;
  };
  std::vector<DiagnosticCollection> diagnosticCollections;
  std::unordered_map<std::string, const Function*> declarations;
  llvm::StructType* taggedType = nullptr;
  llvm::StructType* listType = nullptr;
  llvm::StructType* optionalNumberType = nullptr;
  llvm::Function* printValueFunction = nullptr;
  llvm::Function* printListFunction = nullptr;
  llvm::BasicBlock* currentMissingTarget = nullptr;
  llvm::BasicBlock* rootMissingTarget = nullptr;
  const Expr* currentDiagnosticExpression = nullptr;
  std::string sourcePath;

  Impl() : module(std::make_unique<llvm::Module>("lan_module", context)) {}

  void loadRuntime(const unsigned char* bytes, const unsigned char* end,
                   const std::string& displayName) {
    llvm::StringRef data(reinterpret_cast<const char*>(bytes),
                         reinterpret_cast<uintptr_t>(end) -
                         reinterpret_cast<uintptr_t>(bytes));
    auto runtime = llvm::parseBitcodeFile(
        llvm::MemoryBufferRef(data, displayName), context);
    if (!runtime)
      throw std::runtime_error("无法解析" + displayName + "运行时：" +
                               llvm::toString(runtime.takeError()));
    if (llvm::Linker::linkModules(*module, std::move(*runtime)))
      throw std::runtime_error("无法链接" + displayName + "运行时");
  }

  void loadRecordRuntime() {
    loadRuntime(lan_unicode_bytes, lan_unicode_end, "Unicode");
    loadRuntime(lan_record_bytes, lan_record_end, "Lan");
    for (auto& function : *module)
      if (!function.isDeclaration() && function.getName().starts_with("lan_regex_"))
        function.setLinkage(llvm::GlobalValue::InternalLinkage);
  }

  llvm::Type* llvmType(Type type) {
    switch (type) {
      case Type::Number: return builder.getDoubleTy();
      case Type::Boolean: return builder.getInt1Ty();
      case Type::Unit: return builder.getInt1Ty();  // true = (), false = missing
      case Type::Text: return llvm::PointerType::get(context, 0);
      case Type::List: return llvm::PointerType::get(context, 0);
      case Type::Record: return llvm::PointerType::get(context, 0);
      case Type::Table: return llvm::PointerType::get(context, 0);
      case Type::Regex: return llvm::PointerType::get(context, 0);
      case Type::Instant: return llvm::PointerType::get(context, 0);
      case Type::IO: return llvm::PointerType::get(context, 0);
      case Type::Result: return llvm::PointerType::get(context, 0);
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
    if (type == Type::Unit) return builder.getFalse();
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
    if (value.type == Type::Unit) return value.value;
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
    if (value.type == Type::Unit) return builder.getTrue();
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

  std::string diagnosticFieldPath(const Expr* expression) const {
    if (!expression) return {};
    if (auto* field = dynamic_cast<const FieldExpr*>(expression)) {
      auto prefix = diagnosticFieldPath(field->record.get());
      return prefix + (field->key ? ".(动态字段)" : "." + field->name);
    }
    if (auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
      auto left = diagnosticFieldPath(binary->lhs.get());
      return left.empty() ? diagnosticFieldPath(binary->rhs.get()) : left;
    }
    if (auto* unary = dynamic_cast<const UnaryExpr*>(expression))
      return diagnosticFieldPath(unary->operand.get());
    if (auto* question = dynamic_cast<const PropagateExpr*>(expression))
      return diagnosticFieldPath(question->operand.get());
    if (auto* call = dynamic_cast<const CallExpr*>(expression))
      for (const auto& argument : call->args) {
        auto path = diagnosticFieldPath(argument.get());
        if (!path.empty()) return path;
      }
    return {};
  }

  void emitRuntimeError(const std::string& message, llvm::Value* detail = nullptr) {
    auto failFn = external(
        "lan_runtime_fail", builder.getVoidTy(),
        {llvmType(Type::Text), llvmType(Type::Text), builder.getInt32Ty(),
         builder.getInt32Ty(), builder.getInt64Ty(), builder.getInt64Ty(),
         llvmType(Type::Text)});
    llvm::Value* listIndex = builder.getInt64(-1);
    llvm::Value* tableRow = builder.getInt64(-1);
    if (!diagnosticCollections.empty()) {
      const auto& collection = diagnosticCollections.back();
      if (collection.table) tableRow = collection.index;
      else listIndex = collection.index;
    } else if (auto* binary = dynamic_cast<const BinaryExpr*>(
                   currentDiagnosticExpression);
               binary && inference->isVectorOperation(*binary)) {
      auto indexFn = external("lan_diagnostic_error_index", builder.getInt64Ty(), {});
      listIndex = builder.CreateCall(indexFn, {}, "error.list.index");
    } else if (auto* call = dynamic_cast<const CallExpr*>(
                   currentDiagnosticExpression);
               call && (call->callee == "join" ||
                        call->callee == "flatten")) {
      auto indexFn = external("lan_diagnostic_error_index", builder.getInt64Ty(), {});
      listIndex = builder.CreateCall(indexFn, {}, "error.list.index");
    }
    int line = currentDiagnosticExpression ? currentDiagnosticExpression->line : 0;
    int column = currentDiagnosticExpression ? currentDiagnosticExpression->column : 0;
    const Expr* fieldExpression = currentDiagnosticExpression;
    if (message == "除数不能为零")
      if (auto* binary = dynamic_cast<const BinaryExpr*>(fieldExpression))
        fieldExpression = binary->rhs.get();
    auto field = diagnosticFieldPath(fieldExpression);
    builder.CreateCall(
        failFn,
        {detail ? detail : builder.CreateGlobalString(message), builder.CreateGlobalString(sourcePath),
         builder.getInt32(line), builder.getInt32(column), listIndex, tableRow,
         builder.CreateGlobalString(field)});
    builder.CreateUnreachable();
  }

  void emitMissingError(const std::string& contextName) {
    emitRuntimeError(contextName + "不能使用 missing");
  }

  void emitDiagnosticPush(const std::string& name) {
    auto push = external("lan_diagnostic_push", builder.getVoidTy(),
                         {llvmType(Type::Text)});
    builder.CreateCall(push, {builder.CreateGlobalString(name)});
  }

  void emitDiagnosticPop() {
    auto pop = external("lan_diagnostic_pop", builder.getVoidTy(), {});
    builder.CreateCall(pop, {});
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
                         builder.getInt32Ty(), llvmType(Type::Text),
                         builder.getInt32Ty()});
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
      case Type::Regex: return 6;
      case Type::Unit: return 7;
      case Type::IO: return 8;
      case Type::Instant: return 9;
      case Type::Result: return 10;
      case Type::Dynamic: break;
      case Type::Unknown: break;
    }
    throw std::runtime_error("内部错误：列表元素类型未知");
  }

  llvm::Value* adoptHeap(llvm::Value* pointer, Type type) {
    auto adopt = external("lan_arc_adopt", builder.getVoidTy(),
        {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
    builder.CreateCall(adopt,
        {builder.getInt32(tagFor(type)),
         llvm::ConstantFP::get(builder.getDoubleTy(), 0.0), pointer});
    return pointer;
  }

  TypedValue adoptValue(TypedValue value) {
    if (value.type != Type::Text && value.type != Type::List &&
        value.type != Type::Record && value.type != Type::Table &&
        value.type != Type::Dynamic && value.type != Type::Regex &&
        value.type != Type::Instant && value.type != Type::IO && value.type != Type::Result)
      return value;
    auto parts = dynamicParts(value);
    auto adopt = external("lan_arc_adopt", builder.getVoidTy(),
        {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
    builder.CreateCall(adopt, {parts[0], parts[1], parts[2]});
    return value;
  }

  void beginArcFrame() {
    auto push = external("lan_arc_push", builder.getVoidTy(), {});
    builder.CreateCall(push, {});
  }

  void endArcFrame(const TypedValue* returned = nullptr) {
    llvm::Value* tag = builder.getInt32(0);
    llvm::Value* number = llvm::ConstantFP::get(builder.getDoubleTy(), 0.0);
    llvm::Value* pointer = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(llvmType(Type::Text)));
    if (returned) {
      auto parts = dynamicParts(*returned);
      tag = parts[0];
      number = parts[1];
      pointer = parts[2];
    }
    auto pop = external("lan_arc_pop", builder.getVoidTy(),
        {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
    builder.CreateCall(pop, {tag, number, pointer});
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
    else if (value.type != Type::Unit) pointer = rawPayload(value);
    return {tag, number, pointer};
  }

  llvm::Value* emitRecord(const RecordExpr& record) {
    auto emptyFn = external("lan_record_empty", llvmType(Type::Record), {});
    auto assocFn = external("lan_record_assoc", llvmType(Type::Record),
                            {llvmType(Type::Record), llvmType(Type::Text),
                             builder.getInt32Ty(), builder.getDoubleTy(),
                             llvmType(Type::Text)});
    auto rootFn = external("lan_arc_record_root", llvmType(Type::Record), {});
    auto updateFn = external("lan_arc_record_update", builder.getVoidTy(),
                             {llvmType(Type::Record), llvmType(Type::Record)});
    auto* slot = builder.CreateCall(rootFn, {}, "record.ownership");
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
      builder.CreateCall(updateFn, {slot, updated});
      root = updated;
    }
    return root;
  }

  llvm::Value* fieldKey(const std::string& name, const Expr* key) {
    if (!key) return builder.CreateGlobalString(name);
    return requirePresent(emitExpr(*key), "动态字段名").value;
  }

  void emitFieldTypeError(Type expected) {
    emitRuntimeError("读取到的值不是" + std::string(typeName(expected)));
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
    if (expected == Type::Unit)
      return {builder.CreateNot(absent), expected, kMaybeMissing};
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
    auto* result = builder.CreateCall(
        assocFn, {record.value, key, parts[0], parts[1], parts[2]},
        "record.update");
    return {adoptHeap(result, Type::Record),
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
      auto* result = builder.CreateCall(
          assocFn, {records[i], keys[i], parts[0], parts[1], parts[2]},
          "record.nested.update");
      current = {adoptHeap(result, Type::Record),
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
    else if (storageType == Type::Boolean || storageType == Type::Unit)
      elementStorage = builder.getInt8Ty();
    else if (storageType == Type::Text || storageType == Type::List ||
             storageType == Type::Record || storageType == Type::Regex ||
             storageType == Type::Instant)
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
    builder.CreateStore(builder.getInt32(1),
                        builder.CreateStructGEP(listStorage, list, 4));
    auto noteAllocation = external("lan_arc_note_object_allocated",
                                   builder.getVoidTy(), {});
    builder.CreateCall(noteAllocation, {});
    adoptHeap(list, Type::List);
    return {list, data, presence, storageType};
  }

  void storeListElement(const ListAllocation& storage, llvm::Value* index,
                        const TypedValue& value) {
    if (storage.elementType == Type::Unit) {
      auto* slot = builder.CreateGEP(builder.getInt8Ty(), storage.data, index,
                                     "unit.output");
      builder.CreateStore(builder.CreateZExt(presentFlag(value), builder.getInt8Ty()),
                          slot);
      return;
    }
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
        storage.elementType == Type::Record || storage.elementType == Type::Regex ||
        storage.elementType == Type::Instant) {
      auto parts = dynamicParts(value);
      auto retain = external("lan_value_retain", builder.getVoidTy(),
          {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
      builder.CreateCall(retain, {parts[0], parts[1], parts[2]});
      auto* slot = builder.CreateGEP(llvmType(Type::Text), storage.data, index,
                                     "pointer.output");
      builder.CreateStore(rawPayload(value), slot);
      return;
    }
    auto* slot = builder.CreateGEP(taggedValueType(), storage.data, index,
                                   "element.output");
    auto parts = dynamicParts(value);
    auto retain = external("lan_value_retain", builder.getVoidTy(),
        {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
    builder.CreateCall(retain, {parts[0], parts[1], parts[2]});
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
    return {adoptHeap(row, Type::Record), Type::Record, kPresent};
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
    diagnosticCollections.push_back({index, receiver.type == Type::Table});
    auto value = emitElementBoundary(body);
    diagnosticCollections.pop_back();
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
      adoptHeap(merged, Type::Record);
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
      return {adoptHeap(filtered, receiver.type), receiver.type, kPresent};
    }

    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "rows.to.table.status");
    auto tableFn = external("lan_table_from_rows", llvmType(Type::Table),
                            {llvmType(Type::List),
                             llvm::PointerType::get(context, 0)});
    auto* table = builder.CreateCall(tableFn, {mapped, status}, "rows.to.table");
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "Table 的所有行必须具有相同字段和列类型");
    return {adoptHeap(table, Type::Table), Type::Table, kPresent};
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
    emitRuntimeError(message);
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
        dynamic_cast<const UnitExpr*>(&expression) ||
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
    return {adoptHeap(result, Type::Table), Type::Table, kPresent};
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
    return {adoptHeap(result, Type::Table), Type::Table, kPresent};
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
      return {adoptHeap(table, Type::Table), Type::Table, kPresent};
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

  void requireRuntimeSuccess(llvm::Value* status, const std::string& message,
                             bool distinguishDivisionByZero = false) {
    auto* function = builder.GetInsertBlock()->getParent();
    auto* validBlock = llvm::BasicBlock::Create(context, "operation.valid", function);
    auto* errorBlock = llvm::BasicBlock::Create(context, "operation.invalid", function);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                         validBlock, errorBlock);
    builder.SetInsertPoint(errorBlock);
    if (!distinguishDivisionByZero) {
      emitTableError(message);
    } else {
      auto* zeroBlock = llvm::BasicBlock::Create(
          context, "division.by.zero", function);
      auto* otherErrorBlock = llvm::BasicBlock::Create(
          context, "operation.type.invalid", function);
      builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(-4)),
                           zeroBlock, otherErrorBlock);
      builder.SetInsertPoint(zeroBlock);
      emitTableError("除数不能为零");
      builder.SetInsertPoint(otherErrorBlock);
      emitTableError(message);
    }
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
                  : binary.op == "%" ? 13
                  : binary.op == "==" ? 4 : binary.op == "!=" ? 5
                  : binary.op == "<" ? 6 : binary.op == ">" ? 7
                  : binary.op == "<=" ? 8 : binary.op == ">=" ? 9
                  : binary.op == "and" ? 10 : binary.op == "or" ? 11 : 12;
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "list.operation.status");
    unsigned leftRank = inference->expressionListRank(*binary.lhs);
    unsigned rightRank = inference->expressionListRank(*binary.rhs);
    auto leafType = [&](const Expr& expression, unsigned rank) {
      return rank == 0 ? inference->expressionType(expression)
                       : inference->expressionListElementType(expression);
    };
    Type leftLeaf = leafType(*binary.lhs, leftRank);
    Type rightLeaf = leafType(*binary.rhs, rightRank);
    bool compactBooleanPair = leftRank == 1 && rightRank == 1 &&
        leftLeaf == Type::Boolean && rightLeaf == Type::Boolean &&
        operation >= 4;
    if (compactBooleanPair) {
      auto operationFn = external(
          "lan_boolean_list_binary", llvmType(Type::List),
          {llvmType(Type::List), llvmType(Type::List), builder.getInt32Ty(),
           llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(
          operationFn, {left.value, right.value, builder.getInt32(operation), status},
          "boolean.list.operation");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "整列逻辑运算要求 Boolean 元素");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    bool compactBooleanScalar = std::max(leftRank, rightRank) == 1 &&
        std::min(leftRank, rightRank) == 0 && leftLeaf == Type::Boolean &&
        rightLeaf == Type::Boolean && operation >= 4;
    if (compactBooleanScalar) {
      bool listOnLeft = leftRank == 1;
      auto list = listOnLeft ? left : right;
      auto scalar = listOnLeft ? right : left;
      llvm::Value* encodedScalar = nullptr;
      if (scalar.presence == kPresent) {
        encodedScalar = builder.CreateAdd(
            builder.CreateZExt(rawPayload(scalar), builder.getInt32Ty()),
            builder.getInt32(1), "boolean.scalar.encoded");
      } else {
        encodedScalar = builder.CreateZExt(scalar.value, builder.getInt32Ty(),
                                           "boolean.scalar.encoded");
      }
      auto operationFn = external(
          "lan_boolean_list_scalar_binary", llvmType(Type::List),
          {llvmType(Type::List), builder.getInt32Ty(), builder.getInt32Ty(),
           builder.getInt32Ty(), llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(
          operationFn,
          {list.value, encodedScalar, builder.getInt32(listOnLeft ? 1 : 0),
           builder.getInt32(operation), status},
          "boolean.list.scalar.operation");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "整列逻辑广播要求 Boolean 元素");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    bool compactNumberPair = leftRank == 1 && rightRank == 1 &&
        leftLeaf == Type::Number && rightLeaf == Type::Number &&
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
                            "整列数字运算要求 Number 元素",
                            binary.op == "/" || binary.op == "%");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    bool compactNumberScalar = std::max(leftRank, rightRank) == 1 &&
        std::min(leftRank, rightRank) == 0 && leftLeaf == Type::Number &&
        rightLeaf == Type::Number && (operation <= 3 || operation == 13);
    if (compactNumberScalar) {
      bool listOnLeft = leftRank == 1;
      auto list = listOnLeft ? left : right;
      auto scalar = listOnLeft ? right : left;
      auto operationFn = external(
          "lan_number_list_scalar_binary", llvmType(Type::List),
          {llvmType(Type::List), builder.getDoubleTy(), builder.getInt32Ty(),
           builder.getInt32Ty(), llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(
          operationFn,
          {list.value, rawPayload(scalar), builder.getInt32(listOnLeft ? 1 : 0),
           builder.getInt32(operation), status},
          "number.list.scalar.operation");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "整列数字广播要求 Number 元素",
                            binary.op == "/" || binary.op == "%");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    auto compactLeaf = [](Type type) {
      return type == Type::Number || type == Type::Boolean ||
          type == Type::Text || type == Type::Instant;
    };
    bool compactFlat = std::max(leftRank, rightRank) == 1 &&
        compactLeaf(leftLeaf) && compactLeaf(rightLeaf);
    if (compactFlat) {
      auto operationFn = external(
          "lan_flat_list_binary", llvmType(Type::List),
          {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text),
           builder.getInt32Ty(), builder.getInt32Ty(), builder.getDoubleTy(),
           llvmType(Type::Text), builder.getInt32Ty(), builder.getInt32Ty(),
           llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(
          operationFn,
          {leftParts[0], leftParts[1], leftParts[2], builder.getInt32(leftRank),
           rightParts[0], rightParts[1], rightParts[2], builder.getInt32(rightRank),
           builder.getInt32(operation), status},
          "flat.list.operation");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "紧凑整列运算的元素类型不正确",
                            binary.op == "/" || binary.op == "%");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
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
                          "整列运算要求元素类型正确",
                          binary.op == "/" || binary.op == "%");
    return {adoptHeap(result, Type::List), Type::List, kPresent};
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
    return {adoptHeap(result, Type::List), Type::List, kPresent};
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
    return {adoptHeap(result, Type::List), Type::List, kPresent};
  }

  TypedValue emitRegexBuiltin(const CallExpr& call) {
    auto text = requirePresent(emitExpr(*call.args[0]), call.callee + " 的 Text");
    auto* pointer = llvmType(Type::Text);
    auto* error = builder.CreateAlloca(builder.getInt8Ty(), builder.getInt64(512),
                                       "regex.error");
    auto* output = builder.CreateAlloca(pointer, nullptr, "regex.output");
    auto* matched = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "regex.matched");
    llvm::Value* status;
    bool constructor = call.callee == "regex";
    if (constructor) {
      auto length = external("strlen", builder.getInt64Ty(), {pointer});
      auto compile = external("lan_regex_compile", builder.getInt32Ty(),
          {pointer, builder.getInt64Ty(), pointer, pointer, builder.getInt64Ty()});
      status = builder.CreateCall(compile,
          {text.value, builder.CreateCall(length, {text.value}), output, error,
           builder.getInt64(512)});
    } else {
      auto pattern = requirePresent(emitExpr(*call.args[1]), call.callee + " 的模式");
      int operation = call.callee == "regex_contains" ? 0
          : call.callee == "regex_matches" ? 1 : call.callee == "regex_find" ? 2
          : call.callee == "regex_find_all" ? 3 : 4;
      auto evaluate = external("lan_regex_evaluate", builder.getInt32Ty(),
          {pointer, pointer, builder.getInt32Ty(), pointer, pointer, pointer,
           builder.getInt64Ty()});
      status = builder.CreateCall(evaluate, {text.value, pattern.value,
          builder.getInt32(operation), matched, output, error, builder.getInt64(512)});
    }
    auto* function = builder.GetInsertBlock()->getParent();
    auto* valid = llvm::BasicBlock::Create(context, "regex.valid", function);
    auto* invalid = llvm::BasicBlock::Create(context, "regex.invalid", function);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)), valid, invalid);
    builder.SetInsertPoint(invalid);
    emitRuntimeError("regex 操作失败", error);
    builder.SetInsertPoint(valid);
    Type type = inference->expressionType(call);
    if (type == Type::Boolean)
      return {builder.CreateICmpNE(builder.CreateLoad(builder.getInt32Ty(), matched),
                                   builder.getInt32(0)), type, kPresent};
    if (constructor) {
      auto note = external("lan_arc_note_object_allocated", builder.getVoidTy(), {});
      builder.CreateCall(note, {});
    }
    auto* result = builder.CreateLoad(pointer, output);
    return {adoptHeap(result, type), type,
            call.callee == "regex_find" ? kMaybeMissing : kPresent};
  }

  TypedValue emitTextBuiltin(const CallExpr& call) {
    if (call.callee == "starts_with" || call.callee == "ends_with" ||
        call.callee == "text_contains") {
      auto text = requirePresent(emitExpr(*call.args[0]),
                                 "函数“" + call.callee + "”的 Text");
      auto part = requirePresent(emitExpr(*call.args[1]),
                                 "函数“" + call.callee + "”的匹配内容");
      const char* runtimeName = call.callee == "starts_with"
          ? "lan_text_starts_with"
          : call.callee == "ends_with" ? "lan_text_ends_with"
                                        : "lan_text_contains";
      auto function = external(
          runtimeName,
          builder.getInt32Ty(), {llvmType(Type::Text), llvmType(Type::Text)});
      auto* result = builder.CreateCall(function, {text.value, part.value},
                                        call.callee + ".result");
      return {builder.CreateICmpNE(result, builder.getInt32(0)),
              Type::Boolean, kPresent};
    }
    if (call.callee == "split") {
      auto text = requirePresent(emitExpr(*call.args[0]), "split 的 Text");
      auto separator = requirePresent(emitExpr(*call.args[1]), "split 的分隔符");
      auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                          "split.status");
      auto function = external("lan_text_split", llvmType(Type::List),
          {llvmType(Type::Text), llvmType(Type::Text),
           llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(
          function, {text.value, separator.value, status}, "split.result");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "split 的分隔符不能为空");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    if (call.callee == "join") {
      auto parts = requirePresent(emitExpr(*call.args[0]), "join 的 Text List");
      auto separator = requirePresent(emitExpr(*call.args[1]), "join 的分隔符");
      auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                          "join.status");
      auto function = external("lan_text_join", llvmType(Type::Text),
          {llvmType(Type::List), llvmType(Type::Text),
           llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(
          function, {parts.value, separator.value, status}, "join.result");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "join 的元素必须是存在的 Text");
      return {adoptHeap(result, Type::Text), Type::Text, kPresent};
    }
    auto text = requirePresent(emitExpr(*call.args[0]),
                               "函数“" + call.callee + "”的输入");
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        call.callee + ".status");
    auto function = external(
        call.callee == "trim" ? "lan_text_trim" : "lan_text_case",
        llvmType(Type::Text),
        call.callee == "trim"
            ? std::vector<llvm::Type*>{llvmType(Type::Text),
                                      llvm::PointerType::get(context, 0)}
            : std::vector<llvm::Type*>{llvmType(Type::Text), builder.getInt32Ty(),
                                      llvm::PointerType::get(context, 0)});
    llvm::Value* result = call.callee == "trim"
        ? builder.CreateCall(function, {text.value, status}, call.callee + ".result")
        : builder.CreateCall(function,
            {text.value, builder.getInt32(call.callee == "uppercase"), status},
            call.callee + ".result");
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "Text 包含无效的 UTF-8");
    return {adoptHeap(result, Type::Text), Type::Text, kPresent};
  }

  TypedValue emitNumberBuiltin(const CallExpr& call) {
    auto number = requirePresent(emitExpr(*call.args[0]),
                                 "函数“" + call.callee + "”的输入");
    auto* output = builder.CreateAlloca(builder.getDoubleTy(), nullptr,
                                        call.callee + ".output");
    llvm::Value* status = nullptr;
    if (call.callee == "abs" || call.callee == "floor" ||
        call.callee == "ceil" || call.callee == "sign" ||
        call.callee == "sqrt" || call.callee == "exp" ||
        call.callee == "sin" || call.callee == "cos" ||
        call.callee == "tan" || call.callee == "asin" ||
        call.callee == "acos" || call.callee == "atan" ||
        call.callee == "radians" || call.callee == "degrees") {
      int operation = call.callee == "abs" ? 0
                    : call.callee == "floor" ? 1
                    : call.callee == "ceil" ? 2
                    : call.callee == "sign" ? 3
                    : call.callee == "sqrt" ? 4
                    : call.callee == "exp" ? 5
                    : call.callee == "sin" ? 6
                    : call.callee == "cos" ? 7
                    : call.callee == "tan" ? 8
                    : call.callee == "asin" ? 9
                    : call.callee == "acos" ? 10
                    : call.callee == "atan" ? 11
                    : call.callee == "radians" ? 12 : 13;
      auto function = external("lan_number_unary", builder.getInt32Ty(),
          {builder.getDoubleTy(), builder.getInt32Ty(),
           llvm::PointerType::get(context, 0)});
      status = builder.CreateCall(function,
          {number.value, builder.getInt32(operation), output},
          call.callee + ".status");
    } else if (call.callee == "pow" || call.callee == "log" ||
               call.callee == "hypot" || call.callee == "atan2") {
      llvm::Value* right = nullptr;
      if (call.callee == "log" && call.args.size() == 1) {
        right = llvm::ConstantFP::get(builder.getDoubleTy(),
                                     0x1.5bf0a8b145769p+1);
      } else {
        right = requirePresent(emitExpr(*call.args[1]),
                               "函数“" + call.callee + "”的第二个输入").value;
      }
      int operation = call.callee == "pow" ? 0
                    : call.callee == "log" ? 1
                    : call.callee == "hypot" ? 2 : 3;
      auto function = external("lan_number_binary", builder.getInt32Ty(),
          {builder.getDoubleTy(), builder.getDoubleTy(), builder.getInt32Ty(),
           llvm::PointerType::get(context, 0)});
      status = builder.CreateCall(
          function, {number.value, right, builder.getInt32(operation), output},
          call.callee + ".status");
    } else if (call.callee == "round") {
      llvm::Value* digits = llvm::ConstantFP::get(builder.getDoubleTy(), 0.0);
      if (call.args.size() == 2)
        digits = requirePresent(emitExpr(*call.args[1]),
                                "round 的小数位数").value;
      auto function = external("lan_number_round", builder.getInt32Ty(),
          {builder.getDoubleTy(), builder.getDoubleTy(),
           llvm::PointerType::get(context, 0)});
      status = builder.CreateCall(function, {number.value, digits, output},
                                  "round.status");
    } else {
      auto minimum = requirePresent(emitExpr(*call.args[1]), "clamp 的最小值");
      auto maximum = requirePresent(emitExpr(*call.args[2]), "clamp 的最大值");
      auto function = external("lan_number_clamp", builder.getInt32Ty(),
          {builder.getDoubleTy(), builder.getDoubleTy(), builder.getDoubleTy(),
           llvm::PointerType::get(context, 0)});
      status = builder.CreateCall(
          function, {number.value, minimum.value, maximum.value, output},
          "clamp.status");
    }
    std::string error = "数学函数要求有限 Number，并且结果必须有限";
    if (call.callee == "round")
      error = "round 的小数位数必须是 -308 到 308 之间的整数";
    else if (call.callee == "clamp")
      error = "clamp 要求有限 Number 且最小值不能大于最大值";
    else if (call.callee == "sqrt")
      error = "sqrt 要求不小于零的有限 Number";
    else if (call.callee == "pow")
      error = "pow 的定义域无效或结果不是有限 Number";
    else if (call.callee == "log")
      error = "log 要求正数，且底数必须为正数并且不能等于 1";
    else if (call.callee == "asin" || call.callee == "acos")
      error = "函数“" + call.callee + "”要求 -1 到 1 之间的有限 Number";
    requireRuntimeSuccess(status, error);
    return {builder.CreateLoad(builder.getDoubleTy(), output,
                               call.callee + ".result"),
            Type::Number, kPresent};
  }

  TypedValue emitConversionBuiltin(const CallExpr& call) {
    auto input = requirePresent(emitExpr(*call.args[0]),
                                "函数“" + call.callee + "”的输入");
    if (call.callee == "to_text") {
      auto parts = dynamicParts(input);
      auto function = external("lan_value_to_text", llvmType(Type::Text),
          {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
      auto* result = builder.CreateCall(
          function, {parts[0], parts[1], parts[2]}, "text.value");
      requireRuntimeSuccess(builder.CreateSelect(builder.CreateIsNull(result),
          builder.getInt32(-1), builder.getInt32(0)),
          "to_text 不支持 Regex 或包含 Regex 的集合");
      return {adoptHeap(result, Type::Text),
              Type::Text, kPresent};
    }
    auto* output = builder.CreateAlloca(builder.getDoubleTy(), nullptr,
                                        "parsed.number");
    auto function = external("lan_parse_number", builder.getInt32Ty(),
        {llvmType(Type::Text), llvm::PointerType::get(context, 0)});
    auto* valid = builder.CreateICmpNE(
        builder.CreateCall(function, {input.value, output}), builder.getInt32(0),
        "number.parsed");
    llvm::Value* option = llvm::UndefValue::get(maybeNumberType());
    option = builder.CreateInsertValue(option, valid, 0);
    option = builder.CreateInsertValue(
        option, builder.CreateLoad(builder.getDoubleTy(), output), 1);
    return {option, Type::Number, kMaybeMissing};
  }

  TypedValue emitTimeBuiltin(const CallExpr& call) {
    auto first = requirePresent(emitExpr(*call.args[0]),
                                "函数“" + call.callee + "”的第一个参数");
    auto* pointer = llvmType(Type::Text);
    if (call.callee == "parse_time") {
      auto* output = builder.CreateAlloca(pointer, nullptr, "parsed.time");
      builder.CreateStore(llvm::ConstantPointerNull::get(
          llvm::cast<llvm::PointerType>(pointer)), output);
      auto function = external("lan_parse_time", builder.getInt32Ty(),
                               {pointer, llvm::PointerType::get(context, 0)});
      builder.CreateCall(function, {first.value, output});
      auto* result = builder.CreateLoad(pointer, output, "time.value");
      return {adoptHeap(result, Type::Instant), Type::Instant, kMaybeMissing};
    }
    if (call.callee == "format_time") {
      auto function = external("lan_format_time", pointer, {pointer});
      auto* result = builder.CreateCall(function, {first.value}, "formatted.time");
      return {adoptHeap(result, Type::Text), Type::Text, kPresent};
    }
    if (call.callee == "unix_seconds") {
      auto function = external("lan_unix_seconds", builder.getDoubleTy(), {pointer});
      return {builder.CreateCall(function, {first.value}, "unix.seconds"),
              Type::Number, kPresent};
    }
    if (call.callee == "seconds_between") {
      auto second = requirePresent(emitExpr(*call.args[1]),
                                   "seconds_between 的结束时间");
      auto function = external("lan_seconds_between", builder.getDoubleTy(),
                               {pointer, pointer});
      return {builder.CreateCall(function, {first.value, second.value},
                                 "seconds.between"),
              Type::Number, kPresent};
    }

    auto* output = builder.CreateAlloca(pointer, nullptr, "time.result");
    llvm::Value* status;
    if (call.callee == "time_from_unix") {
      auto function = external("lan_time_from_unix", builder.getInt32Ty(),
          {builder.getDoubleTy(), llvm::PointerType::get(context, 0)});
      status = builder.CreateCall(function, {first.value, output});
    } else {
      auto added = requirePresent(emitExpr(*call.args[1]),
                                  "add_seconds 的秒数");
      auto function = external("lan_add_seconds", builder.getInt32Ty(),
          {pointer, builder.getDoubleTy(), llvm::PointerType::get(context, 0)});
      status = builder.CreateCall(function, {first.value, added.value, output});
    }
    requireRuntimeSuccess(status,
        call.callee == "time_from_unix"
            ? "time_from_unix 要求有限且处于 Instant 支持范围内的 Unix 秒数"
            : "add_seconds 要求有限秒数，且结果不能超出 Instant 支持范围");
    auto* result = builder.CreateLoad(pointer, output, "time.value");
    return {adoptHeap(result, Type::Instant), Type::Instant, kPresent};
  }

  TypedValue emitListBuiltin(const CallExpr& call) {
    auto input = requirePresent(emitExpr(*call.args[0]),
                                "函数“" + call.callee + "”的 List");
    if (call.callee == "reverse") {
      auto function = external("lan_list_reverse", llvmType(Type::List),
                               {llvmType(Type::List)});
      auto* result = builder.CreateCall(function, {input.value}, "reversed.list");
      return {adoptHeap(result, Type::List),
              Type::List, kPresent};
    }
    if (call.callee == "distinct") {
      auto function = external("lan_list_distinct", llvmType(Type::List),
                               {llvmType(Type::List)});
      auto* result = builder.CreateCall(function, {input.value}, "distinct.list");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    if (call.callee == "flatten") {
      auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                          "list.flatten.status");
      auto function = external("lan_list_flatten", llvmType(Type::List),
          {llvmType(Type::List), llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(function, {input.value, status},
                                        "flattened.list");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "flatten 的子列表不能是 missing");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    if (call.callee == "sort") {
      auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                          "list.sort.status");
      auto function = external("lan_list_sort", llvmType(Type::List),
          {llvmType(Type::List), llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(function, {input.value, status},
                                        "sorted.list");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "sort 只能用于 Number、Text 或 Instant List");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    if (call.callee == "concat") {
      auto right = requirePresent(emitExpr(*call.args[1]), "concat 的右侧 List");
      auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                          "concat.status");
      auto function = external("lan_list_concat", llvmType(Type::List),
          {llvmType(Type::List), llvmType(Type::List),
           llvm::PointerType::get(context, 0)});
      auto* result = builder.CreateCall(
          function, {input.value, right.value, status}, "concatenated.list");
      requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                            "concat 的两个 List 必须具有相同元素类型");
      return {adoptHeap(result, Type::List), Type::List, kPresent};
    }
    auto amount = requirePresent(emitExpr(*call.args[1]),
                                 "函数“" + call.callee + "”的数量");
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "list.slice.status");
    auto function = external("lan_list_slice", llvmType(Type::List),
        {llvmType(Type::List), builder.getDoubleTy(), builder.getInt32Ty(),
         llvm::PointerType::get(context, 0)});
    auto* result = builder.CreateCall(function,
        {input.value, amount.value, builder.getInt32(call.callee == "drop"), status},
        "sliced.list");
    requireRuntimeSuccess(builder.CreateLoad(builder.getInt32Ty(), status),
                          "take/drop 的数量必须是非负整数");
    return {adoptHeap(result, Type::List), Type::List, kPresent};
  }

  TypedValue emitRecordBuiltin(const CallExpr& call) {
    auto record = requirePresent(emitExpr(*call.args[0]),
                                 "函数“" + call.callee + "”的 Record");
    if (call.callee == "fields") {
      auto function = external("lan_record_fields", llvmType(Type::List),
                               {llvmType(Type::Record)});
      auto* result = builder.CreateCall(function, {record.value}, "record.fields");
      return {adoptHeap(result, Type::List),
              Type::List, kPresent};
    }
    if (call.callee == "merge") {
      auto right = requirePresent(emitExpr(*call.args[1]), "merge 的右侧 Record");
      auto function = external("lan_record_merge", llvmType(Type::Record),
          {llvmType(Type::Record), llvmType(Type::Record)});
      auto* result = builder.CreateCall(function, {record.value, right.value},
                                        "merged.record");
      return {adoptHeap(result, Type::Record),
              Type::Record, kPresent};
    }
    auto key = requirePresent(
        emitExpr(*call.args[1]),
        call.callee == "has_field" ? "has_field 的字段名"
                                   : "remove_field 的字段名");
    if (call.callee == "remove_field") {
      auto function = external("lan_record_remove_field", llvmType(Type::Record),
          {llvmType(Type::Record), llvmType(Type::Text)});
      auto* result = builder.CreateCall(function, {record.value, key.value},
                                        "record.without.field");
      return {adoptHeap(result, Type::Record),
              Type::Record, kPresent};
    }
    auto function = external("lan_record_has_field", builder.getInt32Ty(),
        {llvmType(Type::Record), llvmType(Type::Text)});
    auto* found = builder.CreateCall(function, {record.value, key.value},
                                     "record.has.field");
    return {builder.CreateICmpNE(found, builder.getInt32(0)),
            Type::Boolean, kPresent};
  }

  TypedValue emitTableTransform(const CallExpr& call) {
    const std::string& displayName = call.callee;
    auto table = requirePresent(emitExpr(*call.args[0]),
                                "操作“" + displayName + "”的 Table");
    auto* status = builder.CreateAlloca(builder.getInt32Ty(), nullptr,
                                        "table.operation.status");
    llvm::Value* result = nullptr;
    if (call.callee == "select_columns") {
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
    return {adoptHeap(result, Type::Table), Type::Table, kPresent};
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
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "start", printValueFunction));
    auto print = external("lan_value_print", builder.getVoidTy(),
                          {builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
    builder.CreateCall(print, {printValueFunction->getArg(0),
                              printValueFunction->getArg(1),
                              printValueFunction->getArg(2)});
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
    auto function = external("lan_text_concat", llvmType(Type::Text),
        {llvmType(Type::Text), llvmType(Type::Text)});
    auto* result = builder.CreateCall(function, {lhs, rhs}, "text.concat");
    return adoptHeap(result, Type::Text);
  }

  llvm::Value* floorModulo(llvm::Value* lhs, llvm::Value* rhs) {
    auto* remainder = builder.CreateFRem(lhs, rhs, "modulo.remainder");
    auto* zero = llvm::ConstantFP::get(builder.getDoubleTy(), 0.0);
    auto* nonzero = builder.CreateFCmpONE(remainder, zero, "modulo.nonzero");
    auto* remainderNegative = builder.CreateFCmpOLT(
        remainder, zero, "modulo.remainder.negative");
    auto* divisorNegative = builder.CreateFCmpOLT(
        rhs, zero, "modulo.divisor.negative");
    auto* adjust = builder.CreateAnd(
        nonzero, builder.CreateXor(remainderNegative, divisorNegative),
        "modulo.adjust");
    auto* adjusted = builder.CreateSelect(
        adjust, builder.CreateFAdd(remainder, rhs, "modulo.adjusted"), remainder,
        "modulo.signed");
    return builder.CreateSelect(builder.CreateFCmpOEQ(adjusted, zero), zero,
                                adjusted, "modulo.normalized");
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

  llvm::Value* resultIsOk(const TypedValue& value) {
    auto test = external("lan_result_is_ok", builder.getInt32Ty(), {llvmType(Type::Result)});
    return builder.CreateICmpNE(builder.CreateCall(test, {rawPayload(value)}), builder.getInt32(0));
  }

  TypedValue resultPayload(const TypedValue& value, Type type) {
    auto* slot = builder.CreateAlloca(taggedValueType(), nullptr, "result.payload");
    auto get = external("lan_result_payload", builder.getVoidTy(),
                         {llvmType(Type::Result), llvmType(Type::Text)});
    builder.CreateCall(get, {rawPayload(value), slot});
    return decodeLookupValue(builder.getTrue(), builder.CreateLoad(taggedValueType(), slot), type);
  }

  TypedValue emitResult(const CallExpr& call) {
    return makeResult(emitExpr(*call.args[0]), call.callee == "ok");
  }

  TypedValue makeResult(const TypedValue& payload, bool success) {
    auto parts = dynamicParts(payload);
    auto construct = external("lan_result_new", llvmType(Type::Result),
        {builder.getInt32Ty(), builder.getInt32Ty(), builder.getDoubleTy(), llvmType(Type::Text)});
    auto* value = builder.CreateCall(construct,
        {builder.getInt32(success), parts[0], parts[1], parts[2]}, "result");
    return {adoptHeap(value, Type::Result), Type::Result, kPresent};
  }

  TypedValue emitDo(const DoExpr& block) {
    const bool resultDo = inference->isResultDo(block);
    const Type outputType = inference->expressionType(block);
    const Presence outputPresence = inference->expressionPresence(block);
    auto* function = builder.GetInsertBlock()->getParent();
    auto* merge = llvm::BasicBlock::Create(context, "do.result", function);
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;

    std::function<void(const Expr&)> emitStep = [&](const Expr& step) {
      if (auto* let = dynamic_cast<const LetExpr*>(&step)) {
        auto value = emitElementBoundary(*let->value);
        scopes.emplace_back();
        scopes.back().emplace(let->name, value);
        emitStep(*let->body);
        scopes.pop_back();
        return;
      }
      if (auto* bind = dynamic_cast<const DoBindExpr*>(&step)) {
        auto input = emitElementBoundary(*bind->input);
        auto* next = llvm::BasicBlock::Create(context, "do.next", function);
        auto* stop = llvm::BasicBlock::Create(context, "do.stop", function);
        llvm::Value* continues = resultDo ? resultIsOk(input) : presentFlag(input);
        builder.CreateCondBr(continues, next, stop);

        builder.SetInsertPoint(stop);
        TypedValue stopped = resultDo
            ? input
            : TypedValue{missingValue(outputType), outputType, kMissing};
        stopped = convertPresence(stopped, outputPresence);
        incoming.push_back({stopped.value, builder.GetInsertBlock()});
        builder.CreateBr(merge);

        builder.SetInsertPoint(next);
        TypedValue payload = resultDo
            ? resultPayload(input,
                inference->resultPayloadType(*bind->input, false))
            : TypedValue{rawPayload(input), input.type, kPresent};
        scopes.emplace_back();
        scopes.back().emplace(bind->name, payload);
        emitStep(*bind->body);
        scopes.pop_back();
        return;
      }
      auto* final = dynamic_cast<const DoResultExpr*>(&step);
      if (!final) throw std::runtime_error("内部错误：do 块包含未知步骤");
      auto value = emitElementBoundary(*final->value);
      if (resultDo) value = makeResult(value, true);
      value = convertPresence(value, outputPresence);
      incoming.push_back({value.value, builder.GetInsertBlock()});
      builder.CreateBr(merge);
    };

    emitStep(*block.body);
    builder.SetInsertPoint(merge);
    auto* result = builder.CreatePHI(encodedType(outputType, outputPresence),
                                     incoming.size(), "do.value");
    for (const auto& [value, source] : incoming) result->addIncoming(value, source);
    return {result, outputType, outputPresence};
  }

  TypedValue emitRequire(const CallExpr& call) {
    auto input = requirePresent(emitExpr(*call.args[0]), "require 的输入");
    if (input.type != Type::Result) return input;
    auto* function = builder.GetInsertBlock()->getParent();
    auto* success = llvm::BasicBlock::Create(context, "require.ok", function);
    auto* failure = llvm::BasicBlock::Create(context, "require.err", function);
    builder.CreateCondBr(resultIsOk(input), success, failure);
    builder.SetInsertPoint(failure);
    auto format = external("lan_result_error_text", llvmType(Type::Text), {llvmType(Type::Result)});
    auto* detail = adoptHeap(builder.CreateCall(format, {rawPayload(input)}), Type::Text);
    emitRuntimeError("require 收到 err", detail);
    builder.SetInsertPoint(success);
    return resultPayload(input, inference->expressionType(call));
  }

  TypedValue emitMatch(const MatchExpr& match) {
    auto subject = emitExpr(*match.subject);  // Evaluated and owned exactly once.
    auto type = inference->expressionType(match);
    auto presence = inference->expressionPresence(match);
    auto* function = builder.GetInsertBlock()->getParent();
    auto* merge = llvm::BasicBlock::Create(context, "match.result", function);
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
    MatchCoverage covered;
    using Kind = MatchPattern::Kind;
    for (const auto& arm : match.arms) {
      auto* branch = llvm::BasicBlock::Create(context, "match.arm", function);
      auto* next = llvm::BasicBlock::Create(context, "match.next", function);
      auto kind = arm.pattern.kind;
      llvm::Value* matches = builder.getTrue();
      if (kind == Kind::Missing) matches = builder.CreateNot(presentFlag(subject));
      else if (kind == Kind::Present) matches = presentFlag(subject);
      else if (resultPattern(arm.pattern)) {
        auto* success = resultIsOk(subject);
        matches = builder.CreateAnd(presentFlag(subject),
            kind == Kind::Ok ? success : builder.CreateNot(success));
      }
      else if (kind == Kind::Literal) {
        // Never inspect the payload of a missing value. In particular it may
        // be a null Text pointer; boolean 'and' alone would not guard strcmp.
        auto* compare = llvm::BasicBlock::Create(context, "match.literal", function);
        builder.CreateCondBr(presentFlag(subject), compare, next);
        builder.SetInsertPoint(compare);
        auto literal = emitExpr(*arm.pattern.literal);
        if (subject.type == Type::Text) {
          auto strcmpFn = external("strcmp", builder.getInt32Ty(),
                                   {llvmType(Type::Text), llvmType(Type::Text)});
          matches = builder.CreateICmpEQ(builder.CreateCall(strcmpFn,
              {rawPayload(subject), rawPayload(literal)}), builder.getInt32(0));
        } else if (subject.type == Type::Number) {
          matches = builder.CreateFCmpOEQ(rawPayload(subject), rawPayload(literal));
        } else {
          matches = builder.CreateICmpEQ(rawPayload(subject), rawPayload(literal));
        }
      }
      builder.CreateCondBr(matches, branch, next);
      builder.SetInsertPoint(branch);
      Presence narrowed = subject.presence;
      if (kind == Kind::Missing) narrowed = kMissing;
      else if (resultPattern(arm.pattern) || kind == Kind::Present ||
               kind == Kind::Literal || covered.missing)
        narrowed = kPresent;
      else if (covered.present) narrowed = kMissing;
      TypedValue bound = narrowed == kMissing
          ? TypedValue{missingValue(subject.type), subject.type, kMissing}
          : narrowed == kPresent
              ? TypedValue{rawPayload(subject), subject.type, kPresent} : subject;
      scopes.emplace_back();
      if (auto* variable = dynamic_cast<const VariableExpr*>(match.subject.get()))
        scopes.back()[variable->name] = bound;
      if (bindsName(arm.pattern)) {
        if (resultPattern(arm.pattern))
          bound = resultPayload(subject, inference->resultPayloadType(
              *match.subject, kind == Kind::Err));
        scopes.back()[arm.pattern.name] = bound;
      }
      auto value = convertPresence(emitExpr(*arm.body), presence);
      scopes.pop_back();
      incoming.push_back({value.value, builder.GetInsertBlock()});
      builder.CreateBr(merge);
      builder.SetInsertPoint(next);
      covered.add(arm.pattern);
    }
    // Static coverage checking guarantees that this edge cannot be taken.
    builder.CreateUnreachable();
    builder.SetInsertPoint(merge);
    auto* result = builder.CreatePHI(encodedType(type, presence), incoming.size(), "matched");
    for (const auto& [value, block] : incoming) result->addIncoming(value, block);
    return {result, type, presence};
  }

  TypedValue emitLogical(const BinaryExpr& binary) {
    auto lhs = requirePresent(emitExpr(*binary.lhs), "逻辑运算左侧");
    auto* function = builder.GetInsertBlock()->getParent();
    auto* rhsBlock = llvm::BasicBlock::Create(context, "logic.next", function);
    auto* shortcutBlock = llvm::BasicBlock::Create(context, "logic.shortcut", function);
    auto* mergeBlock = llvm::BasicBlock::Create(context, "logic.result", function);
    const bool isAnd = binary.op == "and";
    if (isAnd) builder.CreateCondBr(rawPayload(lhs), rhsBlock, shortcutBlock);
    else builder.CreateCondBr(rawPayload(lhs), shortcutBlock, rhsBlock);

    builder.SetInsertPoint(rhsBlock);
    auto rhs = requirePresent(emitExpr(*binary.rhs), "逻辑运算右侧");
    auto* rhsEnd = builder.GetInsertBlock();
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(shortcutBlock);
    builder.CreateBr(mergeBlock);

    builder.SetInsertPoint(mergeBlock);
    auto* result = builder.CreatePHI(builder.getInt1Ty(), 2, "logic");
    result->addIncoming(rawPayload(rhs), rhsEnd);
    result->addIncoming(builder.getInt1(!isAnd), shortcutBlock);
    return {result, Type::Boolean, kPresent};
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

  TypedValue emitStandardIOBuiltin(const CallExpr& call) {
    if (call.callee == "now" || call.callee == "random" ||
        call.callee == "random_between" ||
        call.callee == "random_integer" || call.callee == "choose" ||
        call.callee == "shuffle")
      return emitStatefulIOBuiltin(call);
    bool write = call.callee == "write_stdout";
    llvm::Value* text = write
        ? requirePresent(emitExpr(*call.args[0]), "write_stdout 的输入").value : nullptr;
    auto savedIP = builder.saveIP();
    auto savedCollections = std::move(diagnosticCollections);
    diagnosticCollections.clear();
    auto* pointer = llvmType(Type::Text);
    auto* signature = llvm::FunctionType::get(builder.getVoidTy(), {pointer, pointer}, false);
    auto* callback = llvm::Function::Create(signature, llvm::Function::InternalLinkage,
                                            "lan.io." + call.callee, module.get());
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "io.start", callback));
    emitDiagnosticPush(call.callee);
    auto* error = builder.CreateAlloca(builder.getInt8Ty(), builder.getInt64(512), "io.error");
    std::vector<llvm::Type*> types;
    std::vector<llvm::Value*> args;
    if (write) {
      auto capture = external("lan_io_capture_at", pointer, {pointer, builder.getInt64Ty()});
      auto* storage = builder.CreateCall(capture, {callback->getArg(0), builder.getInt64(0)});
      auto* value = builder.CreateLoad(taggedValueType(), storage);
      types.push_back(pointer);
      args.push_back(builder.CreateExtractValue(value, 2));
    }
    types.insert(types.end(), {pointer, pointer, builder.getInt64Ty()});
    args.insert(args.end(), {callback->getArg(1), error, builder.getInt64(512)});
    auto operation = external("lan_io_" + call.callee, builder.getInt32Ty(), types);
    auto* status = builder.CreateCall(operation, args);
    auto* success = llvm::BasicBlock::Create(context, "io.success", callback);
    auto* failure = llvm::BasicBlock::Create(context, "io.failure", callback);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)), success, failure);
    builder.SetInsertPoint(failure);
    emitRuntimeError(call.callee, error);
    builder.SetInsertPoint(success);
    emitDiagnosticPop();
    builder.CreateRetVoid();
    diagnosticCollections = std::move(savedCollections);
    builder.restoreIP(savedIP);
    auto create = external("lan_io_new", pointer, {pointer, builder.getInt64Ty()});
    auto* action = adoptHeap(builder.CreateCall(create,
        {callback, builder.getInt64(write ? 1 : 0)}, "io.action"), Type::IO);
    if (write) {
      auto capture = external("lan_io_capture", builder.getVoidTy(),
          {pointer, builder.getInt64Ty(), builder.getInt32Ty(), builder.getDoubleTy(), pointer});
      builder.CreateCall(capture, {action, builder.getInt64(0), builder.getInt32(tagFor(Type::Text)),
          llvm::ConstantFP::get(builder.getDoubleTy(), 0.0), text});
    }
    return {action, Type::IO, kPresent};
  }

  TypedValue emitStatefulIOBuiltin(const CallExpr& call) {
    std::vector<TypedValue> captures;
    for (const auto& argument : call.args)
      captures.push_back(requirePresent(emitExpr(*argument),
          "函数“" + call.callee + "”的参数"));

    auto savedIP = builder.saveIP();
    auto savedCollections = std::move(diagnosticCollections);
    diagnosticCollections.clear();
    auto* pointer = llvmType(Type::Text);
    auto* signature = llvm::FunctionType::get(builder.getVoidTy(), {pointer, pointer}, false);
    auto* callback = llvm::Function::Create(signature, llvm::Function::InternalLinkage,
                                            "lan.io." + call.callee, module.get());
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "io.start", callback));
    emitDiagnosticPush(call.callee);
    auto* error = builder.CreateAlloca(builder.getInt8Ty(), builder.getInt64(512), "io.error");
    std::vector<llvm::Type*> types;
    std::vector<llvm::Value*> args;
    if (!captures.empty()) {
      auto captureAt = external("lan_io_capture_at", pointer,
                                {pointer, builder.getInt64Ty()});
      for (std::size_t i = 0; i < captures.size(); ++i) {
        auto* storage = builder.CreateCall(
            captureAt, {callback->getArg(0), builder.getInt64(i)});
        auto* value = builder.CreateLoad(taggedValueType(), storage);
        if (captures[i].type == Type::Number) {
          types.push_back(builder.getDoubleTy());
          args.push_back(builder.CreateExtractValue(value, 1));
        } else {
          types.push_back(pointer);
          args.push_back(builder.CreateExtractValue(value, 2));
        }
      }
    }
    types.insert(types.end(), {pointer, pointer, builder.getInt64Ty()});
    args.insert(args.end(), {callback->getArg(1), error, builder.getInt64(512)});
    auto operation = external("lan_io_" + call.callee, builder.getInt32Ty(), types);
    auto* status = builder.CreateCall(operation, args);
    auto* success = llvm::BasicBlock::Create(context, "io.success", callback);
    auto* failure = llvm::BasicBlock::Create(context, "io.failure", callback);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)), success, failure);
    builder.SetInsertPoint(failure);
    emitRuntimeError(call.callee, error);
    builder.SetInsertPoint(success);
    emitDiagnosticPop();
    builder.CreateRetVoid();
    diagnosticCollections = std::move(savedCollections);
    builder.restoreIP(savedIP);

    auto create = external("lan_io_new", pointer, {pointer, builder.getInt64Ty()});
    auto* action = adoptHeap(builder.CreateCall(create,
        {callback, builder.getInt64(captures.size())}, "io.action"), Type::IO);
    auto capture = external("lan_io_capture", builder.getVoidTy(),
        {pointer, builder.getInt64Ty(), builder.getInt32Ty(), builder.getDoubleTy(), pointer});
    for (std::size_t i = 0; i < captures.size(); ++i) {
      auto parts = dynamicParts(captures[i]);
      builder.CreateCall(capture,
          {action, builder.getInt64(i), parts[0], parts[1], parts[2]});
    }
    return {action, Type::IO, kPresent};
  }

  TypedValue executeIO(TypedValue action, Type resultType) {
    action = requirePresent(action, "执行的 IO action");
    auto* output = builder.CreateAlloca(taggedValueType(), nullptr, "io.result");
    auto run = external("lan_io_run", builder.getVoidTy(),
                         {llvmType(Type::IO), llvmType(Type::Text)});
    builder.CreateCall(run, {action.value, output});
    auto* dynamic = builder.CreateLoad(taggedValueType(), output);
    return adoptValue(decodeLookupValue(builder.getTrue(), dynamic, resultType));
  }

  TypedValue emitIO(const IOExpr& io) {
    std::vector<std::pair<std::string, TypedValue>> captures;
    for (const auto& name : freeVariables(*io.body)) {
      bool found = false;
      for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (auto value = scope->find(name); value != scope->end()) {
          captures.emplace_back(name, value->second);
          found = true;
          break;
        }
      }
      if (!found)
        if (const auto* row = lazyTableRow(name))
          captures.emplace_back(name, tableRowElement(row->table, row->index));
    }

    auto savedIP = builder.saveIP();
    auto savedScopes = std::move(scopes);
    auto savedRows = std::move(lazyTableRows);
    auto savedCollections = std::move(diagnosticCollections);
    auto* savedMissing = currentMissingTarget;
    auto* savedRoot = rootMissingTarget;
    auto* savedDiagnostic = currentDiagnosticExpression;
    scopes.clear();
    scopes.emplace_back();
    lazyTableRows.clear();
    diagnosticCollections.clear();
    currentMissingTarget = rootMissingTarget = nullptr;
    auto* pointer = llvmType(Type::Text);
    auto* signature = llvm::FunctionType::get(builder.getVoidTy(), {pointer, pointer}, false);
    auto* callback = llvm::Function::Create(signature, llvm::Function::InternalLinkage,
                                            "lan.io.action", module.get());
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "io.start", callback));
    beginArcFrame();
    emitDiagnosticPush("io@" + std::to_string(io.line) + ":" + std::to_string(io.column));
    auto readCapture = external("lan_io_capture_at", pointer,
                                 {pointer, builder.getInt64Ty()});
    for (std::size_t i = 0; i < captures.size(); ++i) {
      auto* storage = builder.CreateCall(readCapture,
          {callback->getArg(0), builder.getInt64(i)}, "io.capture");
      auto value = decodeLookupValue(builder.getTrue(),
          builder.CreateLoad(taggedValueType(), storage), captures[i].second.type);
      scopes.back().emplace(captures[i].first,
                            convertPresence(value, captures[i].second.presence));
    }
    auto result = emitExpr(*io.body);
    auto parts = dynamicParts(result);
    llvm::Value* dynamic = llvm::UndefValue::get(taggedValueType());
    for (unsigned i = 0; i < parts.size(); ++i)
      dynamic = builder.CreateInsertValue(dynamic, parts[i], i);
    builder.CreateStore(dynamic, callback->getArg(1));
    emitDiagnosticPop();
    endArcFrame(&result);
    builder.CreateRetVoid();
    if (rootMissingTarget)
      throw std::runtime_error("内部错误：IO 步骤缺少表达式边界");

    scopes = std::move(savedScopes);
    lazyTableRows = std::move(savedRows);
    diagnosticCollections = std::move(savedCollections);
    currentMissingTarget = savedMissing;
    rootMissingTarget = savedRoot;
    currentDiagnosticExpression = savedDiagnostic;
    builder.restoreIP(savedIP);
    auto create = external("lan_io_new", pointer, {pointer, builder.getInt64Ty()});
    auto* action = adoptHeap(builder.CreateCall(create,
        {callback, builder.getInt64(captures.size())}, "io.action"), Type::IO);
    auto capture = external("lan_io_capture", builder.getVoidTy(),
        {pointer, builder.getInt64Ty(), builder.getInt32Ty(), builder.getDoubleTy(), pointer});
    for (std::size_t i = 0; i < captures.size(); ++i) {
      auto parts = dynamicParts(captures[i].second);
      builder.CreateCall(capture,
          {action, builder.getInt64(i), parts[0], parts[1], parts[2]});
    }
    return {action, Type::IO, kPresent};
  }

  TypedValue emitExpr(const Expr& expr) {
    struct DiagnosticExpressionScope {
      const Expr*& current;
      const Expr* previous;
      DiagnosticExpressionScope(const Expr*& slot, const Expr& expression)
          : current(slot), previous(slot) {
        current = &expression;
      }
      ~DiagnosticExpressionScope() { current = previous; }
    } diagnosticScope(currentDiagnosticExpression, expr);
    Type type = inference->expressionType(expr);
    Presence presence = inference->expressionPresence(expr);
    if (auto* io = dynamic_cast<const IOExpr*>(&expr)) return emitIO(*io);
    if (auto* block = dynamic_cast<const DoExpr*>(&expr)) return emitDo(*block);
    if (auto* execution = dynamic_cast<const IOBindExpr*>(&expr)) {
      auto action = emitElementBoundary(*execution->action);
      auto value = executeIO(action, inference->ioResultType(*execution->action));
      scopes.emplace_back();
      if (!execution->name.empty()) scopes.back().emplace(execution->name, value);
      auto result = emitExpr(*execution->body);
      scopes.pop_back();
      return convertPresence(result, presence);
    }
    if (auto* final = dynamic_cast<const IOResultExpr*>(&expr)) {
      auto value = emitElementBoundary(*final->value);
      if (value.type == Type::IO) value = executeIO(value, type);
      return convertPresence(value, presence);
    }
    if (auto* number = dynamic_cast<const NumberExpr*>(&expr))
      return {llvm::ConstantFP::get(builder.getDoubleTy(), number->value), type, kPresent};
    if (auto* boolean = dynamic_cast<const BooleanExpr*>(&expr))
      return {builder.getInt1(boolean->value), type, kPresent};
    if (dynamic_cast<const UnitExpr*>(&expr))
      return {builder.getTrue(), type, kPresent};
    if (auto* text = dynamic_cast<const TextExpr*>(&expr)) {
      auto create = external("lan_text_from_cstr", llvmType(Type::Text),
                             {llvmType(Type::Text)});
      auto* value = builder.CreateCall(
          create, {builder.CreateGlobalString(text->value)}, "text.literal");
      return {adoptHeap(value, Type::Text), type, kPresent};
    }
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
      if (variable->name == "pi")
        return {llvm::ConstantFP::get(builder.getDoubleTy(),
                                     0x1.921fb54442d18p+1),
                Type::Number, kPresent};
      if (variable->name == "e")
        return {llvm::ConstantFP::get(builder.getDoubleTy(),
                                     0x1.5bf0a8b145769p+1),
                Type::Number, kPresent};
      if (variable->name == "tau")
        return {llvm::ConstantFP::get(builder.getDoubleTy(),
                                     0x1.921fb54442d18p+2),
                Type::Number, kPresent};
    }
    if (auto* let = dynamic_cast<const LetExpr*>(&expr)) {
      auto value = emitElementBoundary(*let->value);
      scopes.emplace_back();
      scopes.back().emplace(let->name, value);
      auto result = emitExpr(*let->body);
      scopes.pop_back();
      return convertPresence(result, presence);
    }
    if (auto* conditional = dynamic_cast<const IfExpr*>(&expr)) return emitIf(*conditional);
    if (auto* match = dynamic_cast<const MatchExpr*>(&expr)) return emitMatch(*match);
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
      auto operand = requirePresent(emitExpr(*unary->operand), "一元运算");
      if (unary->op == '!') {
        return {builder.CreateNot(operand.value, "not"), Type::Boolean, kPresent};
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
          } else if (lhs.type == Type::Instant) {
            auto compare = external("lan_instant_compare", builder.getInt32Ty(),
                                    {llvmType(Type::Instant), llvmType(Type::Instant)});
            equal = builder.CreateICmpEQ(
                builder.CreateCall(compare, {left, right}), builder.getInt32(0),
                "instant.equal");
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
        if (op == "/" || op == "%") {
          auto* function = builder.GetInsertBlock()->getParent();
          auto* divideBlock = llvm::BasicBlock::Create(
              context, "division.valid", function);
          auto* zeroBlock = llvm::BasicBlock::Create(
              context, "division.by.zero", function);
          builder.CreateCondBr(
              builder.CreateFCmpOEQ(
                  right, llvm::ConstantFP::get(builder.getDoubleTy(), 0.0),
                  "divisor.is.zero"),
              zeroBlock, divideBlock);
          builder.SetInsertPoint(zeroBlock);
          emitTableError("除数不能为零");
          builder.SetInsertPoint(divideBlock);
          return op == "/" ? builder.CreateFDiv(left, right, "quotient")
                           : floorModulo(left, right);
        }
        if (lhs.type == Type::Instant) {
          auto compare = external("lan_instant_compare", builder.getInt32Ty(),
                                  {llvmType(Type::Instant), llvmType(Type::Instant)});
          auto* compared = builder.CreateCall(compare, {left, right},
                                               "instant.compare");
          if (op == "<") return builder.CreateICmpSLT(compared, builder.getInt32(0));
          if (op == ">") return builder.CreateICmpSGT(compared, builder.getInt32(0));
          if (op == "<=") return builder.CreateICmpSLE(compared, builder.getInt32(0));
          return builder.CreateICmpSGE(compared, builder.getInt32(0));
        }
        if (op == "<") return builder.CreateFCmpOLT(left, right, "less");
        if (op == ">") return builder.CreateFCmpOGT(left, right, "greater");
        if (op == "<=") return builder.CreateFCmpOLE(left, right, "at.most");
        return builder.CreateFCmpOGE(left, right, "at.least");
      });
    }
    if (auto* collection = dynamic_cast<const CollectionExpr*>(&expr))
      return emitCollection(*collection);
    if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
      if (call->callee == "ok" || call->callee == "err") return emitResult(*call);
      if (isIOBuiltin(call->callee)) return emitStandardIOBuiltin(*call);
      if (isRegexBuiltin(call->callee)) return emitRegexBuiltin(*call);
      if (call->callee == "is_missing") {
        auto value = emitExpr(*call->args[0]);
        return {builder.CreateNot(presentFlag(value), "is.missing"),
                Type::Boolean, kPresent};
      }
      if (call->callee == "require") return emitRequire(*call);
      if (call->callee == "same") return emitSame(*call);
      if (call->callee == "table") return emitTableValidation(*call);
      if (call->callee == "row_count") return emitTableRowCount(*call);
      if (call->callee == "sum" || call->callee == "average" ||
          call->callee == "min" || call->callee == "max" ||
          call->callee == "count")
        return call->args.size() == 1 ? emitListAggregate(*call)
                                      : emitTableAggregate(*call);
      if (call->callee == "fill_missing") return emitFillMissing(*call);
      if (call->callee == "trim" || call->callee == "lowercase" ||
          call->callee == "uppercase" || call->callee == "starts_with" ||
          call->callee == "ends_with" || call->callee == "text_contains" ||
          call->callee == "split" ||
          call->callee == "join")
        return emitTextBuiltin(*call);
      if (call->callee == "abs" || call->callee == "floor" ||
          call->callee == "ceil" || call->callee == "round" ||
          call->callee == "clamp" || call->callee == "sign" ||
          call->callee == "sqrt" || call->callee == "pow" ||
          call->callee == "exp" || call->callee == "log" ||
          call->callee == "hypot" || call->callee == "sin" ||
          call->callee == "cos" || call->callee == "tan" ||
          call->callee == "asin" || call->callee == "acos" ||
          call->callee == "atan" || call->callee == "atan2" ||
          call->callee == "radians" || call->callee == "degrees")
        return emitNumberBuiltin(*call);
      if (call->callee == "to_text" || call->callee == "parse_number")
        return emitConversionBuiltin(*call);
      if (isTimeBuiltin(call->callee)) return emitTimeBuiltin(*call);
      if (call->callee == "concat" || call->callee == "flatten" ||
          call->callee == "reverse" || call->callee == "distinct" ||
          call->callee == "sort" ||
          ((call->callee == "take" || call->callee == "drop") &&
           inference->expressionType(*call->args[0]) == Type::List))
        return emitListBuiltin(*call);
      if (call->callee == "fields" || call->callee == "has_field" ||
          call->callee == "merge" || call->callee == "remove_field")
        return emitRecordBuiltin(*call);
      if (call->callee == "select_columns" || call->callee == "take" ||
          call->callee == "drop" || call->callee == "sort_by")
        return emitTableTransform(*call);
      if (call->callee == "length") {
        auto input = requirePresent(emitExpr(*call->args[0]), "length 的输入");
        if (input.type == Type::Text) {
          auto* output = builder.CreateAlloca(builder.getDoubleTy(), nullptr,
                                              "text.length");
          auto function = external("lan_text_length", builder.getInt32Ty(),
              {llvmType(Type::Text), llvm::PointerType::get(context, 0)});
          auto* status = builder.CreateCall(function, {input.value, output},
                                            "text.length.status");
          requireRuntimeSuccess(status, "Text 包含无效的 UTF-8");
          return {builder.CreateLoad(builder.getDoubleTy(), output),
                  Type::Number, kPresent};
        }
        return guarded({input}, Type::Number, presence, [&]() {
          auto* length = builder.CreateLoad(
              builder.getInt64Ty(),
              builder.CreateStructGEP(listValueType(), rawPayload(input), 0), "list.length");
          return builder.CreateUIToFP(length, builder.getDoubleTy(), "length.number");
        });
      }
      if (call->callee == "first") return emitListEndpoint(*call, false, type);
      if (call->callee == "last") return emitListEndpoint(*call, true, type);
      const auto& target = *declarations.at(call->callee);
      auto targetPresences = inference->parameterPresences(target);
      std::vector<std::optional<TypedValue>> supplied(call->args.size());
      std::vector<std::optional<LazyTableRow>> suppliedRows(call->args.size());
      bool hasLazyRowArgument = false;
      for (std::size_t i = 0; i < call->args.size(); ++i) {
        auto* variable = dynamic_cast<const VariableExpr*>(call->args[i].get());
        auto* row = variable ? lazyTableRow(variable->name) : nullptr;
        if (row) {
          suppliedRows[i] = *row;
          hasLazyRowArgument = true;
        } else {
          supplied[i] = emitExpr(*call->args[i]);
        }
      }
      auto slots = emittedArgumentSlots(*call, target);
      scopes.emplace_back();
      std::size_t inlinedRows = 0;
      std::vector<llvm::Value*> args;
      for (std::size_t i = 0; i < target.params.size(); ++i) {
        if (slots[i] >= 0 && suppliedRows[slots[i]]) {
          const auto& row = *suppliedRows[slots[i]];
          lazyTableRows.push_back(
              {scopes.size() - 1, row.table, row.index, target.params[i].name});
          ++inlinedRows;
          continue;
        }
        auto argument = slots[i] >= 0 ? *supplied[slots[i]]
                                      : emitExpr(*target.params[i].defaultValue);
        argument = convertPresence(argument, targetPresences[i]);
        scopes.back().emplace(target.params[i].name, argument);
        args.push_back(argument.value);
      }
      if (hasLazyRowArgument) {
        emitDiagnosticPush(target.name);
        auto returned = emitElementBoundary(*target.body);
        emitDiagnosticPop();
        for (std::size_t i = 0; i < inlinedRows; ++i) lazyTableRows.pop_back();
        scopes.pop_back();
        return convertPresence(returned, presence);
      }
      scopes.pop_back();
      auto returnedPresence = inference->resultPresence(target);
      TypedValue returned{builder.CreateCall(module->getFunction(call->callee), args,
                                             call->callee + ".result"),
                          type, returnedPresence};
      return convertPresence(adoptValue(returned), presence);
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
    beginArcFrame();
    currentMissingTarget = nullptr;
    rootMissingTarget = nullptr;
    emitDiagnosticPush(function.name);
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
    emitDiagnosticPop();
    endArcFrame(&result);
    builder.CreateRet(result.value);
    if (rootMissingTarget) {
      builder.SetInsertPoint(rootMissingTarget);
      emitDiagnosticPop();
      endArcFrame();
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
    } else if (result.type == Type::Unit) {
      builder.CreateCall(printfFn, {builder.CreateGlobalString("()\n")});
    } else if (result.type == Type::Text) {
      builder.CreateCall(printfFn, {builder.CreateGlobalString("%s\n"), raw});
    } else if (result.type == Type::Boolean) {
      auto* yes = builder.CreateGlobalString("true");
      auto* no = builder.CreateGlobalString("false");
      auto* word = builder.CreateSelect(raw, yes, no, "boolean.text");
      builder.CreateCall(printfFn, {builder.CreateGlobalString("%s\n"), word});
    } else if (result.type == Type::Regex) {
      builder.CreateCall(printfFn, {builder.CreateGlobalString("<Regex>\n")});
    } else if (result.type == Type::Instant) {
      auto format = external("lan_format_time", llvmType(Type::Text),
                             {llvmType(Type::Instant)});
      auto* text = builder.CreateCall(format, {raw}, "display.time");
      adoptHeap(text, Type::Text);
      builder.CreateCall(printfFn, {builder.CreateGlobalString("%s\n"), text});
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
      auto parts = dynamicParts(result);
      builder.CreateCall(dynamicPrint, {parts[0], parts[1], parts[2]});
      builder.CreateCall(printfFn, {builder.CreateGlobalString("\n")});
    }
  }

  void emitProgramResult(const Program& program) {
    auto* signature = llvm::FunctionType::get(builder.getInt32Ty(),
        {builder.getInt32Ty(), llvmType(Type::Text)}, false);
    auto* mainFunction = llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                                "main", module.get());
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "start", mainFunction));
    auto initialize = external("lan_io_initialize", builder.getVoidTy(),
        {builder.getInt32Ty(), llvmType(Type::Text)});
    builder.CreateCall(initialize, {mainFunction->getArg(0), mainFunction->getArg(1)});
    beginArcFrame();
    scopes.clear();
    scopes.emplace_back();
    currentMissingTarget = nullptr;
    rootMissingTarget = nullptr;
    emitDiagnosticPush("程序");
    auto* pointer = llvmType(Type::Text);
    auto printfFn = external("printf", builder.getInt32Ty(), {pointer}, true);
    auto result = requirePresent(emitExpr(*program.result), "程序最终结果");
    if (result.type == Type::IO) {
      result = executeIO(result, inference->ioResultType(*program.result));
      auto* present = llvm::BasicBlock::Create(context, "io.present", mainFunction);
      auto* absent = llvm::BasicBlock::Create(context, "io.missing", mainFunction);
      auto* done = llvm::BasicBlock::Create(context, "io.displayed", mainFunction);
      builder.CreateCondBr(presentFlag(result), present, absent);
      builder.SetInsertPoint(present);
      displayPresent(result, printfFn);
      builder.CreateBr(done);
      builder.SetInsertPoint(absent);
      builder.CreateCall(printfFn, {builder.CreateGlobalString("missing\n")});
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
    } else displayPresent(result, printfFn);
    emitDiagnosticPop();
    endArcFrame();
    auto assertArc = external("lan_arc_assert_empty", builder.getVoidTy(), {});
    builder.CreateCall(assertArc, {});
    builder.CreateRet(builder.getInt32(0));
    if (rootMissingTarget) {
      builder.SetInsertPoint(rootMissingTarget);
      builder.CreateCall(printfFn, {builder.CreateGlobalString("missing\n")});
      emitDiagnosticPop();
      endArcFrame();
      auto assertArc = external("lan_arc_assert_empty", builder.getVoidTy(), {});
      builder.CreateCall(assertArc, {});
      builder.CreateRet(builder.getInt32(0));
    }
    currentMissingTarget = nullptr;
    rootMissingTarget = nullptr;
  }
};

CodeGenerator::CodeGenerator() : impl_(std::make_unique<Impl>()) {}
CodeGenerator::~CodeGenerator() = default;

bool CodeGenerator::needsRegexRuntime() const {
  for (const char* name : {"lan_regex_compile", "lan_regex_search"})
    if (auto* function = impl_->module->getFunction(name))
      if (!function->use_empty()) return true;
  return false;
}

bool CodeGenerator::generate(const Program& program, std::string& error) {
  try {
    if (!program.result) throw std::runtime_error("程序最后需要有一个要计算或显示的内容");
    impl_->inference = std::make_unique<Inference>(program);
    impl_->sourcePath = program.sourcePath;
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
