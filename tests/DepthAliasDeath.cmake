execute_process(
    COMMAND "${TEST_EXECUTABLE}" --depth-alias-death "${ALIAS_CASE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 60)
if(WIN32)
    set(expected_exit 321)
else()
    # POSIX process status preserves only the low eight bits of _Exit(321).
    set(expected_exit 65)
endif()
if(ALIAS_CASE STREQUAL "cross-format")
    set(diagnostic "unsupported cross-format multisample depth copy")
else()
    set(diagnostic "unsupported unequal-sample depth overlap copy")
endif()
set(log "${output}\n${errors}")
if(NOT "${result}" STREQUAL "${expected_exit}" OR
   NOT log MATCHES "unsupported depth alias reached: ${ALIAS_CASE}" OR
   NOT log MATCHES "${diagnostic}")
    message(FATAL_ERROR "Depth alias did not fail at the expected guard (exit ${result}):\n${log}")
endif()
