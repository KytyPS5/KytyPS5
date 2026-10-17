# Mutation tests of the VS-store helpers (perVertexVsStore.h) and of the SPIR-V emitter side (spirvEmitterPvStore.cpp).
# The production sources carry no mutant switches: every mutant is a copy of the source with one expression changed,
# generated at configure time into the build directory. A mutant whose `from` text no longer exists fails the
# configuration, so the list cannot silently go stale.

set(KYTY_PV_MUTANT_DIR "${CMAKE_CURRENT_BINARY_DIR}/pv_vsstore_mutants")
set(KYTY_PV_HEADER_REL "graphics/host_gpu/renderer/perVertexVsStore.h")
set(KYTY_PV_EMITTER_REL "graphics/shader/recompiler/backend/spirv/spirvEmitterPvStore.cpp")
set(KYTY_PV_RUNTIME_REL "graphics/host_gpu/renderer/perVertexVsStoreRuntime.cpp")

# kyty_pv_make_mutant(<name> <path relative to src> <from> <to>) writes <dir>/<name>/<path> and sets
# KYTY_PV_MUTANT_ROOT to <dir>/<name>. `from` has to occur exactly once.
function(kyty_pv_make_mutant name rel from to)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KYTY_SOURCE_DIR}/${rel}")
	file(READ "${KYTY_SOURCE_DIR}/${rel}" text)
	string(FIND "${text}" "${from}" first)
	if(first EQUAL -1)
		message(FATAL_ERROR "pv_vsstore mutant ${name}: text not found in ${rel}: ${from}")
	endif()
	string(REPLACE "${from}" "@@PV_MUTANT_SITE@@" probe "${text}")
	string(REGEX MATCHALL "@@PV_MUTANT_SITE@@" hits "${probe}")
	list(LENGTH hits count)
	if(NOT count EQUAL 1)
		message(FATAL_ERROR "pv_vsstore mutant ${name}: text occurs ${count} times in ${rel}: ${from}")
	endif()
	string(REPLACE "${from}" "${to}" mutated "${text}")
	file(WRITE "${KYTY_PV_MUTANT_DIR}/${name}/${rel}" "${mutated}")
	set(KYTY_PV_MUTANT_ROOT "${KYTY_PV_MUTANT_DIR}/${name}" PARENT_SCOPE)
endfunction()

# Header mutants: name, from, to (all in perVertexVsStore.h).
set(KYTY_PV_HEADER_MUTANTS
	binding_fits_no_tail
	first_third
	no_instance
	no_first
	half_swap
	no_first_index
	no_vertex_offset
	dropped_ok
	reuse_region
	param31_ok
	push_no_first_instance
	push_always
	index_no_adjust
	index_range_no_round
	pool_reuse_busy
	no_touch
	no_instanced_class
	align_ignored
	ring_ignored
	stride_zero_ok
	records_wrap_ok
	cap_barycentric_ignored
	cap_moltenvk_ignored
	cap_off_ignored
	cap_on_ignored
	parse_trailing
	scan_no_offset
	scan_drop_ok
	scan_end_inclusive
	scan_overflow_ok)
set(KYTY_PV_FROM_first_third "\treturn k;\n}\n\n// Record written")
set(KYTY_PV_TO_first_third "\treturn k == 0 ? 2u : (k == 2 ? 0u : k);\n}\n\n// Record written")
set(KYTY_PV_FROM_no_instance "return base + (instance - first_instance) * span + (vertex_index - first);")
set(KYTY_PV_TO_no_instance "return base + (vertex_index - first);")
set(KYTY_PV_FROM_no_first "return base + (instance - first_instance) * span + (vertex_index - first);")
set(KYTY_PV_TO_no_first "return base + (instance - first_instance) * span + vertex_index;")
set(KYTY_PV_FROM_half_swap "return (word >> ((element & 1u) * 16u)) & 0xffffu;")
set(KYTY_PV_TO_half_swap "return (word >> (((element & 1u) ^ 1u) * 16u)) & 0xffffu;")
set(KYTY_PV_FROM_no_first_index "PvVsStoreFetchIndex(index_words, draw.index32, element);")
set(KYTY_PV_TO_no_first_index "PvVsStoreFetchIndex(index_words, draw.index32, element - draw.first_index);")
set(KYTY_PV_FROM_no_vertex_offset "const uint32_t vertex_index = index + static_cast<uint32_t>(draw.vertex_offset);")
set(KYTY_PV_TO_no_vertex_offset "const uint32_t vertex_index = index;")
set(KYTY_PV_FROM_dropped_ok "if (request.range_dropped) return reject(V::Dropped);")
set(KYTY_PV_TO_dropped_ok "")
set(KYTY_PV_FROM_reuse_region "m_used += records;")
set(KYTY_PV_TO_reuse_region "")
set(KYTY_PV_FROM_param31_ok "if (((vs_location_mask | ps_location_mask) >> kPvVsStoreInstanceLocation) & 1u)\n\t\treturn V::Param31;")
set(KYTY_PV_TO_param31_ok "")
set(KYTY_PV_FROM_push_no_first_instance "block[kPvPushFirstInstance] = draw.first_instance;")
set(KYTY_PV_TO_push_no_first_instance "block[kPvPushFirstInstance] = 0;")
set(KYTY_PV_FROM_push_always "return native_bytes + (vs_store_variant ? kPvVsStorePushDwords * 4u : 0u);")
set(KYTY_PV_TO_push_always "return native_bytes + kPvVsStorePushDwords * 4u;")
set(KYTY_PV_FROM_index_no_adjust "return {aligned, static_cast<uint32_t>(delta / element_size), true};")
set(KYTY_PV_TO_index_no_adjust "return {aligned, 0, true};")
set(KYTY_PV_FROM_index_range_no_round "return (bytes + 3u) & ~uint64_t {3};")
set(KYTY_PV_TO_index_range_no_round "return bytes;")
set(KYTY_PV_FROM_pool_reuse_busy "const bool reusable = slab.tick == tick || !slab.used || is_free(slab.tick);")
set(KYTY_PV_TO_pool_reuse_busy "const bool reusable = true;")
set(KYTY_PV_FROM_no_touch "if (slab < m_slabs.size() && m_slabs[slab].tick < tick) m_slabs[slab].tick = tick;")
set(KYTY_PV_TO_no_touch "")
set(KYTY_PV_FROM_no_instanced_class "return instance_count > 1 ? F::Instanced : F::Range;")
set(KYTY_PV_TO_no_instanced_class "return F::Range;")
set(KYTY_PV_FROM_align_ignored "if (!index_aligned) {")
set(KYTY_PV_TO_align_ignored "if (false && !index_aligned) {")
set(KYTY_PV_FROM_ring_ignored "if (!place(decision.plan)) {")
set(KYTY_PV_TO_ring_ignored "if (!place(decision.plan) && false) {")
set(KYTY_PV_FROM_stride_zero_ok "request.range_end <= request.range_first || request.record_stride_vec4 == 0)")
set(KYTY_PV_TO_stride_zero_ok "request.range_end <= request.range_first)")
set(KYTY_PV_FROM_records_wrap_ok "request.range_end > 0xffffffffull || records > 0xffffffffull ||")
set(KYTY_PV_TO_records_wrap_ok "request.range_end > 0xffffffffull ||")
set(KYTY_PV_FROM_cap_barycentric_ignored "return !fragment_shader_barycentric && moltenvk;")
set(KYTY_PV_TO_cap_barycentric_ignored "return moltenvk;")
set(KYTY_PV_FROM_cap_moltenvk_ignored "return !fragment_shader_barycentric && moltenvk;")
set(KYTY_PV_TO_cap_moltenvk_ignored "return !fragment_shader_barycentric;")
set(KYTY_PV_FROM_cap_off_ignored "if (mode == PvVsStoreSwitch::Off) return false;")
set(KYTY_PV_TO_cap_off_ignored "")
set(KYTY_PV_FROM_cap_on_ignored "if (mode == PvVsStoreSwitch::On) return true;")
set(KYTY_PV_TO_cap_on_ignored "")
set(KYTY_PV_FROM_parse_trailing "if (value == nullptr || value[0] == '\\0' || value[1] != '\\0') return PvVsStoreSwitch::Auto;")
set(KYTY_PV_TO_parse_trailing "if (value == nullptr || value[0] == '\\0') return PvVsStoreSwitch::Auto;")
set(KYTY_PV_FROM_scan_no_offset "static_cast<int64_t>(raw) + vertex_offset;")
set(KYTY_PV_TO_scan_no_offset "static_cast<int64_t>(raw);")
set(KYTY_PV_FROM_scan_drop_ok "\t\t\tdropped = true;\n\t\t\tcontinue;")
set(KYTY_PV_TO_scan_drop_ok "\t\t\tcontinue;")
set(KYTY_PV_FROM_scan_end_inclusive "range.end   = static_cast<uint64_t>(hi) + 1;")
set(KYTY_PV_TO_scan_end_inclusive "range.end   = static_cast<uint64_t>(hi);")
set(KYTY_PV_FROM_binding_fits_no_tail "return desc.ok && desc.tail_fits;")
set(KYTY_PV_TO_binding_fits_no_tail "return desc.ok;")
set(KYTY_PV_FROM_scan_overflow_ok "if (adjusted > INT32_MAX) return {};")
set(KYTY_PV_TO_scan_overflow_ok "")

foreach(mutant IN LISTS KYTY_PV_HEADER_MUTANTS)
	kyty_pv_make_mutant(${mutant} "${KYTY_PV_HEADER_REL}" "${KYTY_PV_FROM_${mutant}}" "${KYTY_PV_TO_${mutant}}")
endforeach()
# Emitter mutant: the dynamic record stride is read from the wrong push dword.
kyty_pv_make_mutant(stride_dword "${KYTY_PV_EMITTER_REL}"
	"constexpr uint32_t kStrideDword = kPvPushStride;" "constexpr uint32_t kStrideDword = kPvPushSpan;")

# CPU test of the helpers and its mutants (a mutated header copy comes first in the include path).
add_executable(per_vertex_vsstore_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/PerVertexVsStoreTests.cpp")
target_include_directories(per_vertex_vsstore_tests PRIVATE ${inc_headers})
foreach(mutant IN LISTS KYTY_PV_HEADER_MUTANTS)
	add_executable(per_vertex_vsstore_mutant_${mutant}_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/PerVertexVsStoreTests.cpp")
	target_include_directories(per_vertex_vsstore_mutant_${mutant}_tests BEFORE PRIVATE "${KYTY_PV_MUTANT_DIR}/${mutant}")
	target_include_directories(per_vertex_vsstore_mutant_${mutant}_tests PRIVATE ${inc_headers})
endforeach()
# Seams of the test file itself: the index descriptor clamp / tail check switched off.
foreach(mutant IN ITEMS NO_CLAMP NO_TAIL_CHECK)
	string(TOLOWER ${mutant} mutant_lower)
	add_executable(per_vertex_vsstore_test_mutant_${mutant_lower}_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/PerVertexVsStoreTests.cpp")
	target_include_directories(per_vertex_vsstore_test_mutant_${mutant_lower}_tests PRIVATE ${inc_headers})
	target_compile_definitions(per_vertex_vsstore_test_mutant_${mutant_lower}_tests PRIVATE KYTY_PVVS_TEST_MUTANT_${mutant}=1)
endforeach()

# Emitter mutant: the PS asks for the Geometry capability (a feature the device does not enable) again.
kyty_pv_make_mutant(capability_geometry "${KYTY_PV_EMITTER_REL}"
	"RequireCapability(spv::CapabilityTessellation);" "RequireCapability(spv::CapabilityGeometry);")

# Emitter mutant: location 31 aborts the emission again.
kyty_pv_make_mutant(location31_exit "${KYTY_PV_EMITTER_REL}"
	"EXIT_IF(host_location > kPvVsStoreInstanceLocation);" "EXIT_IF(host_location >= kPvVsStoreInstanceLocation);")

# Emitter tests (full emulator build): the shader_cfg_tests binary checks the VS-store emission; its mutants
# swap the first and third corner in the header and the stride push dword in the emitter.
add_kyty_full_emulator_test(shader_cfg_pv_vsstore_mutant_first_third_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp")
target_include_directories(shader_cfg_pv_vsstore_mutant_first_third_tests BEFORE PRIVATE "${KYTY_PV_MUTANT_DIR}/first_third")
add_kyty_full_emulator_test(shader_cfg_pv_vsstore_mutant_stride_dword_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp"
	"${KYTY_SOURCE_DIR}/${KYTY_PV_EMITTER_REL}" "${KYTY_PV_MUTANT_DIR}/stride_dword/${KYTY_PV_EMITTER_REL}")
add_kyty_full_emulator_test(shader_cfg_pv_vsstore_mutant_capability_geometry_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp"
	"${KYTY_SOURCE_DIR}/${KYTY_PV_EMITTER_REL}" "${KYTY_PV_MUTANT_DIR}/capability_geometry/${KYTY_PV_EMITTER_REL}")
add_kyty_full_emulator_test(shader_cfg_pv_vsstore_mutant_location31_exit_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp"
	"${KYTY_SOURCE_DIR}/${KYTY_PV_EMITTER_REL}" "${KYTY_PV_MUTANT_DIR}/location31_exit/${KYTY_PV_EMITTER_REL}")

# Runtime mutant: RememberPvVsStoreShader calls EmitProgram with four arguments, so the pv_store pointer lands on the
# maintenance8 flag and the stored module is the plain one.
kyty_pv_make_mutant(args4 "${KYTY_PV_RUNTIME_REL}"
	"push_data_start_dword,\n\t                                                  options.maintenance8_enabled, false, &info);"
	"push_data_start_dword, &info);")
add_kyty_full_emulator_test(shader_cfg_pv_vsstore_mutant_args4_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp"
	"${KYTY_SOURCE_DIR}/${KYTY_PV_RUNTIME_REL}" "${KYTY_PV_MUTANT_DIR}/args4/${KYTY_PV_RUNTIME_REL}")
