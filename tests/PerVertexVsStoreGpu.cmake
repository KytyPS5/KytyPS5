# GPU harness (needs MoltenVK through SDL_VULKAN_LIBRARY, so it stays opt-in and out of ctest).
# `per_vertex_vsstore_gpu_tests --dump` prints the emitted SPIR-V without a GPU.
add_kyty_full_emulator_test(per_vertex_vsstore_gpu_tests "${KYTY_TESTS_DIR}/PerVertexVsStoreGpuTests.cpp")
add_kyty_full_emulator_test(per_vertex_vsstore_gpu_mutant_first_third_tests "${KYTY_TESTS_DIR}/PerVertexVsStoreGpuTests.cpp")
target_include_directories(per_vertex_vsstore_gpu_mutant_first_third_tests BEFORE PRIVATE "${KYTY_PV_MUTANT_DIR}/first_third")
add_kyty_full_emulator_test(per_vertex_vsstore_gpu_mutant_range_tests "${KYTY_TESTS_DIR}/PerVertexVsStoreGpuTests.cpp")
target_include_directories(per_vertex_vsstore_gpu_mutant_range_tests BEFORE PRIVATE "${KYTY_PV_MUTANT_DIR}/index_range_no_round")
