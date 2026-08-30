execute_process(
  COMMAND "${LANC}" "${SOURCE}" -o "${OUTPUT}"
  RESULT_VARIABLE result
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Lan compilation failed: ${error}")
endif()

file(READ "${OUTPUT}" ir)
string(REGEX MATCHALL "call ptr @lan_table_row\\(" row_calls "${ir}")
list(LENGTH row_calls row_count)
if(NOT row_count EQUAL 1)
  message(FATAL_ERROR
          "expected only find's matched branch to materialize a row, got ${row_count}")
endif()

string(REGEX MATCHALL "call i32 @lan_table_cell\\(" cell_calls "${ir}")
list(LENGTH cell_calls cell_count)
if(cell_count LESS 2)
  message(FATAL_ERROR
          "expected filter/find predicates to read columns directly, got ${cell_count}")
endif()
