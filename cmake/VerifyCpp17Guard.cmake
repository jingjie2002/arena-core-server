if(NOT DEFINED ARENA_COMPILER OR NOT DEFINED ARENA_SOURCE_DIR OR NOT DEFINED ARENA_BINARY_DIR)
  message(FATAL_ERROR "VerifyCpp17Guard requires compiler, source and binary directories")
endif()

set(probe_object "${ARENA_BINARY_DIR}/cpp20_guard_probe.o")
execute_process(
  COMMAND "${ARENA_COMPILER}"
    -std=c++20
    -I "${ARENA_SOURCE_DIR}/include"
    -c "${ARENA_SOURCE_DIR}/tests/compile/cpp17_guard_probe.cpp"
    -o "${probe_object}"
  RESULT_VARIABLE probe_result
  OUTPUT_VARIABLE probe_stdout
  ERROR_VARIABLE probe_stderr
)

if(probe_result EQUAL 0)
  file(REMOVE "${probe_object}")
  message(FATAL_ERROR "C++20 probe compiled successfully; the C++17 guard is ineffective")
endif()

set(probe_output "${probe_stdout}\n${probe_stderr}")
if(NOT probe_output MATCHES "requires exactly C\\+\\+17")
  message(FATAL_ERROR "C++20 probe failed for an unexpected reason:\n${probe_output}")
endif()

message(STATUS "C++20 compilation was rejected by the project language guard")
