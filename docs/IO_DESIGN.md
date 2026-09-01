# Lan IO Monad 设计

本文件定义分阶段实现的 IO 语义。Lan 的普通表达式和函数保持纯函数；外部副作用只能
通过显式 `IO<T>` 值描述，并由程序入口顺序执行。当前时间与随机数的专门规则记录在
[`TIME_RANDOM_DESIGN.md`](TIME_RANDOM_DESIGN.md)。

当前进度：已实现 `Unit`、`IO<T>`、`io` 块、`<-`、纯值 action 的延迟构造/执行、
捕获环境 ARC、重复执行、标准输入输出和命令行参数。文件和其他外部 IO API 尚未实现。
下文使用 `read_text`、`write_text` 等函数的示例仍是待实现规范。

当前可运行的例子：

```lan
number(value) = io { value }
io {
  first <- number(10);
  next = first + 2;
  number(next)
}
```

## 目标与边界

- IO 必须在类型和源码中可见，普通函数不能隐式执行副作用。
- 构造 `IO<T>` 是纯计算；只有文件最后产生的 `IO<T>` 会由运行器执行一次。
- IO action 严格按源码从上到下执行，LLVM 不得删除、合并或跨 action 重排它们。
- 继续禁止递归、通用循环、无限流和隐式后台任务。
- 第一阶段不引入通用 Monad、类型类、用户可见的 `bind` 或通用 lambda。

纯程序仍然合法，文件最后的普通值继续由运行器显示。

## 类型

增加两个编译器内建类型：

- `IO<T>`：执行后产生 `T` 的外部操作。
- `Unit`：只有一个值 `()`，表示操作成功但没有有意义的结果。

Unit 已实现的规则：

- `()` 是单元字面量，不等于 `missing`；Unit 仍具有 Present/Missing/MaybeMissing 状态。
- `unit` 是普通名称，不作为字面量或保留字；类型名仍为 `Unit`。
- 可以作为函数参数、默认值、返回值、Record 字段以及 List 元素和 Table 列元素。
- 允许 Unit 间的 `==`、`!=`、`same`，不允许算术、排序比较或当作 Boolean 条件。
- `?`、`??`、`require` 和 `is_missing` 沿用普通值规则。
- 调试显示和 `to_text` 输出 `()`；这不是 JSON 序列化支持。
- 不分配 Unit 堆对象，可能缺失时编码为 `i1`；紧凑 List 以字节保存存在标记。

`IO<T>` 本身确定存在，`Option<IO<T>>` 才表示 action 可能缺失；`IO<Option<Number>>`
表示 action 成功后得到一个可能缺失的 Number。两层可选性分别推断；执行 action 前必须
消除外层 Option，消费执行结果时再按结果类型决定是否需要解包。IO 失败不等于 missing。

第一阶段的 `IO<T>`：

- 可以作为函数参数、返回值和局部绑定。
- 不允许比较，也不接受 `same()`。
- 不允许序列化。
- 不允许放入 List、Record 或 Table。
- 捕获的不可变参数必须满足堆对象 DAG，并由 ARC 保持到 action 执行完成。

同一个 action 值每执行一次都会重新产生一次外部操作，不做隐式缓存。

## `io` 块

`io` 块提供专用的 Monad 顺序语法：

```lan
copy(source, destination) = io
  text <- read_text(source)
  write_text(destination, text)
  text
```

它具有以下规则：

- `name <- action` 要求右侧为 `IO<T>`，执行 action 并将结果绑定为 `name`。
- `name = expression` 是普通纯绑定，不执行 IO。
- 非末尾的裸表达式必须产生 IO action；执行后丢弃其结果。
- 返回 Result 的非末尾 IO 步骤不能隐式丢弃结果；绑定后处理，或用 `_ <- action` 明确忽略。
- 最后一行是块的结果。纯值自动提升为 `IO<T>`；IO action 则直接作为最后一步执行。
- 块严格从上到下执行，运行时拒绝会停止后续 action；返回 `err(error)` 是普通数据，不自动短路。
- 缩进与花括号/分号形式必须降低为同一个 AST。

编译器可以在内部将其理解为 `bind` 和 `pure`，但不向用户暴露这些函数或内部 lambda。

```lan
program = io {
  text <- read_text("input.txt");
  write_stdout(text);
  ()
}
```

`if` 可以选择不同 action，但两个分支必须像普通条件表达式一样具有统一类型。

## 程序入口

- 文件最后产生普通值时，行为与当前版本一致：求值并显示结果。
- 文件最后产生 `IO<T>` 时，运行器执行该 action 一次，并显示其最终 `T`。
- action 本身为 missing 时拒绝执行；执行成功而数据结果为 missing 时显示 `missing`。
- 没有到达文件最后结果的 action 只是未执行的普通值。
- 构建产物不得在静态初始化或程序装载期间执行用户 IO。

例如：

```lan
program = io
  text <- read_text("input.txt")
  write_stdout(text)

program
```

## 第一阶段 API

IO 库只搬运 UTF-8 Text，并保持文件系统与数据格式解耦：

```text
read_text(path)         -> IO<Text>
write_text(path, text)  -> IO<Unit>
read_stdin()            -> IO<Text>
write_stdout(text)      -> IO<Unit>
arguments()             -> IO<List<Text>>
```

其中 `read_stdin`、`write_stdout`、`arguments` 已实现；文件 API 尚未实现。

### 标准输入输出与参数的确定规则

- 三个函数均只构造 action；未执行的 action 不读取、写入或验证外部数据。
- `read_stdin()` 读取到 EOF，保留原有换行，空输入返回空 Text；每次执行继续消费输入，
  已到 EOF 时再次执行返回空 Text。输入上限固定为 8 MiB（字节），超过立即拒绝。
- 标准输入必须是有效 UTF-8；当前 Text 为 NUL 结尾表示，因此输入中的 NUL 被拒绝。
  读取可能等待终端或管道的输入/EOF；资源上限不保证外部 IO 在有限时间内完成。
- `write_stdout(text)` 要求存在的 Text，写入原文、不添加换行，成功前刷新输出，返回 `()`。
  输出不是原子的，失败前可能已经写出部分内容；关闭的管道报告 IO 错误而不是因 SIGPIPE 终止。
- `arguments()` 不包含可执行文件名，保留顺序、空参数及空格。执行时验证 UTF-8，
  参数最多 65536 项、合计最多 8 MiB，每次执行返回新的不可变 List 快照。
- `lanc --run program.lan -- 参数...` 原样转发参数；直接执行生成的程序也支持参数。
- 运行时错误写入 stderr，保留 action 名、源码位置和调用链；系统错误附带 errno 和原因。
  失败后不执行后续步骤。程序入口仍自动显示最终结果，包括 Unit 的 `()`。

CSV 和 JSON 使用纯编解码函数组合：

```lan
program = io
  source <- read_text("people.csv")
  people = require(parse_csv(source))
  adults = people |> filter(.age >= 18)
  write_text("adults.csv", format_csv(adults))
```

第一阶段不提供 `read_csv`、`read_json`、文件句柄、追加写入、目录遍历、网络、当前时间、
随机数、交互式逐行输入或无限数据流。当前时间和随机数在基础 IO 稳定后按独立设计加入。

## 后续数据格式层

第一阶段完成后增加 JSON、CSV 和 XLSX，但保持“原始 IO、纯转换、便捷 IO”三层分离：

1. `read_text`、`write_text`、`read_bytes`、`write_bytes` 只搬运原始内容。
2. `parse_*`、`format_*` 是不执行 IO 的纯转换。
3. `read_*`、`write_*` 是面向常用文件工作流的 `IO<T>` 便捷操作。

TXT 就是 UTF-8 Text，不增加 `read_txt`、`write_txt` 等同义名称。

### CSV

CSV 是单个具名列矩形数据，因此直接转换为 Table，而不是先公开为 `List<Record>`：

```text
parse_csv(text)       -> Option<Table>
format_csv(table)     -> Text
read_csv(path)        -> IO<Table>
write_csv(path, table) -> IO<Unit>
```

- 第一行必须是非空且不重复的列名；暂不支持无表头 CSV。
- 每行必须具有相同列数，行顺序保持不变。
- 未加引号的空字段产生 missing；加引号的空字段 `""` 产生存在的空 Text。
- 每列忽略 missing 后整体推断为 Boolean、Number 或 Text；不能统一为前两者时整列为 Text。
- 全 missing 列默认细化为 `List<Text>`。
- 输出只接受 Table；Number、Boolean、Text 和 missing 可以写为单元格，嵌套 List 和 Record
  单元格运行时拒绝。
- Table 在语义上没有列顺序，因此默认输出按字段名 UTF-8 字节顺序排列；以后可以增加
  显式 `columns` 参数控制输出列序，但不把输入文件顺序附着在 Table 值上。

`read_csv` 在概念上组合 `read_text` 与 `parse_csv`，但保留具体的路径、行号和列号诊断；
它不是另一套 CSV 数据模型。

### JSON

JSON 保持其树形结构，不自动猜测为 Table：

```text
parse_json(text)       -> Option<Value>
format_json(value)     -> Text
read_json(path)        -> IO<Value>
write_json(path, value) -> IO<Unit>
```

- `null` 映射为 missing，对象映射为 Record，数组映射为单类型 List。
- JSON Number 映射为 Lan Number；超出可无损表示范围的数值必须拒绝。
- 数组必须能统一为一种 Lan 元素类型，否则解析失败。
- JSON 对象数组不会自动变成 Table；需要时显式调用 `table(value)`。
- 只有 Number、Boolean、Text、List、Record、Table 和 missing 可以格式化为 JSON；`IO`、
  `Unit` 及其他宿主对象不可序列化。
- `Value` 是对现有动态值表示的受控公开类型，需要在实现 JSON 前单独完成类型细化规则；
  它不是绕过 List 单类型约束的任意动态容器。

### XLSX

XLSX 是多工作表容器，因此整个文件不直接等同于一个 Table。增加静态细化类型
`Workbook = Record<Table>`，运行时仍复用 Record 和 Table：

```text
parse_xlsx(bytes)          -> Option<Workbook>
format_xlsx(workbook)      -> Bytes
read_xlsx(path)            -> IO<Workbook>
write_xlsx(path, workbook) -> IO<Unit>
sheet(workbook, name)      -> Option<Table>
```

- Record 键是工作表名称，值是对应的 Table；工作表顺序不成为语言语义。
- 每张工作表的首行作为列名，并采用与 CSV 相同的列类型推断和 missing 规则。
- 公式只读取文件内缓存结果，不在 Lan 中执行 Excel 公式。
- 合并单元格只保留左上角值，覆盖范围内其余单元格为 missing。
- 第一版只处理 Number、Boolean、Text 和空单元格；日期、错误单元格、样式、图表、宏和
  公式写回需要另行设计。
- `format_xlsx` 创建新的数据工作簿，不承诺保留原文件样式或未知部件。

`Bytes` 是不可变、不透明、不可比较的堆值，只用于二进制 IO 和编解码；使用空指针
niche 表示 missing，并遵循 ARC 与堆对象 DAG。它不提供任意逐字节编程接口。

## 错误与 missing

下列情况是 IO 执行错误并立即终止后续 action：

- 文件不存在或没有权限。
- 路径无效。
- 读取超过资源限制。
- 写入或原子替换失败。
- 标准输入或标准输出失败。

诊断必须包含源码位置、Lan 调用链、action 名称、相关路径和宿主系统错误。第一阶段不
提供宿主错误捕获；语言已支持 `Result<T, E>`、ok/err、match 与 require，但标准 IO
尚未迁移为可恢复 Result 接口。Result 不自动捕获运行时拒绝，也不把 IO 错误转换为 missing。

`missing` 继续只表示数据层面的缺失。`?` 只传播 missing，不捕获 IO 错误，也不采用
`OptionT IO` 的“退出整个 io 块”语义。需要拒绝 missing 时使用 `require`，需要条件执行
action 时使用显式 `if`。

每个 `=` 绑定的右侧、`<-` 的右侧、裸 action 和最后的表达式各自形成 `?` 的边界。
纯绑定得到 missing 后继续后续步骤；若 `<-` 的右侧表达式可能得到 missing，则编译期
拒绝，必须先选用备用 action 或使用显式 `require`。最后的纯表达式可返回 missing，
最后的 action 则必须存在。

## 文件写入语义

- 每个文件写入使用同目录临时文件并原子替换目标路径。
- 多个 action 不是一个事务；后续 action 失败时不回滚已经完成的 action。
- 后续读取必须可以观察此前已经完成的写入。
- 文件路径按 action 实际执行时解析。

运行器必须提供可配置的读取和写入范围，并在打开文件前完成规范化路径与权限检查。

## Lowering 与运行时

- `io` 块降低为显式顺序控制流；纯计算仍可在 action 之间正常优化。
- IO runtime 调用不得标记为 `readnone`、`readonly` 或其他允许错误重排的属性。
- action 捕获环境是不可变 DAG；重复使用 action 值不复制捕获的堆对象，只增加所需引用。
- 每个 action 开始前建立诊断上下文，完成或失败后按所有权规则释放参数和结果。
- 整文件读取必须受大小上限约束，避免以无限或不受控资源破坏语言的有限执行边界。

## 实现阶段

1. ~~增加 `Unit`、`IO<T>` 类型检查和禁止使用位置。~~ **已完成**。
2. ~~解析并验证缩进与花括号形式的 `io` 块和 `<-` 绑定。~~ **已完成**。
3. ~~实现只包含 `pure` 步骤的 IO lowering，验证顺序控制流和最终执行入口。~~ **已完成**。
4. ~~实现标准输入输出、命令行参数及结构化错误诊断。~~ **已完成**。
5. 实现受限的原子 Text 文件读写。
6. ~~完成 action 捕获环境的 ARC、DAG 验证及重复执行测试。~~ **纯 action 和标准 IO 已完成**：
   只捕获现有不可变值，构造后不暴露环境更新接口；递归函数及无限嵌套类型被拒绝。
   标准 IO 已验证 Text 捕获、Text/List 结果及错误路径的所有权；后续文件 API 需继续验证。
7. 增加 `Bytes` 和受限二进制文件 IO。
8. 实现 CSV 的纯转换和直接 Table IO，并验证确定列序、类型推断和单元格诊断。
9. 完成受控 `Value` 类型细化后实现 JSON 转换与 IO。
10. 增加复用 Record/Table 的 Workbook 细化，再实现 XLSX 转换与 IO。
11. 按 `TIME_RANDOM_DESIGN.md` 增加 Instant、测试时钟和版本化随机序列。
