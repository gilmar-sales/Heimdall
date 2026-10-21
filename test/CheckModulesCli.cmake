# Validates C++20 module support in `heimdall check`: structural errors inside one unit and
# the diagnostics that span files (cycles, duplicate interfaces, unexported partitions).
# Variables: HEIMDALL_EXECUTABLE, FIXTURES.

function(run_check expected_exit expected_pattern forbidden_pattern)
  execute_process(
    COMMAND "${HEIMDALL_EXECUTABLE}" check ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if (NOT result EQUAL ${expected_exit})
    message(FATAL_ERROR "expected exit ${expected_exit} for ${ARGN}; got ${result}\nstdout:\n${output}\nstderr:\n${error}")
  endif ()
  if (NOT "${expected_pattern}" STREQUAL "" AND NOT output MATCHES "${expected_pattern}")
    message(FATAL_ERROR "expected '${expected_pattern}' for ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
  endif ()
  if (NOT "${forbidden_pattern}" STREQUAL "" AND output MATCHES "${forbidden_pattern}")
    message(FATAL_ERROR "did not expect '${forbidden_pattern}' for ${ARGN}\nstdout:\n${output}\nstderr:\n${error}")
  endif ()
endfunction()

run_check(1 "cpp/module-import-cycle" "" "${FIXTURES}/cycle_a.cppm" "${FIXTURES}/cycle_b.cppm")
run_check(1 "cpp/module-duplicate-interface" "" "${FIXTURES}/duplicate_1.cppm" "${FIXTURES}/duplicate_2.cppm")
run_check(1 "cpp/module-partition-export" "" "${FIXTURES}/partition_primary.cppm" "${FIXTURES}/partition_hidden.cppm")
run_check(1 "cpp/module-structure" "" "${FIXTURES}/bad_structure.cpp")

# Cross-file rules need every file in the run: a lone file never claims a cycle.
run_check(0 "" "cpp/module-import-cycle" "${FIXTURES}/cycle_a.cppm")
run_check(0 "" "cpp/module-" "${FIXTURES}/ok_base.cppm" "${FIXTURES}/ok_top.cppm")
