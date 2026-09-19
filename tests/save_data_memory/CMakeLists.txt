cmake_minimum_required(VERSION 3.16)
project(KytySaveDataMemoryTests LANGUAGES CXX)

# Exercise the production storage implementation without renderer/launcher dependencies.
#
# The test is registered in the top-level build as well; this project exists so it can also be run
# on a machine with no Vulkan SDK, Qt or glslangValidator:
#
#   cmake -S tests/save_data_memory -B _Build/save-memory -G Ninja
#   cmake --build _Build/save-memory && ctest --test-dir _Build/save-memory --output-on-failure
enable_testing()
find_package(Threads REQUIRED)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(WIN32)
	set(KYTY_PLATFORM KYTY_PLATFORM_WINDOWS)
else()
	set(KYTY_PLATFORM KYTY_PLATFORM_LINUX)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	set(KYTY_COMPILER CLANG)
else()
	set(KYTY_COMPILER GCC)
endif()
set(KYTY_BUILD KYTY_BUILD_RELEASE)
set(KYTY_ENDIAN KYTY_ENDIAN_LITTLE)
configure_file("../../src/cmake_config.h.in" "cmake_config.h")
add_executable(save_data_memory_tests
	"../SaveDataMemoryTests.cpp"
	"../../src/libs/saveDataMemory.cpp")
target_include_directories(save_data_memory_tests PRIVATE "../../src" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(save_data_memory_tests PRIVATE KYTY_SAVE_DATA_TESTS=1)
target_link_libraries(save_data_memory_tests PRIVATE Threads::Threads)
add_test(NAME save_data_memory COMMAND save_data_memory_tests)
set_tests_properties(save_data_memory PROPERTIES TIMEOUT 120)
