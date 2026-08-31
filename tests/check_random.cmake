file(MAKE_DIRECTORY "${OUTPUT}")

function(run_seeded source seed output error status)
  execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/${source}.lan"
    --random-seed "${seed}" WORKING_DIRECTORY "${OUTPUT}" TIMEOUT 30
    RESULT_VARIABLE result OUTPUT_VARIABLE text ERROR_VARIABLE diagnostic)
  set(${output} "${text}" PARENT_SCOPE)
  set(${error} "${diagnostic}" PARENT_SCOPE)
  set(${status} "${result}" PARENT_SCOPE)
endfunction()

run_seeded(stdlib_random 42 first error status)
run_seeded(stdlib_random 42 second error2 status2)
set(expected "{bounded: -6.83946, chosen: \"a\", first: 0.244911, fixed: 7, integer: -1, repeat_first: 0.539401, repeat_second: 0.939482, shuffled: [2, 3, 1, missing]}\n")
if(NOT status EQUAL 0 OR NOT status2 EQUAL 0 OR
   NOT first STREQUAL second OR NOT first STREQUAL expected)
  message(FATAL_ERROR "lan-pcg64-v1 sequence changed: ${first}${second}${error}${error2}")
endif()

run_seeded(stdlib_random_empty 42 empty_output error status)
run_seeded(stdlib_random_reference 42 reference error2 status2)
string(REGEX MATCH "sample: ([0-9.]+)" sample_match "${empty_output}")
set(sample "${CMAKE_MATCH_1}")
if(NOT status EQUAL 0 OR NOT status2 EQUAL 0 OR NOT sample_match OR
   NOT reference STREQUAL "${sample}\n")
  message(FATAL_ERROR "empty choose/shuffle consumed randomness: ${empty_output}${reference}${error}${error2}")
endif()

run_seeded(stdlib_random 43 different error status)
if(NOT status EQUAL 0 OR different STREQUAL first)
  message(FATAL_ERROR "different random seeds produced the same sequence")
endif()

execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/stdlib_random_reference.lan"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status
  OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT actual MATCHES "^[0-9.e+-]+\n$")
  message(FATAL_ERROR "default OS entropy initialization failed: ${actual}${error}")
endif()

execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/io_arguments.lan"
  --random-seed 42 WORKING_DIRECTORY "${OUTPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "[]\n")
  message(FATAL_ERROR "runner seed leaked into arguments(): ${actual}${error}")
endif()

foreach(seed "-1" "1.5" "9007199254740992")
  execute_process(COMMAND "${LANC}" --run "${ROOT}/tests/stdlib_random_reference.lan"
    --random-seed "${seed}" WORKING_DIRECTORY "${OUTPUT}"
    RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE error)
  if(NOT status EQUAL 2 OR NOT error MATCHES "随机种子")
    message(FATAL_ERROR "invalid seed '${seed}' was accepted: ${actual}${error}")
  endif()
endforeach()

execute_process(COMMAND "${LANC}" --build "${ROOT}/tests/stdlib_random_reference.lan"
  --random-seed 42 WORKING_DIRECTORY "${OUTPUT}"
  RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 2 OR NOT error MATCHES "只能用于 --run")
  message(FATAL_ERROR "--build accepted --random-seed: ${error}")
endif()

# Construction is pure: invalid lazy bounds and a bad hidden seed are irrelevant
# when none of the actions execute.
execute_process(COMMAND "${LANC}" --build "${ROOT}/tests/stdlib_random_unused.lan"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "building unused random actions failed: ${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env LAN_INTERNAL_RANDOM_SEED=invalid
  "${OUTPUT}/.lan/stdlib_random_unused/stdlib_random_unused"
  WORKING_DIRECTORY "${OUTPUT}" RESULT_VARIABLE status
  OUTPUT_VARIABLE actual ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR NOT actual STREQUAL "42\n")
  message(FATAL_ERROR "unused random action observed state: ${actual}${error}")
endif()
