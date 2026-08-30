execute_process(
  COMMAND "${LANC}" --run "${SOURCE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(result EQUAL 0)
  message(FATAL_ERROR "program unexpectedly succeeded: ${output}")
endif()
set(combined "${output}${error}")
if(NOT combined MATCHES "${EXPECTED}")
  message(FATAL_ERROR
          "runtime error did not contain '${EXPECTED}': ${combined}")
endif()
string(FIND "${combined}" "位置：${SOURCE}:" location)
if(location EQUAL -1)
  message(FATAL_ERROR "runtime error did not contain a source location: ${combined}")
endif()
string(FIND "${combined}" "调用链：程序" call_chain)
if(call_chain EQUAL -1)
  message(FATAL_ERROR "runtime error did not contain the call chain: ${combined}")
endif()
if(DEFINED EXPECTED_INDEX)
  string(FIND "${combined}" "List 元素索引：${EXPECTED_INDEX}" list_index)
  if(list_index EQUAL -1)
    message(FATAL_ERROR
            "runtime error did not contain List index ${EXPECTED_INDEX}: ${combined}")
  endif()
endif()
