execute_process(COMMAND "${LANC}" "${SOURCE}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${EXPECTED}")
  message(FATAL_ERROR "expected compilation rejection '${EXPECTED}': ${output}${error}")
endif()
