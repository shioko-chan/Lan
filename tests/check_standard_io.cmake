file(MAKE_DIRECTORY "${OUTPUT}")
file(WRITE "${OUTPUT}/input.txt" "你好")
file(WRITE "${OUTPUT}/empty.txt" "")

function(run_program name input expected)
  execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/${name}.lan" ${ARGN}
    INPUT_FILE "${input}" WORKING_DIRECTORY "${OUTPUT}" TIMEOUT 20
    RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
  if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
    message(FATAL_ERROR "${name} failed (${status}): ${actual}${error}; expected: ${expected}")
  endif()
endfunction()

execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/io_standard.lan"
  -- "a b" "" "--run" "$HOME" "'quoted'"
  INPUT_FILE "${OUTPUT}/input.txt" WORKING_DIRECTORY "${OUTPUT}" TIMEOUT 20
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
set(expected "你好你好\n{args: [\"a b\", \"\", \"--run\", \"$HOME\", \"'quoted'\"], input_length: 2}\n")
if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
  message(FATAL_ERROR "standard IO/argument forwarding failed: ${actual}${error}")
endif()
run_program(io_read_stdin "${OUTPUT}/empty.txt" "\n")
run_program(io_read_twice "${OUTPUT}/input.txt" "{first: \"你好\", second: \"\"}\n")
run_program(io_write_stdout "${OUTPUT}/empty.txt" "Hello\n()\n")
run_program(io_arguments "${OUTPUT}/empty.txt" "[]\n")

# --build must construct no actions; running its output accepts OS argv directly.
execute_process(COMMAND "${LANC}" --build "${ROOT}/tests/io_arguments.lan"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "build failed: ${error}")
endif()
execute_process(COMMAND "${OUTPUT}/.lan/io_arguments/io_arguments" "direct arg" "中文"
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "[\"direct arg\", \"中文\"]\n")
  message(FATAL_ERROR "native argv failed: ${actual}${error}")
endif()

string(ASCII 255 invalid)
file(WRITE "${OUTPUT}/invalid.txt" "${invalid}")
string(REPEAT "a" 8388608 limit)
file(WRITE "${OUTPUT}/limit.txt" "${limit}")
file(WRITE "${OUTPUT}/oversized.txt" "${limit}a")
unset(limit)
foreach(case invalid oversized)
  execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/io_read_stdin.lan"
    INPUT_FILE "${OUTPUT}/${case}.txt" WORKING_DIRECTORY "${OUTPUT}" TIMEOUT 20
    RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
  if(case STREQUAL "invalid")
    set(expected "UTF-8")
  else()
    set(expected "8 MiB")
  endif()
  if(status EQUAL 0 OR NOT actual STREQUAL "" OR
     NOT error MATCHES "read_stdin.*${expected}" OR
     NOT error MATCHES "位置：.*io_read_stdin.lan:" OR
     NOT error MATCHES "调用链：程序 -> read_stdin")
    message(FATAL_ERROR "${case}: missing structured stderr diagnostic: ${actual}${error}")
  endif()
endforeach()

# Constructing an unused read must not consume even invalid input.
run_program(io_unused_standard "${OUTPUT}/invalid.txt" "42\n")
execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/io_output_order_error.lan"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status
  OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT actual STREQUAL "before\n" OR NOT error MATCHES "除数不能为零")
  message(FATAL_ERROR "IO ordering or stderr separation failed: ${actual}${error}")
endif()

# The cap is inclusive; test the length without collecting 8 MiB of display output.
execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/io_stdin_length.lan"
  INPUT_FILE "${OUTPUT}/limit.txt" WORKING_DIRECTORY "${OUTPUT}" TIMEOUT 20
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "true\n")
  message(FATAL_ERROR "exact input limit failed: ${actual}${error}")
endif()

execute_process(COMMAND "${LANC}" --build "${ROOT}/tests/io_arguments.lan" -- arg
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 2 OR NOT error MATCHES "只能用于 --run")
  message(FATAL_ERROR "--build unexpectedly accepted program arguments: ${error}")
endif()
