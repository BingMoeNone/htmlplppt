cmake_minimum_required(VERSION 3.20)
project(htmlppt_watchdog VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HTMLPPT_BUILD_TESTS "Build unit tests" ON)
option(HTMLPPT_WERROR "Treat warnings as errors" OFF)

function(htmlppt_enable_warnings target)
  if (MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if (HTMLPPT_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if (HTMLPPT_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h"
  @ONLY)

file(GLOB HTMLPPT_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
add_library(htmlppt_core STATIC ${HTMLPPT_CORE_SOURCES})
target_include_directories(htmlppt_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_features(htmlppt_core PUBLIC cxx_std_20)
htmlppt_enable_warnings(htmlppt_core)

if (WIN32)
  file(GLOB HTMLPPT_WIN32_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/*.cpp")
  add_library(htmlppt_win32 STATIC ${HTMLPPT_WIN32_SOURCES})
  target_link_libraries(htmlppt_win32 PUBLIC htmlppt_core dbghelp ole32
    oleaut32 shell32)
  htmlppt_enable_warnings(htmlppt_win32)
endif()

add_executable(htmlppt-watchdog src/app/main.cpp src/app/cli.cpp
  src/app/supervisor.cpp)
target_link_libraries(htmlppt-watchdog PRIVATE htmlppt_core)
if (WIN32)
  target_link_libraries(htmlppt-watchdog PRIVATE htmlppt_win32)
endif()
htmlppt_enable_warnings(htmlppt-watchdog)

if (HTMLPPT_BUILD_TESTS)
  enable_testing()
  file(GLOB HTMLPPT_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")
  add_executable(htmlppt_core_tests ${HTMLPPT_TEST_SOURCES})
  target_include_directories(htmlppt_core_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(htmlppt_core_tests PRIVATE htmlppt_core)
  htmlppt_enable_warnings(htmlppt_core_tests)
  add_test(NAME core_tests COMMAND htmlppt_core_tests)
  set_tests_properties(core_tests PROPERTIES TIMEOUT 30)

  add_test(NAME cli_version COMMAND htmlppt-watchdog --version)
  set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "htmlppt-watchdog 0[.]1[.]0")
  add_test(NAME cli_help COMMAND htmlppt-watchdog --help)
  set_tests_properties(cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: htmlppt-watchdog")

  set(HTMLPPT_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME cli_print_schema COMMAND htmlppt-watchdog --print-schema)
  set_tests_properties(cli_print_schema PROPERTIES
    PASS_REGULAR_EXPRESSION "htmlppt watchdog config")

  add_test(NAME cli_validate_ok COMMAND htmlppt-watchdog --validate
    --config "${HTMLPPT_FIXTURES}/watcher.json"
    --deck-info "${HTMLPPT_FIXTURES}/deck-info.json")
  set_tests_properties(cli_validate_ok PROPERTIES
    PASS_REGULAR_EXPRESSION "OK: configuration is valid")

  add_test(NAME cli_resolve_ok COMMAND htmlppt-watchdog --resolve
    --config "${HTMLPPT_FIXTURES}/watcher.json"
    --deck-info "${HTMLPPT_FIXTURES}/deck-info.json")
  set_tests_properties(cli_resolve_ok PROPERTIES
    PASS_REGULAR_EXPRESSION "page2.html")

  add_test(NAME cli_validate_missing_html
    COMMAND ${CMAKE_COMMAND}
      "-DEXE=$<TARGET_FILE:htmlppt-watchdog>"
      "-DCONFIG=${HTMLPPT_FIXTURES}/watcher-missing-html.json"
      "-DDECK_INFO=${HTMLPPT_FIXTURES}/deck-info.json"
      "-DEXPECT=HTML file does not exist"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/expect_validation_failure.cmake")

  add_test(NAME cli_contract
    COMMAND ${CMAKE_COMMAND}
      "-DEXE=$<TARGET_FILE:htmlppt-watchdog>"
      "-DFIXTURES=${HTMLPPT_FIXTURES}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/cli_contract.cmake")

  if (WIN32)
    file(GLOB HTMLPPT_WIN32_TEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/win32/*.cpp")
    add_executable(htmlppt_win32_tests
      ${HTMLPPT_WIN32_TEST_SOURCES}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_main.cpp")
    target_include_directories(htmlppt_win32_tests PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(htmlppt_win32_tests PRIVATE htmlppt_win32)
    target_compile_definitions(htmlppt_win32_tests PRIVATE
      "HTMLPPT_EXE_PATH=\"$<TARGET_FILE:htmlppt-watchdog>\"")
    add_dependencies(htmlppt_win32_tests htmlppt-watchdog)
    htmlppt_enable_warnings(htmlppt_win32_tests)
    add_test(NAME win32_tests COMMAND htmlppt_win32_tests)
    set_tests_properties(win32_tests PROPERTIES TIMEOUT 120)
  endif()
endif()
