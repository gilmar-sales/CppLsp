execute_process(
  COMMAND "${CPPLSP_EXECUTABLE}" check --semantic
          --compile-commands "${COMPILE_COMMANDS_FILE}" "${INPUT_FILE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)

if (NOT result EQUAL 1)
  message(FATAL_ERROR "expected cpplsp check to exit 1; got ${result}\nstdout:\n${output}\nstderr:\n${error}")
endif ()

if (NOT output MATCHES "CPPLSP101: local variable 'definitely_unused' is never used")
  message(FATAL_ERROR "expected CPPLSP101 diagnostic\nstdout:\n${output}\nstderr:\n${error}")
endif ()
