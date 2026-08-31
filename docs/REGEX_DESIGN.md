# Lan 正则表达式设计

本文件定义已实现的第一阶段正则表达式标准库及后续边界。正则用于结构化文本搜索，不替代普通的字面
子串操作；`text_contains(text, part)` 始终按原样查找 Text，不把 `part` 解释为模式。

## 定位与引擎

Lan 采用 RE2 语法和执行模型，不自行实现正则引擎。RE2 对输入长度提供渐近线性的
匹配时间保证，适合 Lan 通过限制表达能力换取有限、可预测执行的定位。相应地，不支持
反向引用、环视和其他需要任意回溯的 PCRE 扩展。

- RE2 项目与执行保证：<https://github.com/google/re2>
- RE2 正则语法：<https://github.com/google/re2/wiki/syntax>

使用 RE2 默认语法；大小写、多行和 dot-all 等选项使用 RE2 支持的模式内标记，不再
设计另一套 Lan flags Record。模式和输入均为 UTF-8 Text。

## Regex 类型

```text
regex(pattern) -> Regex
```

`Regex` 是不可变、不透明的堆值，保存原始模式和编译后的 RE2 对象。它可以像其他值
一样绑定、传参、返回和存入同类型 List 或 Record，但不公开字段或方法。所有 Regex 在
语言语义上仍是 `Option<Regex>`；可能缺失时使用空指针 niche。

`regex(pattern)` 是确定的纯函数。模式无效时属于程序输入错误，运行时拒绝并报告 RE2
错误，不返回 missing。RE2 的公共 API 提供错误信息和错误片段，不提供可靠的模式
字节偏移；不得通过查找同名片段伪造精确偏移。编译器遇到模式字面量时可以提前检查，但动态 Text
仍在运行时编译。

Regex wrapper 使用引用计数管理；复制绑定只 retain，最后一个引用消失时 release 编译
结果。语言的完整 ARC 已经实现，Regex 可以直接接入现有 owned、borrowed、transferred
边界，不引入每次匹配重新编译模式或不可观察的全局缓存。

## 第一阶段 API

```text
regex_contains(text, pattern) -> Boolean
regex_matches(text, pattern)  -> Boolean
regex_find(text, pattern)     -> Option<Text>
regex_find_all(text, pattern) -> List<Text>
regex_split(text, pattern)    -> List<Text>
```

其中 `pattern` 必须是 Regex：

```lan
digits = regex("[0-9]+")

regex_contains("item-123", digits)  # true
regex_matches("123", digits)        # true
regex_find("item-123", digits)      # "123"
regex_find("item", digits)          # missing
regex_find_all("a1 b22", digits)    # ["1", "22"]
regex_split("a, b; c", regex("[,;] *"))
```

- `regex_contains` 判断任意位置是否匹配。
- `regex_matches` 要求整个 Text 匹配，不等同于 `regex_contains`。
- `regex_find` 返回最左侧第一个匹配的完整片段；没有匹配时返回 missing。
- `regex_find_all` 按从左到右、互不重叠的顺序返回完整匹配片段。
- `regex_split` 按从左到右的匹配边界切分，并像现有 `split` 一样保留首尾空 Text。
- `regex_find_all` 和 `regex_split` 遇到零长度匹配时按一个 Unicode code point 前进，保证
  有限执行且不重复返回同一位置。
- 尾部位置的空匹配也保留；例如 `regex_find_all("a", regex("a*"))` 返回 `["a", ""]`，
  `regex_split("你好", regex(""))` 返回 `["", "你", "好", ""]`。
  提取/切分不能产生破损 UTF-8：匹配边界切断 code point 时运行时拒绝，即使 RE2 的
  `\\C` 字节匹配能识别该片段。Boolean 匹配不提取片段，不受此边界限制。

所有操作也支持普通管道传入第一个参数：

```lan
input |> regex_contains(digits)
input |> regex_find_all(digits)
```

普通参数遇到 missing 时遵循现有规则并运行时拒绝；只有“没有找到”是
`regex_find` 的正常 missing 结果。List 和 Table 不隐式广播，需要显式使用 `map` 或
Table 元素表达式。集合中从左到右定位第一个模式错误或运行时错误。

## 资源与 lowering

- 模式 Text 最长 65536 字节；每个 RE2 对象的 `max_mem` 设置为 8 MiB。
  这是 RE2 引擎的内存预算，不是整个包装对象或进程的严格内存上限。
- Text 本身继续遵守 Lan 的有限对象大小约束。
- 编译器通过一个小型 `extern "C"` C ABI shim 调用 RE2；RE2 保持独立的成熟 C++ 库。
- 循环不变量 Regex 在 `map` 和 Table 元素循环之外只编译一次并复用。
- Regex wrapper 纳入 owned、borrowed、transferred 所有权分析和完整 ARC。
- 错误诊断沿用源码位置、调用链、List 索引、Table 行号和字段路径。

线性时间保证针对固定编译模式的**单次匹配**。find-all/split 会重复搜索剩余输入，
不能笼统承诺整个枚举也线性：某些模式在每次返回短匹配前会扫描剩余后缀，整体可能
达到平方时间。结果收集与片段复制另计；这些操作仍有限，且不存在回溯爆炸。

Regex 通过 `same` 按原始模式 Text 比较，不比较对象地址，也不判断正则语言等价性；
不提供 `==`/`!=`。最终显示中的 Regex 使用固定占位符 `<Regex>`，不读取模式内容。

## 暂不提供

- 不增加 `/pattern/flags` 正则字面量语法；模式仍由 `regex(Text)` 显式构造。
- 不接受 Text 作为匹配函数的 pattern 参数，避免在循环中隐式重复编译。
- 不提供 PCRE 兼容模式、反向引用、环视、递归模式或运行时引擎选择。
- 第一阶段不提供捕获组 API。后续需要先定义未参与匹配的可选组、编号组和命名组的
  返回结构。
- 第一阶段不提供 `regex_replace`。后续需要先确定替换第一个/全部、捕获引用语法和
  replacement 中的转义规则。
- Regex 不参与 `to_text`、JSON、CSV 或 XLSX 序列化；如需调试显示，后续单独提供安全
  的模式来源读取能力。

## 实现顺序

1. 已完成 Text、List、Record 的完整 ARC，并为新堆类型保留统一接入点。
2. 引入 RE2，并实现带 retain/release 的 Regex runtime wrapper 与 C ABI shim。
3. 增加 Regex 静态类型、空指针 niche、`regex` 构造和错误诊断。
4. 实现 contains、完整匹配和单次查找。
5. 实现有限的 find-all 与 split，覆盖零长度匹配和 Unicode 前进规则。
6. 验证普通函数、管道、List `map`、Table 元素上下文和跨函数 Regex 复用。

## 实现进度与依赖构建

- [x] 固定 RE2 2025-11-05，以及该版本上游声明的 Abseil 20250512.1。
- [x] 编写独立的 C ABI 适配层 `runtime/regex/`：编译、retain/release、
  完整匹配及按字节范围查找；异常不跨 C ABI 边界，错误使用调用方缓冲区。
- [x] 编写 C 调用方测试，覆盖引用复用、空匹配、UTF-8、锚点、内嵌 NUL、
  不支持的模式、参数校验和模式长度上限；C 语法检查已通过。
- [x] 离线构建 RE2/Abseil 及适配层，并运行 C ABI 测试。
- [x] 将静态运行时集成到编译器产物链接及资源分发流程。
- [x] 接入 Regex 类型、Lan ARC 对象统计、构造与第一阶段语言 API。
- [x] 完成 find-all/split 的 Unicode 零长度前进规则及 Lan 端到端回归测试。

根项目构建已经集成适配层。只验证独立 C ABI 时也可以运行：

```sh
cmake -S runtime/regex -B build/regex -DCMAKE_BUILD_TYPE=Release
cmake --build build/regex -j
ctest --test-dir build/regex --output-on-failure
```

默认从官方仓库下载固定版本。离线构建可提供已解压的源码目录：
`-DFETCHCONTENT_SOURCE_DIR_ABSEIL=/path/to/abseil`
和 `-DFETCHCONTENT_SOURCE_DIR_RE2=/path/to/re2`。
适配层只返回借用输入中的字节范围，不自行创建 Lan Text/List；这些值的物化、
missing 和源码诊断由 Lan runtime/lowering 处理。

编译器内嵌 Lan/Unicode bitcode、RE2/Abseil 原生静态库和许可证。优化后仍有正则调用时，
`--build`/`--run` 才会将库和许可证写入程序的 `.lan` 目录并静态链接，普通程序不链接 RE2。
RE2 不作为执行代码链接进编译器本身。该打包不消除 LLVM 发行版自身的动态依赖，
也不内嵌 Clang 工具链；完全独立的编译器发行包仍是另一个构建目标。
