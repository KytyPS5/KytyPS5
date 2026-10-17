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
configure_file("${INPUT_FILE}" "${OUTPUT_FILE}")

# Hash of the sources under directories of SHADER_CACHE_SOURCE_DIR (paths and contents) and of
# the toolchain.
function(kyty_source_hash out)
	set(digest "${SHADER_CACHE_TOOLCHAIN}\n")
	foreach(directory IN LISTS ARGN)
		file(GLOB_RECURSE sources LIST_DIRECTORIES false RELATIVE "${SHADER_CACHE_SOURCE_DIR}"
			"${SHADER_CACHE_SOURCE_DIR}/${directory}/*")
		list(SORT sources)
		foreach(source IN LISTS sources)
			file(SHA256 "${SHADER_CACHE_SOURCE_DIR}/${source}" source_hash)
			string(APPEND digest "${source} ${source_hash}\n")
		endforeach()
	endforeach()
	string(SHA256 hash "${digest}")
	string(SUBSTRING "${hash}" 0 16 hash)
	set(${out} "${hash}" PARENT_SCOPE)
endfunction()

if(SHADER_CACHE_SOURCE_DIR)
	# Recorded shader inputs come from code across the graphics tree: they need all of it unchanged.
	kyty_source_hash(KYTY_GRAPHICS_CACHE_VERSION graphics)
	configure_file("${SHADER_CACHE_INPUT_FILE}" "${SHADER_CACHE_OUTPUT_FILE}")
endif()
