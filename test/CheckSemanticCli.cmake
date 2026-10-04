execute_process(
  COMMAND "${HEIMDALL_EXECUTABLE}" check --semantic
          --compile-commands "${COMPILE_COMMANDS_FILE}" "${INPUT_FILE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)

if (NOT result EQUAL 1)
  message(FATAL_ERROR "expected heimdall check to exit 1; got ${result}\nstdout:\n${output}\nstderr:\n${error}")
endif ()

if (NOT output MATCHES "semantic/no-unused-local: local variable 'definitely_unused' is never used")
  message(FATAL_ERROR "expected semantic/no-unused-local diagnostic\nstdout:\n${output}\nstderr:\n${error}")
endif ()
