# Lan 集合与管道语法设计

## 1. 数据模型

Lan 的核心复合数据结构为 `Record`、`List<T>` 和 `Table`：

| 类型 | 逻辑含义 | 示例 |
| --- | --- | --- |
| `Record` | 一个带字段的值 | `{name: "Alice", age: 20}` |
| `List<T>` | 有序的同类型元素集合 | `[1, 2, 3]` |
| `Table` | 有固定 schema 的有序 Record 集合 | `table({name: [...], age: [...]})` |

完整的逻辑值层次为：

```text
Value
├── Scalar
│   ├── Number
│   ├── Boolean
│   └── Text
├── Record
├── List<T>
└── Table
```

`missing` 不是与这些类型并列的新类型。所有类型在语言语义上都可以表示为
`Option<T>`，因此 Number、Record、List 和 Table 等值都可能缺失。函数不是 Value；
Lan 不提供可保存、传递或返回的函数值。

Table 在语言语义上是 `List<Record>` 的结构化、列式版本，但不是类型上的
`List<Record>`。

Table 具有以下性质：

- 每列长度相同。
- 每列的非 missing 元素类型统一。
- 每个逻辑行具有相同字段集合，缺失单元格用 `missing` 表示。
- 行顺序由所有列的相同位置共同确定。
- Table 是静态细化类型，运行时仍可使用 `Record<List<T>>` 表示。
- 动态字段允许 schema 只能在运行时完整确定；静态已知字段记录在 TableShape 中。

核心原则是：

> List 和 Table 共享逐元素语义；Record 定义元素的字段语义；Table 只在存储、
> schema、类型结构和容器形态保持方面特殊，不拥有另一套元素表达式语言。

## 2. 统一管道

所有管道阶段都必须写出函数名和括号：

```lan
value |> function(arguments)
```

普通管道阶段把左侧值作为函数的第一个参数：

```lan
numbers |> sum()           # sum(numbers)
people |> take(10)         # take(people, 10)
people |> sort_by("age")  # sort_by(people, "age")
```

不提供省略函数名、括号或参数边界的流式形式。

## 3. 元素表达式

在建立元素上下文的集合操作中：

```lan
.       # 当前逻辑元素
.name   # 当前元素的固定字段
.(key)  # 当前元素的动态字段
```

List 与 Table 使用完全相同的字段语义：

```lan
users |> map(.name)
users |> filter(.age >= 18)
```

如果 `users` 是 `List<Record>`，当前元素是 List 元素；如果 `users` 是 Table，当前
逻辑元素是 Table 的一行 Record。

需要命名当前元素时使用元素绑定子句：

```lan
users |> map(user => user.name)
users |> filter(user => user.active and user.age >= 18)
```

`name => expression` 不是通用 lambda：

- 只能出现在明确允许元素表达式的集合操作中。
- 名称只在右侧表达式中有效，并且不可变。
- 可以读取外层不可变绑定。
- 不能保存、返回或作为普通函数参数传递。
- 不产生函数对象、闭包或函数指针。
- 编译器直接将它降低为循环或列式执行计划。

复杂元素逻辑可以调用普通具名函数：

```lan
adult(age) = age >= 18
double(value) = value * 2

users |> filter(adult(.age))
numbers |> map(double(.))
```

## 4. 普通函数不自动广播

普通函数调用严格按照参数类型传值，不自动提升成逐元素调用：

```lan
adult(ages) # adult 接受 Number 而 ages 是 List<Number> 时产生类型错误
```

逐元素调用必须明确写在集合操作中：

```lan
ages |> map(adult(.))
users |> filter(adult(.age))
```

整体调用和逐元素调用具有不同且明确的写法：

```lan
values |> normalize()        # normalize(values)
values |> map(normalize(.))  # 逐元素调用 normalize
```

## 5. 核心集合操作

### 5.1 `map`

`map` 执行元素映射，并且总是返回 List：

```text
List<A> |> map(A -> B)   -> List<B>
Table   |> map(Row -> B) -> List<B>
```

无论 `users` 是 `List<Record>` 还是 Table：

```lan
users |> map(.name)
```

都返回 `List<Text>`。映射为 Record 仍然返回 `List<Record>`：

```lan
users |> map({
  name: .name,
  adult: .age >= 18
})
```

### 5.2 `filter`

`filter` 删除不满足条件的元素，但保留输入的容器种类和元素结构：

```text
List<T> |> filter(T -> Boolean?)   -> List<T>
Table   |> filter(Row -> Boolean?) -> Table
```

predicate 接受规则为：

```text
true    -> 保留
false   -> 删除
missing -> 删除
```

普通运算严格拒绝 missing。需要允许 predicate 中的 missing 继续传播时使用 `?`，
需要替代值时使用 `??`。完整规则见“missing 语义”一节：

```lan
users |> filter(.age? >= 18)
users |> filter((.age ?? 0) >= 18)
```

### 5.3 `select`

`select` 是 Table 专用的 schema 投影和计算操作，并且总是返回 Table：

```lan
users |> select({
  name: .name,
  adult: .age >= 18
})
```

`select` 只接受产生 Record 的结构形式。单列 Table 必须显式保留列名：

```lan
users |> select({name: .name})
```

而 `users |> map(.name)` 返回普通 `List<Text>`。

### 5.4 `add` 与 `replace`

`add` 添加新列，`replace` 替换已有列；二者都返回 Table：

```lan
people |> add({
  adult: .age >= 18,
  adjusted: .score + 5
})

people |> replace({
  score: .score + 5
})
```

`add` 遇到已有字段时拒绝，`replace` 遇到不存在的字段时拒绝。动态列名复用 Record
动态键：

```lan
people |> add({
  (column_name): .score * 2
})
```

核心接口为：

```lan
collection |> map(element_expression)
collection |> map(name => element_expression)

collection |> filter(predicate)
collection |> filter(name => predicate)

table |> select(record_expression)
table |> add(record_expression)
table |> replace(record_expression)
```

其中：

```text
map     总是返回 List
filter  保留输入容器种类
select  总是返回 Table
add     返回具有新增字段的 Table
replace 返回字段值被替换的 Table
```

## 6. missing 语义

### 6.1 基本模型

`missing` 表示数据不存在，不表示程序执行错误。核心数字运算、比较和转换要求实际
操作数存在：

```lan
missing + 1       # 运行时拒绝
missing >= 18     # 运行时拒绝
```

处理 missing 使用四种显式操作：

```lan
value?             # 将 missing 传播到当前表达式边界
value ?? fallback  # missing 时延迟计算并使用 fallback
is_missing(value)  # 检查是否 missing
require(value)     # missing 时运行时拒绝，否则取出值
```

`is_missing` 可以触发分支内的存在性细化：

```lan
if not is_missing(value)
  value + 1
else
  0
```

第一个分支中的 `value` 被细化为确定存在。暂不提供与 `is_missing` 含义相反的重复
别名。

### 6.2 `filter` 与 `find`

`filter` 只保留 predicate 结果恰好为 `true` 的元素：

```text
true    -> 保留
false   -> 删除
missing -> 删除
```

这项规则只消费最终 predicate，不改变 predicate 内部运算的严格性：

```lan
users |> filter(.active)       # active 为 missing 时删除
users |> filter(.age >= 18)    # age 为 missing 时比较运算拒绝
users |> filter(.age? >= 18)   # age 为 missing 时 predicate 为 missing，随后删除
users |> filter((.age ?? 0) >= 18)
```

`find` 使用相同的 predicate 判断：`true` 表示命中，`false` 或 `missing` 表示继续
查找。

### 6.3 Boolean 三值逻辑

`and`、`or` 和 `not` 是明确的 missing-aware Boolean 运算，采用三值逻辑：

```text
not missing       -> missing

false and missing -> false
true  and missing -> missing
missing and false -> false
missing and true  -> missing

true  or missing  -> true
false or missing  -> missing
missing or true   -> true
missing or false  -> missing
```

求值顺序固定为从左到右。`and` 的左侧为 `false`、`or` 的左侧为 `true` 时不求值
右侧；左侧为 missing 时，仍需根据右侧判断结果是否可以确定。

### 6.4 `any` 与 `all`

`any` 和 `all` 接受与 `filter` 相同的元素表达式或元素绑定：

```lan
users |> any(.active)
users |> all(.age? >= 0)
```

`any` 的结果为：

```text
存在 true                 -> true
全部为 false              -> false
没有 true，但存在 missing -> missing
```

`all` 的结果为：

```text
存在 false                 -> false
全部为 true                -> true
没有 false，但存在 missing -> missing
```

二者从左到右短路：`any` 遇到 `true` 后停止，`all` 遇到 `false` 后停止。空集合采用
逻辑单位元：

```lan
[] |> any(.) # false
[] |> all(.) # true
```

### 6.5 函数边界

函数可以返回、传递或保存 missing；调用者不需要立即使用 `?`。只有严格消费者实际
取得 missing 时才运行时拒绝：

```lan
value = find_age()
value                    # 可以是 missing
find_age() + 1           # 实际缺失时拒绝
find_age()? + 1          # 显式传播
(find_age() ?? 0) + 1    # 显式替代
```

普通函数仍然不自动广播，Option 行为也不能因为参数位于 List 或 Table 中而改变。

### 6.6 missing 与运行时错误

除零、无效转换、错误的动态类型和其他无效操作是运行时错误，不自动转换成 missing：

```lan
1 / 0              # 运行时拒绝
to_number("hello") # 运行时拒绝
```

除数为 `+0.0` 或 `-0.0` 都视为除零。List 只检查实际参与运算且两侧都存在的元素：
被较短长度截去的位置不求值，任一侧为 missing 的元素直接得到 missing。实现可以先按
语言顺序扫描第一个有效的零除数，再执行 SIMD 除法，从而同时保持确定的错误顺序和
紧凑列性能。

需要安全转换时应使用显式返回 Option 的 `try_*` 操作，而不是改变普通转换的错误
语义；具体转换集合另行定义。

聚合操作继续采用各自明确的 missing 策略：`sum`、`average`、`min` 和 `max` 忽略
missing，`count` 统计非 missing 元素，`same` 将 missing 当作可比较的结构状态。

### 6.7 运行时诊断

missing 或动态类型导致的运行时拒绝应报告：

- 源文件、行和列。
- 当前函数调用链。
- List 元素索引。
- Table 行号。
- 涉及的字段或字段路径。
- 实际遇到的类型或 missing 状态。

例如：

```text
第 18 行：比较运算需要 Number，但 users 第 27 行的 age 字段为 missing
调用链：report -> active_users
```

不为每个值长期附带调试元数据。标量循环维护当前索引；列式或 SIMD 执行在可能失败
时必须定位按语言求值顺序出现的第一个错误。

## 7. 其他集合操作

以下操作对整个管道值执行一次：

```lan
items |> first()
items |> last()
items |> length()
items |> take(10)
items |> drop(5)
items |> fill_missing(0)

numbers |> sum()
numbers |> average()
numbers |> min()
numbers |> max()
numbers |> count()

people |> row_count()
people |> sort_by("score")
```

`count()` 表示统计非 missing 元素；Table 的总行数使用 `row_count()`。暂不提供
`count(predicate)`，满足条件的行数使用组合形式：

```lan
people
  |> filter(.age? >= 18)
  |> row_count()
```

`find` 使用与 `filter` 相同的元素表达式和元素绑定语法：

```lan
numbers |> find(. > 0)
users |> find(.name == "Alice")
users |> find(user => user.active and user.age >= 18)
```

类型与行为为：

```text
List<T> |> find(T -> Boolean?)   -> Option<T>
Table   |> find(Row -> Boolean?) -> Option<Record>
```

- 严格按照集合顺序从左到右求值。
- 返回第一个 predicate 为 `true` 的非 missing 元素。
- predicate 为 `false` 或 `missing` 时继续查找。
- missing 元素自身不会成为匹配结果。
- 没有匹配结果时返回 `missing`。
- 找到结果后不再求值后续元素。
- Table 只需在匹配后物化一个行 Record；不需要预先构造全部行。

由于 `find` 具有可观察的短路和错误顺序，只有在编译器证明 predicate 不会产生运行时
拒绝时，才允许用整列或 SIMD 方式提前计算 predicate；否则必须保持从左到右执行。

`any(predicate)` 和 `all(predicate)` 的三值逻辑见 missing 语义一节。分组、连接、
去重、反转、压平和基于元素表达式的排序不属于本阶段设计范围。

## 8. `table()` 转换

`table()` 支持两种输入：

```text
table(Record<List<T>>) -> Table
table(List<Record>)    -> Table
```

- `table(Record<List<T>>)` 只验证和细化，不复制 Record 或 List。
- `table(List<Record>)` 执行行转列，需要分配和物化。
- `map({...}) |> table()` 可以由编译器融合为直接构造结果列。
- 常规 Table schema 变换优先使用 `select`。

## 9. Table lowering 与 SIMD

Table 的逐行 Record 只是抽象语义，不是强制运行时表示。编译器根据表达式选择执行
方式：

1. `map(.field)` 直接复用或借用对应列。
2. 简单数字、比较和布尔表达式降低为紧凑列循环。
3. 在不改变从左到右错误语义的前提下，允许 LLVM 对适合的循环执行 SIMD 向量化。
4. 普通用户函数退化为逐行标量循环，只提取需要的字段，不构造完整行 Record。
5. `map({...}) |> table()` 融合为直接构造结果列。

SIMD 是执行优化，不属于可观察的语言语义，也不保证每个表达式都能向量化。实现应为
Number、Boolean 和指针列提供紧凑数组及存在位，并向 LLVM 提供足够的类型、对齐和
别名信息；无法向量化时使用语义等价的标量循环。

未来改用 Arrow 列、数据库执行计划或 GPU buffer 时，也不得改变这里定义的元素语义。

## 10. 实现范围与状态

1. `|>` 与必须带括号的管道阶段。
2. 当前元素 `.` 和 `.field`。
3. 受限元素绑定 `name => expression`。
4. List 与 Table 共用的 `map`、`filter` 元素语义。
5. List 与 Table 共用、从左到右短路的 `find`。
6. `any`、`all` 和 Boolean 三值短路逻辑。
7. `is_missing`、`require` 和分支存在性细化。
8. Table 专用的 `select`、`add` 和 `replace`。
9. 普通函数不自动广播。
10. TableShape 静态细化与列式 lowering。
11. `table(List<Record>)` 行转列及相邻操作融合。
12. 紧凑列 ABI、LLVM 循环向量化和 SIMD 验证。
13. 带集合位置、字段路径和调用链的运行时诊断。

### 当前实现状态

- [x] `|>` 与必须带括号的管道阶段。
- [x] 当前元素 `.`、`.field`、`.(key)`。
- [x] 受限元素绑定 `name => expression`，不产生函数值或闭包。
- [x] List 与 Table 的 `map`、`filter`、`find`、`any`、`all`。
- [x] Table 的 `select`、`add`、`replace`。
- [x] 普通函数不自动广播。
- [x] Boolean 三值短路逻辑、`is_missing`、`require` 和简单分支存在性细化。
- [x] `table(Record<List<T>>)` 零复制验证与 `table(List<Record>)` 行转列。
- [x] TableShape 静态 schema 传播与更精确的固定字段类型；多个调用点的不同 schema 保守地取交集。
- [x] 固定字段 predicate 直接按列/行号读取，`find` 仅在命中后物化返回行。
- [x] `map(.field)` 在 TableShape 证明列存在时直接复用底层 List。
- [x] `map({...}) |> table()` 的相邻阶段融合；纯字段投影直接复用列，安全标量表达式在单个按行循环中直接写结果列。
- [x] 含 `?`、函数调用或动态类型的融合候选保守回退，避免改变可观察的错误顺序。
- [x] 编译期已知元素类型的紧凑列 ABI；Number 使用 `double` 与存在标记，Boolean 使用三态字节，指针列使用 null niche。
- [x] 一维 Number 列间算术、比较和 `??` 的专用循环及 LLVM SIMD 验证。
- [x] 标量广播、Boolean/Text、一元运算和嵌套广播按叶类型生成紧凑结果；嵌套外层使用指针列。
- [x] Number 标量广播使用专用紧凑循环并允许 LLVM SIMD；除法先按语言顺序检查零除数。
- [x] 动态类型继续使用语义等价的标量回退；Boolean 三态列的列间及标量逻辑、相等比较和 `??` 使用专用紧凑循环。
- [x] 用户函数直接接收 Table 逻辑行时在调用点内联函数体；固定字段按列和行号读取，不物化完整 Record。
- [x] 带源码行列、List 索引、Table 行号、字段路径和函数调用链的结构化运行时诊断；集合位置只在执行循环中维护，不附着到值上。
