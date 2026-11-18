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

# A supervisor without a config must be rejected by the CLI, never started: the
# timeout keeps a regression that starts one from hanging the suite.
execute_process(
  COMMAND "${EXE}" --supervisor
  TIMEOUT 10
  ERROR_VARIABLE supervisor_err
  RESULT_VARIABLE supervisor_rc)
if (NOT supervisor_rc EQUAL 2)
  message(FATAL_ERROR "--supervisor without --config: rc=${supervisor_rc}")
endif()
if (NOT supervisor_err MATCHES "config is required")
  message(FATAL_ERROR "--supervisor without --config: ${supervisor_err}")
endif()

# --reload is the editor's hot-reload request: it needs a run's state directory
# but no config, prints the confirmation, and creates the empty request file the
# running supervisor polls. The directory is created when missing.
set(reload_dir "${CMAKE_CURRENT_BINARY_DIR}/reload-request-test")
file(REMOVE_RECURSE "${reload_dir}")
expect_result(reload_request 0 "reload requested"
  --reload --state-dir "${reload_dir}" --run-id ui-contract)
if (NOT EXISTS "${reload_dir}/reload.request")
  message(FATAL_ERROR "--reload did not create reload.request")
endif()
file(SIZE "${reload_dir}/reload.request" reload_size)
if (NOT reload_size EQUAL 0)
  message(FATAL_ERROR "--reload request file is not empty")
endif()
file(REMOVE_RECURSE "${reload_dir}")

# --reload names the state directory explicitly: without one there is no file
# to write, so the CLI must refuse instead of guessing a location.
execute_process(
  COMMAND "${EXE}" --reload --run-id ui-contract
  RESULT_VARIABLE reload_missing_state_rc
  ERROR_VARIABLE reload_missing_state_err)
if (NOT reload_missing_state_rc EQUAL 2)
  message(FATAL_ERROR
    "--reload without --state-dir: rc=${reload_missing_state_rc}")
endif()
if (NOT reload_missing_state_err MATCHES "state-dir")
  message(FATAL_ERROR
    "--reload without --state-dir: ${reload_missing_state_err}")
endif()
