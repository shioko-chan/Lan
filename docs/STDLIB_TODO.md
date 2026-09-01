# Lan 标准库清单

本文件记录用户可调用的公开函数和集合管道操作。语法、类型系统、编译器和运行时事项
记录在 [`SYNTAX_TODO.md`](SYNTAX_TODO.md)，IO Monad 的完整规则记录在
[`IO_DESIGN.md`](IO_DESIGN.md)，数学函数规则记录在
[`MATH_DESIGN.md`](MATH_DESIGN.md)，时间与随机规则记录在
[`TIME_RANDOM_DESIGN.md`](TIME_RANDOM_DESIGN.md)，正则表达式规则记录在
[`REGEX_DESIGN.md`](REGEX_DESIGN.md)。

当前标准库以编译器 intrinsic 和随程序链接的运行时函数实现，但这不改变其公开 API
身份。管道 `value |> operation(args)` 等价于把 `value` 作为第一个输入；建立元素上下文
的操作（如 `map`、`filter`）由语言识别其参数表达式，但仍属于标准集合 API。

标记说明：

- `[x]` 加删除线：已经实现并验证。
- `[ ]`：尚未实现或尚未决定。

## Missing 与结构检查

- [x] ~~`is_missing(value)` 返回确定存在的 Boolean。~~
- [x] ~~`require(value)` 在 missing 时运行时拒绝，否则返回原值。~~
- [x] ~~`ok(value)` / `err(error)` 构造 Result；`require(result)` 成功解一层、失败携带载荷运行时拒绝。~~
- [x] ~~`same(left, right)` 递归比较 List、Record 和 Table 的完整结构。~~

## List

- [x] ~~`length(list)` 返回元素数量。~~
- [x] ~~`first(list)` 和 `last(list)` 读取端点，空 List 返回 missing。~~
- [x] ~~`fill_missing(list, value)` 显式填充缺失元素。~~
- [x] ~~`sum`、`average`、`min`、`max` 聚合 Number List。~~
- [x] ~~`count(list)` 统计任意单类型 List 的非 missing 元素。~~
- [x] ~~聚合忽略 missing；空或全 missing List 的 `average`、`min`、`max` 返回 missing。~~
- [x] ~~`map` 对 List 逐元素转换并返回 List。~~
- [x] ~~`filter` 保留 predicate 为 true 的元素，missing predicate 视为 false。~~
- [x] ~~`find` 从左到右返回第一个匹配元素或 missing。~~
- [x] ~~`any`、`all` 使用三值逻辑并从左到右短路。~~
- [x] ~~`concat(left, right)` 连接两个元素类型相同的 List。~~
- [x] ~~`flatten(lists)` 只展开一层嵌套 List；子列表为 Option 时须先显式处理，否则编译拒绝。~~
- [x] ~~`reverse(list)` 返回逆序 List。~~
- [x] ~~将 `take(list, n)` 和 `drop(list, n)` 扩展到 List，并保持操作有界。~~
- [x] ~~`sort(list)` 稳定排序 Number、Text 或 Instant List，missing 固定放在末尾。~~
- [x] ~~`distinct(list)` 使用结构化哈希与相等去重并保留第一次出现的位置；多个 missing
  只保留一个；平均时间和额外空间均为线性，冲突不影响正确性。~~
- [x] ~~不增加 `contains`；使用 `items |> any(. == target)` 表达。~~
- [x] ~~不增加 `is_empty`；使用 `length(items) == 0` 表达。~~
- [x] ~~不增加 `head`、`tail` 等同义名称；使用现有 `first`、`last`。~~
- [x] ~~不引入无限 range 或惰性迭代器；所有集合操作必须保证有限。~~

## Text

- [x] ~~`length(text)` 按 Unicode grapheme cluster 而非 UTF-8 字节返回长度。~~
- [x] ~~`trim(text)` 去除首尾 Unicode White_Space。~~
- [x] ~~`lowercase(text)` 和 `uppercase(text)` 做 Unicode 简单大小写转换。~~
- [x] ~~`starts_with(text, prefix)` 和 `ends_with(text, suffix)` 判断前后缀。~~
- [x] ~~`text_contains(text, part)` 判断是否包含子文本。~~
- [x] ~~`split(text, separator)` 返回 `List<Text>`，并拒绝空分隔符。~~
- [x] ~~`join(parts, separator)` 将不存在 missing 元素的 `List<Text>` 连接成 Text。~~
- [x] ~~按 `REGEX_DESIGN.md` 增加基于 RE2 的不透明 `Regex` 类型、构造和第一阶段匹配 API：
  `regex_contains`、`regex_matches`、`regex_find`、`regex_find_all`、`regex_split`。
  已接入 ARC、Unicode 空匹配前进、结构化错误诊断及按需静态链接。~~

## Number

- [x] ~~`abs(number)` 返回绝对值。~~
- [x] ~~`sign(number)` 返回 `-1`、`0` 或 `1`。~~
- [x] ~~`floor(number)`、`ceil(number)` 返回对应的整数值，结果类型仍为 Number。~~
- [x] ~~`round(number, digits = 0)` 使用 ties-to-even 按指定小数位舍入。~~
- [x] ~~`clamp(number, minimum, maximum)` 将数值限制在闭区间内。~~
- [x] ~~增加普通 Number 常量 `pi`、`e` 和 `tau`。~~
- [x] ~~`sqrt`、`pow`、`exp`、`log` 和 `hypot` 提供根、幂、指数和对数运算。~~
- [x] ~~`sin`、`cos`、`tan`、`asin`、`acos`、`atan` 和 `atan2` 使用弧度。~~
- [x] ~~`radians` 和 `degrees` 在角度单位边界做显式转换。~~
- [x] ~~数学函数只接受标量 Number；List 和 Table 元素计算显式使用 `map` 或元素表达式。~~
- [x] ~~定义域错误、无效参数以及产生 NaN 或无穷大的数学结果运行时拒绝。~~
- [x] ~~不增加 `mod`、`ln`、`log2`、`log10` 等平行入口；使用 `%` 和带可选 base 的 `log`。~~

## 转换

- [x] ~~`to_text(value)` 将 Number、Boolean、Unit、Text、Instant、List、Record 和 Table 转换为确定的 Text 表示。~~
- [x] ~~`parse_number(text)` 严格解析有限 Number；解析失败返回 missing。~~
- [x] ~~不增加 `to_number`、`try_number` 等同义入口；严格解析使用 `require(parse_number(text))`。~~

## Record

- [x] ~~`fields(record)` 按与 Record 输出一致的确定顺序返回 `List<Text>`。~~
- [x] ~~`has_field(record, key)` 判断字段是否存在，并区分字段缺失与字段值为 missing。~~
- [x] ~~`merge(left, right)` 合并两个 Record；同名字段由右侧覆盖。~~
- [x] ~~`remove_field(record, key)` 返回移除指定字段后的 Record。~~
- [x] ~~不提供 `values(record)`；Record 字段可以异型，无法自然组成单类型 List。~~
- [x] ~~不单独提供字段数量函数；使用 `length(fields(record))`。~~

## Table 构造与读取

- [x] ~~`table(record)` 对 `Record<List<T>>` 做零复制验证和类型细化。~~
- [x] ~~`table(List<Record>)` 将行式输入转换为等长列。~~
- [x] ~~Table 验证要求所有列都是 List 且长度相同。~~
- [x] ~~普通 Record 和 `List<Record>` 必须经过 `table` 才能调用 Table 操作。~~
- [x] ~~`row_count(table)` 返回行数。~~
- [x] ~~使用 `table.column` 读取完整列，行没有名称。~~

## Table 转换

- [x] ~~`table |> filter(predicate)` 保留匹配行，missing predicate 视为 false。~~
- [x] ~~`table |> map(expression)` 返回普通 List。~~
- [x] ~~`table |> find(predicate)` 返回首个匹配行或 missing。~~
- [x] ~~`table |> any(predicate)` 和 `all(predicate)` 使用三值短路逻辑。~~
- [x] ~~`table |> add({...})` 添加计算列，并拒绝同名列。~~
- [x] ~~`table |> replace({...})` 替换计算列，并拒绝不存在的列。~~
- [x] ~~`table |> select({...})` 投影并计算新 schema。~~
- [x] ~~`select_columns(table, names)` 选择非空列名集合。~~
- [x] ~~`take(table, n)` 和 `drop(table, n)` 做有界截取。~~
- [x] ~~`sort_by(table, name)` 稳定升序排序，missing 固定放在末尾。~~
- [ ] `rename_columns(table, mapping)` 按显式映射重命名列。
- [ ] `distinct_by(table, names)` 按指定列去重并保留第一次出现的行。
- [ ] 设计分组聚合 API。
- [ ] 单独设计 Table join；明确 join 类型、重复键展开、重名列处理、missing 键语义和输出顺序后再实现。

## Table 聚合

- [x] ~~`sum(table, name)`、`average`、`min`、`max` 聚合 Number 列。~~
- [x] ~~`count(table, name)` 统计任意列的非 missing 单元格。~~
- [x] ~~聚合忽略 missing；空列的 `average`、`min`、`max` 返回 missing。~~

## IO 与数据格式

语言核心已支持 `IO<T>`、`io`/`<-`、纯值 action、标准输入输出及命令行参数；文件 IO 仍待实现。

- [ ] 为可恢复的标准 IO 失败设计 `IO<Result<T, E>>` 接口与统一错误载荷；当前宿主错误仍运行时拒绝。

- [ ] `read_text(path) -> IO<Text>` 受权限范围和整文件大小上限约束。
- [ ] `write_text(path, text) -> IO<Unit>` 使用同目录临时文件和原子替换。
- [x] ~~`read_stdin() -> IO<Text>` 读取到 EOF，最多 8 MiB；验证 UTF-8 并拒绝 NUL。~~
- [x] ~~`write_stdout(text) -> IO<Unit>` 按 action 顺序写入并刷新标准输出，不自动加换行。~~
- [x] ~~`arguments() -> IO<List<Text>>` 返回不含可执行文件名的参数快照；支持 `--run … -- 参数`。~~
- [ ] 增加不透明 `Bytes`、`read_bytes` 和 `write_bytes`，仅用于有限二进制 IO 与编解码。
- [ ] `parse_csv`、`format_csv` 提供纯转换；`read_csv`、`write_csv` 直接读写 Table。
- [ ] 完成受控 `Value` 类型细化，并提供 JSON 的纯转换与文件 IO；对象数组只在显式
  `table(value)` 后成为 Table。
- [ ] 增加复用 `Record<Table>` 的 Workbook 静态细化，并提供 XLSX 的纯转换、文件 IO
  和 `sheet(workbook, name)`。
- [x] ~~TXT 直接使用 UTF-8 Text API，不增加 `read_txt`、`write_txt` 同义名称。~~
- [x] ~~第一阶段不提供文件句柄、追加写入、目录遍历、网络、交互式逐行输入或无限数据流；时间和随机数由独立 IO action 提供。~~

## Time 与 Random

- [x] ~~增加可比较、可存入集合的 UTC `Instant` 值类型；支持结构比较、去重和稳定排序。~~
- [x] ~~`now() -> IO<Instant>`，并为测试运行器提供 `--fixed-time`。~~
- [x] ~~`parse_time`、`format_time`、Unix 秒数转换、`add_seconds` 和 `seconds_between`。~~
- [x] ~~`random()` 和 `random_between` 产生连续 Number 样本。~~
- [x] ~~`random_integer` 使用安全整数边界和无偏拒绝采样。~~
- [x] ~~`choose(list)` 从非 missing 元素抽样；`shuffle(list)` 使用 Fisher–Yates。~~
- [x] ~~使用版本化 `lan-pcg64-v1` 序列，并通过 `--random-seed` 支持跨平台复现。~~
- [x] ~~Time 和 Random 只能通过显式 IO action 观察宿主状态，不提供伪装成纯函数的全局状态。~~
- [x] ~~第一阶段不提供 sleep、后台定时器、时区数据库、密码学随机数或统计分布。~~

## 标准库设计原则

- [x] ~~操作名保持普通标识符；`map`、`filter`、`select`、`add`、`replace`、`find`、`any`、`all` 只在管道位置具有上下文含义。~~
- [x] ~~普通用户函数不自动广播；逐元素调用必须显式位于集合操作中。~~
- [x] ~~不保留 `where`、`add_column`、`replace_column` 等过时并行入口。~~
- [x] ~~要求 T 的参数在编译期拒绝未经解包的 Option<T>；显式接受 Option 的函数与集合元素缺失策略另行定义。~~
- [x] ~~每项能力只保留一个规范名称，不增加同义别名。~~
- [x] ~~所有集合操作保证有限，所有可观察输出顺序必须确定。~~
- [x] ~~纯标准库不加入隐式副作用；文件与终端能力只通过显式 `IO<T>` 提供。~~
- [x] ~~管道只负责传入左侧值，不引入方法调用语义。~~
- [ ] 为每个新增 API 明确输入类型、missing 策略、求值顺序、错误行为和复杂度。
- [ ] 优先组合现有小操作，避免增加同义名称和只为单一场景服务的函数。
