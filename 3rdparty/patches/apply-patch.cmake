# Applies PATCH_FILE to the current directory, skipping it when it is already applied,
# so a FetchContent patch step can run again on an already-patched tree.
# Usage: cmake -DGIT_EXECUTABLE=<git> -DPATCH_FILE=<file> -P apply-patch.cmake

execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${PATCH_FILE}"
	RESULT_VARIABLE kyty_patch_applied
	OUTPUT_QUIET ERROR_QUIET
)
if(kyty_patch_applied EQUAL 0)
	message(STATUS "Patch already applied: ${PATCH_FILE}")
	return()
endif()

execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH_FILE}"
	RESULT_VARIABLE kyty_patch_result
)
if(NOT kyty_patch_result EQUAL 0)
	message(FATAL_ERROR "Failed to apply patch: ${PATCH_FILE}")
endif()
