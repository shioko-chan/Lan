file(MAKE_DIRECTORY "${OUTPUT}")

function(reject name source expected)
  set(path "${OUTPUT}/${name}.lan")
  file(WRITE "${path}" "${source}\n")
  execute_process(COMMAND "${LANC}" "${path}" -o "${OUTPUT}/${name}.ll"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected}")
    message(FATAL_ERROR "${name}: expected '${expected}': ${output}${error}")
  endif()
endfunction()

reject(number_incomplete "match 1 { 1 => 10 }" "未穷尽")
reject(boolean_incomplete "match true { true => 1 }" "未穷尽")
reject(optional_incomplete "match parse_number(\"1\") {value(x) => x}" "未穷尽")
reject(missing_incomplete "match parse_number(\"1\") {missing => 0}" "未穷尽")
reject(duplicate "match 0 { 0 => 1; -0.0 => 2; _ => 3 }" "重复|不可达")
reject(duplicate_text "match \"x\" { \"x\" => 1; \"x\" => 2; _ => 3 }" "重复|不可达")
reject(duplicate_missing "match missing { missing => 1; missing => 2 }" "重复|不可达")
reject(after_wildcard "match 1 { _ => 0; 1 => 1 }" "不可达")
reject(after_binding "match 1 { x => x; _ => 1 }" "不可达")
reject(after_present "match 1 { value(x) => x; 1 => 0; missing => 2 }" "不可达")
reject(pattern_type "match 1 { \"x\" => 0; _ => 1 }" "种类不同")
reject(branch_type "match 1 { 0 => true; _ => 1 }" "种类不同")
reject(scope "{result = match 1 {value(x) => x}; x}" "找不到名称")
reject(arm_scope "match 1 {0 => {x = 1; x}; _ => x}" "找不到名称")
reject(catchall_optional "match parse_number(\"1\") { x => x + 1 }" "Option 类型错误")
reject(shadow_optional "match parse_number(\"1\") {missing => 0; value(x) => {x = missing; x + 1}}" "Option 类型错误")
reject(original_missing "{x = parse_number(\"1\"); match x {missing => x + 1; value(n) => n}}" "Option 类型错误")
reject(recursion "f(x) = match x { _ => f(x) }\nf(1)" "递归")
reject(capture_scope "{a = io {match 1 {value(x) => x}}; io {a; x}}" "找不到名称")
reject(empty "match 1 {}" "无法理解")
reject(constructor "match 1 { just(x) => x; _ => 0 }" "模式构造器")
reject(separator "match 1 { 1 => 1 _ => 0 }" "无法理解")
reject(list_pattern "match [1] { [x] => x; _ => 0 }" "无法理解")
reject(record_pattern "match {a: 1} { {a: x} => x; _ => 0 }" "无法理解")
reject(pattern_expression "match 2 { 1 + 1 => 0; _ => 1 }" "无法理解")
reject(option_boolean "f(x) = match x {true => 1; false => 0}\nf(missing)" "未穷尽")
reject(scope_after_match "{x = parse_number(\"1\"); r = match x {missing => 0; value(_) => x + 1}; x + 1}" "Option 类型错误")
reject(wildcard_optional "{x = parse_number(\"1\"); match x {_ => x + 1}}" "Option 类型错误")

# Exercise the same matching rules through both layout spellings.
file(WRITE "${OUTPUT}/layout.lan" "f(x) =\n  match x\n    missing => 0\n    value(n) => n + 1\nf(2)\n")
file(WRITE "${OUTPUT}/braces.lan" "f(x) = match x {missing => 0; value(n) => n + 1}\nf(2)\n")
foreach(style layout braces)
  execute_process(COMMAND "${LANC}" --run "${OUTPUT}/${style}.lan"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0 OR NOT output STREQUAL "3\n")
    message(FATAL_ERROR "${style}: ${output}${error}")
  endif()
endforeach()

# A ? on the subject escapes before pattern matching; a branch ? escapes the
# match expression, not its surrounding sequence of bindings.
file(WRITE "${OUTPUT}/escape.lan" [=[
{
  stopped = match missing? {missing => 1 / 0; _ => 2};
  branch = match 1 {value(_) => missing?};
  answer = (match 1 {value(_) => missing?}) ?? 42;
  is_missing(stopped) and is_missing(branch) and answer == 42
}
]=])
execute_process(COMMAND "${LANC}" --run "${OUTPUT}/escape.lan"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output STREQUAL "true\n")
  message(FATAL_ERROR "match escape boundary: ${output}${error}")
endif()

# Compile the same path in two layouts and compare diagnostic-free function IR.
# Using only constants avoids source-location metadata differences in the IR.
foreach(style layout braces)
  if(style STREQUAL "layout")
    set(source "match true\n  true => 1\n  false => 2\n")
  else()
    set(source "match true {true => 1; false => 2}\n")
  endif()
  file(WRITE "${OUTPUT}/equivalent.lan" "${source}")
  execute_process(COMMAND "${LANC}" "${OUTPUT}/equivalent.lan" -o "${OUTPUT}/${style}.ll"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${style} IR: ${error}")
  endif()
  file(READ "${OUTPUT}/${style}.ll" ${style}_ir)
endforeach()
if(NOT layout_ir STREQUAL braces_ir)
  message(FATAL_ERROR "match layout spellings produced different IR")
endif()
