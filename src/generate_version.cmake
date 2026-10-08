set(KYTY_GIT_VERSION "unknown")
set(KYTY_GIT_HASH "unknown")
set(KYTY_GIT_REVISION "unknown")
set(KYTY_GIT_WORKTREE_FINGERPRINT "unknown")
# Include native source/test/build inputs. Runtime notes and workflow documentation
# do not change compiled code and must not rewrite this shared header on every edit.
set(KYTY_BUILD_INPUT_PATHS
	CMakeLists.txt src tests 3rdparty cmake resources assets
	CMakePresets.json CMakeUserPresets.json Makefile
	vcpkg.json vcpkg-configuration.json .gitmodules .gitattributes
)
if(GIT_EXECUTABLE)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" describe --tags --always
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

		# The commit plus relevant tracked/untracked inputs identifies local code.
		# Keep documentation and generated/ignored artifacts out of the code identity.
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" diff --binary HEAD -- ${KYTY_BUILD_INPUT_PATHS}
			WORKING_DIRECTORY "${GIT_WORKING_DIRECTORY}"
			OUTPUT_VARIABLE GIT_TRACKED_DIFF
			RESULT_VARIABLE GIT_DIFF_RESULT
			ERROR_QUIET
		)
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" ls-files --others --exclude-standard -- ${KYTY_BUILD_INPUT_PATHS}
			WORKING_DIRECTORY "${GIT_WORKING_DIRECTORY}"
			OUTPUT_VARIABLE GIT_UNTRACKED_FILES
			OUTPUT_STRIP_TRAILING_WHITESPACE
			RESULT_VARIABLE GIT_UNTRACKED_RESULT
			ERROR_QUIET
		)
		if(GIT_DIFF_RESULT EQUAL 0 AND GIT_UNTRACKED_RESULT EQUAL 0)
			set(GIT_WORKTREE_MATERIAL "revision:${KYTY_GIT_REVISION}\ntracked:\n${GIT_TRACKED_DIFF}\nuntracked:\n")
			if(NOT GIT_UNTRACKED_FILES STREQUAL "")
				string(REPLACE "\r\n" "\n" GIT_UNTRACKED_FILES "${GIT_UNTRACKED_FILES}")
				string(REPLACE "\n" ";" GIT_UNTRACKED_LIST "${GIT_UNTRACKED_FILES}")
				list(SORT GIT_UNTRACKED_LIST)
				foreach(GIT_UNTRACKED_FILE IN LISTS GIT_UNTRACKED_LIST)
					if(NOT GIT_UNTRACKED_FILE STREQUAL "" AND
					   EXISTS "${GIT_WORKING_DIRECTORY}/${GIT_UNTRACKED_FILE}" AND
					   NOT IS_DIRECTORY "${GIT_WORKING_DIRECTORY}/${GIT_UNTRACKED_FILE}")
						file(SHA256 "${GIT_WORKING_DIRECTORY}/${GIT_UNTRACKED_FILE}" GIT_UNTRACKED_HASH)
						string(APPEND GIT_WORKTREE_MATERIAL
						       "${GIT_UNTRACKED_FILE}:${GIT_UNTRACKED_HASH}\n")
					endif()
				endforeach()
			endif()
			string(SHA256 KYTY_GIT_WORKTREE_FINGERPRINT "${GIT_WORKTREE_MATERIAL}")
		endif()

		# The captured diff already covers staged and unstaged changes. Reuse it
		# instead of rescanning the whole input tree through diff-index.
		if(NOT GIT_DIFF_RESULT EQUAL 0 OR NOT GIT_UNTRACKED_RESULT EQUAL 0 OR
		   NOT GIT_TRACKED_DIFF STREQUAL "" OR NOT GIT_UNTRACKED_FILES STREQUAL "")
			string(APPEND KYTY_GIT_HASH "-dirty")
			if(NOT KYTY_GIT_VERSION STREQUAL "unknown")
				string(APPEND KYTY_GIT_VERSION "-dirty")
			endif()
		endif()
	endif()
endif()
configure_file("${INPUT_FILE}" "${OUTPUT_FILE}")
