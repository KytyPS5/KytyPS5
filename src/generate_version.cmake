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

set(PER_VERTEX_SOURCES
	"${GIT_WORKING_DIRECTORY}/src/graphics/host_gpu/renderer/perVertexTransform.h"
	"${GIT_WORKING_DIRECTORY}/src/graphics/host_gpu/renderer/perVertexTransform.cpp"
	"${GIT_WORKING_DIRECTORY}/src/graphics/host_gpu/renderer/perVertexEmbeddedSpv.h"
	"${GIT_WORKING_DIRECTORY}/src/graphics/host_gpu/renderer/perVertexPrototype.h"
	"${GIT_WORKING_DIRECTORY}/src/graphics/host_gpu/renderer/perVertexPrototype.cpp"
)
set(PER_VERTEX_COMBINED_HASH "")
foreach(src ${PER_VERTEX_SOURCES})
	if(EXISTS "${src}")
		file(SHA256 "${src}" SRC_HASH)
		string(APPEND PER_VERTEX_COMBINED_HASH "${SRC_HASH}")
	endif()
endforeach()
if(PER_VERTEX_COMBINED_HASH STREQUAL "")
	set(KYTY_PER_VERTEX_TRANSFORM_SIGNATURE "none")
else()
	string(SHA256 KYTY_PER_VERTEX_TRANSFORM_SIGNATURE "${PER_VERTEX_COMBINED_HASH}")
endif()

configure_file("${INPUT_FILE}" "${OUTPUT_FILE}")
