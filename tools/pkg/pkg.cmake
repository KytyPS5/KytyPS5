# PKG support is compiled into common; managed-reader publishing is optional.
# This keeps the existing native build usable without the .NET SDK.
option(KYTY_BUILD_PKG_HELPER "Build the separate .NET PKG reader" OFF)
set(KYTY_PKG_SOURCE "" CACHE PATH "Existing LibProsperoPKG source for offline helper builds")
set(_kyty_pkg_output "${PROJECT_BINARY_DIR}/pkg")
if(KYTY_BUILD_PKG_HELPER)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(KYTY_DOTNET dotnet REQUIRED)
    set(_kyty_pkg_source_args)
    if(KYTY_PKG_SOURCE)
        list(APPEND _kyty_pkg_source_args --source "${KYTY_PKG_SOURCE}")
    endif()
    add_custom_target(kyty_pkg_helper
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/pkg/setup.py"
            --output "${_kyty_pkg_output}" ${_kyty_pkg_source_args}
        BYPRODUCTS "${_kyty_pkg_output}/KytyPkgMount.dll"
            "${_kyty_pkg_output}/KytyPkgMount.deps.json"
            "${_kyty_pkg_output}/KytyPkgMount.runtimeconfig.json"
        COMMENT "Building read-only PKG helper"
        VERBATIM)
    add_dependencies(common kyty_pkg_helper)
endif()
set(_kyty_pkg_destination "pkg")
if(APPLE AND KYTY_BUILD_LAUNCHER)
    set(_kyty_pkg_destination "KytyPS5.app/Contents/MacOS/pkg")
endif()
# Also install a reader explicitly published to <build>/pkg without the option.
install(DIRECTORY "${_kyty_pkg_output}/" DESTINATION "${_kyty_pkg_destination}"
    OPTIONAL FILES_MATCHING PATTERN "KytyPkgMount.dll"
    PATTERN "KytyPkgMount.deps.json" PATTERN "KytyPkgMount.runtimeconfig.json")
