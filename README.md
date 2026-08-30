# Lan

Lan 是一门面向非程序员的纯函数式语言。写 Lan 程序很像写数学公式或电子表格公式：
给事物起名字、描述计算关系，然后在文件最后写出想看到的结果。

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
greeting(name, punctuation = "!", excited = false) = ...

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
- 文字：`"Hello"`、`"你好"`
- 单类型列表：`[1, 2, 3]`、`["红", "绿", "蓝"]`
- Record：`{name: "小明", age: 20}`
- 数字计算：`+`、`-`、`*`、`/`
- 文字连接：`"Hello, " + name`
- 比较：`==`、`!=`、`<`、`>`、`<=`、`>=`
- 逻辑：`and`、`or`、`not`，也兼容 `&&`、`||`、`!`
- 中文或英文名称

## 单类型列表

一个列表中的所有元素必须属于同一种类型。编译器自动推断元素类型，不需要用户写
`List<Number>` 这样的标注：

```lan
numbers() = [1, 2, 3]
names() = ["小明", "小兰"]
```

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
`count` 接受任意单类型 List。列表本身为 missing 时仍遵循普通参数规则并运行时拒绝。

`first(list)` 和 `last(list)` 分别读取第一个和最后一个元素，空列表返回 `missing`。

### 集合管道与元素表达式

所有管道阶段都必须使用 `|>`，并写出函数名和括号：

```lan
numbers |> sum()                    # sum(numbers)
people |> take(10)                  # take(people, 10)
people |> sort_by("score")          # sort_by(people, "score")
```

不支持 `numbers sum`、`people take 10` 等省略 `|>` 或括号的旧写法。

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

Table 没有独立的字面量语法，也不是新的运行时数据类型。使用内置的 `table(record)`
验证一个 Record：它的每个字段都必须是 List，并且所有 List 长度相同。

```lan
people = table({name: ["小明", "小兰", "小红"], age: [20, 17, missing], score: [80, 90, missing]})
```

验证成功后返回原来的 Record 指针，不复制 Record 或任何 List。编译器把这个值静态
细化为 Table；LLVM 和运行时仍使用 Record 指针。普通 Record 不能直接传给表格操作，
必须先经过 `table(...)`。

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
field = "age"
person = {name: "Alice", (field): 21}
```

`{field: 21}` 的字段名固定为 `"field"`，而 `{(field): 21}` 使用变量 `field` 的值。
动态键表达式必须产生 Text。

不存在的字段得到 `missing`。可以用 `??` 提供默认值；如果把它直接用于普通运算，
则按缺失值规则在运行时拒绝：

```lan
person.city ?? "未知"
person.height + 1       # height 不存在时运行错误
```

函数块中可以用符合初学者直觉的写法添加或替换字段：

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
updated = person with {
  age: 21,
  city: "Sydney"
}
```

更新块同样支持 `(field): value` 动态键。嵌套更新写成
`person.address.city = "Sydney"`，它会从内向外构造新的 Record 路径。更新目标必须以
局部绑定名称为根；`get_person().age = 21` 会在编译期被拒绝。缺失或不是 Record 的
中间字段会在运行时拒绝。直接的 `person.self = person` 也会被拒绝。

Record 在语义上无序。为了让结果可复现，显示 Record 时按字段名称的 UTF-8 字节顺序
排序；顺序不是插入顺序。

底层使用持久化 HAMT。更新只复制从根到目标字段的路径，其余分支由新旧 Record 共享，
HAMT 节点用引用计数管理。第一阶段尚未启用“引用计数为 1 时原地更新”的优化；当前
始终采用安全的路径复制，后续补齐编译器的所有权保留与释放后可在不改变语义的前提下
启用该优化。

## 缺失值、运行时检查与显式传播

任何类型的值都可以是 `missing`。语言类型系统统一按 `Option<T>` 理解值：它要么是
`Value(value)`，要么是 `Missing`，但日常代码不需要手工书写包装名称。

```lan
lookup(found) =
  if found
    "数据"
  else
    missing

lookup(false) ?? "没有找到"
```

`??` 只在左侧缺失时计算并使用右侧默认值。普通运算不再自动传播缺失；如果实际
使用的值是 `missing`，程序会在运行时拒绝这次运算：

```lan
missing + 1       # 运行错误
length(missing)  # 运行错误
if missing
       # 运行错误
  1
else  0
```

后缀 `?` 明确允许当前表达式传播缺失。运算仍然从左到右求值；被 `?` 检查的值一旦
缺失，当前表达式直接得到 `missing`，后续部分不再求值：

```lan
missing? + expensive_calculation()  # missing，不调用 expensive_calculation
```

`?` 的传播边界是表达式，不是函数提前返回。外层 `??` 可以在同一个函数中接住传播
出来的缺失：

```lan
(lookup(false)? + "!") ?? "没有找到"  # "没有找到"
```

这与 Rust 式 `?` 不同：上例会继续计算 `??` 的右侧，而不是提前退出 `lookup` 的
调用者。`?` 的优先级高于算术、比较和 `??`。

函数参数仍然可以接收 `missing`，让函数使用 `??` 主动处理；只有普通运算实际消费
未处理的缺失值时才报错：

```lan
normalize(value) = value ?? 0

normalize(missing)  # 0
```

`and`、`or` 和 `not` 使用三值逻辑：`false and missing` 是 `false`，
`true and missing` 是 `missing`，`true or missing` 是 `true`，
`false or missing` 是 `missing`，而 `not missing` 仍是 `missing`。求值顺序保持从左到右；
左侧已经决定结果时不会求值右侧，左侧为 missing 时则继续查看右侧是否能决定结果。

普通除法遇到 `+0.0` 或 `-0.0` 会运行时拒绝，不产生 Infinity、NaN 或 missing。
List 除法只检查实际参与运算且两侧都存在的元素；被较短长度截去的位置以及结果已经
由 missing 决定的位置不会执行除法。

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
require(value)     # missing 时运行时拒绝，否则返回原值
```

简单的 `if not is_missing(value)` 会把 then 分支中的 `value` 细化为确定存在。
程序最终结果如果可能缺失，也必须使用 `?` 明确允许显示 `missing`。

“所有值可缺失”是语言语义，不代表所有值都在运行时装箱。编译器同时推断值是否
确定存在，并为不同类型选择紧凑表示：

| 情况                          | LLVM 表示                      |
| ----------------------------- | ------------------------------ |
| 确定存在的数字                | 裸 `double` SSA 值             |
| 可能缺失的数字                | `{ i1 present, double value }` |
| 确定存在的真假值              | `i1`                           |
| 可能缺失的真假值              | `i2`：0 缺失、1 false、2 true  |
| 可能缺失的文字、列表或 Record | `ptr`：空指针表示缺失          |
| 已知类型的数字列表元素        | `double[]` 加逐元素存在标记    |
| 已知类型的真假值列表元素      | 三态字节：missing/false/true   |
| 文字、列表或 Record 列表元素  | `ptr[]`：空指针表示缺失        |
| 无法静态确定的动态字段/列表   | 带专用 `MISSING` 标签的回退值  |

生成 IR 前会运行 LLVM `-O2` 流水线，因此常量存在标志、无效分支和临时 Option
包装可以继续被消除。

编译期已知元素类型的 List 运算也保持这种表示：Number/Boolean/Text 的标量广播、
一元运算和嵌套广播不会重新退化为逐元素动态标签。嵌套 List 的外层是指针数组，叶层
仍使用对应的紧凑数组。一维 Number 列之间的算术和比较使用专用循环，LLVM 可以在
不改变 missing 语义时将其向量化。

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

需要 CMake、Flex、Bison、Clang 和 LLVM 开发文件：

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

生成 LLVM IR：

```sh
./build/lanc examples/types.lan -o types.ll
lli types.ll
```

`-o` 只用于默认的 IR 输出模式，不能与 `--build` 或 `--run` 一起使用。

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

下一阶段适合补齐容器的所有权分析和释放，在唯一引用路径上启用 Record 原地更新，
然后增加列表遍历和模式匹配。

完整的已完成事项和后续计划见 [`TODO.md`](TODO.md)。
