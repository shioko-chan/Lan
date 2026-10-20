file(MAKE_DIRECTORY "${OUTPUT}")
file(WRITE "${OUTPUT}/syntax.lan" "if true:\n  1\n")
file(WRITE "${OUTPUT}/name.lan" "unknown_name\n")
file(WRITE "${OUTPUT}/type.lan" "1 + \"text\"\n")
file(WRITE "${OUTPUT}/option.lan" "missing + 1\n")

function(check_error source language expected)
  execute_process(COMMAND "${LANC}" --diagnostic-language "${language}" "${OUTPUT}/${source}.lan"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status)
  if(status EQUAL 0 OR NOT error MATCHES "${expected}")
    message(FATAL_ERROR "${source} (${language}): expected ${expected}; status=${status}; stderr=${error}")
  endif()
endfunction()

check_error(syntax en "Line 1: syntax error")
check_error(syntax zh-CN "第 1 行：无法理解这里的写法")
check_error(name en "Unknown name unknown_name")
check_error(name zh-CN "找不到名称“unknown_name”")
check_error(type en "Type mismatch|requires Number")
check_error(option en "Option type error")

execute_process(COMMAND "${LANC}" --diagnostic-language en
    "${ROOT}/tests/indent_error.lan"
  ERROR_VARIABLE indentation_error RESULT_VARIABLE indentation_status)
if(indentation_status EQUAL 0 OR NOT indentation_error MATCHES "invalid indentation")
  message(FATAL_ERROR "lexical indentation diagnostic was not localized: ${indentation_error}")
endif()

execute_process(COMMAND "${LANC}" --diagnostic-language en
    "${ROOT}/tests/list_function_type_error.lan"
  ERROR_VARIABLE context_error RESULT_VARIABLE context_status)
if(context_status EQUAL 0 OR NOT context_error MATCHES
    "argument items passed to function size List element")
  message(FATAL_ERROR "nested type context was not localized: ${context_error}")
endif()

execute_process(COMMAND "${LANC}" --diagnostic-language en
    --diagnostic-language fr "${OUTPUT}/name.lan"
  ERROR_VARIABLE invalid_error RESULT_VARIABLE invalid_status)
if(NOT invalid_status EQUAL 2 OR NOT invalid_error MATCHES "accepts only zh-CN or en")
  message(FATAL_ERROR "invalid language option was not rejected: ${invalid_error}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env LAN_DIAGNOSTIC_LANGUAGE=en
    "${LANC}" "${OUTPUT}/name.lan"
  ERROR_VARIABLE environment_error RESULT_VARIABLE environment_status)
if(environment_status EQUAL 0 OR NOT environment_error MATCHES "Unknown name")
  message(FATAL_ERROR "environment selection failed: ${environment_error}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env LAN_DIAGNOSTIC_LANGUAGE=en
    "${LANC}" --diagnostic-language zh-CN "${OUTPUT}/name.lan"
  ERROR_VARIABLE override_error RESULT_VARIABLE override_status)
if(override_status EQUAL 0 OR NOT override_error MATCHES "找不到名称")
  message(FATAL_ERROR "command-line override failed: ${override_error}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env LAN_DIAGNOSTIC_LANGUAGE=unrecognized
    "${LANC}" "${OUTPUT}/name.lan"
  ERROR_VARIABLE fallback_error RESULT_VARIABLE fallback_status)
if(fallback_status EQUAL 0 OR NOT fallback_error MATCHES "找不到名称")
  message(FATAL_ERROR "environment fallback failed: ${fallback_error}")
endif()
