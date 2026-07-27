set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(cpuinfo)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
CPMAddPackage(
	NAME SDL2
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG 4b69833bc54abf3dd3288d4aa7afbba527775e5b
)
set(SDL2_SOURCE_DIR "${SDL2_SOURCE_DIR}" PARENT_SCOPE)

CPMAddPackage(
	NAME Vulkan-Headers
	GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
	GIT_TAG 2fa203425eb4af9dfc6b03f97ef72b0b5bcb8350
	DOWNLOAD_ONLY YES
)

add_library(Vulkan-Headers INTERFACE)
add_library(Vulkan::Headers ALIAS Vulkan-Headers)
target_include_directories(Vulkan-Headers INTERFACE
	"${Vulkan-Headers_SOURCE_DIR}/include"
)

set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)

CPMAddPackage(
	NAME SPIRV-Headers
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
	GIT_TAG 01e0577914a75a2569c846778c2f93aa8e6feddd
	DOWNLOAD_ONLY YES
)

CPMAddPackage(
	NAME SPIRV-Tools
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
	GIT_TAG 7f2d9ee926f98fc77a3ed1e1e0f113b8c9c49458
)

add_library(spirv-tools ALIAS SPIRV-Tools-static)
add_library(spirv-tools-opt ALIAS SPIRV-Tools-opt)
set(SPIRV-Tools_SOURCE_DIR "${SPIRV-Tools_SOURCE_DIR}" PARENT_SCOPE)

CPMAddPackage(
	NAME xxHash
	GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
	GIT_TAG e573d4d2aaeaba0f3e5a0a9a54144a1f2b4b56e7
	SOURCE_SUBDIR build/cmake
)
set(xxHash_SOURCE_DIR "${xxHash_SOURCE_DIR}" PARENT_SCOPE)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_MODULE OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
CPMAddPackage(
	NAME fmt
	GIT_REPOSITORY https://github.com/fmtlib/fmt.git
	GIT_TAG 11ddbcb7898d2d3445d431a54814367b21dee6ad
	EXCLUDE_FROM_ALL YES
)
set(fmt_SOURCE_DIR "${fmt_SOURCE_DIR}" PARENT_SCOPE)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
CPMAddPackage(
	NAME nlohmann_json
	GIT_REPOSITORY https://github.com/nlohmann/json.git
	GIT_TAG 272411c5e6ea45919af7673524e74e60c62116df
	EXCLUDE_FROM_ALL YES
)

set(SPDLOG_BUILD_ALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_WARNINGS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
CPMAddPackage(
	NAME spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog.git
	GIT_TAG 8671ca4d492c8ee1cdfd3dd88afb9f88dd268178
	EXCLUDE_FROM_ALL YES
)

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
	CPMAddPackage(
		NAME ffmpeg-core
		GIT_REPOSITORY https://github.com/shadps4-emu/ext-ffmpeg-core.git
		GIT_TAG 94dde08c8a9e4271a93a2a7e4159e9fb05d30c0a
		GIT_SHALLOW TRUE
	)
	add_library(FFmpeg::ffmpeg ALIAS ffmpeg)
endif()

CPMAddPackage(
	NAME LibAtrac9
	GIT_REPOSITORY https://github.com/shadps4-emu/ext-LibAtrac9.git
	GIT_TAG ec8899dadf393f655f2871a94e0fe4b3d6220c9a
	DOWNLOAD_ONLY YES
)

file(GLOB LIBATRAC9_SOURCES "${LibAtrac9_SOURCE_DIR}/C/src/*.c")
add_library(LibAtrac9 STATIC ${LIBATRAC9_SOURCES})
target_include_directories(LibAtrac9 PUBLIC "${LibAtrac9_SOURCE_DIR}/C/src")
if(MSVC)
	target_compile_options(LibAtrac9 PRIVATE /W0)
endif()

set(TRACY_STATIC ON CACHE BOOL "" FORCE)
set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_DELAYED_INIT ON CACHE BOOL "" FORCE)
set(TRACY_MANUAL_LIFETIME ON CACHE BOOL "" FORCE)
CPMAddPackage(
	NAME tracy
	GIT_REPOSITORY https://github.com/wolfpld/tracy.git
	GIT_TAG 00c079cec91e6a374519aa5719a073e7f4539c9d
	EXCLUDE_FROM_ALL YES
)
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

CPMAddPackage(
	NAME VulkanMemoryAllocator
	GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
	GIT_TAG a1d434708c217b2a6c7b365f1fe41fa03a562e59
	DOWNLOAD_ONLY YES
)
set(VulkanMemoryAllocator_SOURCE_DIR "${VulkanMemoryAllocator_SOURCE_DIR}" PARENT_SCOPE)

if (CLANG AND MSVC)
add_subdirectory(winpthread)
endif()
