cmake_minimum_required(VERSION 3.12...4.3)

# Enforce minimum policy version for bundled dependencies (3rdparty)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for bundled dependencies")
endif()

project(Katy)

# Directory paths for source, tests, and third-party dependencies
set(KATY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(KATY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(KATY_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty")

# Detect Linux platform (distinguish from cross-compilation scenarios)
if(CMAKE_SYSTEM_NAME MATCHES ".*Linux")
	set(LINUX TRUE)
endif()

# KatyPS5 supports Windows, Linux, and macOS with Rosetta 2 on Apple Silicon
if (NOT (WIN32 OR LINUX OR APPLE))
    message(FATAL_ERROR "only Windows, Linux, and macOS builds are supported")
endif()

# C++ standard: C++20 required for std::ranges, std::format, and other modern features
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include("${KATY_SOURCE_DIR}/utils.cmake")
include(CTest)

option(KATY_ENABLE_CLANG_TIDY "Run clang-tidy checks during builds" OFF)

# Build provenance: "Source" (development), "Official" (erickson558/KatyPS5), or "Fork" (other forks)
set(KATY_BUILD_ORIGIN "Source" CACHE STRING "Build provenance shown in diagnostics")
set_property(CACHE KATY_BUILD_ORIGIN PROPERTY STRINGS Source Official Fork)
if(NOT KATY_BUILD_ORIGIN STREQUAL "Source" AND NOT KATY_BUILD_ORIGIN STREQUAL "Official" AND
   NOT KATY_BUILD_ORIGIN STREQUAL "Fork")
	message(FATAL_ERROR "KATY_BUILD_ORIGIN must be Source, Official, or Fork")
endif()

# Official builds embed the release tag; fork builds embed the repository name
set(KATY_RELEASE_TAG "" CACHE STRING "Release tag embedded in official builds")
set(KATY_BUILD_REPOSITORY "" CACHE STRING "GitHub repository embedded in fork builds")
set(KATY_OFFICIAL_BUILD OFF)
set(KATY_FORK_BUILD OFF)
if(KATY_BUILD_ORIGIN STREQUAL "Official")
	if(KATY_RELEASE_TAG STREQUAL "")
		message(FATAL_ERROR "Official builds require KATY_RELEASE_TAG")
	endif()
	set(KATY_OFFICIAL_BUILD ON)
elseif(KATY_BUILD_ORIGIN STREQUAL "Fork")
	if(KATY_BUILD_REPOSITORY STREQUAL "")
		message(FATAL_ERROR "Fork builds require KATY_BUILD_REPOSITORY")
	endif()
	set(KATY_FORK_BUILD ON)
endif()

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

include(TestBigEndian)

# KatyPS5 is a 64-bit only emulator (for both guest PS5 and host platforms)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "only 64-bit builds are supported")
endif()

test_big_endian(BIG_ENDIAN)
if(BIG_ENDIAN)
	set(KATY_ENDIAN KATY_ENDIAN_BIG)
else()
	set(KATY_ENDIAN KATY_ENDIAN_LITTLE)
endif()

if(CMAKE_BUILD_TYPE MATCHES Debug)
	set(KATY_BUILD KATY_BUILD_DEBUG)
else()
	set(KATY_BUILD KATY_BUILD_RELEASE)
endif()

if(LINUX OR APPLE)
	# macOS rides the POSIX/Linux code paths until it gets a dedicated platform
	set(KATY_PLATFORM KATY_PLATFORM_LINUX)
else()
	set(KATY_PLATFORM KATY_PLATFORM_WINDOWS)
endif()

set(CLANG FALSE)
set(GCC FALSE)
set(KATY_CLANG_CL FALSE)
get_filename_component(KATY_CXX_COMPILER_NAME "${CMAKE_CXX_COMPILER}" NAME_WE)
string(TOLOWER "${KATY_CXX_COMPILER_NAME}" KATY_CXX_COMPILER_NAME)

if(CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
	set(CLANG TRUE)
	set(KATY_COMPILER CLANG)
	if(WIN32 AND KATY_CXX_COMPILER_NAME STREQUAL "clang-cl")
		set(KATY_CLANG_CL TRUE)
		set(KATY_LINKER LLD_LINK)
	elseif(APPLE)
		set(KATY_LINKER LD64) # Apple's default linker; lld flags don't apply
	else()
		set(KATY_LINKER LLD)
	endif()
	if ((CMAKE_AR MATCHES "llvm") AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0.0) AND (CMAKE_CXX_COMPILER MATCHES "winlibs"))
		string(REPLACE "llvm-ar.exe" "ar.exe" CMAKE_AR ${CMAKE_AR}) # llvm-ar.exe 11.0.0 from winlibs is broken
	endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(GCC TRUE)
	set(KATY_COMPILER GCC)
	set(KATY_LINKER LD)
else()
	message(FATAL_ERROR "only Clang and GCC are supported; found ${CMAKE_CXX_COMPILER_ID}")
endif()

set(KATY_PROJECT_NAME "Emulator" CACHE STRING "Project name suffix")

string(TOLOWER ${KATY_COMPILER} KATY_COMPILER_ID)
string(TOLOWER ${KATY_LINKER} KATY_LINKER_ID)

if (CLANG AND (KATY_LINKER STREQUAL LLD))
	set(KATY_LD_OPTIONS "-fuse-ld=lld")
endif()

if (KATY_LINKER STREQUAL LD AND NOT LINUX)
	set(KATY_LD_OPTIONS "-Wl,--image-base=0x100000000000")
endif()

project(Katy${KATY_PROJECT_NAME}${CMAKE_BUILD_TYPE}${KATY_COMPILER} VERSION 0.4.0)

include_directories(
	${KATY_THIRD_PARTY_DIR}/gtest/include
	${KATY_THIRD_PARTY_DIR}/gtest
	${KATY_THIRD_PARTY_DIR}/fmt/include
	${KATY_THIRD_PARTY_DIR}/magic_enum/include/magic_enum
	${PROJECT_BINARY_DIR}
	${KATY_SOURCE_DIR}
)

set(KATY_VERSION "${PROJECT_VERSION}")

configure_file(
  ${KATY_SOURCE_DIR}/cmake_config.h.in
  ${PROJECT_BINARY_DIR}/cmake_config.h
  )

find_package(Git)

add_custom_target(KatyGitVersion
	COMMAND ${CMAKE_COMMAND}
		-D INPUT_FILE=${KATY_SOURCE_DIR}/katyGitVersion.h.in
		-D OUTPUT_FILE=${PROJECT_BINARY_DIR}/katyGitVersion.h
		-D GIT_EXECUTABLE=${GIT_EXECUTABLE}
		-D GIT_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
		-P ${KATY_SOURCE_DIR}/generate_version.cmake
	BYPRODUCTS ${PROJECT_BINARY_DIR}/katyGitVersion.h
	COMMENT "Generate katyGitVersion.h"
)

add_library(katy_git_version INTERFACE)
add_dependencies(katy_git_version KatyGitVersion)

if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0.0)
	list(APPEND KATY_IWYU
		katy_emulator
		common
		#launcher
	)
	list(APPEND KATY_CLANG_TIDY
		katy_emulator
		#common
		launcher
	)
endif()

option(KATY_BUILD_LAUNCHER "Build Qt launcher" ON)

config_compiler_and_linker()

add_subdirectory("${KATY_THIRD_PARTY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/3rdparty")
add_subdirectory("${KATY_SOURCE_DIR}/common" "${CMAKE_CURRENT_BINARY_DIR}/common")

file(GLOB katy_emulator_src CONFIGURE_DEPENDS
	"${KATY_SOURCE_DIR}/libs/*.cpp"
	"${KATY_SOURCE_DIR}/libs/*.h"
	"${KATY_SOURCE_DIR}/graphics/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/*.h"
	"${KATY_SOURCE_DIR}/graphics/guest_gpu/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/guest_gpu/*.h"
	"${KATY_SOURCE_DIR}/graphics/guest_gpu/command_processor/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/guest_gpu/command_processor/*.h"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/*.h"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/*.h"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/*.h"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/image/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/image/*.h"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/*.h"
	"${KATY_SOURCE_DIR}/graphics/shader/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/*.h"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/*.h"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/cfg/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/cfg/*.h"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/decompiler/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/decompiler/*.h"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/emitter/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/emitter/*.h"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/*.h"
	"${KATY_SOURCE_DIR}/graphics/presentation/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/presentation/*.h"
	"${KATY_SOURCE_DIR}/graphics/presentation/window/*.cpp"
	"${KATY_SOURCE_DIR}/graphics/presentation/window/*.h"
	"${KATY_SOURCE_DIR}/kernel/*.cpp"
	"${KATY_SOURCE_DIR}/kernel/*.h"
	"${KATY_SOURCE_DIR}/loader/*.cpp"
	"${KATY_SOURCE_DIR}/loader/*.h"
)

find_program(KATY_GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(gpu_tiler_shader_dir "${KATY_SOURCE_DIR}/graphics/host_gpu/shaders")
set(gpu_tiler_generated_dir "${PROJECT_BINARY_DIR}/gpu_tiler_shaders")
set(gpu_tiler_shader_names
	standard256
	standard4
	standard4_3d
	standard64
	standard64_3d
	prt
	prt_3d
	render_target
	depth
	promote_d16
	demote_d16
	swap_bgra16
)
file(GLOB gpu_tiler_shader_includes CONFIGURE_DEPENDS "${gpu_tiler_shader_dir}/gpu_tiler_*.inc")
foreach(shader_name IN LISTS gpu_tiler_shader_names)
	set(shader_source "${gpu_tiler_shader_dir}/gpu_tiler_${shader_name}.comp")
	set(shader_spv "${gpu_tiler_generated_dir}/gpu_tiler_${shader_name}.spv")
	set(shader_header "${gpu_tiler_generated_dir}/gpu_tiler_${shader_name}_spv.h")
	string(TOUPPER "GPU_TILER_${shader_name}_SPV" shader_symbol)
	add_custom_command(
		OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_tiler_generated_dir}"
		COMMAND "${KATY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 -Os
		        "-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
		        -DSYMBOL=${shader_symbol} -P "${KATY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}" ${gpu_tiler_shader_includes}
		VERBATIM
	)
	list(APPEND gpu_tiler_shader_headers "${shader_header}")
endforeach()
list(APPEND katy_emulator_src ${gpu_tiler_shader_headers})

set(gpu_blit_generated_dir "${PROJECT_BINARY_DIR}/gpu_blit_shaders")
set(gpu_blit_shader_sources
	"${gpu_tiler_shader_dir}/gpu_blit_fs_triangle.vert"
	"${gpu_tiler_shader_dir}/gpu_blit_color_to_ms_depth.frag"
)
foreach(shader_source IN LISTS gpu_blit_shader_sources)
	get_filename_component(shader_name "${shader_source}" NAME_WE)
	set(shader_spv "${gpu_blit_generated_dir}/${shader_name}.spv")
	set(shader_header "${gpu_blit_generated_dir}/${shader_name}_spv.h")
	string(TOUPPER "${shader_name}_SPV" shader_symbol)
	add_custom_command(
		OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_blit_generated_dir}"
		COMMAND "${KATY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 -Os
		        "-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
		        -DSYMBOL=${shader_symbol} -P "${KATY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}"
		VERBATIM
	)
	list(APPEND gpu_blit_shader_headers "${shader_header}")
endforeach()
list(APPEND katy_emulator_src ${gpu_blit_shader_headers})

list(APPEND katy_emulator_src
	"${KATY_SOURCE_DIR}/emulator.h"
	"${KATY_SOURCE_DIR}/emulator.cpp"
)
list(REMOVE_DUPLICATES katy_emulator_src)

if (KATY_CLANG_CL)
	set_source_files_properties(${katy_emulator_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
endif()

set(katy_emulator_link_libraries common Vulkan::Headers spirv-tools-opt spirv-tools SDL2-static xxhash FFmpeg::ffmpeg fmt::fmt nlohmann_json::nlohmann_json LibAtrac9 katy_git_version)

# Linux system libraries required by the static FFmpeg archive.
if(LINUX)
	find_package(Threads REQUIRED)
	list(APPEND katy_emulator_link_libraries m ${CMAKE_DL_LIBS} Threads::Threads)

	# Optional FFmpeg dependencies.
	find_package(ZLIB)
	if(ZLIB_FOUND)
		list(APPEND katy_emulator_link_libraries ZLIB::ZLIB)
	endif()
	find_library(KATY_BZ2_LIBRARY bz2)
	if(KATY_BZ2_LIBRARY)
		list(APPEND katy_emulator_link_libraries ${KATY_BZ2_LIBRARY})
	endif()
	find_library(KATY_LZMA_LIBRARY lzma)
	if(KATY_LZMA_LIBRARY)
		list(APPEND katy_emulator_link_libraries ${KATY_LZMA_LIBRARY})
	endif()
endif()

set(inc_headers
	${KATY_SOURCE_DIR}
	${KATY_THIRD_PARTY_DIR}/SDL2/include
	${KATY_THIRD_PARTY_DIR}/VulkanMemoryAllocator/include
	${KATY_THIRD_PARTY_DIR}/SPIRV-Tools/include
	${KATY_THIRD_PARTY_DIR}/fmt/include
	${KATY_THIRD_PARTY_DIR}/xxhash
	${KATY_THIRD_PARTY_DIR}/cpuinfo/include
	${KATY_THIRD_PARTY_DIR}/stb
)

if (KATY_CLANG_CL)
	list(APPEND katy_emulator_link_libraries winpthread)
	list(APPEND inc_headers
		${KATY_THIRD_PARTY_DIR}/winpthread/include
	)
endif()

list(APPEND check_headers
	${KATY_SOURCE_DIR}
)

function(add_katy_full_emulator_test target source)
	add_executable(${target} EXCLUDE_FROM_ALL ${source} ${katy_emulator_src})
	target_link_libraries(${target} ${katy_emulator_link_libraries})
	target_include_directories(${target} PRIVATE ${inc_headers})
	if (WIN32)
		target_link_libraries(${target} iphlpapi)
	endif()
	if (KATY_CLANG_CL)
		target_link_libraries(${target} onecore)
		add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:${target}>/libwinpthread-1.dll)
	endif()
	# The macOS x86_64 guest address space needs its .zerofill segments anchored
	# by linker flags, or the kernel kills the binary on load (posix_spawn EIO).
	configure_macos_guest_address_space(${target})
endfunction()

function(configure_macos_guest_address_space target)
	if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
			(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")))
		target_sources(${target} PRIVATE "${KATY_SOURCE_DIR}/kernel/macosGuestAddressSpace.cpp")
		target_compile_definitions(${target} PRIVATE KATY_LINKED_GUEST_ADDRESS_SPACE=1)
		target_link_options(${target} PRIVATE
			-Wl,-ld_classic,-no_pie,-no_fixup_chains,-no_huge,-pagezero_size,0x40000,-segaddr,SYSTEM_MANAGED,0x40000,-segaddr,SYSTEM_RESERVED,0x7ffffc000,-segaddr,USER_AREA,0x7000000000,-image_base,0x700000000000)
	endif()
endfunction()

add_katy_full_emulator_test(shader_cfg_tests "${KATY_TESTS_DIR}/shaderCfgTests.cpp")

add_executable(scalar_provenance_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/ScalarProvenanceTests.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/ReadLaneElimination.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/ScalarProvenance.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/SrtWalker.cpp"
)
target_link_libraries(scalar_provenance_tests fmt::fmt)
target_include_directories(scalar_provenance_tests PRIVATE ${inc_headers})

add_executable(page_manager_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/PageManagerTests.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
)
target_include_directories(page_manager_tests PRIVATE ${inc_headers})

add_executable(bit_array_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/BitArrayTests.cpp"
)
target_include_directories(bit_array_tests PRIVATE ${inc_headers})

add_executable(memory_tracker_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/MemoryTrackerTests.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/memoryTracker.cpp"
)
target_link_libraries(memory_tracker_tests fmt::fmt common)
target_include_directories(memory_tracker_tests PRIVATE ${inc_headers})

add_executable(shader_vertex_metadata_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/ShaderVertexMetadataTests.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/shaderVertexMetadata.cpp"
)
target_include_directories(shader_vertex_metadata_tests PRIVATE ${inc_headers})

add_executable(shader_stage_runtime_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/ShaderStageRuntimeTests.cpp"
	"${KATY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/shaderStageRuntime.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/ResourceMaterialization.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/ScalarProvenance.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/SrtWalker.cpp"
)
target_link_libraries(shader_stage_runtime_tests fmt::fmt)
target_include_directories(shader_stage_runtime_tests PRIVATE ${inc_headers})

add_executable(resource_tracking_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/ResourceTrackingTests.cpp"
	"${KATY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/ScalarProvenance.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/SrtWalker.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/SrtPatcher.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/ResourceTracking.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/ResourceMaterialization.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/ShaderInfoCollection.cpp"
	"${KATY_SOURCE_DIR}/graphics/shader/recompiler/ir/BindingLayout.cpp"
)
target_link_libraries(resource_tracking_tests fmt::fmt)
target_include_directories(resource_tracking_tests PRIVATE ${inc_headers})

add_executable(resource_mutex_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/ResourceMutexTests.cpp"
	"${KATY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/resourceMutex.cpp"
)
target_link_libraries(resource_mutex_tests common)
target_include_directories(resource_mutex_tests PRIVATE ${inc_headers})

add_executable(audio_out2_port_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/AudioOut2PortTests.cpp"
	"${KATY_SOURCE_DIR}/libs/libAudio2.cpp"
	"${KATY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(audio_out2_port_tests common fmt::fmt)
target_include_directories(audio_out2_port_tests PRIVATE ${inc_headers})

# The timing regression includes production audio.cpp with deterministic SDL queues and clock.
add_executable(audio_out_timing_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/AudioOutTimingTests.cpp"
	"${KATY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(audio_out_timing_tests common fmt::fmt)
target_include_directories(audio_out_timing_tests PRIVATE ${inc_headers})

add_executable(event_queue_lifetime_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/EventQueueLifetimeTests.cpp"
	"${KATY_SOURCE_DIR}/kernel/eventQueue.cpp"
	"${KATY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(event_queue_lifetime_tests common fmt::fmt)
target_include_directories(event_queue_lifetime_tests PRIVATE ${inc_headers})

add_executable(image_page_table_tests EXCLUDE_FROM_ALL
	"${KATY_TESTS_DIR}/ImagePageTableTests.cpp"
)
target_link_libraries(image_page_table_tests common fmt::fmt)
target_include_directories(image_page_table_tests PRIVATE ${inc_headers})

add_katy_full_emulator_test(shader_recompiler_compute_tests "${KATY_TESTS_DIR}/ShaderRecompilerComputeTests.cpp")

set(gpu_test_generated_dir "${PROJECT_BINARY_DIR}/gpu_test_shaders")
set(gpu_test_ms_depth_source
	"${gpu_tiler_shader_dir}/gpu_test_ms_depth.comp")
set(gpu_test_ms_depth_spv
	"${gpu_test_generated_dir}/gpu_test_ms_depth.spv")
set(gpu_test_ms_depth_header
	"${gpu_test_generated_dir}/gpu_test_ms_depth_spv.h")
add_custom_command(
	OUTPUT "${gpu_test_ms_depth_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_test_generated_dir}"
	COMMAND "${KATY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 -Os
	        -o "${gpu_test_ms_depth_spv}" "${gpu_test_ms_depth_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${gpu_test_ms_depth_spv}
	        -DOUTPUT=${gpu_test_ms_depth_header} -DSYMBOL=GPU_TEST_MS_DEPTH_SPV
	        -P "${KATY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${gpu_test_ms_depth_source}"
	VERBATIM
)
target_sources(shader_recompiler_compute_tests PRIVATE
	"${gpu_test_ms_depth_header}")

add_katy_full_emulator_test(virtual_memory_allocation_tests "${KATY_TESTS_DIR}/VirtualMemoryAllocationTests.cpp")
add_katy_full_emulator_test(kernel_file_system_tests "${KATY_TESTS_DIR}/KernelFileSystemTests.cpp")
# Include the production SaveData translation unit in the test to use its exact guest ABI.
set(save_memory_emulator_src ${katy_emulator_src})
list(REMOVE_ITEM katy_emulator_src "${KATY_SOURCE_DIR}/libs/libSaveData.cpp")
add_katy_full_emulator_test(save_data_memory_tests "${KATY_TESTS_DIR}/SaveDataMemoryTests.cpp")
set(katy_emulator_src ${save_memory_emulator_src})
# Only the save-memory boundary handles host allocation/filesystem exceptions.
if(KATY_CLANG_CL)
	set(save_memory_exception_option /EHsc)
else()
	set(save_memory_exception_option -fexceptions)
endif()
set_property(SOURCE "${KATY_SOURCE_DIR}/libs/libSaveData.cpp"
	"${KATY_SOURCE_DIR}/libs/saveDataMemoryStore.cpp"
	"${KATY_TESTS_DIR}/SaveDataMemoryTests.cpp"
	APPEND PROPERTY COMPILE_OPTIONS ${save_memory_exception_option})
target_compile_definitions(virtual_memory_allocation_tests PRIVATE
	KATY_VIRTUAL_MEMORY_ALLOCATION_TESTS=1)

# These tests use exceptions.
if(NOT KATY_CLANG_CL)
	foreach(katy_exception_test scalar_provenance_tests resource_tracking_tests
			virtual_memory_allocation_tests)
		target_compile_options(${katy_exception_test} PRIVATE -fexceptions)
	endforeach()
endif()

if(BUILD_TESTING)
	add_test(NAME shader_cfg COMMAND $<TARGET_FILE:shader_cfg_tests>)
	add_test(NAME scalar_provenance COMMAND $<TARGET_FILE:scalar_provenance_tests>)
	add_test(NAME image_page_table COMMAND $<TARGET_FILE:image_page_table_tests>)
	add_test(NAME memory_tracker COMMAND $<TARGET_FILE:memory_tracker_tests>)
	add_test(NAME page_manager COMMAND $<TARGET_FILE:page_manager_tests>)
	add_test(NAME bit_array COMMAND $<TARGET_FILE:bit_array_tests>)
	add_test(NAME shader_vertex_metadata COMMAND $<TARGET_FILE:shader_vertex_metadata_tests>)
	add_test(NAME shader_stage_runtime COMMAND $<TARGET_FILE:shader_stage_runtime_tests>)
	add_test(NAME resource_tracking COMMAND $<TARGET_FILE:resource_tracking_tests>)
	add_test(NAME resource_mutex COMMAND $<TARGET_FILE:resource_mutex_tests>)
	add_test(NAME event_queue_lifetime COMMAND $<TARGET_FILE:event_queue_lifetime_tests>)
	add_test(NAME kernel_file_system COMMAND $<TARGET_FILE:kernel_file_system_tests>)
	add_test(NAME save_data_memory COMMAND $<TARGET_FILE:save_data_memory_tests>)
	set(save_memory_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/save-memory-regression")
	add_test(NAME save_data_memory_write
		COMMAND $<TARGET_FILE:save_data_memory_tests> --write "${save_memory_fixture_dir}")
	add_test(NAME save_data_memory_restart
		COMMAND $<TARGET_FILE:save_data_memory_tests> --read "${save_memory_fixture_dir}")
	add_test(NAME save_data_memory_cleanup
		COMMAND $<TARGET_FILE:save_data_memory_tests> --cleanup "${save_memory_fixture_dir}")
	set_tests_properties(save_data_memory_write PROPERTIES FIXTURES_SETUP save_memory_restart)
	set_tests_properties(save_data_memory_restart PROPERTIES FIXTURES_REQUIRED save_memory_restart)
	set_tests_properties(save_data_memory_cleanup PROPERTIES FIXTURES_CLEANUP save_memory_restart)
	set_tests_properties(save_data_memory save_data_memory_write save_data_memory_restart
		save_data_memory_cleanup PROPERTIES TIMEOUT 60)
	add_test(NAME audio_out2_port COMMAND $<TARGET_FILE:audio_out2_port_tests>)
	add_test(NAME audio_out_timing COMMAND $<TARGET_FILE:audio_out_timing_tests>)
	# Turn deadlocks into bounded failures, including the cross-thread invalid-seek regression.
	set_tests_properties(event_queue_lifetime kernel_file_system audio_out2_port audio_out_timing PROPERTIES TIMEOUT 60)
	add_test(NAME shader_recompiler_compute COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	add_test(NAME virtual_memory_allocation
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests>)
	add_test(NAME command_scheduler_timeline
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --scheduler-only)
	add_test(NAME stream_buffer_ring
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --stream-buffer-only)
	add_test(NAME gpu_command_lane
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-command-lane-only)
	add_test(NAME gpu_tiler
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-tiler-only)

	add_test(NAME texture_cache_layered_image
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --layered-image-only)
	add_test(NAME texture_cache_image_views
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-view-cache-only)
	add_test(NAME texture_cache_storage_sampled
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --storage-sampled-only)
	add_test(NAME texture_cache_depth_readback
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --depth-readback-only)
	add_test(NAME buffer_cache_dirty_gc
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-gc-only)

	if(WIN32)
		# These tests still depend on the Windows multisample-depth path.
		add_test(NAME texture_cache_image_overlap
			COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-overlap-only)
		add_test(NAME texture_cache_htile_clear
			COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --htile-clear-only)
		add_test(NAME buffer_cache_ranges
			COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-range-only)
	endif()

	add_custom_target(katy_tests DEPENDS
		shader_cfg_tests
		scalar_provenance_tests
		image_page_table_tests
		memory_tracker_tests
		page_manager_tests
		bit_array_tests
		shader_vertex_metadata_tests
		shader_stage_runtime_tests
		resource_tracking_tests
		resource_mutex_tests
		event_queue_lifetime_tests
		kernel_file_system_tests
		save_data_memory_tests
		audio_out2_port_tests
		audio_out_timing_tests
		shader_recompiler_compute_tests
		virtual_memory_allocation_tests
	)
endif()


add_executable(katy_emulator "${KATY_SOURCE_DIR}/main.cpp" ${katy_emulator_src})
configure_macos_guest_address_space(katy_emulator)

target_link_libraries(katy_emulator ${katy_emulator_link_libraries})
if (WIN32)
	target_link_libraries(katy_emulator opengl32)
	target_link_libraries(katy_emulator iphlpapi)
	target_link_libraries(katy_emulator setupapi)
	target_link_libraries(katy_emulator ws2_32)
endif()
if (KATY_CLANG_CL)
	target_link_libraries(katy_emulator onecore)
endif()
if (CLANG AND NOT KATY_CLANG_CL)
	target_link_libraries(katy_emulator pthread)
endif()
# dlopen/dlsym/dladdr for RenderDoc.
target_link_libraries(katy_emulator ${CMAKE_DL_LIBS})
target_include_directories(katy_emulator PRIVATE ${inc_headers})

clang_tidy_check(katy_emulator "" "${check_headers}" "${inc_headers}")

include_what_you_use(katy_emulator "${inc_headers}")

set(KATY_EMULATOR_MAP_NAME "katy_emulator_${KATY_COMPILER_ID}_${KATY_LINKER_ID}.map")

set(KATY_EMULATOR_MAP_LINK_PATH "${CMAKE_CURRENT_BINARY_DIR}/${KATY_EMULATOR_MAP_NAME}")
set(KATY_EMULATOR_PDB_LINK_PATH "${CMAKE_CURRENT_BINARY_DIR}/katy_emulator.pdb")

if(KATY_CLANG_CL)
	target_link_options(katy_emulator PRIVATE
		"/DYNAMICBASE:NO"
		"/DEBUG:FULL"
		"/PDB:${KATY_EMULATOR_PDB_LINK_PATH}"
		"/lldmap:${KATY_EMULATOR_MAP_LINK_PATH}"
	)
	add_custom_command(TARGET katy_emulator POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KATY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:katy_emulator>/libwinpthread-1.dll)
elseif(WIN32 OR LINUX)
	set_target_properties(katy_emulator PROPERTIES LINK_FLAGS "${KATY_LD_OPTIONS} -Wl,-Map=${KATY_EMULATOR_MAP_LINK_PATH}")
endif()

if(APPLE)
	# The emulator writes x86-64 trampolines/PLT stubs into memory and executes them, and
	# runs guest code under Rosetta. That requires the JIT / unsigned-executable-memory
	# entitlements, so re-sign the binary after every link (an unsigned/relinked binary
	# reverts to the hardened defaults and aborts when it first executes written code).
	add_custom_command(TARGET katy_emulator POST_BUILD
		COMMAND codesign -s - --force --options runtime
			--entitlements "${KATY_SOURCE_DIR}/macos_jit.entitlements"
			$<TARGET_FILE:katy_emulator>
		COMMENT "Codesign katy_emulator with JIT entitlements (macOS)")
endif()

install(TARGETS katy_emulator DESTINATION .)
# Keep the project license and historical attribution beside distributed binaries.
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION .)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/LICENSES" DESTINATION .)
if(KATY_BUILD_LAUNCHER)
	add_subdirectory("${KATY_SOURCE_DIR}/launcher" "${CMAKE_CURRENT_BINARY_DIR}/launcher")
endif()
if(KATY_CLANG_CL)
	install(FILES "${KATY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" DESTINATION .)
endif()
