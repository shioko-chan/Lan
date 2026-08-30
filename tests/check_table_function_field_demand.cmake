execute_process(
  COMMAND "${LANC}" --run "${SOURCE}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "cross-function Table program failed: ${run_error}")
endif()
if(NOT run_output MATCHES
   "adults: \\[true, false\\].*names: \\[\"Alice\\?\", \"Bob\\?\"\\]")
  message(FATAL_ERROR "unexpected cross-function output: ${run_output}")
endif()

execute_process(
  COMMAND "${LANC}" "${SOURCE}" -o "${OUTPUT}"
  RESULT_VARIABLE compile_result
  ERROR_VARIABLE compile_error)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR "Lan compilation failed: ${compile_error}")
endif()
file(READ "${OUTPUT}" ir)
if(ir MATCHES "call ptr @lan_table_row\\(")
  message(FATAL_ERROR "user function materialized a complete logical Table row")
endif()
string(REGEX MATCHALL "call i32 @lan_table_cell\\(" cell_calls "${ir}")
list(LENGTH cell_calls cell_count)
if(cell_count LESS 2)
  message(FATAL_ERROR "user functions did not read demanded fields directly")
endif()
