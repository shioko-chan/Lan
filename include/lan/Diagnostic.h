#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lan {

enum class DiagnosticLanguage { Chinese, English };

// A compiler invocation selects one language before parsing starts. The lexer,
// parser and type checkers all use the same catalog. Unknown environment values
// keep the historical Chinese default.
inline DiagnosticLanguage& diagnosticLanguage() {
  static DiagnosticLanguage language = DiagnosticLanguage::Chinese;
  return language;
}

inline const char* diagnosticPrefix() {
  return diagnosticLanguage() == DiagnosticLanguage::English ? "Lan: " : "Lan：";
}

inline bool parseDiagnosticLanguage(std::string_view value,
                                    DiagnosticLanguage& language) {
  if (value == "zh-CN" || value == "zh") language = DiagnosticLanguage::Chinese;
  else if (value == "en") language = DiagnosticLanguage::English;
  else return false;
  return true;
}

inline std::string compilerContext(const std::string& source) {
  if (diagnosticLanguage() == DiagnosticLanguage::Chinese) return source;
  static const std::pair<const char*, const char*> contexts[] = {
    {"if 后面的条件", "if condition"},
    {"if 和 else 的结果", "if/else results"},
    {"条件的结果", "conditional result"},
    {"列表中的元素", "List element"},
    {"列表", "List"},
    {"List 下标", "List index"},
    {"List 下标读取左侧", "List index lookup object"},
    {"List 下标读取结果", "List index lookup result"},
    {"List 元素", "List element"},
    {"List 表达式", "List expression"},
    {"Option 元素类型", "Option element type"},
    {"嵌套 List", "nested List"},
    {"filter 保留输入容器类型", "filter input container type"},
    {"默认值 result", "default value result"},
    {"列表、Record、Table、Regex", "collection or Record"},
    {"“+”号", "+ operator"},
    {"“+”号的结果", "+ result"},
    {"“??”两边", "both sides of ??"},
    {"“?”的结果", "? result"},
    {"整列运算", "column operation"},
    {"整列运算结果", "column operation result"},
    {"整列运算左侧", "left side of column operation"},
    {"整列运算右侧", "right side of column operation"},
    {"整列比较", "column comparison"},
    {"整列比较结果", "column comparison result"},
    {"整列顺序比较", "ordered column comparison"},
    {"整列逻辑运算左侧", "left side of column logic operation"},
    {"整列逻辑运算右侧", "right side of column logic operation"},
    {"整列逻辑运算结果", "column logic operation result"},
    {"match 分支结果", "match arm results"},
    {"match 字面量模式", "match literal pattern"},
    {"函数调用", "function call"},
    {"字段访问左侧", "field lookup object"},
    {"字段更新左侧", "field update object"},
    {"字段更新结果", "field update result"},
    {"嵌套字段更新左侧", "nested field update object"},
    {"嵌套字段更新结果", "nested field update result"},
    {"下标读取左侧", "index lookup object"},
    {"一元运算", "unary operation"},
    {"一元整列运算", "unary column operation"},
    {"一元整列运算结果", "unary column operation result"},
    {"比较结果", "comparison result"},
    {"顺序比较", "ordered comparison"},
    {"逻辑结果", "logical result"},
    {"计算结果", "calculation result"},
    {"加法", "addition"},
    {"单项计算", "unary calculation"},
    {"集合元素", "collection element"},
    {"集合操作的输入", "collection operation input"},
    {"集合操作的 List 输入", "collection operation List input"},
    {"IO 结果", "IO result"},
    {"IO 执行结果", "executed IO result"},
    {"IO 后续步骤", "subsequent IO step"},
    {"io 的最终结果", "final io result"},
    {"do 最终值", "final do value"},
    {"do 后续步骤", "subsequent do step"},
    {"Result do 的 <- 输入", "Result do <- input"},
    {"Result do 的成功值", "Result do success value"},
    {"Result do 的结果", "Result do result"},
    {"Result do 的错误类型", "Result do error type"},
    {"Option do 的最终值", "final Option do value"},
    {"Result 构造/模式", "Result constructor/pattern"},
    {"解包的结果", "unwrapped result"},
    {"Table 逻辑行", "Table logical row"},
    {"Table 更新 schema", "Table schema update"},
    {"Table schema 传播", "Table schema propagation"},
    {"Table schema 操作的输入", "Table schema operation input"},
    {"Table schema 操作的结果", "Table schema operation result"},
    {"Table schema 操作的字段表达式", "Table schema field expression"},
    {"Table 字面量的列", "Table literal column"},
    {"Table 的列", "Table column"},
    {"Table 的逻辑行", "Table logical row"},
    {"Table 的动态列元素", "dynamic Table column element"},
    {"table 的行列表", "table row List"},
    {"table 的每一行", "each table row"},
    {"动态字段", "dynamic field"},
    {"动态字段名", "dynamic field name"},
    {"动态值", "dynamic value"},
    {"数字", "Number"},
    {"文字", "Text"},
    {"真假值", "Boolean"},
    {"单元值 ()", "Unit value ()"},
    {"命令行参数", "command-line argument"},
    {"尚未确定的类型", "unknown type"},
    {"未知类型", "unknown type"},
    {"是/否值", "Boolean"},
  };
  for (const auto& [chinese, english] : contexts)
    if (source == chinese) return english;

  const auto quoted = [&](const std::string& prefix, const std::string& label)
      -> std::string {
    if (source.rfind(prefix, 0) != 0) return {};
    const auto end = source.find("”", prefix.size());
    if (end == std::string::npos) return {};
    const auto name = source.substr(prefix.size(), end - prefix.size());
    const auto rest = source.substr(end + std::string("”").size());
    if (rest.empty()) return label + name;
    if (rest == "的结果") return "result of " + label + name;
    if (rest == "的输入") return "input to " + label + name;
    if (rest == "的参数") return "argument of " + label + name;
    const auto argumentPrefix = std::string("的参数“");
    if (rest.rfind(argumentPrefix, 0) == 0 &&
        rest.size() >= argumentPrefix.size() + std::string("”").size() &&
        rest.compare(rest.size() - std::string("”").size(),
                     std::string("”").size(), "”") == 0)
      return "argument " + rest.substr(argumentPrefix.size(),
          rest.size() - argumentPrefix.size() - std::string("”").size()) +
          " of " + label + name;
    if (rest == "的列名") return "column name for " + label + name;
    if (rest == "的第一个输入") return "first input to " + label + name;
    if (rest == "的第二个输入") return "second input to " + label + name;
    if (rest == "的第一个参数") return "first argument of " + label + name;
    if (rest == "的第二个参数") return "second argument of " + label + name;
    if (rest == "的列表元素") return "List element of " + label + name;
    if (rest == "的列元素") return "column element of " + label + name;
    if (rest == "的匹配内容") return "matched content of " + label + name;
    if (rest == "左侧" || rest == "左边") return "left side of " + label + name;
    if (rest == "右侧" || rest == "右边") return "right side of " + label + name;
    const auto fieldPrefix = std::string("的字段“");
    if (rest.rfind(fieldPrefix, 0) == 0 &&
        rest.size() >= fieldPrefix.size() + std::string("”").size() &&
        rest.compare(rest.size() - std::string("”").size(),
                     std::string("”").size(), "”") == 0)
      return "field " + rest.substr(fieldPrefix.size(),
          rest.size() - fieldPrefix.size() - std::string("”").size()) +
          " of " + label + name;
    if (rest == "的默认值") return "default value of " + label + name;
    if (rest == "的行数") return "row count of " + label + name;
    return {};
  };
  for (const auto& [prefix, label] : {
      std::pair<const char*, const char*>{"函数“", "function "},
      {"名称“", "name "}, {"参数“", "parameter "},
      {"Record 字段“", "Record field "},
      {"Table 列“", "Table column "},
      {"table 的列“", "table column "},
      {"Table 字面量的列“", "Table literal column "},
      {"数学常量“", "math constant "},
      {"符号“", "symbol "},
      {"局部公式“", "local formula "}}) {
    if (auto translated = quoted(prefix, label); !translated.empty()) return translated;
  }
  if (source.rfind("传给函数“", 0) == 0) {
    const auto start = std::string("传给函数“").size();
    const auto end = source.find("”", start);
    const auto argumentPrefix = std::string("”的参数“");
    if (end != std::string::npos &&
        source.compare(end, argumentPrefix.size(), argumentPrefix) == 0 &&
        source.size() >= end + argumentPrefix.size() + std::string("”").size()) {
      const auto argumentStart = end + argumentPrefix.size();
      const auto argumentEnd = source.find("”", argumentStart);
      if (argumentEnd != std::string::npos) {
        auto translated = "argument " +
            source.substr(argumentStart, argumentEnd - argumentStart) +
            " passed to function " + source.substr(start, end - start);
        return compilerContext(translated +
            source.substr(argumentEnd + std::string("”").size()));
      }
    }
  }
  if (source.rfind("比较号“", 0) == 0) {
    const auto start = std::string("比较号“").size();
    const auto end = source.find("”", start);
    if (end != std::string::npos)
      return "comparison operator " + source.substr(start, end - start);
  }
  if (source.rfind("“", 0) == 0) {
    const auto start = std::string("“").size();
    const auto end = source.find("”", start);
    if (end != std::string::npos) {
      const auto symbol = source.substr(start, end - start);
      const auto suffix = source.substr(end + std::string("”").size());
      if (suffix == "号左边" || suffix == "左边") return "left side of " + symbol;
      if (suffix == "号右边" || suffix == "右边") return "right side of " + symbol;
      if (suffix == "两边") return "both sides of " + symbol;
    }
  }
  static const std::pair<const char*, const char*> suffixes[] = {
    {"的结果元素", " result element"},
    {"的元素结果", " element result"},
    {"的 IO 结果", " IO result"},
    {"的 Record", " Record"},
    {"的 maximum", " maximum"},
    {"的 minimum", " minimum"},
    {"的列表元素", " List element"},
    {"的列元素", " column element"},
    {"的子列表", " sublist"},
    {"的元素", " element"},
    {"的条件", " condition"},
    {"的底数", " base"},
    {"的小数位数", " decimal places"},
    {"的输入元素", " input element"},
    {"的结果", " result"},
    {"的输入", " input"},
    {"的左侧", " left side"},
    {"的右侧", " right side"},
    {"的参数", " argument"},
    {"的 Text", " Text"},
    {"的模式", " pattern"},
    {"的分隔符", " delimiter"},
    {"的列名", " column name"},
    {"的替代值", " replacement value"},
    {"的列表", " List"},
    {"的每一行", " each row"},
    {"的字段名", " field name"},
    {"的字段", " field"},
    {"的载荷", " payload"},
    {"的 ok 载荷", " ok payload"},
    {"的 err 载荷", " err payload"},
  };
  for (const auto& [chinese, english] : suffixes) {
    const std::string ending = chinese;
    if (source.size() > ending.size() &&
        source.compare(source.size() - ending.size(), ending.size(), ending) == 0) {
      auto base = source.substr(0, source.size() - ending.size());
      while (!base.empty() && base.back() == ' ') base.pop_back();
      return compilerContext(base) + english;
    }
  }
  return source;
}

enum class DiagnosticKey {
  Syntax, AssignmentTarget, TemporaryUpdate, SelfUpdate, PipelineArity,
  PatternConstructor, DuplicatePattern, NonexhaustiveMatch, OptionalValue,
  OptionalCoalesce, DiscardedResult, MixedDo, UnknownDoStep,
  SourceLocation, AnalysisCallChain, Program,
  ReservedFunction, DuplicateFunction, UnknownName, UnknownFunction,
  RecursiveCall, DuplicateField, CurrentElement, ElementBinding,
  TypeRequired, TypeMismatch, InfiniteType, CircularType,
  FunctionArity, MissingArgument, UnknownArgument, DuplicateArgument,
  NamedArgumentOrder, BuiltinNamedArgument, InvalidExpression,
  MissingProgramResult, InvalidIndentation, InvalidLanguage,
  DefaultParameterOrder, DuplicateParameter, TooManyArguments,
  ExpectedListOrText, ExpectedListOrTable, ExpectedCollection,
  ExpectedTableRows, IOInValue, IOInCollection, NestedIO, IOResultPayload,
  InvalidAddition, IOComparison, WholeValueComparison, OrderedComparison,
  InvalidFieldObject, InvalidSortElement, AmbiguousCoalesce,
  EmptyDo, WrongDoInput, NoArguments, BuiltinRequirement,
  UnknownDoBinding, InvalidTableColumns, ArgumentCount, GivenValues,
  ParseRuntime, LinkRuntime, UnknownType, UnknownListElement,
  MissingIOBoundary, CannotGenerateExpression, VerifyFunction,
  VerifyProgram, VerifyOptimizedProgram,
  ProgramArgumentsOnlyRun, RandomSeedOnlyRun, FixedTimeOnlyRun,
  InvalidRandomSeed, BuildOutputConflict, CannotOpenFile,
  CannotDetermineProgramName, CannotCreateBuildDirectory,
  CannotWriteIR, CannotWriteFile, CannotWriteRegexArchive,
  CannotWriteRegexLicenses, ObjectBuildFailed, ExecutableBuildFailed,
  SyntaxWithDetail
};

inline std::string diagnostic(DiagnosticKey key,
                              std::initializer_list<std::string> arguments = {}) {
  struct Entry { const char* chinese; const char* english; };
  static constexpr Entry messages[] = {
    {"第 {0} 行：无法理解这里的写法", "Line {0}: syntax error"},
    {"第 {0} 行：等号左边必须是名称或字段", "Line {0}: the left side of = must be a name or field"},
    {"第 {0} 行：字段更新必须以局部名称为根，不能更新临时值", "Line {0}: a field update must start from a local name, not a temporary value"},
    {"第 {0} 行：不能把“{1}”自身写入它的字段", "Line {0}: cannot write {1} into its own field"},
    {"第 {0} 行：集合操作“{1}”需要一个元素表达式", "Line {0}: collection operation {1} requires one element expression"},
    {"模式构造器只支持 value(name)、ok(name)、err(name)", "Pattern constructors support only value(name), ok(name), and err(name)"},
    {"match 模式重复或分支不可达", "Duplicate match pattern or unreachable arm"},
    {"match 未穷尽：请覆盖所有存在值与可能的 missing，或添加 _ 分支", "Nonexhaustive match: cover all present values and possible missing values, or add an _ arm"},
    {"Option 类型错误：{0}需要确定存在的 T，但得到 Option<T>（可能为 missing）；请使用 ?、??、require 或 is_missing 分支处理", "Option type error: {0} requires a present T, but received Option<T> (possibly missing); handle it with ?, ??, require, or an is_missing branch"},
    {"Option 类型错误：?? 的整个值默认项必须与左侧 T 类型一致；List 元素默认值请在解包后使用 fill_missing", "Option type error: the whole-value ?? default must have the same T type as the left side; use fill_missing after unwrapping for List elements"},
    {"IO 步骤不能隐式丢弃 Result；请绑定结果并处理，或用 _ <- 明确忽略", "An IO step cannot implicitly discard a Result; bind and handle it, or explicitly ignore it with _ <-"},
    {"同一个 do 块不能混用 Option 与 Result 的 <- 解包", "A do block cannot mix Option and Result <- unwrapping"},
    {"内部错误：do 块包含未知步骤", "Internal error: unknown step in do block"},
    {"\n位置：{0}:{1}:{2}", "\nLocation: {0}:{1}:{2}"},
    {"\n分析调用链：程序", "\nAnalysis call chain: program"},
    {"程序", "program"},
    {"“{0}”由语言保留，请换一个函数名", "{0} is reserved by the language; choose another function name"},
    {"函数“{0}”定义了不止一次", "Function {0} is defined more than once"},
    {"找不到名称“{0}”，请检查拼写或先给它一个值", "Unknown name {0}; check its spelling or define it first"},
    {"找不到函数“{0}”，请检查它是否已经定义", "Unknown function {0}; check whether it is defined"},
    {"Lan 禁止递归调用：{0}", "Lan does not allow recursive calls: {0}"},
    {"Record 字段“{0}”重复了", "Duplicate Record field {0}"},
    {"当前元素“.”只能写在集合操作的元素表达式中", "The current element . can only appear in a collection operation's element expression"},
    {"元素绑定“=>”只能写在集合操作中", "Element binding => can only appear in a collection operation"},
    {"{0}需要{1}，但这里得到的是{2}", "{0} requires {1}, but received {2}"},
    {"{0}两边的种类不同：一边是{1}，另一边是{2}", "Type mismatch in {0}: one side is {1}, the other is {2}"},
    {"{0}产生无限嵌套的类型", "{0} produces an infinitely nested type"},
    {"结果存在循环类型约束", "Cyclic type constraint in the result"},
    {"函数“{0}”需要 {1} 个参数", "Function {0} requires {1} arguments"},
    {"调用函数“{0}”时缺少必填参数“{1}”", "Call to {0} is missing required argument {1}"},
    {"函数“{0}”没有名为“{1}”的参数", "Function {0} has no argument named {1}"},
    {"函数“{0}”的参数“{1}”被传入了不止一次", "Argument {1} of function {0} was supplied more than once"},
    {"函数“{0}”的位置参数必须放在命名参数之前", "Positional arguments to {0} must precede named arguments"},
    {"内置函数“{0}”暂不接受命名参数", "Built-in function {0} does not accept named arguments"},
    {"编译器暂时不能理解这个表达式", "The compiler cannot handle this expression yet"},
    {"程序最后需要有一个要计算或显示的内容", "The program must end with a value to compute or display"},
    {"第 {0} 行：缩进无效", "Line {0}: invalid indentation"},
    {"--diagnostic-language 只接受 zh-CN 或 en", "--diagnostic-language accepts only zh-CN or en"},
    {"函数“{0}”中，没有默认值的参数“{1}”必须放在默认参数前面", "In function {0}, required parameter {1} must precede parameters with defaults"},
    {"函数“{0}”中参数“{1}”重复了", "Duplicate parameter {1} in function {0}"},
    {"函数“{0}”收到的参数太多了", "Too many arguments for function {0}"},
    {"length 需要 List 或 Text，但这里得到的是{0}", "length requires List or Text, but received {0}"},
    {"take/drop 需要 List 或 Table，但这里得到的是{0}", "take/drop requires List or Table, but received {0}"},
    {"集合操作需要 List 或 Table，但这里得到的是{0}", "Collection operation requires List or Table, but received {0}"},
    {"table 需要 Record<List<T>> 或 List<Record>", "table requires Record<List<T>> or List<Record>"},
    {"IO 不能放入容器、比较或转换为文字", "IO cannot be stored in a collection, compared, or converted to Text"},
    {"IO 不能作为 List 或 Table 列元素", "IO cannot be a List element or Table column element"},
    {"IO 结果不能再是 IO；请在 io 块最后一步执行该 action", "An IO result cannot itself be IO; execute the action as the final step of an io block"},
    {"Result 载荷不能是 IO；请使用 IO<Result<T, E>>", "A Result payload cannot be IO; use IO<Result<T, E>>"},
    {"“+”只能连接两个数字或两段文字，不能用于{0}", "+ can combine two Numbers or two Text values, but not {0}"},
    {"IO 不允许比较", "IO values cannot be compared"},
    {"列表、Record、Table、Regex 和 Result 不能使用 == 或 != 整体比较，请使用 same", "Use same for whole-value comparisons of List, Record, Table, Regex, and Result; == and != are unavailable"},
    {"顺序比较只能用于 Number 或 Instant，不能用于{0}", "Ordered comparison requires Number or Instant, not {0}"},
    {"字段访问左侧需要 Record 或 Table，但这里得到的是{0}", "Field lookup requires Record or Table, but received {0}"},
    {"sort 只能用于 Number、Text 或 Instant List，不能用于{0}", "sort requires a List of Number, Text, or Instant, not {0}"},
    {"Option 类型错误：同一 ?? 不能在不同调用中同时表示整值默认和逐元素填补；逐元素填补请显式使用 fill_missing", "Option type error: the same ?? cannot mean whole-value default and element-wise fill in different calls; use fill_missing explicitly"},
    {"do 块至少需要一个 <- 解包步骤；普通顺序计算请直接使用块", "A do block needs at least one <- step; use an ordinary block for sequential calculations"},
    {"do 的 <- 只解包 Option 或 Result；执行 IO 请使用 io 块", "do <- unwraps only Option or Result; execute IO in an io block"},
    {"{0} 不接受参数", "{0} does not accept arguments"},
    {"{0} 需要 {1}", "{0} requires {1}"},
    {"内部错误：do 绑定不在 do 块内", "Internal error: do binding outside a do block"},
    {"Table 字面量中静态已知的列长度不一致", "Known column lengths differ in Table literal"},
    {"{0} 需要 {1} 个参数", "{0} requires {1} arguments"},
    {"{0} 需要 {1}，但这里给了 {2} 个值", "{0} requires {1}, but received {2} values"},
    {"无法解析{0}运行时：{1}", "Cannot parse {0} runtime: {1}"},
    {"无法链接{0}运行时", "Cannot link {0} runtime"},
    {"内部错误：类型尚未确定", "Internal error: type is still unknown"},
    {"内部错误：列表元素类型未知", "Internal error: List element type is unknown"},
    {"内部错误：IO 步骤缺少表达式边界", "Internal error: IO step is missing an expression boundary"},
    {"内部错误：无法生成表达式", "Internal error: cannot generate expression"},
    {"LLVM 无法验证函数“{0}”", "LLVM failed to verify function {0}"},
    {"LLVM 无法验证生成的程序", "LLVM failed to verify the generated program"},
    {"LLVM 优化后的程序未通过验证", "LLVM failed to verify the optimized program"},
    {"程序参数只能用于 --run，并放在 -- 后面", "Program arguments are available only with --run and must follow --"},
    {"--random-seed 只能用于 --run", "--random-seed is available only with --run"},
    {"--fixed-time 只能用于 --run", "--fixed-time is available only with --run"},
    {"随机种子必须是 0 到 9007199254740991 之间的整数", "Random seed must be an integer from 0 to 9007199254740991"},
    {"--build 和 --run 不能和 -o 同时使用", "--build and --run cannot be combined with -o"},
    {"无法打开文件“{0}”", "Cannot open file {0}"},
    {"无法从输入文件名确定程序名称", "Cannot determine program name from input file name"},
    {"无法创建构建目录“{0}”：{1}", "Cannot create build directory {0}: {1}"},
    {"无法写入 LLVM IR“{0}”：{1}", "Cannot write LLVM IR {0}: {1}"},
    {"无法写入“{0}”：{1}", "Cannot write {0}: {1}"},
    {"无法写入正则运行时静态库", "Cannot write regular-expression runtime archive"},
    {"无法写入正则运行时许可证", "Cannot write regular-expression runtime licenses"},
    {"生成目标文件失败", "Failed to generate object file"},
    {"生成可执行文件失败", "Failed to generate executable"},
    {"第 {0} 行：无法理解这里的写法（{1}）", "Line {0}: syntax error"}
  };
  static_assert(sizeof(messages) / sizeof(messages[0]) ==
                static_cast<unsigned>(DiagnosticKey::SyntaxWithDetail) + 1);
  const auto index = static_cast<unsigned>(key);
  const std::string_view pattern = diagnosticLanguage() == DiagnosticLanguage::English
      ? messages[index].english : messages[index].chinese;
  const std::vector<std::string> values(arguments);
  std::string result;
  for (std::size_t position = 0; position < pattern.size(); ++position) {
    if (position + 2 < pattern.size() && pattern[position] == '{' &&
        pattern[position + 1] >= '0' && pattern[position + 1] <= '9' &&
        pattern[position + 2] == '}') {
      const auto slot = static_cast<std::size_t>(pattern[position + 1] - '0');
      if (slot < values.size()) {
        result += values[slot];
        position += 2;
        continue;
      }
    }
    result += pattern[position];
  }
  return result;
}

enum class RequirementKey {
  OneText, OneTextParameter, OneList, OneNumber, OneValue, OneRecord, OneInstant,
  TwoNumbers, TwoArguments, MinimumMaximum, TextAndMatch,
  TextAndDelimiter, TextListAndDelimiter, NumberOptionalBase,
  NumberOptionalPlaces, NumberMinimumMaximum, TwoLists, NestedList,
  RecordAndField, TwoRecords, TwoValues, ListAndDefault,
  TableAndColumns, ListTableAndCount, TableAndColumn,
  ListOrTableAndColumn, OneTable, OneListOrText, RecordOrRows
};

inline std::string requirementPhrase(RequirementKey key) {
  struct Entry { const char* chinese; const char* english; };
  static constexpr Entry requirements[] = {
    {"1 个 Text", "one Text argument"},
    {"1 个 Text 参数", "one Text argument"},
    {"1 个 List", "one List argument"},
    {"1 个 Number", "one Number argument"},
    {"1 个值", "one value"},
    {"1 个 Record", "one Record argument"},
    {"1 个 Instant", "one Instant argument"},
    {"2 个 Number", "two Number arguments"},
    {"2 个参数", "two arguments"},
    {"minimum 和 maximum", "minimum and maximum"},
    {"Text 和要匹配的 Text", "Text and Text to match"},
    {"Text 和分隔符", "Text and a delimiter"},
    {"Text List 和分隔符", "a Text List and a delimiter"},
    {"Number 和可选的底数", "Number and an optional base"},
    {"Number 和可选的小数位数", "Number and optional decimal places"},
    {"Number、最小值和最大值", "Number, minimum, and maximum"},
    {"两个 List", "two Lists"},
    {"1 个 List<List<T>>", "one List<List<T>>"},
    {"Record 和字段名", "Record and a field name"},
    {"两个 Record", "two Records"},
    {"两个要比较的值", "two values to compare"},
    {"一个列表和一个替代值", "a List and a replacement value"},
    {"一个 Table 和列名列表", "a Table and a List of column names"},
    {"一个 List/Table 和数量", "a List/Table and a count"},
    {"一个 Table 和列名", "a Table and a column name"},
    {"一个 List，或者一个 Table 和列名", "a List, or a Table and column name"},
    {"1 个 Table", "one Table argument"},
    {"1 个 List 或 Text", "one List or Text argument"},
    {"1 个 Record 或 List<Record>", "one Record or List<Record>"}
  };
  static_assert(sizeof(requirements) / sizeof(requirements[0]) ==
                static_cast<unsigned>(RequirementKey::RecordOrRows) + 1);
  const auto& phrase = requirements[static_cast<unsigned>(key)];
  return diagnosticLanguage() == DiagnosticLanguage::English
      ? phrase.english : phrase.chinese;
}

inline std::string requiredArguments(const std::string& name, RequirementKey key) {
  return diagnostic(DiagnosticKey::BuiltinRequirement,
                    {name, requirementPhrase(key)});
}

}  // namespace lan
