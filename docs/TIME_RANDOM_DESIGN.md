# Lan Time 与 Random 设计

本文件定义已经实现的第一阶段时间与随机能力。获取当前时间和随机样本会观察或改变宿主状态，
因此只能构造显式 `IO<T>` action；时间解析、格式化和算术仍是普通纯函数。

## Time 类型

增加编译器内建值类型 `Instant`，表示时间线上的一个 UTC 瞬间：

- 内部保存有符号秒数和纳秒部分，不使用 Number 承担完整时间精度。
- 可以保存在 List、Record 和 Table 中，并遵循普通 presence 规则。
- 支持 `==`、`!=`、`<`、`<=`、`>`、`>=` 和 `same()`。
- 不与 Number 隐式转换，也不直接参与 `+`、`-` 广播。
- generic JSON/CSV 输出不隐式猜测时间格式；调用者先显式使用 `format_time`。

第一阶段不增加 Date、TimeOfDay、时区化 DateTime 或 Duration 类型。日期、当地时间和日历
运算需要时区数据库、夏令时和无效当地时间规则，应当在实际用例出现后单独设计。

## Time API

```text
now()                              -> IO<Instant>
parse_time(text)                   -> Option<Instant>
format_time(instant)               -> Text
time_from_unix(seconds)            -> Instant
unix_seconds(instant)              -> Number
add_seconds(instant, seconds)      -> Instant
seconds_between(start, end)        -> Number
```

- `now()` 每次执行 action 时重新读取系统实时时钟；仅仅构造或保存 action 不读取时间。
- `parse_time` 只接受严格 RFC 3339 时间戳，必须包含 `Z` 或明确的数字 UTC offset，并统一
  归一化为 UTC Instant；解析失败返回 missing。
- `format_time` 始终输出带 `Z` 的规范 UTC RFC 3339 文本，按需要保留最多九位小数秒并
  移除多余尾零。
- Unix 时间转换以 `1970-01-01T00:00:00Z` 为原点；Number 可以带小数秒。
- 时间算术拒绝 missing、NaN、无穷大以及内部秒数范围溢出。
- `seconds_between(start, end)` 返回 `end - start`，因此结果可以为负数。

### 第一阶段的精度与格式边界

- 使用 Unix/POSIX 时间模型，不保存闰秒，`:60` 解析为 missing，不静默归一化。
- 为保证每一个 Instant 都可输出四位年份的 RFC 3339，UTC 范围限定为
  `0000-01-01T00:00:00Z` 至 `9999-12-31T23:59:59.999999999Z`。
  offset 归一化后也必须在范围内；不合法解析返回 missing，算术越界运行时拒绝。
- 必须完整匹配输入，无前后空白、无省略字段；接受 `T`/`t`、`Z`/`z`，输出统一大写。
  小数秒为 1–9 位；更多位数拒绝，不静默截断。没有小数点也有效。
- `-00:00` 表示当地 offset 未知但 UTC 时间已知；与 `+00:00` 对应同一瞬间。
  Instant 不保存 offset 元数据。
- Number 秒数先单独舍入到最近纳秒，中点远离零，再进行整数秒/纳秒进位和借位。
  `add_seconds(t, 0)` 完整保留 t 的纳秒。转回 Number 时受 binary64 精度限制，不能
  保证与原始 Instant 无损往返。
- `==` 等比较沿用 List 的逐元素/尾维广播规则；`same` 比较实际秒数与纳秒，不比较
  地址。`sort`/`sort_by` 按瞬间稳定排序，missing 放在最后；`distinct` 复用同一语义。
- 纯时间函数不隐式广播；批量调用使用 `map`。普通 missing 输入拒绝；显式 `?`
  和 `??` 与其他库函数一致。
- `parse_time` 的时间开销为输入长度 O(n)、额外空间 O(1)；其余标量时间函数为 O(1)。
  时间函数先从左到右求值并检查参数，不捕获普通运行时错误。

格式规则参考 [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339)，Lan 对闰秒和超过纳秒
精度的输入使用上述明确限制。现有 Abseil 的 `ParseTime` 会归一化 `:60`，因此没有直接
将其宽松解析入口作为语言规则。有限 UTC 格式处理留在 LLVM bitcode 运行时内，避免时间
程序为了日历转换而提取整个原生依赖包；日期换算测试与项目固定版本的 Abseil 对照，
覆盖 0000–9999 每年每月共 120000 个日期。

时区名称、当地日期格式、locale 和模糊自然语言日期不属于第一阶段。以后若加入 IANA
时区，必须显式传入时区名称并固定所使用的时区数据库版本。

## Time 的执行与测试

```lan
program() = io
  started <- now()
  result = build_report()
  finished <- now()
  write_stdout(to_text(seconds_between(started, finished)))
  result

program()
```

两个 `now()` action 严格按源码顺序执行。运行器提供测试选项
`--fixed-time <RFC3339>`；启用后，本次运行内所有 `now()` 都返回同一个 Instant。这样测试
不需要模拟或替换用户函数。

`--fixed-time` 仅用于 `--run`，必须放在 `--` 程序参数分隔符之前，不进入 `arguments()`。
固定时间配置在执行 `now()` 时验证；无效配置报带源码位置和调用链的运行时错误。
仅构造未执行的 action 不读取系统时钟，也不会因无效的测试时钟配置失败。

`now()` 使用实时时钟，不用于精确性能测量。若将来需要计时器，应增加不能与 Instant
混用的 MonotonicInstant，而不是改变 `now()` 语义。

## Random API

```text
random()                         -> IO<Number>
random_between(minimum, maximum) -> IO<Number>
random_integer(minimum, maximum) -> IO<Number>
choose(list)                     -> IO<Option<T>>
shuffle(list)                    -> IO<List<T>>
```

以上五个 API 均已实现。

- `random()` 均匀产生 `[0, 1)` 内的 Number。
- `random_between` 均匀产生 `[minimum, maximum)` 内的 Number，并要求两个边界有限且
  `minimum < maximum`。
- `random_integer` 均匀产生包含两端点的整数值 Number；边界必须是
  `[-(2^53 - 1), 2^53 - 1]` 内的整数值，并使用拒绝采样消除取模偏差。
- `choose` 从非 missing 元素中等概率选择一个；空 List 或全 missing List 返回 missing。
- `shuffle` 使用 Fisher–Yates，保留所有元素及其 missing 状态，只改变位置。
- action 按 `io` 块从上到下消耗同一个运行器随机序列；构造 action 不消耗随机数。

这些函数用于模拟、抽样和普通应用逻辑，不承诺密码学安全。安全令牌和密钥生成属于单独
的安全 API，不得复用本随机库。

## 种子与可复现性

运行器在每次启动时创建一个随机状态：

- 默认从操作系统安全熵源取得种子。
- `--random-seed <integer>` 使用显式种子；范围为 `[0, 2^53 - 1]`。
- 给定相同 Lan 版本、种子、输入和 action 顺序，必须产生完全相同的随机结果。
- 算法固定为带版本标识的 PCG64；初始版本标识为 `lan-pcg64-v1`。
- 改变算法或种子展开方式属于可观察的语言版本变更，不能静默替换。

`lan-pcg64-v1` 具体使用 PCG XSL RR 128/64、固定奇数 stream increment，并以
`state = 0; step; state += seed; step` 展开 53 位安全整数种子。连续 Number 使用输出的
高 53 位；整数范围使用拒绝采样，因此固定种子的序列不带取模偏差。

第一阶段不提供运行中重新设种子、全局纯 `random()`、隐式线程局部状态或把随机状态作为
可变用户对象。需要多条独立随机序列时，再设计显式不可变 RandomState 及状态线程 API。

例如：

```lan
program = io
  first <- random_integer(1, 6)
  second <- random_integer(1, 6)
  write_stdout(to_text(first + second))
```

同一个 action 被执行两次会抽取两个连续样本：

```lan
die = random_integer(1, 6)

program = io
  first <- die
  second <- die
  [first, second]
```

## 错误与 missing

- 系统时钟或操作系统熵源失败属于 IO 错误，停止后续 action。
- 无效区间、非整数的整数边界、非有限输入和时间溢出属于带源码位置的运行时拒绝。
- `choose` 的无可选元素是正常数据结果，因此返回 missing，不是 IO 错误。
- `?` 不捕获时钟、熵源或随机参数错误；其规则保持与 `IO_DESIGN.md` 一致。
- List 操作按现有顺序定位输入错误；随机状态只在 action 参数验证全部成功后推进。

最后一条保证无效调用不会悄悄消耗一个随机样本，便于错误修复后复现程序。

## 暂不提供

- `sleep`、定时器、闹钟和后台调度。
- 时区数据库、locale、日历月份加减和自然语言日期。
- 密码学随机字节、UUID 和安全令牌。
- 正态分布等统计分布；应在明确统计库需求后设计。
- 隐式随机 List 广播；批量抽样必须使用明确的有限 API。

## 实现顺序

1. ~~增加 Instant 类型、比较、presence 表示和确定格式化。~~ **已完成**。
2. ~~实现纯 `parse_time`、`format_time` 及秒数转换和算术。~~ **已完成**。
3. ~~在 IO runtime 中实现 `now()` 与 `--fixed-time` 测试时钟。~~ **已完成**。
4. ~~实现版本化 PCG64 状态、宿主熵初始化和 `--random-seed`。~~ **已完成**。
5. ~~实现标量随机 action，再实现泛型 `choose` 与 `shuffle`。~~ **已完成**。
6. ~~覆盖重复 action、错误不消耗样本、调用链和集合诊断测试。~~ **已完成**。
