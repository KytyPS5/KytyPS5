set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(cpuinfo)

include(FetchContent)

# The Bluetooth DualSense transports speaker audio as fixed-size Opus packets.
set(kyty_build_testing_saved "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
	# Opus assumes MSVC accepts SSE4.1 intrinsics without per-file flags, but
	# clang-cl rejects them in its baseline x64 files. Its AVX2 dispatch also
	# depends on the SSE4.1 declarations, so retain the x64 SSE2 baseline only.
	set(OPUS_X86_MAY_HAVE_SSE4_1 OFF CACHE BOOL "" FORCE)
	set(OPUS_X86_MAY_HAVE_AVX2 OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(opus
	GIT_REPOSITORY https://github.com/xiph/opus.git
	GIT_TAG ddbe48383984d56acd9e1ab6a090c54ca6b735a6
	GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(opus)
set(BUILD_TESTING "${kyty_build_testing_saved}")
install(FILES "${opus_SOURCE_DIR}/COPYING" DESTINATION "licenses/opus")

# Headsets are driven through OpenXR. Only the static loader is built; the runtime that serves it
# (for example SteamVR, Monado or Virtual Desktop) comes from the user's system.
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
	GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
	GIT_TAG f2448a8797c85814aa892efc1ab8707900fbcc78 # release-1.1.63
	GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(openxr)
# Keep the loader's headers and library out of the installed package.
set_property(DIRECTORY "${openxr_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
# The loader uses exceptions internally.
if(NOT KYTY_CLANG_CL)
	target_compile_options(openxr_loader PRIVATE -fexceptions)
endif()
install(FILES "${openxr_SOURCE_DIR}/LICENSE" DESTINATION "licenses/openxr")

# Noto Sans (SIL Open Font License 1.1), which libFont uses in place of the console's system fonts.
# Only the hinted Regular, Bold, Italic and Bold Italic of release v2.015 are needed, so they are
# downloaded individually from the Noto project's font distribution (notofonts.github.io) rather
# than with the release's 117 MB archive, along with the distribution's licence.
set(KYTY_NOTO_SANS_REPOSITORY "https://raw.githubusercontent.com/notofonts/notofonts.github.io")
set(KYTY_NOTO_SANS_COMMIT 42af67ccd12bc0e31ed85384bf73e39248265374)
set(KYTY_NOTO_SANS_BASE_URL "${KYTY_NOTO_SANS_REPOSITORY}/${KYTY_NOTO_SANS_COMMIT}/fonts")

# Checksums of the files at that commit.
set(KYTY_NOTO_SANS_LICENSE_SHA256 f2095b08bed08b23a6fe26112fcd679a2bee3f002eef077eb05d215ed1051bd8)
set(KYTY_NOTO_SANS_REGULAR_SHA256 478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823)
set(KYTY_NOTO_SANS_BOLD_SHA256 1df075a380fc7cb898acf64c1f7b3b4dd780de3caa860178bf929de35817a913)
set(KYTY_NOTO_SANS_ITALIC_SHA256 467e3f89eeca4108bb8710a2b9e0cf2281ac56d5b0609211a83776d0505eecb5)
set(KYTY_NOTO_SANS_BOLD_ITALIC_SHA256
	1b602a9d6353be42c91df097a4857b69fa2696f26703d7a33b54a15d87c2622c)

# All the files go to one folder. Sharing it is safe only because of DOWNLOAD_NO_EXTRACT: an extract
# step empties SOURCE_DIR first, which would delete the other files.
set(KYTY_NOTO_SANS_DIR "${FETCHCONTENT_BASE_DIR}/noto-sans")

# Downloads the file at `path` (relative to KYTY_NOTO_SANS_BASE_URL) as the dependency `name`.
function(kyty_fetch_noto_sans name path sha256)
	FetchContent_Declare(${name}
		URL "${KYTY_NOTO_SANS_BASE_URL}/${path}"
		URL_HASH SHA256=${sha256}
		DOWNLOAD_NO_EXTRACT TRUE
		SOURCE_DIR "${KYTY_NOTO_SANS_DIR}"
		TLS_VERIFY TRUE)
	FetchContent_MakeAvailable(${name})
endfunction()

kyty_fetch_noto_sans(noto_sans_license LICENSE ${KYTY_NOTO_SANS_LICENSE_SHA256})
kyty_fetch_noto_sans(noto_sans_regular NotoSans/hinted/ttf/NotoSans-Regular.ttf
	${KYTY_NOTO_SANS_REGULAR_SHA256})
kyty_fetch_noto_sans(noto_sans_bold NotoSans/hinted/ttf/NotoSans-Bold.ttf
	${KYTY_NOTO_SANS_BOLD_SHA256})
kyty_fetch_noto_sans(noto_sans_italic NotoSans/hinted/ttf/NotoSans-Italic.ttf
	${KYTY_NOTO_SANS_ITALIC_SHA256})
kyty_fetch_noto_sans(noto_sans_bold_italic NotoSans/hinted/ttf/NotoSans-BoldItalic.ttf
	${KYTY_NOTO_SANS_BOLD_ITALIC_SHA256})

set(KYTY_NOTO_SANS_DIR "${KYTY_NOTO_SANS_DIR}" PARENT_SCOPE)

# The guest instruction patcher needs a decoder/encoder and a small
# trampoline assembler. Pin the same revisions used by shadPS4's implementation.
set(XBYAK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	xbyak
	GIT_REPOSITORY https://github.com/herumi/xbyak.git
	GIT_TAG 44a72f369268f7d552650891b296693e91db86bb
	GIT_SHALLOW FALSE
)

set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	zydis
	GIT_REPOSITORY https://github.com/zyantific/zydis.git
	GIT_TAG 120e0e705f8e3b507dc49377ac2879979f0d545c
	GIT_SUBMODULES_RECURSE TRUE
	GIT_SHALLOW FALSE
)

FetchContent_MakeAvailable(xbyak zydis)
target_compile_definitions(xbyak INTERFACE XBYAK_NO_EXCEPTION)

# ZArchive provides random-access reads for compressed game dumps. Build only
# the reader/writer library and its zstd dependency; the command-line tool is
# not part of KytyPS5's runtime distribution.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	zstd
	GIT_REPOSITORY https://github.com/facebook/zstd.git
	GIT_TAG f8745da6ff1ad1e7bab384bd1f9d742439278e99
	GIT_SHALLOW FALSE
	SOURCE_SUBDIR build/cmake
)
FetchContent_Declare(
	zarchive_source
	GIT_REPOSITORY https://github.com/Exzap/ZArchive.git
	GIT_TAG d2c717730092c7bf8cbb033b12fd4001b7c4d932
	GIT_SHALLOW FALSE
	PATCH_COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace
		"${CMAKE_CURRENT_SOURCE_DIR}/patches/zarchive-reader.patch"
	SOURCE_SUBDIR kyty-library-only
)
FetchContent_MakeAvailable(zstd zarchive_source)

add_library(zarchive STATIC
	"${zarchive_source_SOURCE_DIR}/src/zarchivereader.cpp"
	"${zarchive_source_SOURCE_DIR}/src/zarchivewriter.cpp"
	"${zarchive_source_SOURCE_DIR}/src/sha_256.c"
)
add_library(ZArchive::zarchive ALIAS zarchive)
target_include_directories(zarchive SYSTEM PUBLIC "${zarchive_source_SOURCE_DIR}/include")
target_include_directories(zarchive PRIVATE "${zstd_SOURCE_DIR}/lib")
target_link_libraries(zarchive PRIVATE libzstd_static)
target_compile_features(zarchive PRIVATE cxx_std_20)
if(MSVC)
	target_compile_options(zarchive PRIVATE /W0)
else()
	target_compile_options(zarchive PRIVATE -w)
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
if(APPLE)
	# SDL's built-in iconv fallback does not support CP932.
	set(SDL_SYSTEM_ICONV ON CACHE BOOL "Use system iconv for CP932 conversion" FORCE)
endif()
add_subdirectory(SDL3)

add_library(Vulkan-Headers INTERFACE)
add_library(Vulkan::Headers ALIAS Vulkan-Headers)
target_include_directories(Vulkan-Headers INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers/include"
)

add_library(imgui STATIC
	imgui/imgui.cpp
	imgui/imgui_draw.cpp
	imgui/imgui_tables.cpp
	imgui/imgui_widgets.cpp
	imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/imgui"
	"${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
)
target_compile_definitions(imgui PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_link_libraries(imgui PUBLIC Vulkan::Headers)
if(MSVC)
	target_compile_options(imgui PRIVATE /W0)
else()
	target_compile_options(imgui PRIVATE -w)
endif()

set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SPIRV-Headers_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SPIRV-Headers" CACHE PATH "" FORCE)
add_subdirectory(SPIRV-Tools)

add_library(spirv-tools ALIAS SPIRV-Tools-static)
add_library(spirv-tools-opt ALIAS SPIRV-Tools-opt)

add_subdirectory(xxHash/build/cmake)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_MODULE OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(fmt EXCLUDE_FROM_ALL)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
add_subdirectory(nlohmann_json EXCLUDE_FROM_ALL)

set(SPDLOG_BUILD_ALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_WARNINGS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
if(WIN32)
	set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(spdlog EXCLUDE_FROM_ALL)

if (NOT TARGET FFmpeg::ffmpeg)
	# On macOS the target arch is driven by CMAKE_OSX_ARCHITECTURES, not the host
	# CMAKE_SYSTEM_PROCESSOR (which reports arm64 even for an x86_64/Rosetta build).
	if(APPLE AND CMAKE_OSX_ARCHITECTURES)
		if(CMAKE_OSX_ARCHITECTURES MATCHES "^(x86_64)$")
			set(ARCHITECTURE x86_64)
		elseif(CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64)$")
			set(ARCHITECTURE arm64)
		else()
			set(ARCHITECTURE x86_64 arm64) # universal
		endif()
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
		set(ARCHITECTURE arm64)
	else()
		set(ARCHITECTURE x86_64)
	endif()
	add_subdirectory(ffmpeg-core)
	add_library(FFmpeg::ffmpeg ALIAS ffmpeg)
endif()

file(GLOB LIBATRAC9_SOURCES LibAtrac9/C/src/*.c)
add_library(LibAtrac9 STATIC ${LIBATRAC9_SOURCES})
target_include_directories(LibAtrac9 PUBLIC LibAtrac9/C/src)
if(MSVC)
	target_compile_options(LibAtrac9 PRIVATE /W0)
endif()

set(TRACY_STATIC ON CACHE BOOL "" FORCE)
set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_DELAYED_INIT ON CACHE BOOL "" FORCE)
set(TRACY_MANUAL_LIFETIME ON CACHE BOOL "" FORCE)
add_subdirectory(tracy EXCLUDE_FROM_ALL)
if (TARGET TracyClient)
	if (MSVC)
		target_compile_options(TracyClient PRIVATE /WX-)
	endif()
	if (CLANG)
		target_compile_options(TracyClient PRIVATE
			-Wno-error
			-Wno-braced-scalar-init
			-Wno-format
			-Wno-unused-variable
		)
	endif()
endif()

if (CLANG AND MSVC)
add_subdirectory(winpthread)
endif()
