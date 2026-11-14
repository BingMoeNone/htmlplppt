if(NOT DEFINED EXE OR NOT DEFINED FIXTURES)
  message(FATAL_ERROR "EXE and FIXTURES are required")
endif()

function(expect_result NAME EXPECTED_EXIT EXPECTED_TEXT)
  execute_process(
    COMMAND "${EXE}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL EXPECTED_EXIT)
    message(FATAL_ERROR
      "${NAME}: expected exit ${EXPECTED_EXIT}, got '${result}'\n${output}${error}")
  endif()
  string(FIND "${output}${error}" "${EXPECTED_TEXT}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "${NAME}: expected output to contain '${EXPECTED_TEXT}'\n${output}${error}")
  endif()
endfunction()

expect_result(print_config 0 "\"version\": 1"
  --print-config --config "${FIXTURES}/watcher.json")
expect_result(map_append 0 "\"slide\": 3"
  --print-config --config "${FIXTURES}/watcher.json" --map 3=page2.html)
expect_result(unknown_option 2 "unknown option" --bogus)
expect_result(invalid_stage_mode 2 "must be file, tree, or direct"
  --validate --config "${FIXTURES}/watcher.json"
  --deck-info "${FIXTURES}/deck-info.json" --stage-mode bogus)
expect_result(invalid_scheduling 2 "must be adaptive, aggressive, balanced, or safe"
  --validate --config "${FIXTURES}/watcher.json"
  --deck-info "${FIXTURES}/deck-info.json" --scheduling turbo)
expect_result(resolve_ok 0 "page2.html"
  --resolve --config "${FIXTURES}/watcher.json"
  --deck-info "${FIXTURES}/deck-info.json")
expect_result(validate_missing_html 1 "HTML file does not exist"
  --validate --config "${FIXTURES}/watcher-missing-html.json"
  --deck-info "${FIXTURES}/deck-info.json")
expect_result(map_duplicate 1 "already mapped by target"
  --resolve --config "${FIXTURES}/watcher.json"
  --deck-info "${FIXTURES}/deck-info.json" --map 1=page2.html)
