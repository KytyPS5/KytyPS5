#include "ShaderCaptureMetadata.h"

#include <limits>
#include <stdexcept>
#include <type_traits>
namespace Libs::Graphics::ShaderRecompiler::Capture {
namespace {
template <class T>
void Scalar(const nlohmann::ordered_json& value, T& result) {
	if constexpr (std::is_same_v<T, bool>) {
		if (!value.is_boolean()) throw std::runtime_error("capture boolean required");
		result = value.get<bool>();
	} else if constexpr (std::is_enum_v<T>) {
		std::underlying_type_t<T> raw {};
		Scalar(value, raw);
		result = static_cast<T>(raw);
	} else if constexpr (std::is_unsigned_v<T>) {
		uint64_t raw;
		if (value.is_number_unsigned())
			raw = value.get<uint64_t>();
		else if (value.is_number_integer() && value.get<int64_t>() >= 0)
			raw = static_cast<uint64_t>(value.get<int64_t>());
		else
			throw std::runtime_error("capture unsigned integer required");
		if (raw > std::numeric_limits<T>::max())
			throw std::runtime_error("capture integer overflow");
		result = static_cast<T>(raw);
	} else {
		int64_t raw;
		if (value.is_number_unsigned()) {
			const auto number = value.get<uint64_t>();
			if (number > static_cast<uint64_t>(std::numeric_limits<T>::max()))
				throw std::runtime_error("capture integer overflow");
			raw = static_cast<int64_t>(number);
		} else if (value.is_number_integer())
			raw = value.get<int64_t>();
		else
			throw std::runtime_error("capture integer required");
		if (raw < std::numeric_limits<T>::min() || raw > std::numeric_limits<T>::max())
			throw std::runtime_error("capture integer overflow");
		result = static_cast<T>(raw);
	}
}
template <class T, size_t N>
void Array(const nlohmann::ordered_json& value, T (&result)[N]) {
	if (!value.is_array() || value.size() != N)
		throw std::runtime_error("capture array length mismatch");
	for (size_t i = 0; i < N; ++i)
		Scalar(value.at(i), result[i]);
}
void Floating(const nlohmann::ordered_json& value, ShaderFloatingPointState& result) {
	Scalar(value.at("known"), result.known);
	Scalar(value.at("float_mode"), result.float_mode);
	Scalar(value.at("ieee_mode"), result.ieee_mode);
	Scalar(value.at("dx10_clamp"), result.dx10_clamp);
}
void Wave(uint32_t value) {
	if (value != 32u && value != 64u) throw std::runtime_error("capture wave size invalid");
}
} // namespace
void ReadPixelInputMetadata(const nlohmann::ordered_json& input, ShaderPixelInputInfo& result) {
	ShaderPixelInputInfo parsed;
	Floating(input.at("initial_fp_state"), parsed.initial_fp_state);
	Scalar(input.at("input_num"), parsed.input_num);
	Scalar(input.at("wave_size"), parsed.wave_size);
	Scalar(input.at("ps_system_input_base"), parsed.ps_system_input_base);
	Scalar(input.at("custom_interpolation_mask"), parsed.custom_interpolation_mask);
	Scalar(input.at("ps_perspective_center_vgpr"), parsed.ps_perspective_center_vgpr);
	Scalar(input.at("ps_perspective_centroid_vgpr"), parsed.ps_perspective_centroid_vgpr);
	Scalar(input.at("scratch_size_dwords"), parsed.scratch_size_dwords);
	Scalar(input.at("ps_pos_x"), parsed.ps_pos_x);
	Scalar(input.at("ps_pos_y"), parsed.ps_pos_y);
	Scalar(input.at("ps_pos_z"), parsed.ps_pos_z);
	Scalar(input.at("ps_pos_w"), parsed.ps_pos_w);
	Scalar(input.at("ps_front_face"), parsed.ps_front_face);
	Scalar(input.at("ps_ancillary"), parsed.ps_ancillary);
	Scalar(input.at("ps_no_perspective"), parsed.ps_no_perspective);
	Scalar(input.at("ps_pixel_kill_enable"), parsed.ps_pixel_kill_enable);
	Scalar(input.at("ps_depth_export_enable"), parsed.ps_depth_export_enable);
	Scalar(input.at("ps_sample_mask_export_enable"), parsed.ps_sample_mask_export_enable);
	Scalar(input.at("ps_sample_shading"), parsed.ps_sample_shading);
	Scalar(input.at("dual_source_blending"), parsed.dual_source_blending);
	Scalar(input.at("ps_early_z"), parsed.ps_early_z);
	Scalar(input.at("ps_execute_on_noop"), parsed.ps_execute_on_noop);
	Array(input.at("interpolator_settings"), parsed.interpolator_settings);
	Array(input.at("target_output_mode"), parsed.target_output_mode);
	const auto& mapping = input.at("target_export_mapping");
	const auto& formats = input.at("target_conversion_format");
	if (!mapping.is_array() || mapping.size() != 8u || !formats.is_array() || formats.size() != 8u)
		throw std::runtime_error("capture target count invalid");
	for (uint32_t i = 0; i < 8u; ++i) {
		Scalar(mapping.at(i), parsed.target_export_mapping[i].packed);
		Scalar(formats.at(i), parsed.target_conversion_format[i]);
	}
	Scalar(input.at("alpha_blend_source"), parsed.alpha_blend_source);
	if (static_cast<uint32_t>(parsed.alpha_blend_source) >
	        static_cast<uint32_t>(ShaderAlphaBlendSource::SourceAlphaZero) ||
	    parsed.input_num > 32u)
		throw std::runtime_error("capture pixel metadata invalid");
	Wave(parsed.wave_size);
	result = parsed;
}
void ReadComputeInputMetadata(const nlohmann::ordered_json& input, ShaderComputeInputInfo& result) {
	ShaderComputeInputInfo parsed;
	Floating(input.at("initial_fp_state"), parsed.initial_fp_state);
	Array(input.at("threads_num"), parsed.threads_num);
	Array(input.at("dispatch_threads_num"), parsed.dispatch_threads_num);
	Array(input.at("group_id"), parsed.group_id);
	Scalar(input.at("lds_size_dwords"), parsed.lds_size_dwords);
	Scalar(input.at("scratch_size_dwords"), parsed.scratch_size_dwords);
	Scalar(input.at("host_subgroup_size"), parsed.host_subgroup_size);
	Scalar(input.at("wave_size"), parsed.wave_size);
	Scalar(input.at("needs_lds_barriers"), parsed.needs_lds_barriers);
	Scalar(input.at("float_mode"), parsed.float_mode);
	Scalar(input.at("dispatch_thread_dimensions"), parsed.dispatch_thread_dimensions);
	Scalar(input.at("thread_ids_num"), parsed.thread_ids_num);
	Scalar(input.at("workgroup_register"), parsed.workgroup_register);
	Scalar(input.at("tg_size_en"), parsed.tg_size_en);
	Wave(parsed.wave_size);
	if (parsed.host_subgroup_size == 0u || parsed.thread_ids_num < 0 || parsed.thread_ids_num > 3 ||
	    parsed.workgroup_register < 0 || parsed.workgroup_register >= 104)
		throw std::runtime_error("capture compute metadata invalid");
	result = parsed;
}
std::vector<uint32_t> ReadCapturedUserData(const nlohmann::ordered_json& manifest) {
    uint32_t count, base;
    Scalar(manifest.at("user_data_count"), count);
    Scalar(manifest.at("user_data_base"), base);
    const auto& data = manifest.at("user_data");
    if (count > 104u || base > 104u - count || !data.is_array() || data.size() != count)
        throw std::runtime_error("capture userdata count invalid");
    std::vector<uint32_t> words(count);
    for (size_t i = 0; i < count; ++i) Scalar(data.at(i), words[i]);
    return words;
}

void ReadHostProfileMetadata(const nlohmann::ordered_json& input, ShaderHostProfile& result) {
    ShaderHostProfile parsed;
#define READ_HOST(field) Scalar(input.at(#field), parsed.field)
    READ_HOST(known);
    READ_HOST(storage_buffer_nonuniform_indexing);
    READ_HOST(sampled_image_nonuniform_indexing);
    READ_HOST(float64);
    READ_HOST(fma_float64);
    READ_HOST(rte_float64);
    READ_HOST(rte_float32);
    READ_HOST(signed_zero_inf_nan_preserve_float64);
#undef READ_HOST
    result = parsed;
}
void ReadWorkgroupLimitsMetadata(const nlohmann::ordered_json& input, ComputeWorkgroupLimits& result) {
    ComputeWorkgroupLimits parsed;
    uint32_t size[3];
    Array(input.at("max_size"), size);
    parsed.max_size = {size[0], size[1], size[2]};
    Scalar(input.at("max_invocations"), parsed.max_invocations);
    Scalar(input.at("native_subgroup_size"), parsed.native_subgroup_size);
    Scalar(input.at("can_require_subgroup_size_64"), parsed.can_require_subgroup_size_64);
    Scalar(input.at("max_shared_memory_bytes"), parsed.max_shared_memory_bytes);
    result = parsed;
}

nlohmann::ordered_json BuildResourceReadMetadata(const IR::SrtRuntime& runtime,
                                                const ResourceReadCapture& capture,
                                                bool succeeded, std::string_view reason) {
    nlohmann::ordered_json result {
        {"kind", "ordered_resource_callbacks"}, {"complete", capture.Complete()},
        {"materialization_succeeded", succeeded}, {"materialization_error", reason},
        {"shader_base", runtime.shader_base},
        {"read_memory", runtime.read_memory != nullptr},
        {"read_specialization_memory", runtime.read_specialization_memory != nullptr},
        {"clamp_memory_range", runtime.clamp_memory_range != nullptr},
        {"compute_workgroups_trusted", runtime.compute_workgroups_trusted},
        {"max_dense_buffers", runtime.max_dense_buffers},
        {"max_native_samplers", runtime.max_native_samplers},
        {"max_dense_images", runtime.max_dense_images},
        {"capture_scalar_selector_values", runtime.capture_scalar_selector_values},
        {"neighboring_stages_read_only", runtime.neighboring_stages_read_only},
        {"events", nlohmann::ordered_json::array()}
    };
    result["compute_workgroups"] = runtime.compute_workgroups
        ? nlohmann::ordered_json(*runtime.compute_workgroups) : nlohmann::ordered_json(nullptr);
    result["evaluation_workgroup_id"] = runtime.evaluation_workgroup_id
        ? nlohmann::ordered_json(*runtime.evaluation_workgroup_id) : nlohmann::ordered_json(nullptr);
    for (const auto& event : capture.Events())
        result["events"].push_back({{"kind", static_cast<uint8_t>(event.kind)},
            {"address", event.address}, {"requested_bytes", event.requested_bytes},
            {"succeeded", event.succeeded}, {"clamped_bytes", event.clamped_bytes},
            {"words", event.words}});
    return result;
}

std::vector<ReadEvent> ReadResourceReadMetadata(const nlohmann::ordered_json& input,
                                               IR::SrtRuntime& runtime) {
    bool complete;
    Scalar(input.at("complete"), complete);
    if (!complete || input.at("kind") != "ordered_resource_callbacks")
        throw std::runtime_error("complete ordered resource capture required");
    IR::SrtRuntime parsed;
    parsed.user_data = runtime.user_data;
    Scalar(input.at("shader_base"), parsed.shader_base);
    Scalar(input.at("compute_workgroups_trusted"), parsed.compute_workgroups_trusted);
    Scalar(input.at("max_dense_buffers"), parsed.max_dense_buffers);
    Scalar(input.at("max_native_samplers"), parsed.max_native_samplers);
    Scalar(input.at("max_dense_images"), parsed.max_dense_images);
    Scalar(input.at("capture_scalar_selector_values"), parsed.capture_scalar_selector_values);
    Scalar(input.at("neighboring_stages_read_only"), parsed.neighboring_stages_read_only);
    const auto triple = [&](const char* name, auto& destination) {
        const auto& value = input.at(name);
        if (value.is_null()) return;
        uint32_t words[3]; Array(value, words);
        destination = std::array<uint32_t, 3>{words[0], words[1], words[2]};
    };
    triple("compute_workgroups", parsed.compute_workgroups);
    triple("evaluation_workgroup_id", parsed.evaluation_workgroup_id);
    bool normal, strict, clamp;
    Scalar(input.at("read_memory"), normal);
    Scalar(input.at("read_specialization_memory"), strict);
    Scalar(input.at("clamp_memory_range"), clamp);
    // These placeholders describe presence only. ResourceReadReplay replaces them.
    const auto unavailable = +[](void*, uint64_t, std::span<uint32_t>) { return false; };
    parsed.read_memory = normal ? unavailable : nullptr;
    parsed.read_specialization_memory = strict ? unavailable : nullptr;
    parsed.clamp_memory_range = clamp ? +[](void*, uint64_t, uint64_t) -> uint64_t { return 0; } : nullptr;
    const auto& entries = input.at("events");
    if (!entries.is_array() || entries.size() > 131072u)
        throw std::runtime_error("capture event count exceeds bound");
    std::vector<ReadEvent> events;
    size_t total_words = 0;
    for (const auto& entry : entries) {
        ReadEvent event {};
        Scalar(entry.at("kind"), event.kind);
        Scalar(entry.at("address"), event.address);
        Scalar(entry.at("requested_bytes"), event.requested_bytes);
        Scalar(entry.at("succeeded"), event.succeeded);
        Scalar(entry.at("clamped_bytes"), event.clamped_bytes);
        const auto& words = entry.at("words");
        if (!words.is_array() || words.size() > 2u * 1024u * 1024u - total_words)
            throw std::runtime_error("capture resource payload exceeds bound");
        if (event.kind == ReadKind::Clamp) {
            if (!clamp || !event.succeeded || !words.empty() || event.clamped_bytes > event.requested_bytes)
                throw std::runtime_error("invalid captured clamp");
        } else if (event.kind == ReadKind::Normal || event.kind == ReadKind::Strict) {
            if (!(event.kind == ReadKind::Normal ? normal : strict) ||
                event.requested_bytes % 4u ||
                (event.succeeded ? words.size() != event.requested_bytes / 4u : !words.empty()))
                throw std::runtime_error("invalid captured resource read");
        } else throw std::runtime_error("invalid captured resource callback kind");
        event.words.resize(words.size());
        for (size_t i = 0; i < words.size(); ++i) Scalar(words.at(i), event.words[i]);
        total_words += words.size();
        events.push_back(std::move(event));
    }
    runtime = parsed;
    return events;
}
} // namespace Libs::Graphics::ShaderRecompiler::Capture
