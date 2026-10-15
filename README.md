# Lan

Lan 是一门简单、简洁、优雅的通用脚本语言，采用纯函数式计算与显式 IO，使用 LLVM 编译为本地可执行文件。

Lan 面向日常脚本、自动化和数据处理，不局限于特定业务领域，也不以自然语言或无代码工具为目标。
语言以不可变数据、表达式和集合管道为核心，通过标准库逐步扩展应用范围。

通用脚本语言是应用定位，不表示所有能力均已实现：当前仍禁止递归和通用 `for`、`while` 循环，
集合计算使用有限管道；已支持标准输入输出、命令行参数、时间和随机数，文件读写等能力仍在规划中。

写 Lan 程序很像组织数学公式或数据处理管道：给事物起名字、描述计算关系，然后在文件最后写出想得到的结果。

为了集中说明单项规则，部分代码块只展示函数或代码块内部的片段；其中出现的相关名称假定已经在外围定义。

## 五分钟入门

最简单的程序就是一个值：

```lan
1 + 2 * 3
```

运行后显示：

```text
7
```

函数是一条带输入的公式：

```lan
square(x) = x * x

square(5)
```

条件使用缩进表示层次：

```lan
price(original, member) =
  if member
    original * 0.8
  else
    original

price(100, true)
```

函数内部可以给中间结果起名字：

```lan
circle_area(radius) =
  pi = 3.1415926
  area = pi * radius * radius
  area

circle_area(10)
```

这些名称是不可变的公式，不是可以反复修改的存储格。

### 缩进与花括号

多行块可以使用缩进，也可以显式使用花括号和分号。下面两段程序生成完全相同的
语法树和 LLVM IR：

```lan
calculate(value) =
  next = value + 1
  doubled = next * 2
  doubled
```

```lan
calculate(value) = {
  next = value + 1;
  doubled = next * 2;
  doubled
}
```

花括号块中的每个中间绑定后必须写 `;`，最后一个结果表达式后的分号可写可不写。
缩进形式使用 layout rule：同级行之间插入虚拟分号，退出缩进时插入虚拟右花括号；
没有开启新块的更深缩进行作为上一表达式的续行。虚拟标记与显式 `{`、`;`、`}`
共用同一套块文法。换行在显式花括号内部只用于排版，不分隔表达式。
`if` 和 `else` 后面都不写冒号；
条件也有两种等价写法：

```lan
choose(value) =
  if value > 0
    "正数"
  else
    "非正数"
```

```lan
choose(value) = {
  if value > 0 {
    "正数"
  } else {
    "非正数"
  }
}
```

`{name: value}` 仍然是 Record，冒号区分 Record 字段，等号和分号区分代码块绑定：

```lan
make_person() = {name: "小明", age: 20}
calculation() = {age = 20; age + 1}
```

### 默认参数

默认参数必须放在所有必填参数后面，调用时可以省略末尾的默认参数：

```lan
greeting(name, punctuation = "!") = "你好，" + name + punctuation

greeting("小明")
greeting("小红", "？")
```

默认表达式可以引用它前面的参数：

```lan
end_from(start, end = start + 10) = end

end_from(5)  # 15
```

以下定义会被拒绝，因为必填参数出现在默认参数后面：

```lan
invalid(first = 1, second) = first + second
```

省略参数和传入 `missing` 是两件不同的事。默认值只在参数被省略时计算：

```lan
keep(value = 10) = value

keep()        # 10
keep(missing) # missing
```

调用也可以在位置参数之后使用命名参数，从而跳过前面带默认值的参数：

```lan
greeting(name, punctuation = "!", excited = false) =
  ending = if excited
    punctuation + punctuation
  else
    punctuation
  "你好，" + name + ending

greeting("小明", excited = true)
```

位置参数必须放在命名参数之前。同一个参数不能重复传入；未知参数名和缺少必填参数
都会在编译期报错。

### 同名名称与遮蔽

同一缩进层级可以再次使用已有名称。右边的公式先读取旧值，然后新的不可变绑定
遮蔽旧绑定：

```lan
next() =
  value = 1
  value = value + 1
  value

next()
```

结果是 `2`。这不是把原来的 `value` 改成 `2`，而是创建了一个新的 `value`。

内层作用域也可以遮蔽外层名称；离开内层后，外层绑定仍然存在：

```lan
example(flag) =
  value = 10
  if flag
    value = 20
    value
  else
    value
```

函数名和同一个函数的参数名仍然不能重复。名称查找始终选择当前位置向外遇到的
最近绑定。

## 数字、是非与文字

Lan 自动判断值的种类，用户不需要写 `i64`、`f64` 或返回类型。

```lan
adult(age) = age >= 18

greeting(name) = "你好，" + name

message(name, age) =
  hello = greeting(name)
  if adult(age)
    hello + "，你已经成年"
  else
    hello + "，你还未成年"

message("小明", 20)
```

当前支持：

- 数字：`10`、`3.14`，底层使用 LLVM `double`
- 是/否值：`true`、`false`
- Unit：`()`，确定存在但没有额外内容的单一值
- 文字：`"Hello"`、`"你好"`
- 单类型列表：`[1, 2, 3]`、`["红", "绿", "蓝"]`
- Record：`{name: "小明", age: 20}`
- Result：`ok(value)` / `err(error)`，类型为 `Result<T, E>`
- 数字计算：`+`、`-`、`*`、`/`、`%`
- 文字连接：`"Hello, " + name`
- 比较：`==`、`!=`、`<`、`>`、`<=`、`>=`
- 逻辑：`and`、`or`、`not`，也兼容 `&&`、`||`、`!`
- 中文或英文名称

### Unit 与 missing

`Unit` 只有一个值 `()`，用于表示完成但没有有意义的返回内容。它不是 `missing`，
也不是 `false`、空 Record 或空 List：

```lan
finish(found) = if found { () } else { missing }

finish(true)           # ()
finish(false)?         # missing；最终结果显式允许缺失
finish(false) ?? ()    # ()
is_missing(())         # false
same((), missing)     # false
```

Unit 可以用于函数参数、默认值、返回值、Record 字段和 `List<Unit>`（包括 Table 列）。
它遵循既有的 `?`、`??` 和 `require` 规则；相等比较也支持 List 逐元素运算和广播。
Unit 不能参与算术或用作 `if` 条件，也不能与 Boolean 混成单类型 List。
`()` 是单元字面量，输出与 `to_text(())` 都使用文字 `()`。`unit` 不再是保留字，
可以用作变量名、函数名或字段名。类型仍称为 `Unit`，不引入其他元组类型。

底层 Unit 不分配堆对象：确定存在时为 LLVM 常量，可能缺失时用 `i1` 表示存在状态；
紧凑 `List<Unit>` 每个元素占一个存在标记字节，动态值使用独立的 Unit 标签。
它也用于表示 `IO<Unit>` 执行完成但没有额外结果。

### IO action 与顺序执行

`io` 块构造一个延迟执行的 `IO<T>` 值。构造时只捕获用到的不可变绑定，不运行块内
代码；`<-` 执行 action 并绑定结果，文件最终结果为 IO 时由程序入口执行一次：

```lan
number(value) = io { value }

program() = io
  first <- number(10)
  next = first + 2
  number(next)

program()  # 12
```

同一块也可以写成 `io { first <- number(10); next = first + 2; number(next) }`。
规则如下：

- `name = expression` 是纯绑定，右侧即使是 IO 也不会自动执行。
- `name <- action` 执行 IO；块内非末尾的裸表达式也必须是 IO，执行后丢弃结果。
- 若非末尾 IO 步骤返回 Result，不能隐式丢弃：必须绑定后处理，或写 `_ <- action` 明确忽略。
- 最后一个表达式若为纯值则作为结果，若为 IO 则执行它并使用结果，不产生嵌套 IO。
- 未使用的 action 不执行；同一 action 多次用于 `<-` 时，每次都会重新执行，不缓存结果。
- 每个步骤按顺序执行，错误立即停止后续步骤。`?` 只结束所在步骤的表达式，不跳过整个块。
- IO 可以作为函数参数、默认值、返回值及局部绑定；不能放进 List/Record/Table，不能比较、
  调用 `same` 或转换为 Text。`io` 中的 `<-` 只执行 IO；`do` 中的 `<-` 只解包数据状态。

action 自身可能为 missing 时编译拒绝执行，可以先用 `??` 选择备用 action。执行成功后产生的
missing 是普通数据，后续步骤可用 `??`、`is_missing` 或 `require` 处理；程序入口显示
IO 的最终数据结果时允许输出 `missing`。纯程序仍沿用最终 missing 必须显式 `?` 的规则。
`err(error)` 也是普通返回数据，不等于运行时拒绝，不会自动停止 IO 后续步骤；`<-` 只执行
IO，不解包 Result。现有标准 IO 的宿主错误仍运行时拒绝，尚未改成可恢复 Result 接口。

已实现以下标准 IO 函数，调用只构造 action，执行时才访问外部环境：

- `read_stdin() -> IO<Text>`：读取到 EOF，保留换行；空输入返回空 Text。
- `write_stdout(text) -> IO<Unit>`：写入原文，不自动加换行，返回前刷新输出。
- `arguments() -> IO<List<Text>>`：返回不含可执行文件名的命令行参数快照。

```lan
io {
  input <- read_stdin();
  args <- arguments();
  write_stdout(input);
  {characters: length(input), arguments: args}
}
```

标准输入最多 8 MiB，必须是有效 UTF-8 且不能包含 NUL；读取可能等待外部输入或 EOF，
大小限制不代表超时保证。同一读取 action 再次执行时继续消费输入，已经到 EOF 则返回空 Text。
命令行参数必须是有效 UTF-8，最多 65536 项、合计 8 MiB；空参数会保留。
IO 错误写入 stderr，包含 action、源码位置和调用链；系统错误还包含 errno，失败后停止后续步骤。
入口仍自动显示最终结果，包括 `IO<Unit>` 的 `()`；这与 `write_stdout` 的显式输出是两回事。
文件读写和文件访问权限控制尚未实现。完整规范见
[`docs/IO_DESIGN.md`](docs/IO_DESIGN.md)。

### 时间

`Instant` 是 UTC 时间线上的瞬间，内部保存整数秒和纳秒，遵循普通 missing 规则。
它可以保存在 List、Record 和 Table 中，支持比较、`same`、`sort` 和 `sort_by`。
Instant 与 Number 不隐式转换，也不直接使用 `+`、`-` 做时间算术。

```lan
{
  started = require(parse_time("2025-06-07T08:09:10.1234+10:00"));
  finished = add_seconds(started, 2.5);
  {
    utc: format_time(started),         # "2025-06-06T22:09:10.1234Z"
    elapsed: seconds_between(started, finished),  # 2.5
    later: finished > started          # true
  }
}
```

纯时间函数：

- `parse_time(text) -> Option<Instant>`：解析带 `Z` 或数字 UTC offset 的 RFC 3339
  时间戳；格式或日期无效返回 missing。
- `format_time(instant) -> Text`：输出规范 UTC 文本，最多九位小数秒并移除尾零。
- `time_from_unix(seconds)` / `unix_seconds(instant)`：与 Unix 秒数显式转换。
- `add_seconds(instant, seconds)`：添加有限秒数，返回新 Instant。
- `seconds_between(start, end)`：返回 `end - start`，可以为负。

第一阶段采用 Unix 时间模型：UTC 年份范围为 `0000` 至 `9999`，小数秒最多九位；
不支持闰秒 `:60`、本地时区名称或日历月份运算。输入可用小写 `t`/`z`；`-00:00`
按 UTC 解析，不保留原 offset。Number 输入的秒数舍入到最近纳秒，中点远离零；
转换回 Number 受浮点精度限制。时间算术拒绝 missing、非有限数和范围溢出。
这些函数不隐式广播；批量转换写为 `values |> map(parse_time(.))`。

当前时间只能通过延迟 IO action 获取：

```lan
io {
  clock = now();
  first <- clock;
  second <- clock;
  seconds_between(first, second)
}
```

每次执行 `now()` 都重新读取系统实时时钟，不缓存结果。它不是单调计时器，结果可能
相同或因系统校时倒退。测试时可固定所有 `now()` 的结果：

```sh
./build/lanc --run report.lan --fixed-time 2025-06-07T08:09:10Z
```

固定时间不进入 `arguments()`。完整规则见
[`docs/TIME_RANDOM_DESIGN.md`](docs/TIME_RANDOM_DESIGN.md)。

### 随机数

随机操作是延迟执行的 IO action；构造 action 不读取系统熵，也不推进随机序列：

```lan
io {
  probability <- random();                 # [0, 1)
  temperature <- random_between(-10, 35);  # [-10, 35)
  die <- random_integer(1, 6);              # 两端都包含
  item <- choose(["red", missing, "blue"]);
  order <- shuffle([1, 2, 3, 4]);
  {probability: probability, die: die, item: item, order: order}
}
```

`choose` 只在非 missing 元素中等概率选择；空 List 或全 missing List 返回 missing。
`shuffle` 使用 Fisher–Yates，保留全部元素及 missing 状态。无效边界会在 action 执行时
拒绝，并且不会消耗随机样本。同一个 action 每执行一次都会取得下一个样本。

默认种子来自操作系统熵源。测试和复现可以指定 `lan-pcg64-v1` 的安全整数种子：

```sh
./build/lanc --run simulation.lan --random-seed 42
```

相同 Lan 版本、种子、输入和 action 顺序产生相同序列。该随机库不用于密码学密钥、
令牌或安全随机字节。完整规则见 [`docs/TIME_RANDOM_DESIGN.md`](docs/TIME_RANDOM_DESIGN.md)。

### Text、Number 与转换函数

`length` 同时接受 Text 和 List。Text 长度按用户看到的 Unicode grapheme cluster 计算，
不是 UTF-8 字节数；组合字符和由零宽连接符组成的 emoji 各算一个字符：

```lan
length("你好")             # 2
length("👨‍👩‍👧‍👦")          # 1
trim("  Hello　")         # "Hello"
lowercase("ÄBC Σ")        # "äbc σ"
uppercase("äbc σ")        # "ÄBC Σ"
starts_with("report.csv", "report")  # true
ends_with("report.csv", ".csv")      # true
text_contains("report.csv", "port") # true
```

`trim` 使用 Unicode White_Space；`lowercase` 和 `uppercase` 使用 Unicode 简单大小写
映射。`split` 保留首尾和相邻分隔符产生的空 Text，空分隔符会被拒绝：

```lan
parts = split("a,,c,", ",")  # ["a", "", "c", ""]
join(parts, "|")             # "a||c|"
```

基础 Number 函数只接受标量，不对 List 隐式广播：

```lan
abs(-3.5)              # 3.5
floor(-1.2)            # -2
ceil(-1.2)             # -1
round(2.5)             # 2，使用 ties-to-even
round(12.25, 1)        # 12.2
clamp(15, 0, 10)       # 10
numbers |> map(abs(.)) # 明确逐元素调用
```

`round` 的小数位数必须是 `-308` 到 `308` 之间的整数值 Number。`clamp` 要求最小值
不大于最大值。这些函数拒绝 missing 和非有限 Number。

完整数学库还提供正确舍入为 Number 的 `pi`、`e` 和 `tau` 常量，以及以下标量函数：

```lan
sign(-8)             # -1；正负零都返回 0
sqrt(9)              # 3
pow(2, 3)            # 8
exp(0)               # 1
log(e)               # 1，默认以 e 为底
log(8, 2)            # 3
hypot(3, 4)          # 5

sin(pi / 2)          # 三角函数使用弧度
cos(0)
tan(0)
asin(0)
acos(1)
atan(0)
atan2(0, 0)
radians(180)         # pi
degrees(pi)          # 180
```

`sqrt`、`pow`、`log`、`asin` 和 `acos` 会检查定义域；所有数学函数都会拒绝非有限
输入和非有限结果。数学函数不隐式广播集合，应写成
`numbers |> map(sqrt(.))`。常量不是关键字，局部绑定可以遮蔽它们。

`parse_number` 严格解析完整 Text，只接受十进制及科学计数法，不忽略首尾空白；失败
返回 missing。`to_text` 接受现有数据值并产生确定的 Text 表示：

```lan
parse_number("-12.5e2")       # -1250
parse_number("12x") ?? 0      # 0
require(parse_number(input))   # 需要严格拒绝失败时
to_text([1, missing, 3])       # "[1, missing, 3]"
```

### 正则表达式

`regex(pattern)` 将 Text 编译为可复用的不透明 Regex，使用 RE2 语法：

```lan
digits = regex("[0-9]+")

{
  contains: "item-123" |> regex_contains(digits),  # true
  matches: "123" |> regex_matches(digits),        # true，要求全文匹配
  first: "item-123" |> regex_find(digits),         # "123"
  all: "a1 b22" |> regex_find_all(digits),         # ["1", "22"]
  parts: "a, b; c" |> regex_split(regex("[,;] *")) # ["a", "b", "c"]
}
```

模式参数必须是 Regex，不接受隐式的 Text 模式转换；无效模式在运行时拒绝。
`regex_find` 未匹配时返回 `missing`，`regex_find_all` 返回空 List。
`regex_split` 保留首尾空片段；空匹配按一个 Unicode code point 前进，例如
`regex_split("你好", regex(""))` 得到 `["", "你", "好", ""]`。
切断 UTF-8 字符边界的提取（例如 `regex_find("你", regex("\\C"))`）会被拒绝。

Regex 支持参数、返回值、List/Record 存储和 ARC。显示为 `<Regex>`，不暴露内部地址；
`to_text` 拒绝 Regex 及包含 Regex 的集合。`same` 比较原始模式文本，不判断两个不同
模式是否匹配同一个语言；Regex 不支持 `==`/`!=`。
普通 missing 参数仍被拒绝，显式 `?` 可以短路。没有隐式广播：

```lan
texts |> map(regex_find(., digits))
```

模式上限为 64 KiB，RE2 引擎内存预算为 8 MiB。不支持反向引用和环视；
大小写等选项使用模式内标记，例如 `regex("(?i)hello")`。详细规则见
[`docs/REGEX_DESIGN.md`](docs/REGEX_DESIGN.md)。

## 单类型列表

一个列表中的所有元素必须属于同一种类型。编译器自动推断元素类型，不需要用户写
`List<Number>` 这样的标注：

```lan
numbers() = [1, 2, 3]
names() = ["小明", "小兰"]
```

Number、Text 和 Instant List 可以稳定排序；missing 固定放在末尾：

```lan
sort([3, missing, 1, 2])       # [1, 2, 3, missing]
names |> sort()                # 管道形式
```

`distinct` 使用与 `same` 一致的结构化相等规则去重，保留每个值第一次出现的位置；多个
missing 只保留一个。它支持所有 List 元素类型，返回的新 List 会共享而不是复制 Text、
List、Record、Regex 和 Instant 元素：

```lan
distinct([2, 1, 2, missing, 1, missing])  # [2, 1, missing]
records |> distinct()
```

实现使用结构化哈希，平均时间与输入的总结构大小呈线性关系，额外空间与外层元素数量
呈线性关系；哈希冲突时仍调用结构化相等，因此不会因冲突错误去重。

混合列表会在编译期被拒绝：

```lan
[1, "two"]  # 编译错误
```

列表可以嵌套，但内层列表的元素类型也必须一致：

```lan
[[1, 2], [3]]       # List<List<Number>>
[[1], ["two"]]     # 编译错误
```

在函数中可以直接返回和传递列表，条件两边也必须产生相同的列表类型：

```lan
choose(flag) =
  if flag
    [1, 2, 3]
  else
    [4]

choose(true)
```

结果显示为：

```text
[1, 2, 3]
```

`missing` 是 `Option<T>` 的缺失状态，不算另一种元素类型，因此下面是合法的
`List<Number>`：

```lan
[1, missing, 3]
```

如果元素来自动态 Record 字段，无法在编译期确定实际标签，构造列表时会检查所有
非 missing 元素类型相同，不一致则运行时拒绝。

`length(numbers())` 返回列表元素数量。List 使用和 Record 一致的点号读取语法：

```lan
numbers.0             # 固定下标
numbers.(index)       # 运行时下标
```

下标从 `0` 开始。越界、负数、非整数下标以及存储值本身为 missing 都得到 `missing`，
可以用 `??` 提供默认值。下标必须是 Number；文字键仍表示 Record 动态字段读取。
固定形式只接受非负整数字面量；需要检查负数或小数时使用 `list.(-1)`、
`list.(1.5)`。连续读取可以直接写成 `matrix.1.0`。
列表不可变。Record 可以作为列表元素，但同一个列表的所有元素都必须是 Record。

List 支持与 Table 列一致的聚合函数：

```lan
sum([10, missing, 20])       # 30
average([10, missing, 20])   # 15
min([10, missing, 20])       # 10
max([10, missing, 20])       # 20
count([10, missing, 20])     # 2
```

聚合忽略列表内部的 `missing`。空列表或全 missing 列表的 `sum` 和 `count` 返回
`0`；`average`、`min` 和 `max` 返回 `missing`。前四个函数要求 `List<Number>`，
`count` 接受任意单类型 List。列表本身若是 `Option<List<T>>`，需要先显式解包；否则编译报错。

`first(list)` 和 `last(list)` 分别读取第一个和最后一个元素，空列表返回 `missing`。

`concat` 连接两个元素类型相同的 List；`flatten` 只展开一层；`reverse`、`take` 和
`drop` 返回新 List，原 List 保持不变。截取数量必须是非负整数，超过长度时截到容器
边界：

```lan
items = concat([1, 2], [3, 4])
flatten([[1, 2], [], [3, 4]])  # [1, 2, 3, 4]
flatten([[[1], [2]], [[3]]])   # [[1], [2], [3]]
reverse(items)       # [4, 3, 2, 1]
take(items, 2)       # [1, 2]
drop(items, 2)       # [3, 4]
items |> reverse() |> take(2)  # [4, 3]
```

`flatten` 的类型是 `List<List<T>> -> List<T>`。外层的 missing 子列表会被拒绝；子列表
内部的 missing 元素会保留。

### 集合管道与元素表达式

所有管道阶段都必须使用 `|>`，并写出函数名和括号：

```lan
numbers |> sum()                    # sum(numbers)
people |> take(10)                  # take(people, 10)
people |> sort_by("score")          # sort_by(people, "score")
```

不支持 `numbers sum`、`people take 10` 等省略 `|>` 或括号的旧写法。

`map`、`filter`、`select`、`add`、`replace`、`find`、`any` 和 `all` 只在 `|>` 后作为
集合操作名称；它们不是全局保留字，可以在普通调用位置定义成用户函数。

`map`、`filter`、`find`、`any` 和 `all` 建立元素上下文。在其中，`.` 是当前逻辑元素，
`.name` 和 `.(key)` 分别读取当前元素的固定或动态字段：

```lan
numbers |> map(. * 2)
users |> map(.name)
users |> filter(.age? >= 18)
users |> find(.name == "Alice")
users |> any(.active)
users |> all(.age? >= 0)
```

`.` 在 List 上表示当前 List 元素，在 Table 上表示当前逻辑行 Record。它不是管道左侧
整体值；元素上下文之外使用 `.` 或 `.field` 会编译报错。需要名称时使用受限元素绑定：

```lan
users |> filter(user => user.active and user.age >= 18)
```

`name => expression` 只允许作为上述集合操作的唯一元素表达式；它不是 lambda，不会
产生函数对象。表达式可以读取外层不可变绑定，也可以调用普通具名函数。

普通函数不会因为收到 List 而自动广播：

```lan
adult(age) = age >= 18

adult(ages)                  # ages 是 List 时编译错误
ages |> map(adult(.))        # 明确逐元素调用
```

`map` 总是返回 List；`filter` 保留 List 或 Table 的容器种类；`find` 返回第一个匹配元素
或 `missing`。`filter` 和 `find` 把最终 predicate 的 `missing` 当作不匹配。`any` 与
`all` 使用三值逻辑并从左到右短路。多行管道可以继续使用更深缩进排版。

## Table 与整列计算

Table 没有独立的字面量语法，也不是新的运行时数据类型。内置 `table(value)` 接受两种
输入形式。列式形式是 `Record<List<T>>`：每个字段都必须是 List，并且所有 List 长度
相同。对于直接传入的 Record/List 字面量，列类型和静态已知长度会在编译期检查；
动态构造的值仍在 `table` 执行时验证。

```lan
people() = table({
  name: ["小明", "小兰", "小红"],
  age: [20, 17, missing],
  score: [80, 90, missing]
})

people()
```

这种形式验证成功后返回原来的 Record 指针，不复制 Record 或任何 List。行式形式是
`List<Record>`，`table` 会将各行转换成等长的列：

```lan
table([
  {name: "小明", age: 20},
  {name: "小兰", age: 17}
])
```

编译器把结果静态细化为 Table；LLVM 和运行时都使用 `Record<List<T>>` 指针。普通
Record 或 `List<Record>` 不能直接传给表格操作，必须先经过 `table(...)`。

```lan
row_count(people)          # 3
sum(people, "age")         # 37
average(people, "score")   # 85
min(people, "age")         # 17
max(people, "age")         # 20
count(people, "score")     # 2
```

列可以直接与一个值或另一列进行逐元素算术和比较。标量会自动广播，
单元格中的 `missing` 只传播到对应的结果单元格：

```lan
people.score + 5           # [85, 95, missing]
people.age >= 18           # [true, false, missing]
fill_missing(people.score, 0)
```

两个 List 长度不同时，只计算双方都有元素的位置，结果长度以较短的 List 为准：

```lan
[1, 2, 3] + [10, 20]      # [11, 22]
[1, 2] < [0, 3, 4]        # [false, true]
```

这只适用于普通的逐元素运算；Table 仍然要求所有列长度相同，以保持矩形结构。

逻辑、一元运算、默认值和文字连接也支持逐元素处理及标量广播：

```lan
[true, false, missing] and false   # [false, false, false]
[true, false, missing] or true     # [true, true, true]
not [true, missing]                # [false, missing]
-[1, missing, -2]                 # [-1, missing, 2]
[1, missing, 3] ?? 0              # [1, 0, 3]
["A", missing] + "!"             # ["A!", missing]
```

逐元素 `and` 和 `or` 使用短路逻辑对应的三值结果：`false and missing` 是
`false`，`true or missing` 是 `true`；其余无法确定的组合得到 `missing`。
文字 `+` 只允许 Text 列与 Text 列或 Text 标量连接。`??` 的左侧为 List 时按元素
提供默认值；`missing ?? list` 仍表示整个 List 的默认值。

嵌套 List 使用从末尾维度对齐的递归广播。先比较静态嵌套层数，将较低维的操作数
广播到缺少的前置维度，然后逐层运算；双方到达同一维度后，每一层都截到较短长度：

```lan
[[1, 2], [3, 4]] == [1, 2]
# [[true, true], [false, false]]

[[1, 2, 3], [4]] + [[10, 20], [30, 40]]
# [[11, 22], [34]]
```

该规则只适用于 List。Table 必须先通过 `table.column` 取出一列，Table 值本身不参与
广播。

需要比较完整结构而不是广播时，使用 `same(left, right)`：

```lan
same([[1, 2], [3, 4]], [1, 2])                 # false
same([1, missing], [1, missing])               # true
same({a: 1, b: 2}, {b: 2, a: 1})              # true
```

`same` 总是返回单个 Boolean，不广播，也不要求两边类型相同。不同类型、不同 List
长度、不同嵌套结构或不同 Record 键集合返回 false。List 按位置递归比较；Record
忽略 HAMT 顺序，按键递归比较。两个 missing 相同，一个 missing 与存在值不同。
Table 按底层 `Record<List<T>>` 结构比较。

以下操作返回新的 Table，输入 Table 保持不变：

```lan
people |> filter(.age? >= 18)
people |> select({name: .name, adult: .age? >= 18})
people |> add({adult: .age? >= 18})
people |> replace({score: .score ?? 0})
people |> select_columns(["name", "score"])
people |> take(10)
people |> drop(10)
people |> sort_by("score")                         # 稳定升序，missing 放末尾
```

`select` 的元素表达式必须产生 Record，并总是返回 Table。`add` 遇到同名字段会运行时
拒绝，`replace` 遇到不存在的字段也会运行时拒绝。`take` 和 `drop` 的行数必须是非负整数，
超过总行数时会截到表格边界。`select_columns` 遇到不存在的列会运行时拒绝。
列名列表不能为空，因为当前的 `Record<List<T>>` 表示无法为零列表格单独保存行数。

`sum`、`average`、`min` 和 `max` 只接受数字列；`count` 可以用于任何列，返回其中
非 missing 单元格的数量。聚合会忽略 missing。空列或全 missing 列的 `sum` 为 0、
`count` 为 0，而 `average`、`min`、`max` 返回 missing，因此可以用 `??` 处理：

```lan
average(people, "unknown")       # 列不存在，运行错误
average(table({age: []}), "age") ?? 0
```

Table 底层仍是 `Record<List<T>>`：HAMT 负责列名查找，单类型 List 保存实际列数据。
表格验证只遍历列描述和读取列表长度，不遍历或复制单元格。筛选、截取、排序、
整列计算会为结果分配新的 List 和 HAMT 路径，但不会修改输入 Table。

编译器会为静态已知的列传播 TableShape。Table 元素表达式中的 `.field`
直接按列和行号读取，不会先构造完整行 Record；`table |> map(.field)`
可直接复用已验证的底层 List。`find` 在检查 predicate 时也不物化行，
只在命中后构造需要返回的 Record。
`map({...}) |> table()` 会在不改变求值和错误顺序时融合：纯字段投影直接
复用源列，安全的算术、比较和逻辑表达式在单个按行循环中写入结果列。
含 `?`、函数调用或动态类型时会保守回退到通用实现。

## Record

Record 是字段名称到值的不可变映射，不是具有方法的对象。字段本身可以保存任意
Lan 值，不属于 Record 的静态类型；因此不同 Record 不需要预先声明相同的字段集合。

```lan
make_person() = {name: "小明", age: 20}

make_person()
```

固定字段和运行时确定的字段统一使用点号读取，不提供 `record[key]`：

```lan
read(person) =
  field = "age"
  [person.name, person.(field)]

read({name: "小明", age: 20})
```

Record 字面量也允许由 Text 表达式提供动态字段名：

```lan
make_person() =
  field = "age"
  {name: "Alice", (field): 21}

make_person()
```

`{field: 21}` 的字段名固定为 `"field"`，而 `{(field): 21}` 使用变量 `field` 的值。
动态键表达式必须产生 Text。

不存在的字段得到 `missing`。无法证明存在的字段读取为 `Option<T>`，可以用 `??`
提供默认值；未解包就用于普通运算会编译报错。编译器能跟踪不可变 Record 的已知固定
字段和已验证 Table 的已知列；动态键或未知字段仍需显式处理。

```lan
person.city ?? "未知"
person.height + 1       # height 可能不存在时编译错误
```

函数块中可以用简洁的更新写法添加或替换字段：

```lan
birthday(person) =
  old = person
  person.age = person.age + 1
  person.city = "悉尼"
  [old, person]
```

`person.age = value` 不是可变赋值，而是下面两步的语法糖：创建一个更新后的新 Record，
再让同级的新 `person` 绑定遮蔽旧绑定。`old` 仍指向更新前的值。动态键也可以写成
`person.(field) = value`。

需要显式保留原绑定时，可以使用 `with` 一次添加或替换多个字段：

```lan
move(person) =
  person with {
    age: 21,
    city: "Sydney"
  }

move({name: "Alice", age: 20})
```

更新块同样支持 `(field): value` 动态键。嵌套更新写成
`person.address.city = "Sydney"`，它会从内向外构造新的 Record 路径。更新目标必须以
局部绑定名称为根；`get_person().age = 21` 会在编译期被拒绝。缺失或不是 Record 的
中间字段会在运行时拒绝。直接的 `person.self = person` 也会被拒绝。

Record 在语义上无序。为了让结果可复现，显示 Record 时按字段名称的 UTF-8 字节顺序
排序；顺序不是插入顺序。

`fields(record)` 按同一确定顺序返回字段名称，`has_field(record, key)` 判断键是否存在。
字段存在和值为 missing 是两件不同的事：

```lan
person = {name: "Alice", age: missing}

fields(person)                 # ["age", "name"]
has_field(person, "age")      # true
has_field(person, "score")    # false
```

`merge` 合并两个 Record，右侧同名字段覆盖左侧；`remove_field` 返回删除字段后的新
Record，字段不存在时返回结构相同的 Record。输入 Record 都不会改变：

```lan
merge({name: "Alice", age: 20}, {age: 21})       # {age: 21, name: "Alice"}
remove_field({name: "Alice", age: 20}, "age")  # {name: "Alice"}
```

底层使用持久化 HAMT。更新只复制从根到目标字段的路径，其余分支由新旧 Record 共享，
HAMT 节点用引用计数管理。Text、List、Record、Table、Regex、Instant 和 IO 都纳入 ARC；复合值持有其子
对象，函数参数借用调用者的值，新建值由当前函数的所有权区域持有，返回值在区域清理前
转移给调用者。运行时内部以 C++ RAII 表示临时拥有的引用，并通过 `owned`/`shared`
容器写入区分所有权转交与共享；LLVM 边界仍使用稳定的 C ABI `LanValue`，不会暴露 C++
对象布局。底层 retain/release 只集中在引用计数实现中。程序正常结束时会验证没有遗留堆对象。当前尚未启用“引用计数为 1 时原地
更新”的优化，仍始终采用安全的路径复制。

## Option 类型、缺失值与显式解包

普通值默认是 `T`，只有可能缺失的值才是 `Option<T>`。类型由编译器推断，不需要手写
类型注解或包装构造器。`T` 可以自动提升为 `Option<T>`，反向转换必须显式处理缺失。
`missing` 是可选类型的缺失状态，其底层值类型由上下文推断。

```lan
lookup(found) =
  if found
    "数据"
  else
    missing

lookup(false) ?? "没有找到"
```

`??` 只在左侧缺失时计算并使用右侧默认值。普通运算和要求 `T` 的函数拒绝未经处理的
`Option<T>`，即使某次运行碰巧能取得值，也必须先解包：

```lan
missing + 1       # 编译错误
length(missing)  # 编译错误

choose_missing() =
  if missing
    1
  else
    0

choose_missing()  # 编译错误：if 条件必须是 Boolean
```

后缀 `?` 明确允许当前表达式传播缺失。运算仍然从左到右求值；被 `?` 检查的值一旦
缺失，当前表达式直接得到 `missing`，后续部分不再求值：

```lan
missing? + expensive_calculation()  # missing，不调用 expensive_calculation
```

`?` 的传播边界是表达式，不是函数提前返回。每个绑定的右侧都是独立边界，包括普通块和
IO 块；绑定得到 missing 后，后续绑定继续执行。外层 `??` 也可以在同一个函数中接住传播
出来的缺失：

```lan
(lookup(false)? + "!") ?? "没有找到"  # "没有找到"
```

这与 Rust 式 `?` 不同：上例会继续计算 `??` 的右侧，而不是提前退出 `lookup` 的
调用者。`?` 的优先级高于算术、比较和 `??`。

函数可以接收、保存或返回 Option，只要函数体不隐式消费它。参数是否允许缺失根据函数体
和调用处推断；命名参数、默认参数和跨函数调用使用相同检查规则：

```lan
normalize(value) = value ?? 0

normalize(missing)  # 0
```

四种解包方式：

```lan
double(value) = value * 2

demo(text) = {
  value = parse_number(text);             # Option<Number>
  defaulted = value ?? 0;                 # Number
  propagated = double(value?);            # Option<Number>
  refined = if is_missing(value) { 0 } else { value + 1 }; # Number
  asserted = require(value);              # Number，缺失时运行时拒绝
  {defaulted: defaulted, propagated: propagated, refined: refined, asserted: asserted}
}

demo("10")
```

`?` 让当前计算继续使用 `T`，但整个表达式仍可能产生 `Option<U>`。
`if is_missing(value)` 的 else 分支及 `if not is_missing(value)` 的 then 分支会把该
局部绑定细化为 `T`。细化不泄漏到分支外，同名遮蔽后也不沿用旧绑定的证明。

标量 `and`、`or`、`not` 只接受 Boolean，不隐式消费 `Option<Boolean>`。
`false and missing` 会编译报错；`false and require(missing)` 合法且短路为 false。
`missing? and false` 则立即短路为 missing，不再检查右侧。集合逐元素逻辑以及
`any`、`all` 保留明确的三值逻辑规则。

容器与元素的可选性分别推断：

```text
List<Number>          # 列表与元素均存在
List<Option<Number>>  # 列表存在，元素可能缺失
Option<List<Number>>  # 整个列表可能缺失
IO<Option<Number>>    # action 存在，执行结果可能缺失
Option<IO<Number>>    # action 本身可能缺失
```

`List<T>` 的下标读取、`first` 和 `last` 仍返回 Option，因为可能越界或列表为空。
`map` 的元素上下文保留元素的可选性：`[1, 2] |> map(. + 1)` 合法，
`[1, missing] |> map(. + 1)` 编译报错，后者可写成 `map(.? + 1)`。
`join` 要求 `List<Text>`，`flatten` 要求每个子 List 存在；可以先用 `fill_missing`
或 `map(require(.))` 处理缺失元素。

左侧是 `Option<List<T>>` 时，`??` 为整个列表选择同类型默认值，例如 `maybe_list ?? []`；
左侧列表确定存在时，`list ?? value` 仍是广播式的逐元素填补。
同一函数内的同一个 `??` 若在不同调用中产生这两种不同解释，编译器会拒绝；要明确逐元素
填补，请使用 `fill_missing`，不要让操作含义依赖调用处。

### 模式匹配

`match` 是表达式，可以直接把 `Option<T>` 分成 missing 和存在两种情况：

```lan
normalize(input) =
  match input
    missing => 0
    value(number) => number + 1
```

`value(name)` 只匹配存在值，并把解包后的 `T` 绑定给 `name`；`value(_)` 忽略该值。
`missing` 只匹配缺失。普通名称和 `_` 是剩余情况模式：名称会绑定剩余值，`_` 不绑定。
如果前面的模式已经覆盖 missing，剩余名称会被细化为 `T`：

```lan
describe(input) = match input {
  missing => "没有数据";
  number => to_text(number)
}
```

模式还可以使用 Number、Text、Boolean 和 Unit 字面量：

```lan
category(number) = match number {
  -1 => "negative one";
  0 => "zero";
  _ => "other"
}

enabled(flag) = match flag {
  true => "yes";
  false => "no";
  missing => "unknown"
}
```

分支按源码顺序测试，输入只求值一次，只有命中的分支求值。重复模式、被前面模式完全
覆盖的分支和未穷尽的 match 都会编译报错。对于既可能存在又可能缺失的 `Option<T>`，
两种情况都必须覆盖；对于确定存在的 `T`，不要求 missing 分支；对于确定缺失的输入，
只覆盖 missing 即可。Boolean 的 `true`/`false` 可以覆盖
全部存在值，Unit 的 `()` 也能覆盖全部存在值；Number 和 Text 字面量仍需 `_`、普通
名称或 `value(name)` 覆盖其他值。

分支绑定只在自己的分支内有效，并允许遮蔽外层同名绑定。匹配原局部名称时，编译器也
会在相应分支细化原名称。分支返回值必须具有同一种底层类型，`T` 与 `Option<T>` 可以按
通常规则合并。

`?` 仍使用表达式边界：写在 match 输入上时，missing 会在进入任何分支前传播；写在
分支中时，会让整个 match 表达式得到 missing。List 和 Record 暂不支持结构解构；可以
先匹配 `value(list)` / `value(record)`，再使用现有下标和字段读取。模式匹配不是运行时
类型分派，字面量模式必须与输入具有相同静态类型。

## Result 与可恢复失败

`Result<T, E>` 表示成功并携带 `T`，或失败并携带 `E`。类型仍由编译器推断，不需要
手写类型注解。`ok`、`err` 是内置构造器而非语法关键字，各接受一个位置参数，不能被
用户函数重定义。普通值不隐式包装为 Result，Result 也不隐式解包：

```lan
divide(a, b) =
  if b == 0 { err("除数不能为零") } else { ok(a / b) }

message = match divide(10, 0) {
  ok(number) => to_text(number);
  err(error) => "计算失败：" + error
}

message
```

函数返回 `Result<Number, Text>`。`ok(())` 表示成功但没有额外数据；错误载荷也可以是
Record、List 或其他数据值。同一 Result 的成功载荷统一为 T，失败载荷统一为 E，二者
不要求相同。构造器尚未确定的另一侧类型由使用上下文约束；不自动将其定义为 Unit。

`ok(name)`、`err(name)` 模式分别绑定解包后的载荷，支持 `_` 忽略载荷；没有兜底分支时
必须覆盖两个状态，即使输入写作 `ok(1)`。对于 `Option<Result<T, E>>`，还必须处理
外层 missing。普通名称模式绑定的是完整的剩余 Result，不会自动取出载荷；
`value(name)` 也只处理外层 Option，不解包 Result。模式可用缩进书写，但第一版构造器
内只支持名称或 `_`；嵌套载荷使用另一个 match，不支持 `ok(err(e))` 这样的嵌套模式。

```lan
ok(missing)      # 成功，载荷是缺失值；不等于 missing 或 err(...)
ok(err("bad"))  # 成功，载荷本身是另一个失败的 Result
```

`require(result)` 是显式断言：成功返回载荷，失败输出错误载荷与源码诊断并终止程序。
它只解开一层 Result，不同时去掉成功载荷的 Option：

```lan
require(ok(42))              # 42
require(ok(missing)) ?? 0     # 0，成功载荷仍可能缺失
require(err("bad"))          # 运行时拒绝
```

对于外层可选的 Result，先用 match、`?` 或 `??` 处理外层缺失，再使用 require。
`?`、`??`、`is_missing` 仍只观察 Option，不传播、填补或检测 err；例如 `err("bad")?`
仍是同一个 Result，而不是函数提前返回或 IO 块短路。已有除零等运行时拒绝不会被自动
捕获为 err，现有 `parse_number` 等 Option 接口也不改变。

Result 可以作为函数参数、默认值、返回值、List 元素和 Record 字段，也可以作为 IO
的结果并被 action 捕获。载荷不能包含 IO action，应使用 `IO<Result<T, E>>`：

```lan
read_config() = io { err("尚未提供配置") }

io {
  result <- read_config();
  match result {
    ok(text) => write_stdout(text);
    err(error) => write_stdout(error)
  }
}
```

这里处于 `io` 块，因此 `<-` 不解包 Result，err 不自动停止后续步骤。执行 IO 的错误恢复 API 尚未接入标准库；
这里的 `read_config` 只演示返回普通 Result 数据。

整体比较使用 `same`，同时比较状态和载荷，不支持 Result 的 `==`、算术或排序。
`to_text` 和结果显示保留 `ok(...)` / `err(...)` 包装；含 Regex 的值仍不能转换为 Text。
第一版 Result 是持有一个带标签载荷的 ARC 堆对象，构造时保留载荷引用，解构时借用，
离开作用域按现有 ARC 规则释放；外层 missing 使用空指针，不会复制内部 List/Record。

## do 短路块

`do` 用一个块简化连续的 Option 或 Result 解包。`name <- expression` 成功时绑定内部值，
失败时立即结束当前 do；`name = expression` 仍是普通绑定，不传播任何状态：

```lan
total = do {
  price <- parse_number(price_text);
  quantity <- parse_number(quantity_text);
  subtotal = price * quantity;
  subtotal
}
```

这里 `<-` 输入是 Option：遇到第一个 missing 时，后续步骤不再求值，整个 do 得到 missing；
全部存在时，最后的普通值成为结果。存在性分析仍能证明某次调用必定成功并生成裸 SSA 值。
确定存在的 T 可以按已有提升规则作为 Option 输入，因此 `do {x <- 1; x}` 合法但没有实际作用。

Result 使用完全相同的表面语法：

```lan
calculate(a, b) = do
  quotient <- checked_divide(a, b)
  adjusted <- validate(quotient)
  quotient + adjusted
```

首个 `<-` 的静态底层类型决定块的载体。Result do 遇到 err 时直接返回原错误，全部成功时
将最后的普通值包装成 ok。所有 Result 输入必须具有同一种错误类型；一个 do 不能混用
Option 与 Result，也不能执行 IO。`IO<Result<T, E>>` 应先在 io 块执行，再在 do 中解包：

```lan
io {
  loaded <- load_config();       # 只执行 IO，loaded 是 Result
  checked = do {
    config <- loaded;            # 解包 Result
    validated <- validate(config);
    validated
  };
  checked
}
```

do 至少需要一个 `<-`；只有普通绑定时直接使用普通块。末尾值总是作为成功载荷，不自动
压平嵌套：`do {x <- ok(1); ok(x)}` 得到 `ok(ok(1))`。`_ <- expression` 可以显式忽略
成功载荷，但仍传播 missing/err。每个 `<-` 右侧、普通 `=` 右侧和末尾表达式都是原有
`?` 的独立表达式边界；`?`、`??` 仍只处理 Option，不获得 Result 语义。

do 是表达式，支持缩进和花括号/分号两种形式。`do` 是控制关键字，但 Record 字段仍可
命名为 do，例如 `{do: 1}.do`。

## 运算错误与运行时表示

普通除法遇到 `+0.0` 或 `-0.0` 会运行时拒绝，不产生 Infinity、NaN 或 missing。
List 除法只检查实际参与运算且两侧都存在的元素；被较短长度截去的位置以及结果已经
由 missing 决定的位置不会执行除法。

`%` 与乘除法具有相同优先级，接受任意 Number，并采用 Python 风格的 floor modulo：

```lan
5 % 2       # 1
5.5 % 2     # 1.5
-5 % 2      # 1
5 % -2      # -1
```

它同样拒绝 `+0.0` 和 `-0.0` 除数，并支持普通 List 广播。missing、截短位置和错误
顺序与 `/` 完全相同；`if` 仍要求显式 Boolean 条件，例如
`numbers |> map(if . % 2 != 0 {100} else {200})`。

运行时拒绝会指出源码行列和函数调用链；错误发生在集合计算中时还会报告 List 元素
索引或 Table 行号，并尽可能给出字段路径。例如：

```text
Lan 运行错误：除数不能为零
位置：report.lan:12:31
Table 行号：1
字段路径：.divisor
调用链：程序 -> report -> ratio
```

这些信息来自函数入口和当前集合循环的轻量诊断上下文，不会长期附着在每个值上。

还可以显式检查或要求值存在：

```lan
is_missing(value)  # 总是返回 Boolean
require(value)     # 对 Option：missing 时运行时拒绝，否则返回原值；Result 规则见上节
```

简单的 `if not is_missing(value)` 会把 then 分支中的 `value` 细化为确定存在。
纯程序最终结果如果可能缺失，编译期要求使用 `?` 明确允许显示 `missing`；IO 的最终
数据结果允许缺失。

Option 类型检查与底层 presence 优化分开：类型检查规定如何消费值，presence 分析仍
推断 `Present`、`Missing` 和 `MaybeMissing`，为不同类型选择紧凑表示，不强制装箱：

| 情况                          | LLVM 表示                      |
| ----------------------------- | ------------------------------ |
| 确定存在的数字                | 裸 `double` SSA 值             |
| 可能缺失的数字                | `{ i1 present, double value }` |
| 确定存在的真假值              | `i1`                           |
| 可能缺失的真假值              | `i2`：0 缺失、1 false、2 true  |
| Unit                         | `i1`：0 缺失、1 ()         |
| IO action                    | `ptr`：空指针表示 action 缺失；捕获环境使用 ARC |
| 可能缺失的文字、列表、Record 或 Regex | `ptr`：空指针表示缺失          |
| 已知类型的数字列表元素        | `double[]` 加逐元素存在标记    |
| 已知类型的真假值列表元素      | 三态字节：missing/false/true   |
| 文字、列表、Record 或 Regex 列表元素 | `ptr[]`：空指针表示缺失        |
| Unit 列表元素                | `i8[]`：0 缺失、1 ()       |
| 无法静态确定的动态字段/列表   | 带专用 `MISSING` 标签的回退值  |

生成 IR 前会运行 LLVM `-O2` 流水线，因此常量存在标志、无效分支和临时 Option
包装可以继续被消除。

编译期已知元素类型的 List 运算也保持这种表示：Number/Boolean/Text 的标量广播、
一元运算和嵌套广播不会重新退化为逐元素动态标签。嵌套 List 的外层是指针数组，叶层
仍使用对应的紧凑数组。一维 Number 列之间的算术和比较使用专用循环，LLVM 可以在
不改变 missing 语义时将其向量化。Boolean 三态列的列间和标量逻辑、相等比较及
`??` 也直接在紧凑字节编码上执行专用循环；动态类型和需要分配 Text 的运算保持
语义等价的标量路径。

Table 元素表达式把整行传给普通具名函数时，编译器会在调用点分析并内联该函数体。
函数实际读取的固定字段直接按列和行号取得，不会为了函数参数预先构造完整行 Record；
默认参数和命名参数仍遵循普通函数调用规则。

由于函数调用图保证无环，存在性分析不再反复扫描到固定点。编译器沿调用 DAG 深度
优先遍历，在返回调用者前得到被调用函数的结果，并缓存每组参数的存在状态；从顶层
不可达的函数则作为独立的 DAG 根分析一次。

## 不允许递归

函数调用关系中不允许形成环。直接递归和通过其他函数返回自身的间接递归都会在
编译时被拒绝：

```lan
repeat(value) = repeat(value)  # 编译错误
```

这个限制也适用于默认参数中的函数调用。语言没有通用 `for` 或 `while`；集合管道使用
输入容器的有限长度执行 `map`、`filter`、`find`、`any` 和 `all`。

`if` 只计算被选择的一边，`and`、`or`、`??` 和显式 `?` 传播也采用各自的短路规则。

## 纯函数规则

- 相同的输入始终产生相同的结果。
- 没有可变赋值、全局可变状态或任意 I/O；字段更新语法创建新 Record 绑定。
- 不允许直接或间接递归。
- 函数和局部名称都不可变。
- 文件最后一个公式是整个程序的结果。
- 显示最终结果是运行器的宿主边界，不属于用户函数中的副作用。

目前函数采用单态类型推断：一个函数在同一程序中应以一致的输入种类使用。无法从
上下文确定的输入默认视为数字。编译器会用面向用户的文字解释类型冲突和拼写错误。

## 构建和运行

需要 CMake 3.22+、Flex、Bison、Clang/Clang++、llvm-ar 和 LLVM 开发文件。
构建时自动下载固定版本的 RE2 2025-11-05 和 Abseil 20250512.1；离线时可向 CMake
传入 `-DFETCHCONTENT_SOURCE_DIR_ABSEIL=/path/to/abseil` 和
`-DFETCHCONTENT_SOURCE_DIR_RE2=/path/to/re2`，指向已解压的源码目录。

Unicode Text 操作使用随源码内置的 utf8proc 2.11.3；构建时不需要另行安装，许可证位于
`third_party/utf8proc/LICENSE.md`。

```sh
cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build
ctest --test-dir build --output-on-failure
```

直接计算并显示结果：

```sh
./build/lanc --run examples/types.lan
./build/lanc --run examples/missing.lan
./build/lanc --run examples/record.lan
```

`--build` 在当前目录创建 `.lan/<程序名>/`，保留 LLVM IR、目标文件和本地可执行文件：

```sh
./build/lanc --build examples/types.lan

# 产生：
# .lan/types/types.ll
# .lan/types/types.o
# .lan/types/types
```

`--run` 进行完全相同的持久化构建，然后自动执行 `.lan/<程序名>/<程序名>`。
它不再创建临时 IR 或通过 `lli` 解释执行。`.lan/` 已加入 `.gitignore`。

使用 `--` 将后续参数原样传给程序，供 `arguments()` 读取：

```sh
./build/lanc --run program.lan -- "a b" "" --verbose
.lan/program/program "a b" "" --verbose
```

`-- 参数...` 只支持 `--run` 模式；程序直接继承标准输入、输出和错误流。
`--random-seed` 同样只支持 `--run`，接受 `0` 到 `9007199254740991` 的整数；它必须写在
程序参数分隔符 `--` 之前，分隔符之后的同名文本只是普通程序参数。
`--fixed-time` 也只支持 `--run`，接受 RFC 3339 时间戳，必须写在 `--` 之前。
固定时间在执行 `now()` 时验证；无效配置产生带源码位置的运行时错误。

Lan/Unicode bitcode 和 RE2 静态库已内嵌到 `lanc`，无需运行时读取源码构建目录。
使用正则的程序还会在自己的 `.lan/<程序名>/` 中生成 `lan_regex_runtime.a` 和
`REGEX_LICENSES.txt`，并静态链接 RE2/Abseil；分发此类程序时应附带许可证。
不使用正则的程序不提取或链接该静态库。
这并不意味着本机构建的编译器已是完全独立的发行包：它仍需 Clang 工具链，且所使用
的 LLVM 发行版可能带有 z3/zstd 等动态依赖。

生成 LLVM IR：

```sh
./build/lanc examples/types.lan -o types.ll
lli types.ll
```

`-o` 只用于默认的 IR 输出模式，不能与 `--build` 或 `--run` 一起使用。
含正则调用的 IR 带有外部 RE2 适配层符号，不能直接用上面的裸 `lli` 命令执行；
请使用 `--build`/`--run` 完成静态链接。

## 编译结构

```text
Lan 公式
  -> Flex：单词、数字、文字和缩进
  -> Bison：表达式语法树
  -> 约束统一：自动类型推断
  -> 持久化 HAMT 运行时：Record 查询、路径复制与引用计数
  -> LLVM：SSA 值、分支和 PHI 节点
  -> verifier：IR 校验
  -> Clang：生成 `.o` 和本地可执行文件
  -> 可执行文件：计算并显示文件末尾的结果
```

用户函数内部直接使用 LLVM SSA 值，不产生可变局部变量。文字连接由隐藏的运行时
边界完成，但从 Lan 程序的观察结果看仍保持引用透明。

后续可在唯一引用路径上启用 Record 原地更新，其他语法与标准库计划见下列文档。

语言核心的已完成事项和后续计划见
[`docs/SYNTAX_TODO.md`](docs/SYNTAX_TODO.md)，标准库 API 清单见
[`docs/STDLIB_TODO.md`](docs/STDLIB_TODO.md)。完整的专题设计文档也统一位于
[`docs/`](docs/) 目录；已实现的第一阶段正则表达式 API 及后续边界见
[`docs/REGEX_DESIGN.md`](docs/REGEX_DESIGN.md)。
