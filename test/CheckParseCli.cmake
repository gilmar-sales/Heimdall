# Validates `heimdall parse` and parser integration in `check`.
# Variables: HEIMDALL_EXECUTABLE, GOOD_FILE, BAD_FILE.

execute_process(
  COMMAND "${HEIMDALL_EXECUTABLE}" parse "${GOOD_FILE}"
  RESULT_VARIABLE good_result
  OUTPUT_VARIABLE good_output
  ERROR_VARIABLE good_error
)
if (NOT good_result EQUAL 0)
  message(FATAL_ERROR "expected heimdall parse on valid file to exit 0; got ${good_result}\nstdout:\n${good_output}\nstderr:\n${good_error}")
endif ()
if (NOT good_output MATCHES "0 syntax errors")
  message(FATAL_ERROR "expected clean parse summary\nstdout:\n${good_output}\nstderr:\n${good_error}")
endif ()

execute_process(
  COMMAND "${HEIMDALL_EXECUTABLE}" parse "${BAD_FILE}"
  RESULT_VARIABLE bad_result
  OUTPUT_VARIABLE bad_output
  ERROR_VARIABLE bad_error
)
if (NOT bad_result EQUAL 1)
  message(FATAL_ERROR "expected heimdall parse on broken file to exit 1; got ${bad_result}\nstdout:\n${bad_output}\nstderr:\n${bad_error}")
endif ()
if (NOT bad_output MATCHES "syntax/parse-error")
  message(FATAL_ERROR "expected syntax/parse-error diagnostic\nstdout:\n${bad_output}\nstderr:\n${bad_error}")
endif ()

execute_process(
  COMMAND "${HEIMDALL_EXECUTABLE}" parse --json --std c++23 "${BAD_FILE}"
  RESULT_VARIABLE json_result
  OUTPUT_VARIABLE json_output
  ERROR_VARIABLE json_error
)
if (NOT json_result EQUAL 1)
  message(FATAL_ERROR "expected heimdall parse --json on broken file to exit 1; got ${json_result}\nstdout:\n${json_output}\nstderr:\n${json_error}")
endif ()
if (NOT json_output MATCHES "\"standard\":\"c\\+\\+23\"")
  message(FATAL_ERROR "expected c++23 standard in JSON output\nstdout:\n${json_output}\nstderr:\n${json_error}")
endif ()
if (NOT json_output MATCHES "\"kind\":\"FunctionDefinition\"")
  message(FATAL_ERROR "expected FunctionDefinition node in JSON output\nstdout:\n${json_output}\nstderr:\n${json_error}")
endif ()

execute_process(
  COMMAND "${HEIMDALL_EXECUTABLE}" check "${BAD_FILE}"
  RESULT_VARIABLE check_result
  OUTPUT_VARIABLE check_output
  ERROR_VARIABLE check_error
)
if (NOT check_result EQUAL 1)
  message(FATAL_ERROR "expected heimdall check on broken file to exit 1; got ${check_result}\nstdout:\n${check_output}\nstderr:\n${check_error}")
endif ()
if (NOT check_output MATCHES "syntax/parse-error")
  message(FATAL_ERROR "expected syntax/parse-error in check output\nstdout:\n${check_output}\nstderr:\n${check_error}")
endif ()
