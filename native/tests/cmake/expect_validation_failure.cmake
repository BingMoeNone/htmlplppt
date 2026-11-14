if(NOT DEFINED EXE OR NOT DEFINED CONFIG OR NOT DEFINED DECK_INFO OR
   NOT DEFINED EXPECT)
  message(FATAL_ERROR "EXE, CONFIG, DECK_INFO, and EXPECT are required")
endif()

execute_process(
  COMMAND "${EXE}" --validate --config "${CONFIG}" --deck-info "${DECK_INFO}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)

if(NOT result EQUAL 1)
  message(FATAL_ERROR "expected exit code 1, got '${result}'\n${output}${error}")
endif()

string(FIND "${output}${error}" "${EXPECT}" found)
if(found EQUAL -1)
  message(FATAL_ERROR "expected output to contain '${EXPECT}'\n${output}${error}")
endif()
