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

add_executable(htmlppt-watchdog src/app/main.cpp)
target_link_libraries(htmlppt-watchdog PRIVATE htmlppt_core)
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
endif()
