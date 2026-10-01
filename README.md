# Lan

[English](README.md) | [简体中文](README.zh-CN.md)

Lan is a simple, concise, elegant general-purpose scripting language with purely functional computation and explicit IO. It uses LLVM to compile to native executables.

Lan is intended for everyday scripting, automation, and data processing. It is not limited to a particular business domain, nor is it intended to be a natural-language or no-code tool.
The language centers on immutable data, expressions, and collection pipelines, with its range of applications expanding through the standard library.

General-purpose scripting describes its intended use, not a claim that every capability is implemented: recursion and general `for` and `while` loops are still prohibited,
and collection computations use finite pipelines. Standard input and output, command-line arguments, time, and random numbers are supported; capabilities such as file reading and writing are still planned.

Writing a Lan program is much like arranging mathematical formulas or data-processing pipelines: name things, describe their computational relationships, then write the desired result at the end of the file.

To focus on individual rules, some code blocks show only fragments from inside a function or block. Any relevant names in those fragments are assumed to have been defined in the surrounding scope.

## Five-Minute Introduction

The simplest program is just a value:

```lan
1 + 2 * 3
```

Running it displays:

```text
7
```

A function is a formula with inputs:

```lan
square(x) = x * x

square(5)
```

Conditionals use indentation to show nesting:

```lan
price(original, member) =
  if member
    original * 0.8
  else
    original

price(100, true)
```

Inside a function, you can name intermediate results:

```lan
circle_area(radius) =
  pi = 3.1415926
  area = pi * radius * radius
  area

circle_area(10)
```

These names are immutable formulas, not storage cells that can be modified repeatedly.

### Indentation and Braces

Multiline blocks can use indentation or explicit braces and semicolons. The following two programs produce exactly the same
syntax tree and LLVM IR:

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

Inside a braced block, every intermediate binding must end with `;`; the semicolon after the final result expression is optional.
The indented form uses a layout rule: virtual semicolons are inserted between lines at the same level, and a virtual closing brace is inserted when leaving an indentation level.
A more deeply indented line that does not open a new block continues the preceding expression. Virtual tokens and explicit `{`, `;`, and `}`
share the same block grammar. Within explicit braces, newlines are only for formatting and do not separate expressions.
Neither `if` nor `else` is followed by a colon.
Conditionals also have two equivalent forms:

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

`{name: value}` is still a Record: colons distinguish Record fields, while equals signs and semicolons distinguish bindings in a code block:

```lan
make_person() = {name: "小明", age: 20}
calculation() = {age = 20; age + 1}
```

### Default Parameters

Default parameters must follow all required parameters. Trailing default parameters can be omitted when calling a function:

```lan
greeting(name, punctuation = "!") = "你好，" + name + punctuation

greeting("小明")
greeting("小红", "？")
```

A default expression can reference parameters that precede it:

```lan
end_from(start, end = start + 10) = end

end_from(5)  # 15
```

The following definition is rejected because a required parameter follows a default parameter:

```lan
invalid(first = 1, second) = first + second
```

Omitting an argument and passing `missing` are different. A default value is evaluated only when the argument is omitted:

```lan
keep(value = 10) = value

keep()        # 10
keep(missing) # missing
```

Calls can also use named arguments after positional arguments to skip earlier parameters that have defaults:

```lan
greeting(name, punctuation = "!", excited = false) =
  ending = if excited
    punctuation + punctuation
  else
    punctuation
  "你好，" + name + ending

greeting("小明", excited = true)
```

Positional arguments must precede named arguments. The same parameter cannot be supplied twice; unknown parameter names and missing required arguments
are compile-time errors.

### Reusing Names and Shadowing

An existing name can be reused at the same indentation level. The formula on the right reads the old value first, and the new immutable binding
then shadows the old one:

```lan
next() =
  value = 1
  value = value + 1
  value

next()
```

The result is `2`. This creates a new `value` rather than changing the original `value` to `2`.

An inner scope can also shadow an outer name; the outer binding remains after leaving the inner scope:

```lan
example(flag) =
  value = 10
  if flag
    value = 20
    value
  else
    value
```

Function names must still be unique, as must parameter names within a single function. Name lookup always selects the nearest binding
found when searching outward from the current position.

## Numbers, Booleans, and Text

Lan infers the kinds of values automatically. Users do not need to write `i64`, `f64`, or return types.

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

Currently supported:

- Numbers: `10`, `3.14`, represented as LLVM `double`
- Boolean values: `true`, `false`
- Unit: `()`, a single value that is present but has no additional content
- Text: `"Hello"`, `"你好"`
- Homogeneous lists: `[1, 2, 3]`, `["红", "绿", "蓝"]`
- Record: `{name: "小明", age: 20}`
- Result: `ok(value)` / `err(error)`, with type `Result<T, E>`
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Text concatenation: `"Hello, " + name`
- Comparisons: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Logic: `and`, `or`, `not`, also accepting `&&`, `||`, `!`
- Chinese or English names

### Unit and missing

`Unit` has just one value, `()`, representing completion without meaningful return content. It is not `missing`,
`false`, an empty Record, or an empty List:

```lan
finish(found) = if found { () } else { missing }

finish(true)           # ()
finish(false)?         # missing; the final result explicitly permits absence
finish(false) ?? ()    # ()
is_missing(())         # false
same((), missing)     # false
```

Unit can be used in function parameters, default values, return values, Record fields, and `List<Unit>` (including Table columns).
It follows the existing `?`, `??`, and `require` rules; equality comparisons also support elementwise List operations and broadcasting.
Unit cannot participate in arithmetic or serve as an `if` condition, and it cannot be mixed with Boolean in a homogeneous List.
`()` is the unit literal; both output and `to_text(())` use the text `()`. `unit` is no longer a reserved word
and can be used as a variable, function, or field name. The type is still called `Unit`; no other tuple types are introduced.

Unit does not allocate a heap object internally: it is an LLVM constant when definitely present, and uses `i1` to represent presence when it may be missing.
A compact `List<Unit>` uses one presence-flag byte per element, while dynamic values use a separate Unit tag.
Unit also represents the completion of `IO<Unit>` with no additional result.

### IO Actions and Sequential Execution

An `io` block constructs a deferred `IO<T>` value. Construction captures only the immutable bindings it uses, without running the code
inside the block. `<-` executes an action and binds its result; when the file's final result is IO, the program entry point executes it once:

```lan
number(value) = io { value }

program() = io
  first <- number(10)
  next = first + 2
  number(next)

program()  # 12
```

The same block can also be written as `io { first <- number(10); next = first + 2; number(next) }`.
The rules are:

- `name = expression` is a pure binding. Even if the right-hand side is IO, it is not executed automatically.
- `name <- action` executes IO. Bare expressions before the end of a block must also be IO; they are executed and their results discarded.
- A nonfinal IO step that returns Result cannot be discarded implicitly: bind and handle it, or write `_ <- action` to ignore it explicitly.
- If the last expression is a pure value, it becomes the result. If it is IO, it is executed and its result is used, without producing nested IO.
- Unused actions do not run. Using the same action with `<-` multiple times executes it again each time, without caching the result.
- Steps execute in order, and an error immediately stops subsequent steps. `?` ends only the expression in its own step, not the entire block.
- IO can be a function parameter, default value, return value, or local binding. It cannot be placed in a List/Record/Table, compared,
  passed to `same`, or converted to Text. `<-` can appear only in a step position inside an `io` block.

Execution is rejected at compile time if the action itself may be missing; use `??` first to select a fallback action. A missing value produced
by successful execution is ordinary data that later steps can handle with `??`, `is_missing`, or `require`. The program entry point may display
`missing` as the final data result of IO. Pure programs retain the rule that a final missing value requires an explicit `?`.
`err(error)` is also ordinary return data: it is not a runtime rejection and does not automatically stop later IO steps. `<-` executes
IO without unwrapping Result. Host errors in the existing standard IO functions still cause runtime rejection; they have not yet been converted to recoverable Result interfaces.

The following standard IO functions are implemented. Calling them only constructs an action; the external environment is accessed when it executes:

- `read_stdin() -> IO<Text>`: reads to EOF, preserving newlines; empty input returns empty Text.
- `write_stdout(text) -> IO<Unit>`: writes the text unchanged, without adding a newline, and flushes output before returning.
- `arguments() -> IO<List<Text>>`: returns a snapshot of command-line arguments excluding the executable name.

```lan
io {
  input <- read_stdin();
  args <- arguments();
  write_stdout(input);
  {characters: length(input), arguments: args}
}
```

Standard input is limited to 8 MiB, must be valid UTF-8, and cannot contain NUL. Reading may wait for external input or EOF;
the size limit does not guarantee a timeout. Executing the same read action again continues consuming input, or returns empty Text if EOF has already been reached.
Command-line arguments must be valid UTF-8, with at most 65536 arguments totaling 8 MiB; empty arguments are preserved.
IO errors are written to stderr with the action, source location, and call chain. System errors also include errno, and failure stops subsequent steps.
The entry point still automatically displays the final result, including `()` for `IO<Unit>`; this is separate from the explicit output of `write_stdout`.
File reading and writing and file-access permission controls are not yet implemented. See
[`docs/IO_DESIGN.md`](docs/IO_DESIGN.md) for the full specification.

### Time

`Instant` represents an instant on the UTC timeline, internally storing integer seconds and nanoseconds and following the usual missing-value rules.
It can be stored in Lists, Records, and Tables, and supports comparisons, `same`, `sort`, and `sort_by`.
Instant and Number do not convert implicitly, and `+` and `-` cannot be used directly for time arithmetic.

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

Pure time functions:

- `parse_time(text) -> Option<Instant>`: parses an RFC 3339 timestamp with `Z` or a numeric UTC offset;
  an invalid format or date returns missing.
- `format_time(instant) -> Text`: produces canonical UTC text with up to nine fractional second digits, removing trailing zeros.
- `time_from_unix(seconds)` / `unix_seconds(instant)`: explicitly convert to and from Unix seconds.
- `add_seconds(instant, seconds)`: adds a finite number of seconds and returns a new Instant.
- `seconds_between(start, end)`: returns `end - start`, which may be negative.

The first phase uses the Unix time model: UTC years range from `0000` to `9999`, with up to nine fractional second digits.
Leap seconds (`:60`), local time-zone names, and calendar-month arithmetic are not supported. Input may use lowercase `t`/`z`; `-00:00`
is parsed as UTC, and the original offset is not preserved. Seconds supplied as Number are rounded to the nearest nanosecond, with ties away from zero;
conversion back to Number is subject to floating-point precision limits. Time arithmetic rejects missing, nonfinite numbers, and range overflow.
These functions do not broadcast implicitly; use `values |> map(parse_time(.))` for batch conversion.

The current time can be obtained only through a deferred IO action:

```lan
io {
  clock = now();
  first <- clock;
  second <- clock;
  seconds_between(first, second)
}
```

Each execution of `now()` reads the system real-time clock again, without caching the result. It is not a monotonic timer: results may
be equal or move backward if the system clock is adjusted. Tests can fix the result of every `now()` call:

```sh
./build/lanc --run report.lan --fixed-time 2025-06-07T08:09:10Z
```

The fixed time is not included in `arguments()`. See
[`docs/TIME_RANDOM_DESIGN.md`](docs/TIME_RANDOM_DESIGN.md) for the full rules.

### Random Numbers

Random operations are deferred IO actions. Constructing an action neither reads system entropy nor advances the random sequence:

```lan
io {
  probability <- random();                 # [0, 1)
  temperature <- random_between(-10, 35);  # [-10, 35)
  die <- random_integer(1, 6);              # both endpoints included
  item <- choose(["red", missing, "blue"]);
  order <- shuffle([1, 2, 3, 4]);
  {probability: probability, die: die, item: item, order: order}
}
```

`choose` selects uniformly among non-missing elements; an empty List or an all-missing List returns missing.
`shuffle` uses Fisher–Yates and preserves all elements and their missing states. Invalid bounds are rejected when the action executes,
without consuming a random sample. Each execution of the same action obtains the next sample.

The default seed comes from the operating system's entropy source. For testing and reproducibility, you can specify a safe-integer seed for `lan-pcg64-v1`:

```sh
./build/lanc --run simulation.lan --random-seed 42
```

The same Lan version, seed, inputs, and action order produce the same sequence. This random library is not intended for cryptographic keys,
tokens, or secure random bytes. See [`docs/TIME_RANDOM_DESIGN.md`](docs/TIME_RANDOM_DESIGN.md) for the full rules.

### Text, Number, and Conversion Functions

`length` accepts both Text and List. Text length counts user-perceived Unicode grapheme clusters,
not UTF-8 bytes; combining character sequences and emoji joined by zero-width joiners each count as one character:

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

`trim` uses Unicode White_Space; `lowercase` and `uppercase` use Unicode simple case
mappings. `split` preserves empty Text produced by leading, trailing, and adjacent delimiters, and rejects an empty delimiter:

```lan
parts = split("a,,c,", ",")  # ["a", "", "c", ""]
join(parts, "|")             # "a||c|"
```

Basic Number functions accept only scalars and do not implicitly broadcast over Lists:

```lan
abs(-3.5)              # 3.5
floor(-1.2)            # -2
ceil(-1.2)             # -1
round(2.5)             # 2, using ties-to-even
round(12.25, 1)        # 12.2
clamp(15, 0, 10)       # 10
numbers |> map(abs(.)) # explicit elementwise call
```

The number of decimal places for `round` must be an integer-valued Number between `-308` and `308`. `clamp` requires the minimum
to be no greater than the maximum. These functions reject missing and nonfinite Numbers.

The full math library also provides the constants `pi`, `e`, and `tau`, correctly rounded to Number, and the following scalar functions:

```lan
sign(-8)             # -1; both positive and negative zero return 0
sqrt(9)              # 3
pow(2, 3)            # 8
exp(0)               # 1
log(e)               # 1, using base e by default
log(8, 2)            # 3
hypot(3, 4)          # 5

sin(pi / 2)          # trigonometric functions use radians
cos(0)
tan(0)
asin(0)
acos(1)
atan(0)
atan2(0, 0)
radians(180)         # pi
degrees(pi)          # 180
```

`sqrt`, `pow`, `log`, `asin`, and `acos` check their domains; all math functions reject nonfinite
inputs and nonfinite results. Math functions do not implicitly broadcast over collections; write
`numbers |> map(sqrt(.))`. Constants are not keywords and can be shadowed by local bindings.

`parse_number` strictly parses the entire Text, accepting only decimal and scientific notation without ignoring leading or trailing whitespace;
failure returns missing. `to_text` accepts existing data values and produces a deterministic Text representation:

```lan
parse_number("-12.5e2")       # -1250
parse_number("12x") ?? 0      # 0
require(parse_number(input))   # when failure must be rejected strictly
to_text([1, missing, 3])       # "[1, missing, 3]"
```

### Regular Expressions

`regex(pattern)` compiles Text into a reusable opaque Regex using RE2 syntax:

```lan
digits = regex("[0-9]+")

{
  contains: "item-123" |> regex_contains(digits),  # true
  matches: "123" |> regex_matches(digits),        # true; requires a full-text match
  first: "item-123" |> regex_find(digits),         # "123"
  all: "a1 b22" |> regex_find_all(digits),         # ["1", "22"]
  parts: "a, b; c" |> regex_split(regex("[,;] *")) # ["a", "b", "c"]
}
```

Pattern arguments must be Regex; Text patterns are not converted implicitly. Invalid patterns are rejected at runtime.
When no match is found, `regex_find` returns `missing` and `regex_find_all` returns an empty List.
`regex_split` preserves leading and trailing empty fragments; empty matches advance by one Unicode code point. For example,
`regex_split("你好", regex(""))` produces `["", "你", "好", ""]`.
Extractions that cut through a UTF-8 character boundary (such as `regex_find("你", regex("\\C"))`) are rejected.

Regex supports parameters, return values, List/Record storage, and ARC. It is displayed as `<Regex>`, without exposing internal addresses.
`to_text` rejects Regex and collections containing Regex. `same` compares the original pattern text; it does not determine whether two different
patterns match the same language. Regex does not support `==`/`!=`.
Ordinary missing arguments are still rejected; an explicit `?` can short-circuit. There is no implicit broadcasting:

```lan
texts |> map(regex_find(., digits))
```

Patterns are limited to 64 KiB, and the RE2 engine has an 8 MiB memory budget. Backreferences and lookaround are not supported.
Options such as case sensitivity use inline pattern flags, for example `regex("(?i)hello")`. See
[`docs/REGEX_DESIGN.md`](docs/REGEX_DESIGN.md) for detailed rules.

## Homogeneous Lists

All elements in a list must have the same type. The compiler infers the element type automatically, without requiring users to write
annotations such as `List<Number>`:

```lan
numbers() = [1, 2, 3]
names() = ["小明", "小兰"]
```

Number, Text, and Instant Lists support stable sorting, with missing always placed last:

```lan
sort([3, missing, 1, 2])       # [1, 2, 3, missing]
names |> sort()                # pipeline form
```

`distinct` removes duplicates using structural equality consistent with `same`, preserving the first occurrence of each value; multiple
missing values are reduced to one. It supports all List element types. The returned List shares, rather than copies, Text,
List, Record, Regex, and Instant elements:

```lan
distinct([2, 1, 2, missing, 1, missing])  # [2, 1, missing]
records |> distinct()
```

The implementation uses structural hashing, with average time linear in the total structural size of the input and additional space
linear in the number of outer elements. Hash collisions still invoke structural equality, so they do not cause incorrect deduplication.

Mixed-type lists are rejected at compile time:

```lan
[1, "two"]  # compile-time error
```

Lists can be nested, but the inner lists must also have the same element type:

```lan
[[1, 2], [3]]       # List<List<Number>>
[[1], ["two"]]     # compile-time error
```

Functions can return and pass lists directly, and both branches of a conditional must produce the same list type:

```lan
choose(flag) =
  if flag
    [1, 2, 3]
  else
    [4]

choose(true)
```

The result is displayed as:

```text
[1, 2, 3]
```

`missing` is the absent state of `Option<T>`, not a separate element type, so the following is a valid
`List<Number>`:

```lan
[1, missing, 3]
```

If elements come from dynamic Record fields and their actual tags cannot be determined at compile time, list construction checks that all
non-missing elements have the same type, rejecting a mismatch at runtime.

`length(numbers())` returns the number of elements in the list. Lists use the same dot-access syntax as Records:

```lan
numbers.0             # fixed index
numbers.(index)       # runtime index
```

Indices start at `0`. Out-of-bounds, negative, and noninteger indices, as well as stored values that are themselves missing, all yield `missing`;
use `??` to provide a default. Indices must be Number; text keys still denote dynamic Record field access.
The fixed form accepts only nonnegative integer literals. To check a negative or fractional index, use `list.(-1)` or
`list.(1.5)`. Chained access can be written directly as `matrix.1.0`.
Lists are immutable. Records can be list elements, but all elements of that list must be Records.

Lists support the same aggregate functions as Table columns:

```lan
sum([10, missing, 20])       # 30
average([10, missing, 20])   # 15
min([10, missing, 20])       # 10
max([10, missing, 20])       # 20
count([10, missing, 20])     # 2
```

Aggregates ignore `missing` elements within the list. For an empty or all-missing list, `sum` and `count` return
`0`, while `average`, `min`, and `max` return `missing`. The first four functions require `List<Number>`;
`count` accepts any homogeneous List. If the list itself is `Option<List<T>>`, it must first be explicitly unwrapped; otherwise compilation fails.

`first(list)` and `last(list)` read the first and last elements respectively, returning `missing` for an empty list.

`concat` joins two Lists with the same element type; `flatten` expands only one level; `reverse`, `take`, and
`drop` return new Lists, leaving the original unchanged. Slice counts must be nonnegative integers and are clamped to the container
boundary when they exceed its length:

```lan
items = concat([1, 2], [3, 4])
flatten([[1, 2], [], [3, 4]])  # [1, 2, 3, 4]
flatten([[[1], [2]], [[3]]])   # [[1], [2], [3]]
reverse(items)       # [4, 3, 2, 1]
take(items, 2)       # [1, 2]
drop(items, 2)       # [3, 4]
items |> reverse() |> take(2)  # [4, 3]
```

`flatten` has type `List<List<T>> -> List<T>`. Missing sublists in the outer List are rejected; missing elements
inside sublists are preserved.

### Collection Pipelines and Element Expressions

Every pipeline stage must use `|>` and include the function name and parentheses:

```lan
numbers |> sum()                    # sum(numbers)
people |> take(10)                  # take(people, 10)
people |> sort_by("score")          # sort_by(people, "score")
```

Older forms that omit `|>` or parentheses, such as `numbers sum` and `people take 10`, are not supported.

`map`, `filter`, `select`, `add`, `replace`, `find`, `any`, and `all` act as collection-operation names only after
`|>`. They are not globally reserved words and can be defined as user functions in ordinary call positions.

`map`, `filter`, `find`, `any`, and `all` establish an element context. Within it, `.` is the current logical element,
and `.name` and `.(key)` read its fixed and dynamic fields respectively:

```lan
numbers |> map(. * 2)
users |> map(.name)
users |> filter(.age? >= 18)
users |> find(.name == "Alice")
users |> any(.active)
users |> all(.age? >= 0)
```

For a List, `.` denotes the current List element; for a Table, it denotes the current logical row Record. It is not the entire value
on the left of the pipeline. Using `.` or `.field` outside an element context is a compile-time error. When a name is needed, use a restricted element binding:

```lan
users |> filter(user => user.active and user.age >= 18)
```

`name => expression` is allowed only as the sole element expression of the collection operations above. It is not a lambda and does not
create a function object. The expression can read outer immutable bindings and call ordinary named functions.

Ordinary functions do not automatically broadcast when passed a List:

```lan
adult(age) = age >= 18

adult(ages)                  # compile-time error if ages is a List
ages |> map(adult(.))        # explicit elementwise call
```

`map` always returns a List; `filter` preserves the List or Table container kind; `find` returns the first matching element
or `missing`. `filter` and `find` treat a final predicate value of `missing` as no match. `any` and
`all` use three-valued logic and short-circuit from left to right. Multiline pipelines can continue to use deeper indentation for formatting.

## Tables and Whole-Column Computation

Table has no separate literal syntax and is not a new runtime data type. The built-in `table(value)` accepts two
input forms. The column-oriented form is `Record<List<T>>`: every field must be a List, and all Lists must have
the same length. For Record/List literals passed directly, column types and statically known lengths are checked at compile time;
dynamically constructed values are still validated when `table` executes.

```lan
people() = table({
  name: ["小明", "小兰", "小红"],
  age: [20, 17, missing],
  score: [80, 90, missing]
})

people()
```

After successful validation, this form returns the original Record pointer without copying the Record or any List. The row-oriented form is
`List<Record>`; `table` converts its rows into columns of equal length:

```lan
table([
  {name: "小明", age: 20},
  {name: "小兰", age: 17}
])
```

The compiler statically refines the result to Table; both LLVM and the runtime use a `Record<List<T>>` pointer. Ordinary
Records and `List<Record>` cannot be passed directly to table operations; they must first go through `table(...)`.

```lan
row_count(people)          # 3
sum(people, "age")         # 37
average(people, "score")   # 85
min(people, "age")         # 17
max(people, "age")         # 20
count(people, "score")     # 2
```

Columns can perform elementwise arithmetic and comparisons directly with a value or another column. Scalars broadcast automatically,
and a cell's `missing` propagates only to the corresponding result cell:

```lan
people.score + 5           # [85, 95, missing]
people.age >= 18           # [true, false, missing]
fill_missing(people.score, 0)
```

When two Lists have different lengths, computation covers only positions where both have elements, and the result takes the length of the shorter List:

```lan
[1, 2, 3] + [10, 20]      # [11, 22]
[1, 2] < [0, 3, 4]        # [false, true]
```

This applies only to ordinary elementwise operations. A Table still requires all columns to have equal lengths to preserve its rectangular structure.

Logic, unary operations, default values, and text concatenation also support elementwise processing and scalar broadcasting:

```lan
[true, false, missing] and false   # [false, false, false]
[true, false, missing] or true     # [true, true, true]
not [true, missing]                # [false, missing]
-[1, missing, -2]                 # [-1, missing, 2]
[1, missing, 3] ?? 0              # [1, 0, 3]
["A", missing] + "!"             # ["A!", missing]
```

Elementwise `and` and `or` use the three-valued results corresponding to short-circuit logic: `false and missing` is
`false`, and `true or missing` is `true`; other indeterminate combinations produce `missing`.
Text `+` permits concatenation only between a Text column and another Text column or a Text scalar. When the left operand of `??` is a List, defaults are
provided elementwise; `missing ?? list` still specifies a default for the entire List.

Nested Lists use recursive broadcasting aligned from the trailing dimensions. Static nesting depths are compared first, and the lower-dimensional operand
is broadcast across missing leading dimensions before computation proceeds level by level. Once both operands reach the same dimension, each level is truncated to the shorter length:

```lan
[[1, 2], [3, 4]] == [1, 2]
# [[true, true], [false, false]]

[[1, 2, 3], [4]] + [[10, 20], [30, 40]]
# [[11, 22], [34]]
```

This rule applies only to Lists. For a Table, first extract a column using `table.column`; Table values themselves do not participate
in broadcasting.

To compare complete structures without broadcasting, use `same(left, right)`:

```lan
same([[1, 2], [3, 4]], [1, 2])                 # false
same([1, missing], [1, missing])               # true
same({a: 1, b: 2}, {b: 2, a: 1})              # true
```

`same` always returns a single Boolean, does not broadcast, and does not require both operands to have the same type. Different types, List
lengths, nesting structures, or Record key sets return false. Lists are compared recursively by position; Records
ignore HAMT order and are compared recursively by key. Two missing values are equal; a missing value and a present value are different.
Tables are compared through their underlying `Record<List<T>>` structure.

The following operations return new Tables, leaving the input Table unchanged:

```lan
people |> filter(.age? >= 18)
people |> select({name: .name, adult: .age? >= 18})
people |> add({adult: .age? >= 18})
people |> replace({score: .score ?? 0})
people |> select_columns(["name", "score"])
people |> take(10)
people |> drop(10)
people |> sort_by("score")                         # stable ascending order, missing last
```

The element expression of `select` must produce a Record, and `select` always returns a Table. `add` rejects an existing field name at runtime,
and `replace` rejects a nonexistent field at runtime. Row counts for `take` and `drop` must be nonnegative integers
and are clamped to the table boundary when they exceed the total number of rows. `select_columns` rejects nonexistent columns at runtime.
The column-name list cannot be empty because the current `Record<List<T>>` representation cannot separately store the row count of a zero-column table.

`sum`, `average`, `min`, and `max` accept only numeric columns; `count` works with any column and returns the number of
non-missing cells. Aggregates ignore missing. For an empty or all-missing column, `sum` and `count` return 0,
while `average`, `min`, and `max` return missing, which can be handled with `??`:

```lan
average(people, "unknown")       # nonexistent column, runtime error
average(table({age: []}), "age") ?? 0
```

A Table is still a `Record<List<T>>` internally: the HAMT looks up column names, and homogeneous Lists store the actual column data.
Table validation traverses only column descriptions and reads list lengths; it does not traverse or copy cells. Filtering, slicing, sorting,
and whole-column computation allocate new Lists and HAMT paths for the result without modifying the input Table.

The compiler propagates TableShape for statically known columns. In a Table element expression, `.field`
reads directly by column and row index without first constructing a complete row Record; `table |> map(.field)`
can directly reuse the validated underlying List. `find` also avoids materializing rows while checking the predicate,
constructing the Record to return only after a match is found.
`map({...}) |> table()` is fused when evaluation and error order can be preserved: pure field projections directly
reuse source columns, while safe arithmetic, comparison, and logical expressions write result columns in a single row loop.
Expressions containing `?`, function calls, or dynamic types conservatively fall back to the general implementation.

## Record

A Record is an immutable mapping from field names to values, not an object with methods. Fields themselves can hold any
Lan value and are not part of the Record's static type, so different Records do not need to declare the same field set in advance.

```lan
make_person() = {name: "小明", age: 20}

make_person()
```

Both fixed fields and fields determined at runtime use dot access; `record[key]` is not provided:

```lan
read(person) =
  field = "age"
  [person.name, person.(field)]

read({name: "小明", age: 20})
```

Record literals also allow dynamic field names supplied by Text expressions:

```lan
make_person() =
  field = "age"
  {name: "Alice", (field): 21}

make_person()
```

`{field: 21}` has the fixed field name `"field"`, whereas `{(field): 21}` uses the value of the variable `field`.
Dynamic key expressions must produce Text.

A nonexistent field yields `missing`. A field that cannot be proven to exist is read as `Option<T>`; use `??`
to provide a default. Using it in ordinary operations without unwrapping causes a compile-time error. The compiler can track known fixed
fields of immutable Records and known columns of validated Tables; dynamic keys and unknown fields still require explicit handling.

```lan
person.city ?? "未知"
person.height + 1       # compile-time error if height may not exist
```

Inside a function block, a concise update syntax can add or replace fields:

```lan
birthday(person) =
  old = person
  person.age = person.age + 1
  person.city = "悉尼"
  [old, person]
```

`person.age = value` is not mutable assignment. It is syntactic sugar for two steps: create a new, updated Record,
then let a new `person` binding at the same level shadow the old binding. `old` still refers to the pre-update value. Dynamic keys can also be written as
`person.(field) = value`.

To explicitly retain the original binding, use `with` to add or replace multiple fields at once:

```lan
move(person) =
  person with {
    age: 21,
    city: "Sydney"
  }

move({name: "Alice", age: 20})
```

Update blocks also support dynamic keys in the form `(field): value`. A nested update is written as
`person.address.city = "Sydney"`, constructing a new Record path from the inside out. The update target must be rooted in
a local binding name; `get_person().age = 21` is rejected at compile time. Intermediate fields that are missing or are not Records
are rejected at runtime. The direct form `person.self = person` is also rejected.

Records are semantically unordered. To make results reproducible, displayed Records sort field names by their UTF-8 byte order,
not insertion order.

`fields(record)` returns field names in that same deterministic order, and `has_field(record, key)` checks whether a key exists.
A field's existence and its value being missing are separate questions:

```lan
person = {name: "Alice", age: missing}

fields(person)                 # ["age", "name"]
has_field(person, "age")      # true
has_field(person, "score")    # false
```

`merge` combines two Records, with right-hand fields overriding left-hand fields of the same name. `remove_field` returns a new
Record with a field removed, or a structurally identical Record if the field does not exist. Neither input Record is changed:

```lan
merge({name: "Alice", age: 20}, {age: 21})       # {age: 21, name: "Alice"}
remove_field({name: "Alice", age: 20}, "age")  # {name: "Alice"}
```

The underlying structure is a persistent HAMT. An update copies only the path from the root to the target field; the old and new Records share the other branches,
and HAMT nodes are reference-counted. Text, List, Record, Table, Regex, Instant, and IO all participate in ARC. Composite values hold their child
objects, function parameters borrow the caller's values, newly created values are held by the current function's ownership region, and return values
are transferred to the caller before region cleanup. Internally, the runtime uses C++ RAII for temporarily owned references and distinguishes
ownership transfer from sharing through `owned`/`shared` container writes. The LLVM boundary still uses the stable C ABI `LanValue`, without exposing C++
object layouts. Low-level retain/release operations are confined to the reference-counting implementation. Normal program termination checks that no heap objects remain. The optimization for in-place
updates when the reference count is 1 is not yet enabled; updates still always use safe path copying.

## Option Types, Missing Values, and Explicit Unwrapping

Ordinary values default to `T`; only values that may be missing have type `Option<T>`. The compiler infers types, so handwritten
type annotations or wrapper constructors are not required. `T` can be promoted automatically to `Option<T>`, but the reverse conversion requires explicit handling of absence.
`missing` is the absent state of an optional type, with its underlying value type inferred from context.

```lan
lookup(found) =
  if found
    "数据"
  else
    missing

lookup(false) ?? "没有找到"
```

`??` evaluates and uses the right-hand default only when the left operand is missing. Ordinary operations and functions requiring `T` reject an unhandled
`Option<T>`. Even if a particular run happens to produce a value, it must be unwrapped first:

```lan
missing + 1       # compile-time error
length(missing)  # compile-time error

choose_missing() =
  if missing
    1
  else
    0

choose_missing()  # compile-time error: an if condition must be Boolean
```

The postfix `?` explicitly allows the current expression to propagate absence. Operations still evaluate from left to right; once a value checked by `?`
is missing, the current expression immediately yields `missing` and its remaining parts are not evaluated:

```lan
missing? + expensive_calculation()  # missing; expensive_calculation is not called
```

The propagation boundary of `?` is an expression, not an early return from a function. Each binding's right-hand side is a separate boundary, in both ordinary
and IO blocks. After a binding receives missing, subsequent bindings continue to execute. An outer `??` can also catch the propagated
absence within the same function:

```lan
(lookup(false)? + "!") ?? "没有找到"  # "没有找到"
```

This differs from Rust-style `?`: the example continues to evaluate the right-hand side of `??`, rather than returning early from the caller
of `lookup`. `?` has higher precedence than arithmetic, comparisons, and `??`.

Functions can accept, store, or return Option as long as the body does not consume it implicitly. Whether a parameter permits missing is inferred from the function body
and call sites; named arguments, default arguments, and calls across functions follow the same checking rules:

```lan
normalize(value) = value ?? 0

normalize(missing)  # 0
```

Four ways to unwrap:

```lan
double(value) = value * 2

demo(text) = {
  value = parse_number(text);             # Option<Number>
  defaulted = value ?? 0;                 # Number
  propagated = double(value?);            # Option<Number>
  refined = if is_missing(value) { 0 } else { value + 1 }; # Number
  asserted = require(value);              # Number; rejected at runtime if missing
  {defaulted: defaulted, propagated: propagated, refined: refined, asserted: asserted}
}

demo("10")
```

`?` lets the current computation continue using `T`, while the overall expression may still produce `Option<U>`.
The else branch of `if is_missing(value)` and the then branch of `if not is_missing(value)` refine that
local binding to `T`. The refinement does not escape the branch, and shadowing a name does not retain the proof for the old binding.

Scalar `and`, `or`, and `not` accept only Boolean and do not implicitly consume `Option<Boolean>`.
`false and missing` is a compile-time error; `false and require(missing)` is valid and short-circuits to false.
`missing? and false` immediately short-circuits to missing without checking the right operand. Elementwise collection logic and
`any` and `all` retain their explicit three-valued logic rules.

The optionality of containers and their elements is inferred separately:

```text
List<Number>          # both list and elements are present
List<Option<Number>>  # list is present; elements may be missing
Option<List<Number>>  # the entire list may be missing
IO<Option<Number>>    # action is present; its execution result may be missing
Option<IO<Number>>    # the action itself may be missing
```

Indexing a `List<T>`, `first`, and `last` still return Option, because an index may be out of bounds or the list may be empty.
The element context of `map` preserves element optionality: `[1, 2] |> map(. + 1)` is valid,
while `[1, missing] |> map(. + 1)` is a compile-time error; the latter can be written as `map(.? + 1)`.
`join` requires `List<Text>`, and `flatten` requires every sublist to be present; use `fill_missing`
or `map(require(.))` first to handle missing elements.

When the left operand is `Option<List<T>>`, `??` selects a default of the same type for the entire list, as in `maybe_list ?? []`.
When the left-hand list is definitely present, `list ?? value` still performs elementwise filling through broadcasting.
If the same `??` within a function would acquire these two different meanings at different call sites, the compiler rejects it. To explicitly fill elements,
use `fill_missing` instead of letting the operation's meaning depend on the call site.

### Pattern Matching

`match` is an expression that can directly split `Option<T>` into missing and present cases:

```lan
normalize(input) =
  match input
    missing => 0
    value(number) => number + 1
```

`value(name)` matches only present values and binds the unwrapped `T` to `name`; `value(_)` ignores the value.
`missing` matches only absence. Ordinary names and `_` are remainder patterns: a name binds the remaining value, while `_` does not bind it.
If a preceding pattern has already covered missing, the remainder name is refined to `T`:

```lan
describe(input) = match input {
  missing => "没有数据";
  number => to_text(number)
}
```

Patterns can also use Number, Text, Boolean, and Unit literals:

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

Branches are tested in source order, the input is evaluated once, and only the matching branch is evaluated. Duplicate patterns, branches fully
covered by preceding patterns, and nonexhaustive matches are compile-time errors. An `Option<T>` that may be either present or missing
must cover both cases. A definitely present `T` does not require a missing branch; for a definitely missing input,
covering missing alone is sufficient. Boolean's `true`/`false` cover
all present values, as does Unit's `()`. Number and Text literals still need `_`, an ordinary
name, or `value(name)` to cover other values.

Branch bindings are valid only within their own branch and can shadow outer bindings with the same name. When matching an original local name, the compiler also
refines that name within the relevant branches. Branch return values must share the same underlying type; `T` and `Option<T>` can be
combined under the usual rules.

`?` still uses expression boundaries: on the match input, missing propagates before any branch is entered; within
a branch, it makes the entire match expression yield missing. Structural destructuring of Lists and Records is not yet supported; first
match `value(list)` / `value(record)`, then use existing indexing and field access. Pattern matching is not runtime
type dispatch; literal patterns must have the same static type as the input.

## Result and Recoverable Failures

`Result<T, E>` represents success carrying `T` or failure carrying `E`. Types are still inferred by the compiler, without
handwritten type annotations. `ok` and `err` are built-in constructors, not syntax keywords. Each accepts one positional argument and cannot be
redefined by a user function. Ordinary values are not implicitly wrapped in Result, and Results are not implicitly unwrapped:

```lan
divide(a, b) =
  if b == 0 { err("除数不能为零") } else { ok(a / b) }

message = match divide(10, 0) {
  ok(number) => to_text(number);
  err(error) => "计算失败：" + error
}

message
```

The function returns `Result<Number, Text>`. `ok(())` represents success without additional data; an error payload can also be a
Record, List, or other data value. Within the same Result, success payloads unify as T and failure payloads as E; the two
need not be the same. The other type left undetermined by a constructor is constrained by its usage context, not automatically defined as Unit.

The `ok(name)` and `err(name)` patterns bind the respective unwrapped payloads, with `_` available to ignore a payload. Without a fallback branch,
both states must be covered even if the input is written as `ok(1)`. For `Option<Result<T, E>>`, the
outer missing case must also be handled. An ordinary name pattern binds the entire remaining Result without automatically extracting its payload;
`value(name)` also handles only the outer Option, without unwrapping Result. Patterns may use indentation, but the first version supports only
a name or `_` inside a constructor. Use another match for nested payloads; nested patterns such as `ok(err(e))` are not supported.

```lan
ok(missing)      # success with a missing payload; not the same as missing or err(...)
ok(err("bad"))  # success whose payload is another Result representing failure
```

`require(result)` is an explicit assertion: success returns the payload; failure prints the error payload and source diagnostics, then terminates the program.
It unwraps only one Result layer, without also removing Option from the success payload:

```lan
require(ok(42))              # 42
require(ok(missing)) ?? 0     # 0; the success payload may still be missing
require(err("bad"))          # runtime rejection
```

For a Result with an outer optional layer, first handle outer absence with match, `?`, or `??`, then use require.
`?`, `??`, and `is_missing` still observe only Option; they do not propagate, fill, or detect err. For example, `err("bad")?`
remains the same Result, rather than causing an early function return or short-circuiting an IO block. Existing runtime rejections, such as division by zero, are not automatically
caught as err, and existing Option interfaces such as `parse_number` do not change.

Result can be used as a function parameter, default value, return value, List element, or Record field, as well as an IO
result captured by an action. Payloads cannot contain IO actions; use `IO<Result<T, E>>`:

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

`<-` does not unwrap Result, and err does not automatically stop subsequent steps. Error-recovery APIs for executing IO have not yet been integrated into the standard library;
`read_config` here only demonstrates returning ordinary Result data.

Use `same` for whole-value comparison, comparing both state and payload. Result does not support `==`, arithmetic, or sorting.
`to_text` and result display preserve the `ok(...)` / `err(...)` wrapper; values containing Regex still cannot be converted to Text.
In the first version, Result is an ARC heap object holding one tagged payload. Construction retains the payload reference, destructuring borrows it,
and leaving scope releases it under the existing ARC rules. Outer missing uses a null pointer, without copying internal Lists/Records.

## Arithmetic Errors and Runtime Representation

Ordinary division by `+0.0` or `-0.0` is rejected at runtime, rather than producing Infinity, NaN, or missing.
List division checks only elements that actually participate in computation and are present on both sides. Positions truncated by a shorter length and positions whose results
are already determined by missing do not perform division.

`%` has the same precedence as multiplication and division, accepts any Number, and uses Python-style floor modulo:

```lan
5 % 2       # 1
5.5 % 2     # 1.5
-5 % 2      # 1
5 % -2      # -1
```

It likewise rejects `+0.0` and `-0.0` divisors and supports ordinary List broadcasting. Missing values, truncated positions, and error
order behave exactly as with `/`. `if` still requires an explicit Boolean condition, for example
`numbers |> map(if . % 2 != 0 {100} else {200})`.

Runtime rejections report the source line and column and the function call chain. Errors during collection computation also report the List element
index or Table row number, with a field path where possible. For example:

```text
Lan 运行错误：除数不能为零
位置：report.lan:12:31
Table 行号：1
字段路径：.divisor
调用链：程序 -> report -> ratio
```

This information comes from lightweight diagnostic contexts at function entry and in the current collection loop; it is not permanently attached to every value.

You can also explicitly check or require a value to be present:

```lan
is_missing(value)  # always returns Boolean
require(value)     # for Option: rejects missing at runtime, otherwise returns the value; see above for Result rules
```

A simple `if not is_missing(value)` refines `value` to definitely present in the then branch.
If a pure program's final result may be missing, the compiler requires `?` to explicitly permit displaying `missing`. The final
data result of IO may be missing.

Option type checking is separate from the underlying presence optimizations: type checking determines how values may be consumed, while presence analysis still
infers `Present`, `Missing`, and `MaybeMissing` and selects compact representations for different types without forcing boxing:

| Case | LLVM representation |
| --- | --- |
| Definitely present number | Bare `double` SSA value |
| Possibly missing number | `{ i1 present, double value }` |
| Definitely present Boolean | `i1` |
| Possibly missing Boolean | `i2`: 0 missing, 1 false, 2 true |
| Unit | `i1`: 0 missing, 1 () |
| IO action | `ptr`: null means the action is missing; captured environments use ARC |
| Possibly missing Text, List, Record, or Regex | `ptr`: null means missing |
| Statically typed numeric list element | `double[]` plus per-element presence flags |
| Statically typed Boolean list element | Three-state byte: missing/false/true |
| Text, List, Record, or Regex list element | `ptr[]`: null means missing |
| Unit list element | `i8[]`: 0 missing, 1 () |
| Dynamic field/list with no statically determined type | Fallback value with a dedicated `MISSING` tag |

The LLVM `-O2` pipeline runs before emitting IR, allowing constant presence flags, dead branches, and temporary Option
wrappers to be eliminated further.

List operations with compile-time-known element types retain this representation: scalar broadcasting, unary operations, and nested broadcasting for
Number/Boolean/Text do not fall back to dynamic tags on each element. The outer levels of nested Lists are pointer arrays, while leaf levels
still use their corresponding compact arrays. Arithmetic and comparisons between one-dimensional Number columns use dedicated loops that LLVM can
vectorize without changing missing semantics. Inter-column and scalar logic, equality comparisons, and
`??` for three-state Boolean columns also run dedicated loops directly on compact byte encodings; dynamic types and operations that allocate Text retain
semantically equivalent scalar paths.

When a Table element expression passes an entire row to an ordinary named function, the compiler analyzes and inlines that function's body at the call site.
Fixed fields actually read by the function are fetched directly by column and row index, without preconstructing a complete row Record for the function argument.
Default and named arguments still follow ordinary function-call rules.

Because the function call graph is guaranteed to be acyclic, presence analysis no longer scans repeatedly to a fixed point. The compiler traverses the call DAG
depth-first, obtaining each callee's result before returning to its caller and caching the presence states for each set of arguments. Functions
unreachable from the top level are analyzed once as separate DAG roots.

## Recursion Is Not Allowed

Function calls cannot form cycles. Direct recursion and indirect recursion that returns to the same function through other functions are both
rejected at compile time:

```lan
repeat(value) = repeat(value)  # compile-time error
```

This restriction also applies to function calls in default parameters. The language has no general `for` or `while`; collection pipelines use
the finite length of the input container to execute `map`, `filter`, `find`, `any`, and `all`.

`if` evaluates only the selected branch. `and`, `or`, `??`, and explicit `?` propagation each follow their own short-circuit rules.

## Pure Function Rules

- The same inputs always produce the same result.
- There is no mutable assignment, global mutable state, or arbitrary I/O; field-update syntax creates a new Record binding.
- Direct and indirect recursion are prohibited.
- Functions and local names are immutable.
- The final formula in a file is the result of the entire program.
- Displaying the final result is the runner's host boundary, not a side effect inside a user function.

Functions currently use monomorphic type inference: a function should be used with consistent input kinds within the same program. Inputs whose kinds
cannot be determined from context default to numbers. The compiler explains type conflicts and spelling errors in user-facing language.

## Building and Running

Requires CMake 3.22+, Flex, Bison, Clang/Clang++, llvm-ar, and LLVM development files.
The build automatically downloads pinned versions RE2 2025-11-05 and Abseil 20250512.1. For offline builds, pass
`-DFETCHCONTENT_SOURCE_DIR_ABSEIL=/path/to/abseil` and
`-DFETCHCONTENT_SOURCE_DIR_RE2=/path/to/re2` to CMake, pointing to unpacked source directories.

Unicode Text operations use utf8proc 2.11.3 bundled with the source; no separate installation is needed for the build. Its license is at
`third_party/utf8proc/LICENSE.md`.

```sh
cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build
ctest --test-dir build --output-on-failure
```

Compute and display results directly:

```sh
./build/lanc --run examples/types.lan
./build/lanc --run examples/missing.lan
./build/lanc --run examples/record.lan
```

`--build` creates `.lan/<program-name>/` in the current directory, retaining the LLVM IR, object file, and native executable:

```sh
./build/lanc --build examples/types.lan

# Produces:
# .lan/types/types.ll
# .lan/types/types.o
# .lan/types/types
```

`--run` performs exactly the same persistent build, then automatically executes `.lan/<program-name>/<program-name>`.
It no longer creates temporary IR or interprets it through `lli`. `.lan/` is included in `.gitignore`.

Use `--` to pass subsequent arguments unchanged to the program for `arguments()` to read:

```sh
./build/lanc --run program.lan -- "a b" "" --verbose
.lan/program/program "a b" "" --verbose
```

`-- arguments...` is supported only in `--run` mode; the program directly inherits standard input, output, and error streams.
`--random-seed` is also supported only with `--run` and accepts integers from `0` to `9007199254740991`. It must appear
before the program-argument separator `--`; the same text after the separator is just an ordinary program argument.
`--fixed-time` is likewise supported only with `--run`, accepts an RFC 3339 timestamp, and must appear before `--`.
The fixed time is validated when `now()` executes; an invalid configuration produces a runtime error with a source location.

Lan/Unicode bitcode and the RE2 static library are embedded in `lanc`, so there is no need to read the source build directory at runtime.
Programs that use regular expressions also generate `lan_regex_runtime.a` and
`REGEX_LICENSES.txt` in their own `.lan/<program-name>/` directory and statically link RE2/Abseil; include the licenses when distributing such programs.
Programs that do not use regular expressions do not extract or link this static library.
This does not make a locally built compiler a fully standalone distribution: it still requires the Clang toolchain, and the LLVM
distribution in use may have dynamic dependencies such as z3/zstd.

Generate LLVM IR:

```sh
./build/lanc examples/types.lan -o types.ll
lli types.ll
```

`-o` is available only in the default IR-output mode and cannot be combined with `--build` or `--run`.
IR containing regular-expression calls has external RE2 adapter symbols and cannot be executed directly with the bare `lli` command above;
use `--build`/`--run` to complete static linking.

## Compilation Structure

```text
Lan formulas
  -> Flex: words, numbers, text, and indentation
  -> Bison: expression syntax tree
  -> Constraint unification: automatic type inference
  -> Persistent HAMT runtime: Record lookup, path copying, and reference counting
  -> LLVM: SSA values, branches, and PHI nodes
  -> verifier: IR validation
  -> Clang: generates `.o` and a native executable
  -> Executable: computes and displays the result at the end of the file
```

User functions use LLVM SSA values directly, without generating mutable local variables. Text concatenation is handled through a hidden runtime
boundary but remains referentially transparent as observed by a Lan program.

Record in-place updates may later be enabled along uniquely referenced paths. Other syntax and standard-library plans are described in the documents below.

For completed work and future plans for the language core, see
[`docs/SYNTAX_TODO.md`](docs/SYNTAX_TODO.md). For the standard-library API inventory, see
[`docs/STDLIB_TODO.md`](docs/STDLIB_TODO.md). Full topical design documents are also collected in
[`docs/`](docs/). For the implemented first-phase regular-expression API and its future boundaries, see
[`docs/REGEX_DESIGN.md`](docs/REGEX_DESIGN.md).
