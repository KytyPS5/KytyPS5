set(KYTY_GIT_VERSION "unknown")
set(KYTY_GIT_HASH "unknown")
set(KYTY_GIT_REVISION "unknown")
if(GIT_EXECUTABLE)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
		WORKING_DIRECTORY "${GIT_WORKING_DIRECTORY}"
		OUTPUT_VARIABLE KYTY_GIT_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE GIT_RESULT
		ERROR_QUIET
	)
	if(NOT GIT_RESULT EQUAL 0)
		set(KYTY_GIT_VERSION "unknown")
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
		WORKING_DIRECTORY "${GIT_WORKING_DIRECTORY}"
		OUTPUT_VARIABLE KYTY_GIT_REVISION
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE GIT_HASH_RESULT
		ERROR_QUIET
	)
	if(NOT GIT_HASH_RESULT EQUAL 0)
		set(KYTY_GIT_HASH "unknown")
		set(KYTY_GIT_REVISION "unknown")
	else()
		string(SUBSTRING "${KYTY_GIT_REVISION}" 0 7 KYTY_GIT_HASH)
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" diff-index --quiet HEAD --
			WORKING_DIRECTORY "${GIT_WORKING_DIRECTORY}"
			RESULT_VARIABLE GIT_DIRTY_RESULT
			ERROR_QUIET
		)
		if(NOT GIT_DIRTY_RESULT EQUAL 0)
			string(APPEND KYTY_GIT_HASH "-dirty")
		endif()
	endif()
endif()
# Hash of everything that can change what the shader recompiler produces. It keys the on-disk
# recompiler cache, so editing the recompiler invalidates it while a new commit elsewhere does not.
file(GLOB_RECURSE RECOMPILER_SOURCES
	"${GIT_WORKING_DIRECTORY}/src/graphics/shader/*.h"
	"${GIT_WORKING_DIRECTORY}/src/graphics/shader/*.cpp"
	"${GIT_WORKING_DIRECTORY}/src/graphics/shader/*.inc"
	"${GIT_WORKING_DIRECTORY}/src/graphics/guest_gpu/gpu_format.h"
	"${GIT_WORKING_DIRECTORY}/src/graphics/guest_gpu/gpu_format.cpp"
	"${GIT_WORKING_DIRECTORY}/src/graphics/guest_gpu/gpu_defs.h"
)
list(SORT RECOMPILER_SOURCES)
set(RECOMPILER_COMBINED_HASH "")
foreach(src ${RECOMPILER_SOURCES})
	file(SHA256 "${src}" SRC_HASH)
	string(APPEND RECOMPILER_COMBINED_HASH "${SRC_HASH}")
endforeach()
if(RECOMPILER_COMBINED_HASH STREQUAL "")
	set(KYTY_RECOMPILER_SIGNATURE "none")
else()
	string(SHA256 KYTY_RECOMPILER_SIGNATURE "${RECOMPILER_COMBINED_HASH}")
endif()

configure_file("${INPUT_FILE}" "${OUTPUT_FILE}")
