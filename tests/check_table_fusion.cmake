execute_process(
  COMMAND "${LANC}" --run "${SOURCE}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "fused program failed: ${run_error}")
endif()
if(NOT run_output MATCHES
   "computed: \\{adult: \\[true, false\\], label: \\[\"Alice!\", \"Bob!\"\\]\\}.*projected: \\{label: \\[\"Alice\", \"Bob\"\\], years: \\[20, missing\\]\\}")
  message(FATAL_ERROR "unexpected fused program output: ${run_output}")
endif()

execute_process(
  COMMAND "${LANC}" "${SOURCE}" -o "${OUTPUT}"
  RESULT_VARIABLE compile_result
  ERROR_VARIABLE compile_error)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR "Lan compilation failed: ${compile_error}")
endif()

file(READ "${OUTPUT}" ir)
if(ir MATCHES "call ptr @lan_table_from_rows\\(")
  message(FATAL_ERROR "projection fusion materialized an intermediate row list")
endif()
if(ir MATCHES "collection\\.map")
  message(FATAL_ERROR "projection fusion emitted a row mapping loop")
endif()

execute_process(
  COMMAND "${LANC}" "${FALLBACK_SOURCE}" -o "${FALLBACK_OUTPUT}"
  RESULT_VARIABLE fallback_result
  ERROR_VARIABLE fallback_error)
if(NOT fallback_result EQUAL 0)
  message(FATAL_ERROR "fallback compilation failed: ${fallback_error}")
endif()
file(READ "${FALLBACK_OUTPUT}" fallback_ir)
if(NOT fallback_ir MATCHES "call ptr @lan_table_from_rows\\(")
  message(FATAL_ERROR
          "a propagating expression was fused despite its observable error order")
endif()
