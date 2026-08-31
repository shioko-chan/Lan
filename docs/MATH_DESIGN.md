# Lan 数学库设计

本文件定义已经实现的纯数学标准库。Lan 只有一种数值类型 Number，运行时使用
IEEE 754 `double`；数学函数不引入 Integer、Decimal 或复数类型。

## 共同规则

- 所有数学函数都是确定的纯函数，不产生 IO 或隐藏状态。
- 参数必须是 Number；Option<Number> 须先用 `?`、`??`、`require` 或分支细化解包，否则编译拒绝。
- 数学函数只处理标量，不隐式广播 List。集合计算显式使用 `map`。
- 角度统一使用弧度；只在输入输出边界使用 `radians` 和 `degrees`。
- 定义域错误、无效参数以及产生 NaN 或无穷大的结果运行时拒绝。
- 下溢到有限的零或次正规数不是错误。
- 错误诊断沿用源码位置、调用链、List 索引、Table 行号和字段路径。
- 函数名是普通全局内置名称，不是关键字；局部绑定仍可按现有作用域规则遮蔽它们。

数学函数的跨平台结果遵循 IEEE 754 和宿主数学库的通常精度，不承诺不同平台间最后一位
比特完全相同。结构化测试应使用误差范围；离散结果和定义域检查必须精确一致。

## 常量

```text
pi   # 圆周率
e    # 自然常数
tau  # 2 * pi
```

这些常量是编译器提供的确定存在 Number，使用正确舍入到 `double` 的值。它们不是零参数
函数，也不成为保留关键字。

## 基础数值函数

```text
abs(number)                         -> Number
sign(number)                        -> Number
floor(number)                       -> Number
ceil(number)                        -> Number
round(number, digits = 0)           -> Number
clamp(number, minimum, maximum)     -> Number
```

- `sign` 返回 `-1`、`0` 或 `1`，`+0.0` 和 `-0.0` 都返回 `0`。
- `floor`、`ceil` 的结果类型仍是 Number。
- `round` 采用 ties-to-even；`digits` 必须是 `[-308, 308]` 内的整数值 Number。
- `clamp` 使用闭区间，并要求 `minimum <= maximum`。

现有 `%` 继续作为接受任意 Number 的 floor modulo 运算符，不增加 `mod` 同义函数。

## 幂、根与对数

```text
sqrt(number)                 -> Number
pow(base, exponent)          -> Number
exp(number)                  -> Number
log(number, base = e)        -> Number
hypot(x, y)                  -> Number
```

- `sqrt` 要求输入不小于零。
- `pow` 的负底数要求指数是整数值；零的负指数被拒绝；`pow(0, 0)` 定义为 `1`。
- `log` 要求 number 大于零，并要求 base 大于零且不等于 `1`。
- 不增加 `ln`、`log2`、`log10` 等同义入口；使用 `log(value)` 或
  `log(value, 10)`。
- `hypot` 应使用避免不必要中间溢出的稳定实现，而不是简单展开为
  `sqrt(x * x + y * y)`。

## 三角函数

```text
sin(angle)       -> Number
cos(angle)       -> Number
tan(angle)       -> Number
asin(number)     -> Number
acos(number)     -> Number
atan(number)     -> Number
atan2(y, x)      -> Number
radians(degrees) -> Number
degrees(radians) -> Number
```

- `sin`、`cos`、`tan` 的输入以及反函数结果均使用弧度。
- `asin` 和 `acos` 要求输入位于闭区间 `[-1, 1]`。
- `atan2(0, 0)` 按宿主 IEEE 数学语义返回有限角度，不额外拒绝。
- 浮点数无法精确表示 `pi / 2`，因此 `tan` 不尝试识别数学上的奇点；只在结果实际为
  NaN 或无穷大时拒绝。

## List 与 Table 使用

普通用户函数和数学内置函数都不因收到 List 而自动广播：

```lan
sqrt(numbers)                 # 编译错误：sqrt 需要 Number
numbers |> map(sqrt(.))       # List<Number>
angles |> map(sin(.))         # List<Number>
table |> add({root: sqrt(.value)})
```

missing 元素不会被数学函数自动忽略。调用者必须显式传播、填充或筛选 missing，例如：

```lan
numbers |> map(sqrt(.?))
numbers |> fill_missing(0) |> map(sqrt(.))
```

集合元素按现有从左到右顺序求值，因此定义域错误报告第一个实际参与计算的元素。

## 暂不提供

- 随机数和随机分布：属于 [`TIME_RANDOM_DESIGN.md`](TIME_RANDOM_DESIGN.md) 中的显式
  IO/随机状态能力，不属于纯数学库。
- 复数、任意精度整数、Decimal 和单位量。
- `gcd`、`lcm`、阶乘及组合数学：需要先决定是否正式表达整数值约束。
- 双曲函数、Gamma 和误差函数：有明确使用需求后再扩展。
- 统计函数：应当作为独立的 List/Table 统计 API 设计。
- 数值积分、微分方程和矩阵代数：不属于第一版基础数学库的范围。

## Lowering

数学常量直接 lower 为 LLVM `double` 常量。数学函数调用随 C 运行时一起编译成 LLVM
bitcode，在链接进 Lan Module 后参与完整的 LLVM `O2` 优化；宿主数学函数通过 `libm`
链接。运行时入口先执行定义域和有限性检查，再返回裸 Number。

数学函数没有独立的集合 ABI。List `map` 和 Table 元素上下文复用普通标量调用，保持
从左到右的求值及首错定位；后续只有在不改变这些语义时才考虑专用 SIMD lowering。
