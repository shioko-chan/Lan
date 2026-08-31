execute_process(
  COMMAND "${LANC}" --run "${SOURCE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "program failed (${result}): ${output}${error}")
endif()
string(STRIP "${output}" output)
if(NOT output STREQUAL EXPECTED)
  message(FATAL_ERROR "expected '${EXPECTED}', got '${output}'")
endif()
