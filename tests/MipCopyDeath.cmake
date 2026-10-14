execute_process(
    COMMAND "${TEST_EXECUTABLE}" --mip-copy-death "${COPY_CASE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 60)
if(WIN32)
    set(expected_exit 321)
else()
    set(expected_exit 65)
endif()
set(log "${output}\n${errors}")
if(NOT "${result}" STREQUAL "${expected_exit}" OR
   NOT log MATCHES "invalid mip copy reached: ${COPY_CASE}" OR
   NOT log MATCHES "Image: mip copy exceeds subresource bounds")
    message(FATAL_ERROR "Mip copy did not fail at the expected guard (exit ${result}):\n${log}")
endif()
