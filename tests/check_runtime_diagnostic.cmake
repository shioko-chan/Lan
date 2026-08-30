execute_process(
  COMMAND "${LANC}" --run "${SOURCE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(result EQUAL 0)
  message(FATAL_ERROR "program unexpectedly succeeded: ${output}")
endif()
set(combined "${output}${error}")
foreach(expected
    "Lan 运行错误：除数不能为零"
    "位置：${SOURCE}:1:31"
    "Table 行号：1"
    "字段路径：.divisor"
    "调用链：程序 -> report -> ratio")
  string(FIND "${combined}" "${expected}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR
            "runtime diagnostic did not contain '${expected}': ${combined}")
  endif()
endforeach()
