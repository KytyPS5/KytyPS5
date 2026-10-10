#include "ShaderCaptureMetadata.h"

#include <bit>
#include <fmt/format.h>
#include <stdexcept>
#include <xxhash.h>
namespace Libs::Graphics::ShaderRecompiler::Capture {
nlohmann::ordered_json
BuildCompileMetadata(const ShaderParams& params, const CompileOptions& options,
                     std::span<const uint32_t>              static_state,
                     std::optional<std::array<uint32_t, 3>> guest_workgroups) {
	const char* stage        = options.stage == ShaderType::Compute  ? "compute"
	                           : options.stage == ShaderType::Vertex ? "vertex"
	                           : options.stage == ShaderType::Pixel  ? "pixel"
	                                                                 : "unknown";
	const auto  content_hash = XXH3_64bits(params.code.data(), params.code.size_bytes());
	const auto  state_hash   = XXH3_64bits(static_state.data(), static_state.size_bytes());
	const auto  stem = fmt::format("{}_{:016x}_{:016x}", stage, options.shader_hash, state_hash);
	nlohmann::ordered_json metadata {
	    {"schema_version", 2},
	    {"kind", "dispatched"},
	    {"stage", stage},
	    {"shader_hash", fmt::format("{:016x}", options.shader_hash)},
	    {"content_hash_xxh3_64", fmt::format("{:016x}", content_hash)},
	    {"static_state_hash_xxh3_64", fmt::format("{:016x}", state_hash)},
	    {"code_file", stem + ".bin"},
	    {"code_size_bytes", params.code.size_bytes()},
	    {"wave_size", options.wave_size},
	    {"user_data_base", options.user_data_base},
	    {"user_data_count", options.user_data.size()},
	    {"user_data", std::vector<uint32_t>(options.user_data.begin(), options.user_data.end())},
	    {"runtime_resources_captured", false},
	    {"back_code_size_bytes", options.back_code.size_bytes()},
	    {"scratch_dwords", options.stage == ShaderType::Compute && options.input_info.compute
	                           ? options.input_info.compute->scratch_size_dwords
	                       : options.stage == ShaderType::Pixel && options.input_info.pixel
	                           ? options.input_info.pixel->scratch_size_dwords
	                       : options.stage == ShaderType::Vertex && options.input_info.vertex
	                           ? options.input_info.vertex->scratch_size_dwords
	                           : 0u},
	    {"metadata_complete", false},
	    {"host_profile",
	     {{"known", options.host_profile.known},
	      {"storage_buffer_nonuniform_indexing",
	       options.host_profile.storage_buffer_nonuniform_indexing},
	      {"sampled_image_nonuniform_indexing",
	       options.host_profile.sampled_image_nonuniform_indexing},
	      {"float64", options.host_profile.float64},
	      {"fma_float64", options.host_profile.fma_float64},
	      {"rte_float64", options.host_profile.rte_float64},
	      {"rte_float32", options.host_profile.rte_float32},
	      {"signed_zero_inf_nan_preserve_float64",
	       options.host_profile.signed_zero_inf_nan_preserve_float64}}},
	    {"static_state", std::vector<uint32_t>(static_state.begin(), static_state.end())},
	};
	metadata["compute_workgroup_limits"] = {
	    {"max_size", options.compute_workgroup_limits.max_size},
	    {"max_invocations", options.compute_workgroup_limits.max_invocations},
	    {"native_subgroup_size", options.compute_workgroup_limits.native_subgroup_size},
	    {"can_require_subgroup_size_64", options.compute_workgroup_limits.can_require_subgroup_size_64},
	    {"max_shared_memory_bytes", options.compute_workgroup_limits.max_shared_memory_bytes},
	};
	if (options.stage == ShaderType::Compute && options.input_info.compute != nullptr) {
		const auto& input             = *options.input_info.compute;
		metadata["metadata_complete"] = true;
		metadata["compute"]           = {
            {"initial_fp_state",
		               {{"known", input.initial_fp_state.known},
		                {"float_mode", input.initial_fp_state.float_mode},
		                {"ieee_mode", input.initial_fp_state.ieee_mode},
		                {"dx10_clamp", input.initial_fp_state.dx10_clamp}}},
            {"threads_num",
		               std::array {input.threads_num[0], input.threads_num[1], input.threads_num[2]}},
            {"dispatch_threads_num",
		               std::array {input.dispatch_threads_num[0], input.dispatch_threads_num[1],
                         input.dispatch_threads_num[2]}},
            {"lds_size_dwords", input.lds_size_dwords},
            {"scratch_size_dwords", input.scratch_size_dwords},
            {"group_id", std::array {input.group_id[0], input.group_id[1], input.group_id[2]}},
            {"dispatch_thread_dimensions", input.dispatch_thread_dimensions},
            {"needs_lds_barriers", input.needs_lds_barriers},
            {"wave_size", input.wave_size},
            {"thread_ids_num", input.thread_ids_num},
            {"host_subgroup_size", input.host_subgroup_size},
            {"float_mode", input.float_mode},
            {"workgroup_register", input.workgroup_register},
            {"tg_size_en", input.tg_size_en},
        };
		if (guest_workgroups.has_value()) {
			metadata["compute"]["guest_workgroups"] = *guest_workgroups;
		}
	}

	if (options.stage == ShaderType::Pixel && options.input_info.pixel != nullptr) {
		const auto&            input = *options.input_info.pixel;
		nlohmann::ordered_json pixel;
		pixel["initial_fp_state"] = {{"known", input.initial_fp_state.known},
		                             {"float_mode", input.initial_fp_state.float_mode},
		                             {"ieee_mode", input.initial_fp_state.ieee_mode},
		                             {"dx10_clamp", input.initial_fp_state.dx10_clamp}};
#define CAPTURE_FIELD(field) pixel[#field] = input.field
		CAPTURE_FIELD(input_num);
		CAPTURE_FIELD(wave_size);
		CAPTURE_FIELD(ps_system_input_base);
		CAPTURE_FIELD(custom_interpolation_mask);
		CAPTURE_FIELD(ps_perspective_center_vgpr);
		CAPTURE_FIELD(ps_perspective_centroid_vgpr);
		CAPTURE_FIELD(scratch_size_dwords);
		CAPTURE_FIELD(ps_pos_x);
		CAPTURE_FIELD(ps_pos_y);
		CAPTURE_FIELD(ps_pos_z);
		CAPTURE_FIELD(ps_pos_w);
		CAPTURE_FIELD(ps_front_face);
		CAPTURE_FIELD(ps_ancillary);
		CAPTURE_FIELD(ps_no_perspective);
		CAPTURE_FIELD(ps_pixel_kill_enable);
		CAPTURE_FIELD(ps_depth_export_enable);
		CAPTURE_FIELD(ps_sample_mask_export_enable);
		CAPTURE_FIELD(ps_sample_shading);
		CAPTURE_FIELD(dual_source_blending);
		CAPTURE_FIELD(ps_early_z);
		CAPTURE_FIELD(ps_execute_on_noop);
#undef CAPTURE_FIELD
		pixel["interpolator_settings"] = std::vector<uint32_t>(
		    std::begin(input.interpolator_settings), std::end(input.interpolator_settings));
		pixel["target_output_mode"]    = std::vector<uint8_t>(std::begin(input.target_output_mode),
		                                                      std::end(input.target_output_mode));
		pixel["target_export_mapping"] = nlohmann::ordered_json::array();
		pixel["target_conversion_format"] = nlohmann::ordered_json::array();
		for (uint32_t target = 0; target < 8u; ++target) {
			pixel["target_export_mapping"].push_back(input.target_export_mapping[target].packed);
			pixel["target_conversion_format"].push_back(
			    static_cast<uint32_t>(input.target_conversion_format[target]));
		}
		pixel["alpha_blend_source"]   = static_cast<uint32_t>(input.alpha_blend_source);
		metadata["pixel"]             = std::move(pixel);
		metadata["metadata_complete"] = true;
	}
	if (options.stage == ShaderType::Vertex && options.input_info.vertex != nullptr) {
		const auto& input = *options.input_info.vertex;
		if (input.resources_num < 0 || input.resources_num > ShaderVertexInputInfo::RES_MAX ||
		    input.buffers_num < 0 || input.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		    input.linked_param_count > ShaderVertexInputInfo::PARAM_LINK_MAX)
			throw std::runtime_error("invalid vertex capture counts");
		nlohmann::ordered_json vertex;
		vertex["initial_fp_state"] = {{"known", input.initial_fp_state.known},
		                              {"float_mode", input.initial_fp_state.float_mode},
		                              {"ieee_mode", input.initial_fp_state.ieee_mode},
		                              {"dx10_clamp", input.initial_fp_state.dx10_clamp}};
#define CAPTURE_FIELD(field) vertex[#field] = input.field
		CAPTURE_FIELD(resources_num);
		CAPTURE_FIELD(buffers_num);
		CAPTURE_FIELD(fetch_attrib_reg);
		CAPTURE_FIELD(fetch_buffer_reg);
		CAPTURE_FIELD(wave_size);
		CAPTURE_FIELD(scratch_size_dwords);
		CAPTURE_FIELD(pa_cl_vs_out_cntl);
		CAPTURE_FIELD(linked_param_count);
		CAPTURE_FIELD(fetch_external);
		CAPTURE_FIELD(fetch_embedded);
#undef CAPTURE_FIELD
		vertex["logical_stage"]        = static_cast<uint32_t>(input.logical_stage);
		vertex["linked_param_sources"] = std::vector<uint32_t>(
		    std::begin(input.linked_param_sources), std::end(input.linked_param_sources));
		vertex["linked_param_locations"] = std::vector<uint32_t>(
		    std::begin(input.linked_param_locations), std::end(input.linked_param_locations));
		vertex["resources"]     = nlohmann::ordered_json::array();
		vertex["resources_dst"] = nlohmann::ordered_json::array();
		for (int index = 0; index < input.resources_num; ++index) {
			vertex["resources"].push_back(
			    std::vector<uint32_t>(std::begin(input.resources[index].fields),
			                          std::end(input.resources[index].fields)));
			const auto& dst = input.resources_dst[index];
			vertex["resources_dst"].push_back({{"register_start", dst.register_start},
			                                   {"registers_num", dst.registers_num},
			                                   {"attr_id", dst.attr_id},
			                                   {"fetch_index", dst.fetch_index}});
		}
		vertex["buffers"] = nlohmann::ordered_json::array();
		for (int index = 0; index < input.buffers_num; ++index) {
			const auto& buffer = input.buffers[index];
			if (buffer.attr_num < 0 || buffer.attr_num > ShaderVertexInputBuffer::ATTR_MAX)
				throw std::runtime_error("invalid vertex capture attribute count");
			vertex["buffers"].push_back(
			    {{"addr", buffer.addr},
			     {"stride", buffer.stride},
			     {"num_records", buffer.num_records},
			     {"fetch_index", buffer.fetch_index},
			     {"attr_num", buffer.attr_num},
			     {"attr_indices",
			      std::vector<int>(std::begin(buffer.attr_indices), std::end(buffer.attr_indices))},
			     {"attr_offsets", std::vector<uint32_t>(std::begin(buffer.attr_offsets),
			                                            std::end(buffer.attr_offsets))}});
		}
		const auto bits = [](const float(&values)[2]) {
			return std::array<uint32_t, 2> {std::bit_cast<uint32_t>(values[0]),
			                                std::bit_cast<uint32_t>(values[1])};
		};
		vertex["clip_space"] = {{"enabled", input.clip_space.enabled},
		                        {"scale_bits", bits(input.clip_space.scale)},
		                        {"offset_bits", bits(input.clip_space.offset)},
		                        {"half_extent_bits", bits(input.clip_space.half_extent)}};
		const auto& mesh     = input.mesh;
		vertex["mesh"]       = {
            {"threads_num", std::array<uint32_t, 3> {mesh.threads_num[0], mesh.threads_num[1],
		                                                   mesh.threads_num[2]}},
            {"lds_size_dwords", mesh.lds_size_dwords},
            {"scratch_size_dwords", mesh.scratch_size_dwords},
            {"host_subgroup_size", mesh.host_subgroup_size},
            {"wave_size", mesh.wave_size},
            {"input_primitive", mesh.input_primitive},
            {"primitives_per_group", mesh.primitives_per_group},
            {"vertices_per_group", mesh.vertices_per_group},
            {"max_vertices", mesh.max_vertices},
            {"max_primitives", mesh.max_primitives},
            {"provoking_vertex", mesh.provoking_vertex}};
		const auto& tess              = input.tess;
		vertex["tess"]                = {{"input_control_points", tess.input_control_points},
		                                 {"output_control_points", tess.output_control_points},
		                                 {"ls_stride", tess.ls_stride},
		                                 {"hs_stride", tess.hs_stride},
		                                 {"domain", tess.domain},
		                                 {"partitioning", tess.partitioning},
		                                 {"output_topology", tess.output_topology}};
		metadata["vertex"]            = std::move(vertex);
		metadata["metadata_complete"] = true;
	}
	if (!options.back_code.empty()) {
		metadata["back_code_file"]         = stem + "_back.bin";
		metadata["back_code_hash_xxh3_64"] = fmt::format(
		    "{:016x}", XXH3_64bits(options.back_code.data(), options.back_code.size_bytes()));
	}
	// ShaderStageRuntime contains compiled outputs/host handles, never compiler inputs.
	metadata["compiled_output_handles_omitted"] = true;

	return metadata;
}
} // namespace Libs::Graphics::ShaderRecompiler::Capture
