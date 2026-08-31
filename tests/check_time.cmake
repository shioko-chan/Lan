file(MAKE_DIRECTORY "${OUTPUT}")

execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/stdlib_time.lan"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status
  OUTPUT_VARIABLE actual ERROR_VARIABLE error)
set(expected "{added: \"1970-01-01T00:00:00.25Z\", between: 0.5, broadcast: [true, false], canonical: \"2000-01-01T00:00:00.1234Z\", comparisons: true, leap_rejected: true, negative_unix: true, parsed_invalid: true, sorted: [1969-12-31T23:59:59.75Z, 1970-01-01T00:00:00.25Z], unix: -0.25}\n")
if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
  message(FATAL_ERROR "time library result mismatch: ${actual}${error}")
endif()

set(fixed "2025-06-07T08:09:10.123456789+10:00")
execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/stdlib_time_now.lan"
  --fixed-time "${fixed}" WORKING_DIRECTORY "${OUTPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
set(expected "{first: \"2025-06-06T22:09:10.123456789Z\", same: true, seconds: 0}\n")
if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
  message(FATAL_ERROR "fixed clock failed: ${actual}${error}")
endif()

execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/stdlib_time_now.lan"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status
  OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT actual MATCHES
   [=[^\{first: "[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T.*Z", same: (true|false), seconds: [-0-9.e+]+\}]=])
  message(FATAL_ERROR "system realtime clock failed: ${actual}${error}")
endif()

execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/io_arguments.lan"
  --fixed-time "${fixed}" WORKING_DIRECTORY "${OUTPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "[]\n")
  message(FATAL_ERROR "runner fixed time leaked into arguments(): ${actual}${error}")
endif()

execute_process(COMMAND "${LANC}" --build "${ROOT}/tests/stdlib_time_unused.lan"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "building unused now action failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env LAN_INTERNAL_FIXED_TIME=invalid
  "${OUTPUT}/.lan/stdlib_time_unused/stdlib_time_unused"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status
  OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "42\n")
  message(FATAL_ERROR "unused now action observed the clock: ${actual}${error}")
endif()

execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/stdlib_time_now.lan"
  --fixed-time invalid WORKING_DIRECTORY "${OUTPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "固定时间")
  message(FATAL_ERROR "invalid fixed time was accepted: ${actual}${error}")
endif()

execute_process(COMMAND "${LANC}" --build "${ROOT}/tests/stdlib_time_now.lan"
  --fixed-time "${fixed}" WORKING_DIRECTORY "${OUTPUT}"
  RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 2 OR NOT error MATCHES "只能用于 --run")
  message(FATAL_ERROR "--build accepted --fixed-time: ${error}")
endif()
