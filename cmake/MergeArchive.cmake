execute_process(COMMAND "${ARCHIVER}" -M INPUT_FILE "${SCRIPT}"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot merge Regex runtime archives: ${result}")
endif()
