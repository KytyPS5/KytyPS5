#include "graphics/host_gpu/renderer/pipeline/pipelineManifest.h"

#include "common/alignment.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/parallelFor.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/nativeRayTracing.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/Serialize.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/fileSystem.h"
#include "loader/systemContent.h"

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <locale>
#include <magic_enum.hpp>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <sstream>
#include <stop_token>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>
#include <zstd.h>

// Buffer formats go up to the FMASK formats.
template <>
struct magic_enum::customize::enum_range<Libs::Graphics::Prospero::BufferFormat> {
	static constexpr int min = 0;
	static constexpr int max = 255;
};

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::IoAll;
using ShaderRecompiler::IR::ResourceSpecialization;
using ShaderRecompiler::IR::Writer;

// The text form, one record per line:
//   M <module id> <stage> code=<hash> [back=<hash>] [ud=<n>] [cur=<n>] <stage input>
//   <specialization> P v=<module id> [h=<id>] [d=<id>] [p=<id>] [plain] <pipeline state> T <layout
//   key> <tessellation layout>
// Fields are key=value or a bare key for a set flag; a field that is missing takes its default
// and an unknown one is ignored, so new fields need no new format. Lists are comma separated;
// the elements of vertex inputs are dot separated tuples and those of a specialization slash
// separated key:value fields ("-" for an element that is all defaults). A module id is the hash
// of the rest of its line, which pipelines name it by.
constexpr std::string_view ManifestHeader = "# kyty pipeline manifest v";

// The stage inputs are visited field by field. The structured bindings fail to compile when a
// field is added or removed, the size checks do so for structures with a base: give a new field a
// name here. Files without it read its default; ManifestFormat changes only when the meaning of a
// field that files hold changes.
static_assert(sizeof(ShaderMeshInputInfo) == 60, "name the new field");
static_assert(sizeof(ShaderComputeInputInfo) == 104, "name the new field");
static_assert(sizeof(PipelineRenderingState) == 44, "name the new field");
static_assert(sizeof(PipelineVertexInputState) == 322, "name the new field");
static_assert(sizeof(PipelineStaticParameters) == 118, "name the new field");

std::string_view TrimPrefix(std::string_view name, std::string_view prefix) {
	return name.starts_with(prefix) && name.size() > prefix.size() ? name.substr(prefix.size())
	                                                               : name;
}

// vk enums: vulkan-hpp's names without the 'e', or the number for values it does not name.
template <class E>
std::string VkName(E value) {
	auto name = vk::to_string(value);
	if (name.empty() || name.find_first_of(" (") != std::string::npos) {
		return std::to_string(static_cast<int64_t>(value));
	}
	return name;
}

template <class E>
const std::unordered_map<std::string, E>& VkNames() {
	static const auto names = [] {
		std::unordered_map<std::string, E> map;
		const auto                         add = [&](int64_t value) {
			const auto name = vk::to_string(static_cast<E>(value));
			if (!name.empty() && name.find_first_of(" (") == std::string::npos) {
				map.emplace(name, static_cast<E>(value));
			}
		};
		for (int64_t value = 0; value < 512; value++) {
			add(value);
		}
		// Extension values: 1000000000 + (extension number - 1) * 1000 + offset.
		for (const int64_t extension: {54, 66, 153, 156, 330, 340, 460, 464, 470, 609}) {
			for (int64_t offset = 0; offset < 100; offset++) {
				add(1000000000 + extension * 1000 + offset);
			}
		}
		return map;
	}();
	return names;
}

bool ParseNumber(std::string_view text, uint64_t& value, int base = 10) {
	if (text.empty()) return false;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
	return error == std::errc() && end == text.data() + text.size();
}

bool ParseSigned(std::string_view text, int64_t& value) {
	if (text.empty()) return false;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc() && end == text.data() + text.size();
}

// Floating-point std::from_chars is missing from some standard libraries and Apple's libc++ makes
// it available only from macOS 26, so those parse with a stream in the classic locale instead.
bool ParseFloat(std::string_view text, float& value) {
	if (text.empty()) return false;
#if defined(__cpp_lib_to_chars) && !defined(__APPLE__)
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc() && end == text.data() + text.size();
#else
	std::istringstream stream {std::string(text)};
	stream.imbue(std::locale::classic());
	stream >> value;
	return !stream.fail() && stream.eof();
#endif
}

template <class E>
bool ParseVk(std::string_view text, E& value) {
	int64_t number = 0;
	if (ParseSigned(text, number)) {
		value = static_cast<E>(number);
		return true;
	}
	const auto& names = VkNames<E>();
	const auto  found = names.find(std::string(text));
	if (found == names.end()) return false;
	value = found->second;
	return true;
}

// Guest enums: magic_enum's names without their prefix.
std::string BufferFormatName(Prospero::BufferFormat format) {
	const auto name = magic_enum::enum_name(format);
	return name.empty() ? std::to_string(static_cast<uint32_t>(format))
	                    : std::string(TrimPrefix(name, "k"));
}

bool ParseBufferFormat(std::string_view text, Prospero::BufferFormat& format) {
	uint64_t number = 0;
	if (ParseNumber(text, number)) {
		format = static_cast<Prospero::BufferFormat>(number);
		return true;
	}
	const auto value = magic_enum::enum_cast<Prospero::BufferFormat>("k" + std::string(text));
	if (!value) return false;
	format = *value;
	return true;
}

std::string DimensionName(ShaderRecompiler::Decoder::ImageDimension dimension) {
	const auto name = magic_enum::enum_name(dimension);
	return name.empty() ? std::to_string(static_cast<uint32_t>(dimension))
	                    : std::string(TrimPrefix(name, "Dim"));
}

bool ParseDimension(std::string_view text, ShaderRecompiler::Decoder::ImageDimension& dimension) {
	for (const auto value: magic_enum::enum_values<ShaderRecompiler::Decoder::ImageDimension>()) {
		if (DimensionName(value) == text) {
			dimension = value;
			return true;
		}
	}
	return false;
}

template <class E>
std::string GuestName(E value) {
	const auto name = magic_enum::enum_name(value);
	return name.empty() ? std::to_string(static_cast<int64_t>(value)) : std::string(name);
}

template <class E>
bool ParseGuest(std::string_view text, E& value) {
	int64_t number = 0;
	if (ParseSigned(text, number)) {
		value = static_cast<E>(number);
		return true;
	}
	const auto parsed = magic_enum::enum_cast<E>(text);
	if (!parsed) return false;
	value = *parsed;
	return true;
}

constexpr std::array<std::pair<ShaderType, std::string_view>, 7> StageNames {{
    {ShaderType::Vertex, "vs"},
    {ShaderType::Pixel, "ps"},
    {ShaderType::Compute, "cs"},
    {ShaderType::Mesh, "mesh"},
    {ShaderType::Local, "ls"},
    {ShaderType::TessellationControl, "hs"},
    {ShaderType::TessellationEvaluation, "tes"},
}};

std::string_view StageName(ShaderType stage) {
	const auto found =
	    std::ranges::find(StageNames, stage, &std::pair<ShaderType, std::string_view>::first);
	return found == StageNames.end() ? std::string_view {} : found->second;
}

bool ParseStage(std::string_view text, ShaderType& stage) {
	const auto found =
	    std::ranges::find(StageNames, text, &std::pair<ShaderType, std::string_view>::second);
	if (found == StageNames.end()) return false;
	stage = found->first;
	return true;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
	std::vector<std::string_view> parts;
	for (size_t start = 0;;) {
		const auto end = text.find(separator, start);
		parts.push_back(text.substr(start, end == std::string_view::npos ? end : end - start));
		if (end == std::string_view::npos) return parts;
		start = end + 1;
	}
}

// Writes the fields that differ from their default.
class TextWriter {
public:
	static constexpr bool Reading = false;
	explicit TextWriter(std::string& text): m_text(text) {}

	void Value(std::string_view key, std::string_view value) {
		m_text += ' ';
		m_text += key;
		m_text += '=';
		m_text += value;
	}
	template <class T>
	void Dec(std::string_view key, T& value, T def) {
		if (value != def)
			Value(key, std::to_string(
			               static_cast<std::conditional_t<std::is_signed_v<T>, int64_t, uint64_t>>(
			                   value)));
	}
	template <class T>
	void Hex(std::string_view key, T& value, T def) {
		if (value != def)
			Value(key, fmt::format("{:x}", static_cast<uint64_t>(
			                                   static_cast<std::make_unsigned_t<T>>(value))));
	}
	void Flag(std::string_view key, bool& value, bool def = false) {
		if (value != def) {
			m_text += ' ';
			m_text += key;
		}
	}
	// Trailing elements equal to their default are left out.
	template <class T, size_t N>
	void DecList(std::string_view key, T (&values)[N], const T (&defs)[N]) {
		size_t used = N;
		while (used > 0 && values[used - 1] == defs[used - 1])
			used--;
		if (used > 0) {
			Value(key, fmt::format("{}", fmt::join(std::span(values).first(used), ",")));
		}
	}
	void FloatList(std::string_view key, float (&values)[2], const float (&defs)[2]) {
		if (std::memcmp(values, defs, sizeof(values)) != 0) {
			std::string text;
			for (const float value: values) {
				char       buffer[32];
				const auto end = std::to_chars(buffer, buffer + sizeof(buffer), value).ptr;
				text += text.empty() ? "" : ",";
				text.append(buffer, end);
			}
			Value(key, text);
		}
	}
	template <class E>
	void Guest(std::string_view key, E& value, E def) {
		if (value != def) Value(key, GuestName(value));
	}

private:
	std::string& m_text;
};

// Reads the fields of a record; a missing one keeps its default.
class TextReader {
public:
	static constexpr bool Reading = true;
	bool                  ok      = true;

	explicit TextReader(std::span<const std::string_view> tokens) {
		for (const auto token: tokens) {
			const auto equals = token.find('=');
			m_fields.emplace_back(token.substr(0, equals), equals == std::string_view::npos
			                                                   ? std::optional<std::string_view> {}
			                                                   : token.substr(equals + 1));
		}
	}
	// A key=value field; nullopt when missing, and a flag reads as an empty value.
	[[nodiscard]] std::optional<std::string_view> Find(std::string_view key) const {
		for (const auto& [name, value]: m_fields) {
			if (name == key) return value.value_or(std::string_view {});
		}
		return {};
	}
	template <class T>
	void Dec(std::string_view key, T& value, T /*def*/) {
		if (const auto text = Find(key)) {
			int64_t number = 0;
			ok             = ok && ParseSigned(*text, number);
			value          = static_cast<T>(number);
		}
	}
	template <class T>
	void Hex(std::string_view key, T& value, T /*def*/) {
		if (const auto text = Find(key)) {
			uint64_t number = 0;
			ok              = ok && ParseNumber(*text, number, 16);
			value           = static_cast<T>(number);
		}
	}
	void Flag(std::string_view key, bool& value, bool def = false) {
		value = Find(key) ? !def : def;
	}
	template <class T, size_t N>
	void DecList(std::string_view key, T (&values)[N], const T (& /*defs*/)[N]) {
		if (const auto text = Find(key)) {
			const auto parts = Split(*text, ',');
			ok               = ok && parts.size() <= N;
			for (size_t i = 0; ok && i < parts.size(); i++) {
				int64_t number = 0;
				ok             = ParseSigned(parts[i], number);
				values[i]      = static_cast<T>(number);
			}
		}
	}
	void FloatList(std::string_view key, float (&values)[2], const float (& /*defs*/)[2]) {
		if (const auto text = Find(key)) {
			const auto parts = Split(*text, ',');
			ok               = ok && parts.size() == 2;
			for (size_t i = 0; ok && i < 2; i++) {
				ok = ParseFloat(parts[i], values[i]);
			}
		}
	}
	template <class E>
	void Guest(std::string_view key, E& value, E /*def*/) {
		if (const auto text = Find(key)) {
			ok = ok && ParseGuest(*text, value);
		}
	}

private:
	std::vector<std::pair<std::string_view, std::optional<std::string_view>>> m_fields;
};

template <class V>
void VisitWorkgroup(V& v, std::string_view prefix, ShaderWorkgroupInputInfo& info,
                    const ShaderWorkgroupInputInfo& base) {
	auto& [threads_num, lds_size_dwords, scratch_size_dwords, host_subgroup_size, wave_size] = info;
	const auto key = [&](std::string_view name) { return std::string(prefix) + std::string(name); };
	v.DecList(key("t"), threads_num, base.threads_num);
	v.Dec(key("lds"), lds_size_dwords, base.lds_size_dwords);
	v.Dec(key("scratch"), scratch_size_dwords, base.scratch_size_dwords);
	v.Dec(key("sub"), host_subgroup_size, base.host_subgroup_size);
	v.Dec(key("wave"), wave_size, base.wave_size);
}

template <class V>
void VisitVertexInput(V& v, ShaderVertexInputInfo& info, const ShaderVertexInputInfo& base) {
	// The stage runtime is not kept, and the logical stage is the record's.
	auto& [resources, resources_dst, buffers, stage, logical_stage, resources_num, fetch_attrib_reg,
	       fetch_buffer_reg, buffers_num, wave_size, scratch_size_dwords, lds_size_dwords,
	       pa_cl_vs_out_cntl, user_clip_planes, clip_space, mesh, tess, indirect_offsets,
	       indirect_draw, has_dynamic_vertex_fetch] = info;
	v.Dec("wave", wave_size, base.wave_size);
	v.Dec("scratch", scratch_size_dwords, base.scratch_size_dwords);
	v.Dec("lds", lds_size_dwords, base.lds_size_dwords);
	v.Hex("vsout", pa_cl_vs_out_cntl, base.pa_cl_vs_out_cntl);
	v.Hex("ucp", user_clip_planes, base.user_clip_planes);
	v.Dec("fa", fetch_attrib_reg, base.fetch_attrib_reg);
	v.Dec("fb", fetch_buffer_reg, base.fetch_buffer_reg);
	v.Flag("dyn", has_dynamic_vertex_fetch, base.has_dynamic_vertex_fetch);
	auto& [vertex_offset, instance_offset] = indirect_offsets;
	v.Dec("iv", vertex_offset, base.indirect_offsets.vertex);
	v.Dec("ii", instance_offset, base.indirect_offsets.instance);
	v.Flag("idraw", indirect_draw, base.indirect_draw);
	auto& [scale, offset, half_extent, enabled] = clip_space;
	v.Flag("clip", enabled, base.clip_space.enabled);
	v.FloatList("cscale", scale, base.clip_space.scale);
	v.FloatList("coffset", offset, base.clip_space.offset);
	VisitWorkgroup(v, "m", mesh, base.mesh);
	v.Dec("mprim", mesh.input_primitive, base.mesh.input_primitive);
	v.Dec("mops", mesh.output_primitive_size, base.mesh.output_primitive_size);
	v.Dec("mppg", mesh.primitives_per_group, base.mesh.primitives_per_group);
	v.Dec("mvpg", mesh.vertices_per_group, base.mesh.vertices_per_group);
	v.Dec("mmaxv", mesh.max_vertices, base.mesh.max_vertices);
	v.Dec("mmaxp", mesh.max_primitives, base.mesh.max_primitives);
	v.Dec("mprov", mesh.provoking_vertex, base.mesh.provoking_vertex);
	v.Flag("mfast", mesh.fast_launch, base.mesh.fast_launch);
	auto& [input_control_points, output_control_points, ls_stride, hs_stride, patch_base,
	       patch_size, domain, partitioning, output_topology] = tess;
	v.Dec("ticp", input_control_points, base.tess.input_control_points);
	v.Dec("tocp", output_control_points, base.tess.output_control_points);
	v.Dec("tls", ls_stride, base.tess.ls_stride);
	v.Dec("ths", hs_stride, base.tess.hs_stride);
	v.Dec("tpb", patch_base, base.tess.patch_base);
	v.Dec("tps", patch_size, base.tess.patch_size);
	v.Dec("tdom", domain, base.tess.domain);
	v.Dec("tpart", partitioning, base.tess.partitioning);
	v.Dec("ttopo", output_topology, base.tess.output_topology);
	// Each attribute: V# words 1 (stride and flags; the address bits are clear), 2 (records) and 3
	// (format, swizzle), then register start, register count, attribute, fetch and buffer index.
	if constexpr (!V::Reading) {
		if (resources_num > 0) {
			std::string text;
			for (int i = 0; i < resources_num; i++) {
				const auto& [fields] = resources[i];
				const auto& [register_start, registers_num, attr_id, fetch_index, buffer_index] =
				    resources_dst[i];
				text += fmt::format("{}{:x}.{}.{:x}.{}.{}.{}.{}.{}", i == 0 ? "" : ",", fields[1],
				                    fields[2], fields[3], register_start, registers_num, attr_id,
				                    fetch_index, buffer_index);
			}
			v.Value("attr", text);
		}
		if (buffers_num > 0) {
			std::string text;
			for (int i = 0; i < buffers_num; i++) {
				const auto& [addr, stride, num_records, fetch_index] = buffers[i];
				text +=
				    fmt::format("{}{}.{}.{}", i == 0 ? "" : ",", stride, num_records, fetch_index);
			}
			v.Value("vbuf", text);
		}
	} else {
		const auto tuples = [&](std::string_view key, auto&& parse) -> int {
			const auto text = v.Find(key);
			if (!text) return 0;
			const auto items = Split(*text, ',');
			v.ok             = v.ok && items.size() <= ShaderVertexInputInfo::RES_MAX;
			for (size_t i = 0; v.ok && i < items.size(); i++) {
				parse(i, Split(items[i], '.'));
			}
			return static_cast<int>(items.size());
		};
		resources_num = tuples("attr", [&](size_t i, const std::vector<std::string_view>& parts) {
			uint64_t words[3] {};
			int64_t  dst[5] {0, 0, -1, 0, 0};
			v.ok = parts.size() <= 8;
			for (size_t j = 0; v.ok && j < parts.size(); j++) {
				v.ok = j < 3 ? ParseNumber(parts[j], words[j], j == 1 ? 10 : 16)
				             : ParseSigned(parts[j], dst[j - 3]);
			}
			resources[i].fields[0] = 0;
			resources[i].fields[1] = static_cast<uint32_t>(words[0]);
			resources[i].fields[2] = static_cast<uint32_t>(words[1]);
			resources[i].fields[3] = static_cast<uint32_t>(words[2]);
			resources_dst[i]       = {.register_start = static_cast<int>(dst[0]),
			                          .registers_num  = static_cast<int>(dst[1]),
			                          .attr_id        = static_cast<int>(dst[2]),
			                          .fetch_index    = static_cast<uint32_t>(dst[3]),
			                          .buffer_index   = static_cast<int>(dst[4])};
		});
		buffers_num   = tuples("vbuf", [&](size_t i, const std::vector<std::string_view>& parts) {
			uint64_t values[3] {};
			v.ok = parts.size() <= 3;
			for (size_t j = 0; v.ok && j < parts.size(); j++) {
				v.ok = ParseNumber(parts[j], values[j]);
			}
			buffers[i] = {.stride      = static_cast<uint32_t>(values[0]),
			              .num_records = static_cast<uint32_t>(values[1]),
			              .fetch_index = static_cast<uint32_t>(values[2])};
		});
	}
}

template <class V>
void VisitPixelInput(V& v, ShaderPixelInputInfo& info, const ShaderPixelInputInfo& base) {
	auto& [interpolator_settings, input_num, wave_size, ps_system_input_base,
	       custom_interpolation_mask, ps_perspective_center_vgpr, ps_perspective_sample_vgpr,
	       ps_perspective_centroid_vgpr, target_output_mode, target_shader_mask,
	       target_export_mapping, target_packed_unorm_mask, target_uint_mask, target_sint_mask,
	       target_round_mode, scratch_size_dwords, lds_size_dwords, ps_pos_x, ps_pos_y, ps_pos_z,
	       ps_pos_w, ps_front_face, ps_front_face_all_bits, ps_ancillary, ps_no_perspective,
	       parameter_mode, ps_pixel_kill_enable, ps_depth_export_enable,
	       ps_sample_mask_export_enable, ps_sample_shading, dual_source_blending,
	       object_primitive_id, alpha_blend_source, ps_early_z, stage] = info;
	v.Dec("wave", wave_size, base.wave_size);
	v.Dec("scratch", scratch_size_dwords, base.scratch_size_dwords);
	v.Dec("lds", lds_size_dwords, base.lds_size_dwords);
	v.Dec("sysbase", ps_system_input_base, base.ps_system_input_base);
	v.Hex("custom", custom_interpolation_mask, base.custom_interpolation_mask);
	v.Dec("center", ps_perspective_center_vgpr, base.ps_perspective_center_vgpr);
	v.Dec("sample", ps_perspective_sample_vgpr, base.ps_perspective_sample_vgpr);
	v.Dec("centroid", ps_perspective_centroid_vgpr, base.ps_perspective_centroid_vgpr);
	v.Hex("smask", target_shader_mask, base.target_shader_mask);
	v.Hex("unorm", target_packed_unorm_mask, base.target_packed_unorm_mask);
	v.Hex("uint", target_uint_mask, base.target_uint_mask);
	v.Hex("sint", target_sint_mask, base.target_sint_mask);
	v.Hex("round", target_round_mode, base.target_round_mode);
	v.DecList("omode", target_output_mode, base.target_output_mode);
	v.Flag("posx", ps_pos_x, base.ps_pos_x);
	v.Flag("posy", ps_pos_y, base.ps_pos_y);
	v.Flag("posz", ps_pos_z, base.ps_pos_z);
	v.Flag("posw", ps_pos_w, base.ps_pos_w);
	v.Flag("face", ps_front_face, base.ps_front_face);
	v.Flag("faceall", ps_front_face_all_bits, base.ps_front_face_all_bits);
	v.Flag("anc", ps_ancillary, base.ps_ancillary);
	v.Flag("noper", ps_no_perspective, base.ps_no_perspective);
	v.Guest("param", parameter_mode, base.parameter_mode);
	v.Flag("kill", ps_pixel_kill_enable, base.ps_pixel_kill_enable);
	v.Flag("zexp", ps_depth_export_enable, base.ps_depth_export_enable);
	v.Flag("mexp", ps_sample_mask_export_enable, base.ps_sample_mask_export_enable);
	v.Flag("sshade", ps_sample_shading, base.ps_sample_shading);
	v.Flag("dual", dual_source_blending, base.dual_source_blending);
	v.Flag("primid", object_primitive_id, base.object_primitive_id);
	v.Guest("asrc", alpha_blend_source, base.alpha_blend_source);
	v.Flag("early", ps_early_z, base.ps_early_z);
	// The export mapping of each target; the interpolators as many as there are inputs.
	if constexpr (!V::Reading) {
		if (!std::ranges::equal(target_export_mapping, base.target_export_mapping)) {
			std::string text;
			for (const auto& mapping: target_export_mapping) {
				text += fmt::format("{}{:x}", text.empty() ? "" : ",", mapping.packed);
			}
			v.Value("emap", text);
		}
		if (input_num > 0) {
			v.Value("in",
			        fmt::format("{:x}",
			                    fmt::join(std::span(interpolator_settings).first(input_num), ",")));
		}
	} else {
		if (const auto text = v.Find("emap")) {
			const auto parts = Split(*text, ',');
			v.ok             = v.ok && parts.size() == target_export_mapping.size();
			for (size_t i = 0; v.ok && i < parts.size(); i++) {
				uint64_t packed = 0;
				v.ok            = ParseNumber(parts[i], packed, 16) && packed <= 0xff;
				target_export_mapping[i].packed = static_cast<uint8_t>(packed);
			}
		}
		if (const auto text = v.Find("in")) {
			const auto parts = Split(*text, ',');
			v.ok             = v.ok && parts.size() <= std::size(interpolator_settings);
			input_num        = static_cast<uint32_t>(parts.size());
			for (size_t i = 0; v.ok && i < parts.size(); i++) {
				uint64_t value           = 0;
				v.ok                     = ParseNumber(parts[i], value, 16);
				interpolator_settings[i] = static_cast<uint32_t>(value);
			}
		}
	}
}

template <class V>
void VisitComputeInput(V& v, ShaderComputeInputInfo& info, const ShaderComputeInputInfo& base) {
	// Dispatch sizes and the indirect argument address are not kept.
	VisitWorkgroup(v, "", info, base);
	v.Hex("fm", info.float_mode, base.float_mode);
	v.Dec("tid", info.thread_ids_num, base.thread_ids_num);
	v.Dec("wgr", info.workgroup_register, base.workgroup_register);
	v.Flag("async", info.async_compute, base.async_compute);
	v.Flag("dims", info.dispatch_thread_dimensions, base.dispatch_thread_dimensions);
	v.Flag("ldsbuf", info.lds_storage, base.lds_storage);
	v.Flag("ithreads", info.dispatch_indirect_threads, base.dispatch_indirect_threads);
	v.Flag("tgsize", info.tg_size_en, base.tg_size_en);
	v.Flag("gx", info.group_id[0], base.group_id[0]);
	v.Flag("gy", info.group_id[1], base.group_id[1]);
	v.Flag("gz", info.group_id[2], base.group_id[2]);
}

// A specialization element: slash separated key:value fields that differ from the default, a
// bare key for a set flag, or "-".
class ElementWriter {
public:
	static constexpr bool Reading = false;
	std::string           text;

	void Value(std::string_view key, std::string_view value) {
		text += text.empty() ? "" : "/";
		text += key;
		text += ':';
		text += value;
	}
	template <class T>
	void Dec(std::string_view key, T& value, T def) {
		if (value != def) Value(key, std::to_string(value));
	}
	template <class T>
	void Hex(std::string_view key, T& value, T def) {
		if (value != def) Value(key, fmt::format("{:x}", value));
	}
	void Flag(std::string_view key, bool& value, bool def = false) {
		if (value != def) {
			text += text.empty() ? "" : "/";
			text += key;
		}
	}
	void Format(std::string_view key, Prospero::BufferFormat& value, Prospero::BufferFormat def) {
		if (value != def) Value(key, BufferFormatName(value));
	}
	void Dimension(std::string_view key, ShaderRecompiler::Decoder::ImageDimension& value,
	               ShaderRecompiler::Decoder::ImageDimension def) {
		if (value != def) Value(key, DimensionName(value));
	}
	template <class E>
	void Guest(std::string_view key, E& value, E def) {
		if (value != def) Value(key, GuestName(value));
	}
};

class ElementReader {
public:
	static constexpr bool Reading = true;
	bool                  ok      = true;

	explicit ElementReader(std::string_view text) {
		if (text == "-") return;
		for (const auto field: Split(text, '/')) {
			const auto colon = field.find(':');
			m_fields.emplace_back(field.substr(0, colon), colon == std::string_view::npos
			                                                  ? std::string_view {}
			                                                  : field.substr(colon + 1));
		}
	}
	[[nodiscard]] std::optional<std::string_view> Find(std::string_view key) const {
		for (const auto& [name, value]: m_fields) {
			if (name == key) return value;
		}
		return {};
	}
	template <class T>
	void Dec(std::string_view key, T& value, T) {
		if (const auto text = Find(key)) {
			uint64_t number = 0;
			ok              = ok && ParseNumber(*text, number);
			value           = static_cast<T>(number);
		}
	}
	template <class T>
	void Hex(std::string_view key, T& value, T) {
		if (const auto text = Find(key)) {
			uint64_t number = 0;
			ok              = ok && ParseNumber(*text, number, 16);
			value           = static_cast<T>(number);
		}
	}
	void Flag(std::string_view key, bool& value, bool def = false) {
		value = Find(key) ? !def : def;
	}
	void Format(std::string_view key, Prospero::BufferFormat& value, Prospero::BufferFormat) {
		if (const auto text = Find(key)) ok = ok && ParseBufferFormat(*text, value);
	}
	void Dimension(std::string_view key, ShaderRecompiler::Decoder::ImageDimension& value,
	               ShaderRecompiler::Decoder::ImageDimension) {
		if (const auto text = Find(key)) ok = ok && ParseDimension(*text, value);
	}
	template <class E>
	void Guest(std::string_view key, E& value, E) {
		if (const auto text = Find(key)) ok = ok && ParseGuest(*text, value);
	}

private:
	std::vector<std::pair<std::string_view, std::string_view>> m_fields;
};

template <class V>
void VisitElement(V& v, ResourceSpecialization::Buffer& buffer) {
	const ResourceSpecialization::Buffer base;
	auto& [packed_stride, descriptor_format, descriptor_swizzle, zero_stride_oob, scalar_empty,
	       indirect_root, indirect_mapping_offset, indirect_search_iterations] = buffer;
	v.Dec("st", packed_stride, base.packed_stride);
	v.Format("f", descriptor_format, base.descriptor_format);
	v.Hex("sw", descriptor_swizzle, base.descriptor_swizzle);
	v.Flag("oob", zero_stride_oob, base.zero_stride_oob);
	v.Flag("empty", scalar_empty, base.scalar_empty);
	v.Dec("ir", indirect_root, base.indirect_root);
	v.Dec("im", indirect_mapping_offset, base.indirect_mapping_offset);
	v.Dec("is", indirect_search_iterations, base.indirect_search_iterations);
}

template <class V>
void VisitElement(V& v, ResourceSpecialization::Image& image) {
	const ResourceSpecialization::Image base;
	auto& [numeric_class, dimension, mip_count, conversion_format, shader_swizzle, srgb_decode,
	       indirect_root, indirect_mapping_offset, indirect_search_iterations, table,
	       table_capacity, table_mapping_offset, table_entry_mask, cube, skip_degamma, fmask,
	       heap] = image;
	v.Guest("n", numeric_class, base.numeric_class);
	v.Dimension("d", dimension, base.dimension);
	v.Dec("m", mip_count, base.mip_count);
	v.Format("cf", conversion_format, base.conversion_format);
	v.Hex("sw", shader_swizzle, base.shader_swizzle);
	v.Format("sd", srgb_decode, base.srgb_decode);
	v.Dec("ir", indirect_root, base.indirect_root);
	v.Dec("im", indirect_mapping_offset, base.indirect_mapping_offset);
	v.Dec("is", indirect_search_iterations, base.indirect_search_iterations);
	v.Dec("t", table, base.table);
	v.Dec("tc", table_capacity, base.table_capacity);
	v.Dec("tm", table_mapping_offset, base.table_mapping_offset);
	v.Hex("te", table_entry_mask, base.table_entry_mask);
	v.Flag("cube", cube, base.cube);
	v.Flag("dg", skip_degamma, base.skip_degamma);
	v.Flag("fmask", fmask, base.fmask);
	v.Flag("heap", heap, base.heap);
}

template <class V>
void VisitElement(V& v, ResourceSpecialization::Sampler& sampler) {
	const ResourceSpecialization::Sampler base;
	auto& [indirect_root, indirect_mapping_offset, indirect_search_iterations] = sampler;
	v.Dec("ir", indirect_root, base.indirect_root);
	v.Dec("im", indirect_mapping_offset, base.indirect_mapping_offset);
	v.Dec("is", indirect_search_iterations, base.indirect_search_iterations);
}

template <class Element>
void WriteElements(TextWriter& w, std::string_view key, std::vector<Element>& elements) {
	if (elements.empty()) return;
	std::string text;
	for (auto& element: elements) {
		ElementWriter writer;
		VisitElement(writer, element);
		text += text.empty() ? "" : ",";
		text += writer.text.empty() ? "-" : writer.text;
	}
	w.Value(key, text);
}

template <class Element>
void ReadElements(TextReader& r, std::string_view key, std::vector<Element>& elements) {
	const auto text = r.Find(key);
	if (!text) return;
	const auto parts = Split(*text, ',');
	r.ok             = r.ok && parts.size() <= 4096;
	elements.assign(r.ok ? parts.size() : 0, Element {});
	for (size_t i = 0; r.ok && i < parts.size(); i++) {
		ElementReader reader(parts[i]);
		VisitElement(reader, elements[i]);
		r.ok = reader.ok;
	}
}

// A module record of `stage` with every field at its default.
ManifestModule DefaultModule(ShaderType stage) {
	ManifestModule module {.stage = stage};
	if (stage == ShaderType::Pixel) {
		module.input.emplace<ShaderPixelInputInfo>();
	} else if (stage == ShaderType::Compute) {
		module.input.emplace<ShaderComputeInputInfo>();
	} else {
		module.input.emplace<ShaderVertexInputInfo>().logical_stage = stage;
	}
	NormalizeManifestInput(module);
	return module;
}

// The line of a normalized module after its id.
std::string ModuleText(ManifestModule& module) {
	auto        base = DefaultModule(module.stage);
	std::string text(StageName(module.stage));
	TextWriter  w(text);
	w.Value("code", fmt::format("{:016x}", module.code_hash));
	if (module.back_hash != 0) w.Value("back", fmt::format("{:016x}", module.back_hash));
	w.Dec("ud", module.user_data_count, 0u);
	w.Dec("cur", module.push_data_cursor, 0u);
	if (auto* vertex = std::get_if<ShaderVertexInputInfo>(&module.input)) {
		VisitVertexInput(w, *vertex, std::get<ShaderVertexInputInfo>(base.input));
	} else if (auto* pixel = std::get_if<ShaderPixelInputInfo>(&module.input)) {
		VisitPixelInput(w, *pixel, std::get<ShaderPixelInputInfo>(base.input));
	} else {
		VisitComputeInput(w, std::get<ShaderComputeInputInfo>(module.input),
		                  std::get<ShaderComputeInputInfo>(base.input));
	}
	auto& [buffers, images, samplers] = module.specialization;
	WriteElements(w, "sb", buffers);
	WriteElements(w, "si", images);
	WriteElements(w, "ss", samplers);
	return text;
}

std::string ModuleId(std::string_view text) {
	return fmt::format("{:012x}", XXH3_64bits(text.data(), text.size()) >> 16u);
}

bool ReadModule(std::span<const std::string_view> tokens, ManifestModule& module) {
	ShaderType stage = ShaderType::Unknown;
	if (tokens.empty() || !ParseStage(tokens[0], stage)) return false;
	module = DefaultModule(stage);
	TextReader r(tokens.subspan(1));
	const auto code = r.Find("code");
	r.ok            = code && ParseNumber(*code, module.code_hash, 16) && module.code_hash != 0;
	if (const auto back = r.Find("back")) r.ok = r.ok && ParseNumber(*back, module.back_hash, 16);
	r.Dec("ud", module.user_data_count, 0u);
	r.Dec("cur", module.push_data_cursor, 0u);
	if (auto* vertex = std::get_if<ShaderVertexInputInfo>(&module.input)) {
		VisitVertexInput(r, *vertex, *vertex);
	} else if (auto* pixel = std::get_if<ShaderPixelInputInfo>(&module.input)) {
		VisitPixelInput(r, *pixel, *pixel);
	} else {
		auto& compute = std::get<ShaderComputeInputInfo>(module.input);
		VisitComputeInput(r, compute, compute);
	}
	auto& [buffers, images, samplers] = module.specialization;
	ReadElements(r, "sb", buffers);
	ReadElements(r, "si", images);
	ReadElements(r, "ss", samplers);
	return r.ok && module.user_data_count <= ShaderParams {}.user_data.size();
}

constexpr std::array<std::string_view, 4> PipelineStageKeys {"v", "h", "d", "p"};

// The line of a pipeline whose stages are module ids.
std::string PipelineText(const ManifestPipeline&      pipeline,
                         std::span<const std::string> module_ids) {
	[[maybe_unused]] const auto& [rendering_fields_a, rendering_fields_b, rendering_fields_c,
	                              rendering_fields_d] = pipeline.key.rendering;
	[[maybe_unused]] const auto& [input_fields_a, input_fields_b, input_fields_c, input_fields_d] =
	    pipeline.key.vertex_input;
	[[maybe_unused]] const auto& [s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15,
	                              s16, s17, s18, s19] = pipeline.key.static_params;
	std::string text                                  = "P";
	TextWriter  w(text);
	for (size_t i = 0; i < PipelineStageKeys.size(); i++) {
		if (pipeline.stages[i] != ManifestPipeline::NoStage) {
			w.Value(PipelineStageKeys[i], module_ids[pipeline.stages[i]]);
		}
	}
	bool plain = pipeline.plain_pixel;
	w.Flag("plain", plain);
	const auto& rendering = pipeline.key.rendering;
	uint32_t    count     = rendering.color_count;
	w.Dec("cc", count, 0u);
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		if (rendering.color_formats[i] != vk::Format::eUndefined) {
			w.Value(fmt::format("c{}", i), VkName(rendering.color_formats[i]));
		}
	}
	if (rendering.depth_format != vk::Format::eUndefined)
		w.Value("depth", VkName(rendering.depth_format));
	if (rendering.stencil_format == rendering.depth_format &&
	    rendering.depth_format != vk::Format::eUndefined) {
		text += " stencil";
	} else if (rendering.stencil_format != vk::Format::eUndefined) {
		w.Value("stencil", VkName(rendering.stencil_format));
	}
	const auto& s = pipeline.key.static_params;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		uint32_t mask = s.color_mask[i];
		w.Hex(fmt::format("m{}", i), mask, 0u);
		// Enable, color source, operation and destination, separate alpha, alpha source,
		// operation and destination; trailing zeros left out.
		const uint32_t blend[] = {s.blend_enable[i] ? 1u : 0u,
		                          s.color_srcblend[i],
		                          s.color_comb_fcn[i],
		                          s.color_destblend[i],
		                          s.separate_alpha_blend[i] ? 1u : 0u,
		                          s.alpha_srcblend[i],
		                          s.alpha_comb_fcn[i],
		                          s.alpha_destblend[i]};
		auto           used    = std::size(blend);
		while (used > 0 && blend[used - 1] == 0)
			used--;
		if (used > 0) {
			w.Value(fmt::format("b{}", i),
			        fmt::format("{}", fmt::join(std::span(blend).first(used), ".")));
		}
	}
	bool negative_one_to_one = s.negative_one_to_one;
	bool depth_clip          = s.depth_clip_enable;
	bool restart             = s.primitive_restart_enable;
	bool sample_shading      = s.sample_shading_enable;
	bool alpha_to_coverage   = s.alpha_to_coverage_enable;
	bool provoking_last      = s.provoking_vtx_last;
	w.Flag("neg", negative_one_to_one);
	w.Flag("noclip", depth_clip, true);
	w.Flag("restart", restart);
	w.Flag("sshade", sample_shading);
	w.Flag("a2c", alpha_to_coverage);
	w.Flag("pvlast", provoking_last);
	if (s.topology != PipelineStaticParameters {}.topology) w.Value("topo", VkName(s.topology));
	uint32_t control_points = s.patch_control_points;
	uint32_t samples        = s.samples;
	w.Dec("pcp", control_points, 0u);
	w.Dec("ms", samples, 1u);
	if (s.polygon_mode != vk::PolygonMode::eFill) w.Value("poly", VkName(s.polygon_mode));
	const auto& input = pipeline.key.vertex_input;
	if (input.binding_count > 0) {
		std::string bindings;
		for (uint32_t i = 0; i < input.binding_count; i++) {
			bindings +=
			    fmt::format("{}{}.{}", i == 0 ? "" : ",", uint32_t {input.bindings[i].stride},
			                uint32_t {input.bindings[i].instance});
		}
		w.Value("vb", bindings);
	}
	if (input.attribute_count > 0) {
		std::string attributes;
		for (uint32_t i = 0; i < input.attribute_count; i++) {
			attributes +=
			    fmt::format("{}{}.{}", i == 0 ? "" : ",", uint32_t {input.attributes[i].offset},
			                uint32_t {input.attributes[i].binding});
		}
		w.Value("va", attributes);
	}
	return text;
}

bool ReadPipeline(std::span<const std::string_view>                     tokens,
                  const std::unordered_map<std::string_view, uint32_t>& modules,
                  ManifestPipeline&                                     pipeline) {
	pipeline = {};
	TextReader r(tokens);
	for (size_t i = 0; i < PipelineStageKeys.size(); i++) {
		if (const auto id = r.Find(PipelineStageKeys[i])) {
			const auto found = modules.find(*id);
			if (found == modules.end()) return false;
			pipeline.stages[i] = found->second;
		}
	}
	r.Flag("plain", pipeline.plain_pixel);
	auto&    rendering = pipeline.key.rendering;
	uint32_t count     = 0;
	r.Dec("cc", count, 0u);
	r.ok                  = r.ok && count <= RENDER_COLOR_ATTACHMENTS_MAX;
	rendering.color_count = count;
	const auto format     = [&](std::string_view key, vk::Format& value) {
		if (const auto text = r.Find(key)) {
			vk::Format parsed = vk::Format::eUndefined;
			r.ok              = r.ok && ParseVk(*text, parsed);
			value             = parsed;
		}
	};
	auto& s = pipeline.key.static_params;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		vk::Format color = vk::Format::eUndefined;
		format(fmt::format("c{}", i), color);
		rendering.color_formats[i] = color;
		uint32_t mask              = 0;
		r.Hex(fmt::format("m{}", i), mask, 0u);
		s.color_mask[i] = mask;
		if (const auto text = r.Find(fmt::format("b{}", i))) {
			const auto parts = Split(*text, '.');
			uint64_t   blend[8] {};
			r.ok = r.ok && parts.size() <= 8;
			for (size_t j = 0; r.ok && j < parts.size(); j++) {
				r.ok = ParseNumber(parts[j], blend[j]);
			}
			s.blend_enable[i]         = blend[0] != 0;
			s.color_srcblend[i]       = static_cast<uint8_t>(blend[1]);
			s.color_comb_fcn[i]       = static_cast<uint8_t>(blend[2]);
			s.color_destblend[i]      = static_cast<uint8_t>(blend[3]);
			s.separate_alpha_blend[i] = blend[4] != 0;
			s.alpha_srcblend[i]       = static_cast<uint8_t>(blend[5]);
			s.alpha_comb_fcn[i]       = static_cast<uint8_t>(blend[6]);
			s.alpha_destblend[i]      = static_cast<uint8_t>(blend[7]);
		}
	}
	vk::Format depth   = vk::Format::eUndefined;
	vk::Format stencil = vk::Format::eUndefined;
	format("depth", depth);
	if (const auto text = r.Find("stencil"); text && text->empty()) {
		stencil = depth;
	} else {
		format("stencil", stencil);
	}
	rendering.depth_format   = depth;
	rendering.stencil_format = stencil;
	bool flag                = false;
	r.Flag("neg", flag);
	s.negative_one_to_one = flag;
	r.Flag("noclip", flag, true);
	s.depth_clip_enable = flag;
	r.Flag("restart", flag);
	s.primitive_restart_enable = flag;
	r.Flag("sshade", flag);
	s.sample_shading_enable = flag;
	r.Flag("a2c", flag);
	s.alpha_to_coverage_enable = flag;
	r.Flag("pvlast", flag);
	s.provoking_vtx_last = flag;
	if (const auto text = r.Find("topo")) {
		vk::PrimitiveTopology topology {};
		r.ok       = r.ok && ParseVk(*text, topology);
		s.topology = topology;
	}
	uint32_t control_points = 0;
	uint32_t samples        = 1;
	r.Dec("pcp", control_points, 0u);
	r.Dec("ms", samples, 1u);
	s.patch_control_points = control_points;
	s.samples              = samples;
	if (const auto text = r.Find("poly")) {
		vk::PolygonMode mode {};
		r.ok           = r.ok && ParseVk(*text, mode);
		s.polygon_mode = mode;
	}
	auto&      input = pipeline.key.vertex_input;
	const auto pairs = [&](std::string_view key, auto&& store) -> uint8_t {
		const auto text = r.Find(key);
		if (!text) return 0;
		const auto items = Split(*text, ',');
		r.ok             = r.ok && items.size() <= ShaderVertexInputInfo::RES_MAX;
		for (size_t i = 0; r.ok && i < items.size(); i++) {
			const auto parts = Split(items[i], '.');
			uint64_t   values[2] {};
			r.ok = parts.size() == 2 && ParseNumber(parts[0], values[0]) &&
			       ParseNumber(parts[1], values[1]);
			store(i, values);
		}
		return static_cast<uint8_t>(items.size());
	};
	input.binding_count   = pairs("vb", [&](size_t i, const uint64_t* values) {
		input.bindings[i] = {.stride   = static_cast<uint32_t>(values[0]),
		                     .instance = static_cast<uint8_t>(values[1])};
	});
	input.attribute_count = pairs("va", [&](size_t i, const uint64_t* values) {
		input.attributes[i] = {.offset  = static_cast<uint32_t>(values[0]),
		                       .binding = static_cast<uint8_t>(values[1])};
	});
	return r.ok;
}

// Why no draw creates `pipeline` from the modules it names, or empty. GetGraphicsPipeline() links
// a VS or mesh stage, or an LS, HS and TES, with an optional PS; it takes the vertex input from the
// VS or LS (a mesh stage has none) and the patch size from the LS, and a rectangle list draws
// patches of 3 points. Creating a pipeline whose modules do not fit it reads what they do not
// have: the vertex formats of a TES, or more of them than the VS has.
std::string PipelineLinkError(const ManifestPipeline&         pipeline,
                              std::span<const ManifestModule> modules) {
	std::array<ShaderType, 4> stages {};
	for (size_t i = 0; i < stages.size(); i++) {
		stages[i] = pipeline.stages[i] == ManifestPipeline::NoStage
		                ? ShaderType::Unknown
		                : modules[pipeline.stages[i]].stage;
	}
	const bool tessellation = stages[0] == ShaderType::Local;
	const bool mesh         = stages[0] == ShaderType::Mesh;
	const bool linked       = tessellation ? stages[1] == ShaderType::TessellationControl &&
	                                       stages[2] == ShaderType::TessellationEvaluation
	                                       : (stages[0] == ShaderType::Vertex || mesh) &&
	                                       stages[1] == ShaderType::Unknown &&
	                                       stages[2] == ShaderType::Unknown;
	if (!linked || (stages[3] != ShaderType::Unknown && stages[3] != ShaderType::Pixel)) {
		std::string names;
		for (const auto stage: stages) {
			const auto name = StageName(stage);
			names += fmt::format("{}{}", names.empty() ? "" : ",", name.empty() ? "-" : name);
		}
		return fmt::format("stages {}", names);
	}
	const auto&    vertex     = std::get<ShaderVertexInputInfo>(modules[pipeline.stages[0]].input);
	const auto&    input      = pipeline.key.vertex_input;
	const uint32_t attributes = input.attribute_count;
	const uint32_t bindings   = input.binding_count;
	const uint32_t expected   = mesh ? 0u : static_cast<uint32_t>(vertex.resources_num);
	if (attributes != expected || (mesh && bindings != 0)) {
		return fmt::format("{} vertex attributes for a {} stage that reads {}", attributes,
		                   StageName(stages[0]), expected);
	}
	for (uint32_t i = 0; i < attributes; i++) {
		const uint32_t binding = input.attributes[i].binding;
		if (binding >= bindings) {
			return fmt::format("vertex attribute {} reads binding {} of {}", i, binding, bindings);
		}
	}
	const auto&    s        = pipeline.key.static_params;
	const auto     topology = s.topology;
	const uint32_t points   = s.patch_control_points;
	const uint32_t drawn    = tessellation ? vertex.tess.input_control_points
	                          : !mesh && topology == vk::PrimitiveTopology::ePatchList ? 3u
	                                                                                   : 0u;
	if (points != drawn ||
	    (tessellation && (points == 0 || topology != vk::PrimitiveTopology::ePatchList))) {
		return fmt::format("patches of {} points where its draws have {}", points, drawn);
	}
	return {};
}

std::string LayoutText(uint64_t key, ShaderTessellationInputInfo layout) {
	std::string text = fmt::format("T {:016x}", key);
	TextWriter  w(text);
	auto& [input_control_points, output_control_points, ls_stride, hs_stride, patch_base,
	       patch_size, domain, partitioning, output_topology] = layout;
	w.Dec("icp", input_control_points, 0u);
	w.Dec("ocp", output_control_points, 0u);
	w.Dec("ls", ls_stride, 0u);
	w.Dec("hs", hs_stride, 0u);
	w.Dec("pb", patch_base, 0u);
	w.Dec("psize", patch_size, 0u);
	w.Dec("domain", domain, 0u);
	w.Dec("part", partitioning, 0u);
	w.Dec("topo", output_topology, 0u);
	return text;
}

bool ReadLayout(std::span<const std::string_view>                 tokens,
                std::pair<uint64_t, ShaderTessellationInputInfo>& entry) {
	if (tokens.empty() || !ParseNumber(tokens[0], entry.first, 16)) return false;
	TextReader r(tokens.subspan(1));
	auto& [input_control_points, output_control_points, ls_stride, hs_stride, patch_base,
	       patch_size, domain, partitioning, output_topology] = entry.second;
	r.Dec("icp", input_control_points, 0u);
	r.Dec("ocp", output_control_points, 0u);
	r.Dec("ls", ls_stride, 0u);
	r.Dec("hs", hs_stride, 0u);
	r.Dec("pb", patch_base, 0u);
	r.Dec("psize", patch_size, 0u);
	r.Dec("domain", domain, 0u);
	r.Dec("part", partitioning, 0u);
	r.Dec("topo", output_topology, 0u);
	return r.ok;
}

std::string SafeFileName(std::string name) {
	for (auto& c: name) {
		if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '-' && c != '_' && c != '.') {
			c = '_';
		}
	}
	return name;
}

template <typename... Args>
void ManifestLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

double Seconds(std::chrono::steady_clock::duration duration) {
	return std::chrono::duration<double>(duration).count();
}

double Milliseconds(uint64_t nanoseconds) {
	return static_cast<double>(nanoseconds) / 1'000'000.0;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
	std::vector<uint8_t> bytes;
	Common::File         file;
	if (!Common::File::IsFileExisting(path) || !file.Open(path, Common::File::Mode::Read)) {
		return bytes;
	}
	bytes.resize(file.Size());
	uint32_t read = 0;
	file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
	bytes.resize(read);
	return bytes;
}

std::vector<uint32_t> StaticKey(const ManifestModule& module) {
	std::vector<uint32_t> key;
	std::visit([&](const auto& input) { BuildStageStaticKey(input, key); }, module.input);
	return key;
}

} // namespace

uint64_t ManifestModule::ProgramHash() const {
	if (back_hash == 0) {
		return code_hash;
	}
	// As PrepareProgram names a GS front and back pair.
	const uint64_t hashes[] = {code_hash, back_hash};
	return XXH3_64bits(hashes, sizeof(hashes));
}

void NormalizeManifestInput(ManifestModule& module) {
	if (auto* vertex = std::get_if<ShaderVertexInputInfo>(&module.input)) {
		for (int i = 0; i < vertex->resources_num; i++) {
			vertex->resources[i].UpdateAddress48(0);
		}
		for (int i = 0; i < vertex->buffers_num; i++) {
			vertex->buffers[i].addr = 0;
		}
		if (module.stage == ShaderType::Mesh) {
			vertex->mesh.host_subgroup_size = 0;
		}
		std::ranges::fill(vertex->clip_space.half_extent, 0.0f);
		vertex->stage = {};
	} else if (auto* pixel = std::get_if<ShaderPixelInputInfo>(&module.input)) {
		pixel->stage = {};
	} else {
		auto& compute              = std::get<ShaderComputeInputInfo>(module.input);
		compute.host_subgroup_size = 0;
		compute.lds_storage        = false;
		std::ranges::fill(compute.dispatch_threads_num, 0u);
		std::ranges::fill(compute.workgroup_counts, 0u);
		compute.indirect_args_address = 0;
		compute.stage                 = {};
	}
}

uint64_t ManifestModuleIdentity(const ManifestModule&     module,
                                std::span<const uint32_t> static_state) {
	Writer writer;
	auto   stage = static_cast<uint8_t>(module.stage);
	IoAll(writer, stage, module.code_hash, module.back_hash, module.user_data_count,
	      module.push_data_cursor, module.specialization);
	writer.Raw(static_state.data(), static_state.size_bytes());
	return XXH3_64bits(writer.bytes.data(), writer.bytes.size());
}

uint64_t ManifestPipelineIdentity(const ManifestPipeline& pipeline) {
	auto key      = pipeline.key;
	key.stage_ids = {};
	Writer writer;
	auto   stages = pipeline.stages;
	auto   plain  = pipeline.plain_pixel;
	IoAll(writer, stages, plain);
	writer.Raw(&key, sizeof(key));
	return XXH3_64bits(writer.bytes.data(), writer.bytes.size());
}

std::vector<uint8_t> SaveManifest(const PipelineManifest& manifest) {
	struct ModuleLine {
		uint64_t    code = 0;
		uint64_t    back = 0;
		std::string id;
		std::string text;
	};
	std::vector<ModuleLine>  modules;
	std::vector<std::string> ids(manifest.modules.size());
	modules.reserve(manifest.modules.size());
	for (size_t i = 0; i < manifest.modules.size(); i++) {
		auto module = manifest.modules[i];
		NormalizeManifestInput(module);
		auto text = ModuleText(module);
		ids[i]    = ModuleId(text);
		modules.push_back({module.code_hash, module.back_hash, ids[i], std::move(text)});
	}
	// Variants of one shader stay together.
	std::ranges::sort(modules, {}, [](const ModuleLine& line) {
		return std::tie(line.code, line.back, line.id);
	});
	const auto same_modules = std::ranges::unique(modules, {}, &ModuleLine::id);
	modules.erase(same_modules.begin(), same_modules.end());
	std::vector<std::string> pipelines;
	pipelines.reserve(manifest.pipelines.size());
	for (const auto& pipeline: manifest.pipelines) {
		pipelines.push_back(PipelineText(pipeline, ids));
	}
	std::ranges::sort(pipelines);
	const auto same_pipelines = std::ranges::unique(pipelines);
	pipelines.erase(same_pipelines.begin(), same_pipelines.end());
	auto layouts = manifest.tessellation_layouts;
	std::ranges::sort(layouts, {}, &std::pair<uint64_t, ShaderTessellationInputInfo>::first);
	const auto same_layouts =
	    std::ranges::unique(layouts, {}, &std::pair<uint64_t, ShaderTessellationInputInfo>::first);
	layouts.erase(same_layouts.begin(), same_layouts.end());

	std::string out = fmt::format("{}{}\n", ManifestHeader, ManifestFormat);
	for (const auto& module: modules) {
		out += fmt::format("M {} {}\n", module.id, module.text);
	}
	for (const auto& pipeline: pipelines) {
		out += pipeline;
		out += '\n';
	}
	for (const auto& [key, layout]: layouts) {
		out += LayoutText(key, layout);
		out += '\n';
	}
	return {out.begin(), out.end()};
}

bool LoadManifest(std::span<const uint8_t> bytes, PipelineManifest& manifest, uint32_t* skipped,
                  std::vector<std::string>* unlinked) {
	manifest               = {};
	uint32_t   bad         = 0;
	const auto set_skipped = [&] {
		if (skipped != nullptr) *skipped = bad;
	};
	const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	// A last line without its newline was cut short: it is left out.
	std::vector<std::vector<std::string_view>> lines;
	for (size_t start = 0; start < text.size();) {
		const auto end = text.find('\n', start);
		if (end == std::string_view::npos) {
			bad++;
			break;
		}
		auto line = text.substr(start, end - start);
		start     = end + 1;
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		if (lines.empty()) {
			if (line.empty()) continue;
			uint64_t format = 0;
			if (!line.starts_with(ManifestHeader) ||
			    !ParseNumber(line.substr(ManifestHeader.size()), format) ||
			    format != ManifestFormat) {
				set_skipped();
				return false;
			}
		}
		std::vector<std::string_view> tokens;
		for (const auto token: Split(line, ' ')) {
			if (!token.empty()) tokens.push_back(token);
		}
		lines.push_back(std::move(tokens));
	}
	if (lines.empty()) {
		set_skipped();
		return false;
	}
	// Modules first: pipelines name them by id.
	std::unordered_map<std::string_view, uint32_t> modules;
	for (const auto& tokens: lines) {
		if (tokens.size() < 2 || tokens[0] != "M") continue;
		ManifestModule module;
		if (!ReadModule(std::span(tokens).subspan(2), module) ||
		    !modules.emplace(tokens[1], static_cast<uint32_t>(manifest.modules.size())).second) {
			bad++;
			continue;
		}
		manifest.modules.push_back(std::move(module));
	}
	for (const auto& tokens: lines) {
		if (tokens.empty() || tokens[0].starts_with("#") || tokens[0] == "M") continue;
		if (tokens[0] == "P") {
			ManifestPipeline pipeline;
			if (!ReadPipeline(std::span(tokens).subspan(1), modules, pipeline) ||
			    pipeline.stages[0] == ManifestPipeline::NoStage ||
			    (pipeline.plain_pixel && pipeline.stages[3] == ManifestPipeline::NoStage)) {
				bad++;
				continue;
			}
			if (auto error = PipelineLinkError(pipeline, manifest.modules); !error.empty()) {
				bad++;
				if (unlinked != nullptr) {
					std::string ids;
					for (const auto token: std::span(tokens).subspan(1)) {
						if (std::ranges::any_of(PipelineStageKeys, [&](std::string_view key) {
							    return token.starts_with(key) && token.substr(key.size(), 1) == "=";
						    })) {
							ids += fmt::format("{}{}", ids.empty() ? "" : " ", token);
						}
					}
					unlinked->push_back(fmt::format("{} ({})", ids, error));
				}
				continue;
			}
			manifest.pipelines.push_back(pipeline);
		} else if (tokens[0] == "T") {
			std::pair<uint64_t, ShaderTessellationInputInfo> layout;
			if (ReadLayout(std::span(tokens).subspan(1), layout)) {
				manifest.tessellation_layouts.push_back(layout);
			} else {
				bad++;
			}
		}
	}
	set_skipped();
	return true;
}

// The manifest of the running title: what it lists (`warm`, immutable once loaded), what the
// session adds to it, and the state of the threads that compile the listed modules and pipelines.
struct PipelineCache::ManifestState {
	PipelineManifest      warm;
	std::filesystem::path record_path; // Empty unless recording.

	// Under `mutex`, from here on.
	std::mutex mutex;
	// The listed records and those the session added, by identity; the index space of `warm`
	// continues into `recorded`.
	PipelineManifest                       recorded;
	std::unordered_map<uint64_t, uint32_t> module_index;
	std::unordered_set<uint64_t>           pipeline_identities;
	std::unordered_set<uint64_t>           layout_keys;
	// This session's module ids: their record and whether the id is the plain variant's.
	std::unordered_map<uint64_t, std::pair<uint32_t, bool>> module_records;
	bool                                                    dirty           = false;
	uint32_t                                                added_modules   = 0;
	uint32_t                                                added_pipelines = 0;

	// Code of the listed modules, copied when the guest maps it.
	struct Code {
		std::shared_ptr<const std::vector<uint32_t>> words;
		std::vector<uint32_t>                        waiting;     // Module records it unblocks.
		// The records it unblocked while the guest had not mapped it.
		std::vector<uint32_t>                        early;
		uint32_t                                     readers = 0; // Records not started yet.
		bool                                         mapped  = false;
	};
	std::unordered_map<uint64_t, Code> code;
	std::vector<uint8_t>               missing_code;     // By module record.
	std::vector<const ShaderModule*>   modules;          // By module record, once compiled.
	std::vector<std::vector<uint32_t>> module_pipelines; // By module record.
	std::vector<uint8_t>               missing_modules;  // By pipeline record.
	// The place of each module record's program in the order of use (UseRank()).
	std::vector<uint32_t>              module_rank;
	// Both in the order of use, and after those the game used before, the rest: modules as they
	// came, pipelines as QueuePipeline() says.
	std::deque<uint32_t>               module_jobs;
	std::deque<uint32_t>               pipeline_jobs;
	std::condition_variable_any        available;
	uint32_t                           running = 0;

	// Progress.
	std::chrono::steady_clock::time_point start;
	std::chrono::steady_clock::time_point last_log;
	bool                                  logged_idle      = true;
	uint32_t                              built            = 0;
	uint32_t                              found            = 0;
	uint32_t                              failed           = 0;
	uint32_t                              pipelines        = 0;
	uint32_t                              pipelines_known  = 0;
	uint32_t                              pipelines_failed = 0;
	uint32_t                              compute          = 0;
	uint64_t                              build_ns         = 0;
	uint64_t                              pipeline_ns      = 0;
	// What building a module and creating a graphics pipeline took lately, for the
	// precompilation's estimates.
	PrecompileMean                        module_time;
	PrecompileMean                        pipeline_time;

	std::vector<std::jthread> threads;
	std::jthread              saver;

	// The shader code store (CodeStoreThread()). Under `code_mutex`: the hashes the listener has
	// passed on in this session, and the code its thread has not looked at yet.
	std::filesystem::path                                   code_path;
	std::mutex                                              code_mutex;
	std::unordered_set<uint64_t>                            code_seen;
	std::deque<std::pair<uint64_t, std::vector<uint32_t>>> code_queue;
	std::condition_variable_any                             code_available;
	std::jthread                                            code_thread;
	// Until the store is read and the game's files are searched: code still arrives from them.
	std::atomic<bool>                                       scanning {true};

	// Queues a module whose code is there: after those the game used before it.
	void QueueModule(uint32_t index) {
		module_jobs.insert(std::upper_bound(module_jobs.begin(), module_jobs.end(), module_rank[index],
		                                    [this](uint32_t value, uint32_t job) {
			                                    return value < module_rank[job];
		                                    }),
		                   index);
	}

	// The place of a graphics pipeline in the order of use: that of the last of its shaders the
	// game used, or NoRank when it did not use them all.
	[[nodiscard]] uint32_t PipelineRank(uint32_t index) const {
		uint32_t rank = 0;
		for (const auto stage: warm.pipelines[index].stages) {
			if (stage != ManifestPipeline::NoStage) {
				rank = std::max(rank, module_rank[stage]);
			}
		}
		return rank;
	}

	// Queues a graphics pipeline whose modules are built: those of the shaders the game used
	// before in its order, then the largest first (by their SPIR-V, which the driver's time
	// follows): one left for the end would keep a thread busy alone.
	void QueuePipeline(uint32_t index) {
		const auto words = [this](uint32_t pipeline) {
			uint64_t total = 0;
			for (const auto stage: warm.pipelines[pipeline].stages) {
				if (stage != ManifestPipeline::NoStage && modules[stage] != nullptr) {
					total += modules[stage]->spirv_words;
				}
			}
			return total;
		};
		const auto key = std::pair {PipelineRank(index), words(index)};
		pipeline_jobs.insert(
		    std::upper_bound(pipeline_jobs.begin(), pipeline_jobs.end(), key,
		                     [&](const std::pair<uint32_t, uint64_t>& value, uint32_t job) {
			                     const auto rank = PipelineRank(job);
			                     return value.first != rank ? value.first < rank
			                                                : value.second > words(job);
		                     }),
		    index);
	}
};

void PipelineCache::ManifestStateDeleter::operator()(ManifestState* state) const {
	delete state;
}

// The file: a header line, then a line "S" where a session begins and one line per program the
// session used for the first time, its hash, in the order of use. Sessions only append to it.
struct PipelineCache::UseOrder {
	static constexpr std::string_view Header  = "# kyty shader use order v1";
	// A file this long is written again as one session.
	static constexpr uintmax_t        MaxSize = 4u << 20u;

	// Earlier sessions, immutable once loaded.
	std::unordered_map<uint64_t, uint32_t> rank;

	std::filesystem::path        path;
	std::mutex                   mutex;
	// This session: the programs noted, and their lines not written yet.
	std::unordered_set<uint64_t> noted;
	std::string                  pending;
	bool                         session_started = false;
	// The code the guest mapped in this session, by hash.
	std::mutex                   mapped_mutex;
	std::unordered_set<uint64_t> mapped;
	std::jthread                 writer;

	void Write() {
		std::string lines;
		{
			std::scoped_lock lock(mutex);
			lines.swap(pending);
		}
		if (lines.empty()) {
			return;
		}
		Common::File::CreateDirectories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::app);
		if (!session_started) {
			if (file.tellp() == std::streampos(0)) {
				file << Header << '\n';
			}
			file << "S\n";
			session_started = true;
		}
		file << lines;
	}
};

void PipelineCache::UseOrderDeleter::operator()(UseOrder* order) const {
	order->writer = {};
	delete order;
}

void PipelineCache::OpenUseOrder(const std::filesystem::path& path) {
	m_use_order.reset(new UseOrder());
	auto& order = *m_use_order;
	order.path  = path;
	// The last session first: the game most likely goes on where it was.
	std::vector<std::vector<uint64_t>> sessions;
	const auto                         bytes = ReadFile(path);
	const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	for (const auto line: Split(text, '\n')) {
		if (line == "S") {
			sessions.emplace_back();
		} else if (uint64_t hash = 0;
		           !sessions.empty() && line.size() == 16 && ParseNumber(line, hash, 16)) {
			sessions.back().push_back(hash);
		}
	}
	std::vector<uint64_t> programs;
	for (const auto& session: sessions | std::views::reverse) {
		for (const auto hash: session) {
			if (order.rank.try_emplace(hash, static_cast<uint32_t>(programs.size())).second) {
				programs.push_back(hash);
			}
		}
	}
	if (bytes.size() > UseOrder::MaxSize) {
		std::string out = fmt::format("{}\nS\n", UseOrder::Header);
		for (const auto hash: programs) {
			out += fmt::format("{:016x}\n", hash);
		}
		auto temporary = path;
		temporary += ".tmp";
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			file << out;
		}
		std::error_code error;
		std::filesystem::rename(temporary, path, error);
	}
	if (!programs.empty()) {
		ManifestLog("Pipeline manifest: the {} shaders used in {} earlier sessions go first ({})",
		            programs.size(), sessions.size(), Common::PathToString(path));
	}
	order.writer = std::jthread([&order](const std::stop_token& stop) {
		constexpr auto Period = std::chrono::seconds(2);
		auto           next   = std::chrono::steady_clock::now() + Period;
		while (!stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			if (std::chrono::steady_clock::now() >= next) {
				order.Write();
				next = std::chrono::steady_clock::now() + Period;
			}
		}
		order.Write();
	});
}

void PipelineCache::NoteUse(Pipeline& pipeline, std::span<const ShaderModule* const> stages) {
	pipeline.use_noted = true;
	if (m_use_order == nullptr) {
		return;
	}
	auto&            order = *m_use_order;
	std::scoped_lock lock(order.mutex);
	for (const auto* stage: stages) {
		if (stage != nullptr && order.noted.insert(stage->info.shader_hash).second) {
			order.pending += fmt::format("{:016x}\n", stage->info.shader_hash);
		}
	}
}

uint32_t PipelineCache::UseRank(uint64_t program_hash) const {
	if (m_use_order == nullptr) {
		return PipelineHelpers::NoRank;
	}
	if (const auto found = m_use_order->rank.find(program_hash); found != m_use_order->rank.end()) {
		return found->second;
	}
	std::scoped_lock lock(m_use_order->mapped_mutex);
	return m_use_order->mapped.contains(program_hash) ? MappedRank : PipelineHelpers::NoRank;
}

void PipelineCache::DecideRuntimeStrides(std::span<PipelineManifest*> manifests) {
	// The compute programs listed, and those with a record whose module reads them at run time:
	// it was recorded by a session that had no record of the program, and is built as it was.
	std::unordered_set<uint64_t> listed;
	std::unordered_set<uint64_t> runtime;
	for (const auto* manifest: manifests) {
		for (const auto& module: manifest->modules) {
			if (module.stage != ShaderType::Compute) {
				continue;
			}
			const auto hash = module.ProgramHash();
			listed.insert(hash);
			if (std::ranges::any_of(module.specialization.buffers, [](const auto& buffer) {
				    return ShaderRecompiler::IR::HasRuntimeStride(buffer.packed_stride);
			    })) {
				runtime.insert(hash);
			}
		}
	}
	for (const auto hash: listed) {
		if (!runtime.contains(hash)) {
			m_baked_stride_programs.insert(hash);
		}
	}
}

void PipelineCache::SetHostInput(ManifestModule& module) const {
	const auto& limits = m_graphics.GetPhysicalDeviceProperties().limits;
	if (auto* compute = std::get_if<ShaderComputeInputInfo>(&module.input)) {
		// As GetComputeProgram() sets them.
		compute->host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
		compute->lds_storage = compute->lds_size_dwords * 4u > limits.maxComputeSharedMemorySize;
	} else if (auto* vertex = std::get_if<ShaderVertexInputInfo>(&module.input)) {
		// As ResolveGraphicsPrograms() sets them.
		if (module.stage == ShaderType::Mesh) {
			vertex->mesh.host_subgroup_size = m_graphics.subgroup_size;
		}
		if (vertex->clip_space.enabled) {
			vertex->clip_space.half_extent[0] =
			    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
			vertex->clip_space.half_extent[1] =
			    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		}
	}
}

const ShaderModule* PipelineCache::FindCompiledModule(
    const ProgramKey& key, const ShaderRecompiler::IR::ResourceSpecialization& specialization,
    uint32_t push_data_cursor) const {
	const auto entry = m_program_cache.find(key);
	if (entry == m_program_cache.end()) {
		return nullptr;
	}
	for (const auto& module: entry->second.modules) {
		const auto& layout = module.info.bindings;
		if (module.function_code.empty() && module.specialization == specialization &&
		    layout.push_data_start_dword == ShaderRecompiler::IR::PushData::StartFor(
		                                        push_data_cursor, layout.ShaderDataDwords())) {
			return &module;
		}
	}
	return nullptr;
}

void PipelineCache::OpenManifest() {
	const bool warm   = Config::PipelineManifestEnabled();
	// A session that uses manifests also records its own: what it creates that no manifest lists
	// is listed for the next sessions of this computer, which compile it ahead with the rest. A
	// title that came with no manifest has one after its first session.
	const bool record = Config::PipelineManifestRecord() || warm;
	const auto title  = PipelineCacheTitleId();
	if ((!warm && !record) || title.empty()) {
		return;
	}
	std::string version;
	Loader::SystemContentParamSfoGetString("APP_VER", &version);
	const auto stem = SafeFileName(version.empty() ? title : title + "_" + version);
	const auto name = stem + ".kpm";
	OpenUseOrder(std::filesystem::path("_PipelineCache") / (stem + ".use"));
	// The manifests built with the emulator, then the one this computer records.
	std::vector<std::filesystem::path> folders;
	if (const char* base = SDL_GetBasePath(); base != nullptr) {
		folders.push_back(Common::PathFromUtf8(base) / "assets" / "pipelines");
	}
	folders.emplace_back("_PipelineCache");
	std::vector<std::filesystem::path> files;
	for (const auto& folder: folders) {
		if (Common::File::IsFileExisting(folder / name)) {
			files.push_back(folder / name);
		}
	}
	// Another version's records name code by its hash: those the guest never maps stay unused.
	if (files.empty()) {
		for (const auto& folder: folders) {
			std::error_code error;
			for (const auto& entry: std::filesystem::directory_iterator(folder, error)) {
				const auto file = Common::PathToString(entry.path().filename());
				if (file.starts_with(title + "_") && file.ends_with(".kpm")) {
					files.push_back(entry.path());
				}
			}
		}
	}

	m_manifest.reset(new ManifestState());
	auto& state = *m_manifest;
	auto& all   = state.recorded;
	// Every file is read first: which compute programs read their strides at run time is decided
	// from all their records.
	struct ReadManifest {
		PipelineManifest                      manifest;
		uint32_t                              skipped = 0;
		std::vector<std::string>              unlinked;
		bool                                  read    = false;
		std::chrono::steady_clock::duration   time {};
	};
	std::vector<ReadManifest> read_files(files.size());
	{
		std::vector<PipelineManifest*> manifests;
		for (size_t i = 0; i < files.size(); i++) {
			auto&      file  = read_files[i];
			const auto start = std::chrono::steady_clock::now();
			file.read = LoadManifest(ReadFile(files[i]), file.manifest, &file.skipped, &file.unlinked);
			file.time = std::chrono::steady_clock::now() - start;
			if (file.read) {
				manifests.push_back(&file.manifest);
			}
		}
		DecideRuntimeStrides(manifests);
	}
	for (const auto& path: files) {
		auto&      file     = read_files[static_cast<size_t>(&path - files.data())];
		auto&      loaded   = file.manifest;
		const auto skipped  = file.skipped;
		const auto start    = std::chrono::steady_clock::now() - file.time;
		const auto& unlinked = file.unlinked;
		if (!file.read) {
			ManifestLog("Pipeline manifest: ignoring {} (another format)",
			            Common::PathToString(path));
			continue;
		}
		std::vector<uint32_t> indices(loaded.modules.size());
		for (size_t i = 0; i < loaded.modules.size(); i++) {
			auto& module = loaded.modules[i];
			SetHostInput(module);
			const auto [found, inserted] =
			    state.module_index.try_emplace(ManifestModuleIdentity(module, StaticKey(module)),
			                                   static_cast<uint32_t>(all.modules.size()));
			if (inserted) {
				all.modules.push_back(std::move(module));
			}
			indices[i] = found->second;
		}
		for (auto& pipeline: loaded.pipelines) {
			for (auto& stage: pipeline.stages) {
				stage = stage == ManifestPipeline::NoStage ? stage : indices[stage];
			}
			if (state.pipeline_identities.insert(ManifestPipelineIdentity(pipeline)).second) {
				all.pipelines.push_back(pipeline);
			}
		}
		for (const auto& layout: loaded.tessellation_layouts) {
			if (state.layout_keys.insert(layout.first).second) {
				all.tessellation_layouts.push_back(layout);
			}
		}
		const auto load_time = std::chrono::steady_clock::now() - start;
		ManifestLog("Pipeline manifest: {} modules, {} pipelines in {} (loaded in {:.1f} ms{})",
		            loaded.modules.size(), loaded.pipelines.size(), Common::PathToString(path),
		            Seconds(load_time) * 1000.0,
		            skipped != 0 ? fmt::format(", {} records left out", skipped) : std::string {});
		if (!unlinked.empty()) {
			ManifestLog("Pipeline manifest: left out {} pipelines of {} that no draw creates from "
			            "the modules they name, such as {}",
			            unlinked.size(), Common::PathToString(path), unlinked.front());
		}
	}
	if (files.empty()) {
		ManifestLog("Pipeline manifest: none for {}", name);
	}
	if (record) {
		state.record_path = folders.back() / name;
		ManifestLog("Pipeline manifest: recording into {}",
		            Common::PathToString(state.record_path));
		// A crash, or a session that is killed, keeps what was recorded until the last write.
		state.saver = std::jthread([this](const std::stop_token& stop) {
			constexpr auto Period = std::chrono::seconds(10);
			auto           next   = std::chrono::steady_clock::now() + Period;
			while (!stop.stop_requested()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				if (std::chrono::steady_clock::now() >= next) {
					SaveManifestRecording();
					next = std::chrono::steady_clock::now() + Period;
				}
			}
		});
	} else if (!warm || all.modules.empty()) {
		m_manifest.reset();
		return;
	}
	if (!warm || all.modules.empty()) {
		return;
	}

	// What this session creates of the manifest comes from the listed records; recording adds
	// to a copy.
	if (record) {
		state.warm = all;
	} else {
		state.warm = std::move(all);
	}
	for (const auto& [key, layout]: state.warm.tessellation_layouts) {
		m_tess_layouts.try_emplace(key, layout);
	}
	const auto module_count = state.warm.modules.size();
	state.missing_code.assign(module_count, 0);
	state.module_rank.resize(module_count);
	for (uint32_t i = 0; i < module_count; i++) {
		state.module_rank[i] = UseRank(state.warm.modules[i].ProgramHash());
	}
	state.modules.assign(module_count, nullptr);
	state.module_pipelines.assign(module_count, {});
	state.missing_modules.assign(state.warm.pipelines.size(), 0);
	for (uint32_t i = 0; i < module_count; i++) {
		const auto& module = state.warm.modules[i];
		for (const auto hash: {module.code_hash, module.back_hash}) {
			if (hash != 0) {
				auto& code = state.code[hash];
				code.waiting.push_back(i);
				code.readers++;
				state.missing_code[i]++;
			}
		}
	}
	for (uint32_t i = 0; i < state.warm.pipelines.size(); i++) {
		for (const auto stage: state.warm.pipelines[i].stages) {
			if (stage != ManifestPipeline::NoStage) {
				state.module_pipelines[stage].push_back(i);
				state.missing_modules[i]++;
			}
		}
	}
	// Pipelines the shader store created are not created again.
	for (const auto& key: m_graphics_pipelines | std::views::keys) {
		m_claimed_graphics.insert(key);
	}
	for (const auto id: m_compute_pipelines | std::views::keys) {
		m_claimed_compute.insert(id);
	}
	m_manifest_warm = true;
	state.start     = std::chrono::steady_clock::now();
	state.last_log  = state.start;
	for (size_t i = 0; i < Common::ParallelThreads(); i++) {
		state.threads.emplace_back([this](const std::stop_token& stop) { ManifestWorker(stop); });
	}
	state.code_path   = folders.back() / (stem + ".code");
	state.code_thread = std::jthread([this](const std::stop_token& stop) { CodeStoreThread(stop); });
	// Shaders the guest mapped before are taken at once.
	ShaderSetMapListener(OnShaderMapped, this);
}

void PipelineCache::CloseManifest() {
	if (m_manifest == nullptr) {
		return;
	}
	auto& state = *m_manifest;
	if (m_manifest_warm) {
		ShaderSetMapListener(nullptr, nullptr);
		state.code_thread = {};
		for (auto& thread: state.threads) {
			thread.request_stop();
		}
		state.available.notify_all();
		state.threads.clear();
		ManifestProgress(true);
	}
	state.saver = {};
	SaveManifestRecording();
}

void PipelineCache::OnShaderMapped(void* userdata, uint64_t hash, std::span<const uint32_t> code) {
	auto& cache = *static_cast<PipelineCache*>(userdata);
	auto& state = *cache.m_manifest;
	{
		std::scoped_lock lock(state.code_mutex);
		if (state.code_seen.insert(hash).second) {
			state.code_queue.emplace_back(hash, std::vector<uint32_t>(code.begin(), code.end()));
		}
	}
	state.code_available.notify_all();
	{
		std::scoped_lock lock(cache.m_use_order->mapped_mutex);
		cache.m_use_order->mapped.insert(hash);
	}
	cache.ProvideCode(hash, code, true);
}

bool PipelineCache::ProvideCode(uint64_t hash, std::span<const uint32_t> code, bool from_guest) {
	auto& state = *m_manifest;
	{
		std::scoped_lock lock(state.mutex);
		const auto       found = state.code.find(hash);
		if (found == state.code.end()) {
			return false;
		}
		auto& entry = found->second;
		// What the guest maps goes before what only its files hold.
		const auto mapped_first = [&](uint32_t index) {
			if (state.module_rank[index] != PipelineHelpers::NoRank) {
				return false;
			}
			state.module_rank[index] = MappedRank;
			return true;
		};
		if (entry.mapped) {
			// Its code came from the store or from the files before: the records of it that are
			// still queued move up.
			for (const auto index: from_guest ? std::exchange(entry.early, {}) : std::vector<uint32_t> {}) {
				if (!mapped_first(index)) {
					continue;
				}
				if (const auto queued = std::ranges::find(state.module_jobs, index);
				    queued != state.module_jobs.end()) {
					state.module_jobs.erase(queued);
					state.QueueModule(index);
				}
			}
			return false;
		}
		// The guest may unmap or reuse the memory later: the threads read a copy.
		entry.words  = std::make_shared<const std::vector<uint32_t>>(code.begin(), code.end());
		entry.mapped = true;
		for (const auto index: entry.waiting) {
			if (from_guest) {
				mapped_first(index);
			}
			if (--state.missing_code[index] == 0) {
				state.QueueModule(index);
			}
		}
		if (!from_guest) {
			entry.early = entry.waiting;
		}
		entry.waiting     = {};
		state.logged_idle = false;
	}
	state.available.notify_all();
	return true;
}

namespace {

// The code store: CodeStoreMagic, then a record per shader, a CodeRecord followed by the zstd
// frame of its code. The hash of a record is that of its code, which a reader checks. A record
// with hash 0 is not a shader: its words are ScanMark and ScanVersion, then the hashes of the
// listed code that the last scan of the game's files did not find (ScanGameFiles()); the last such
// record counts, unless an older scan, which searched less, wrote it.
constexpr uint64_t CodeStoreMagic = 0x3145444F4359544Bull; // "KTYCODE1"
constexpr uint32_t ScanMark       = 0xffffffffu;
constexpr uint32_t ScanVersion    = 2;
struct CodeRecord {
	uint64_t hash;
	uint32_t words;
	uint32_t compressed_bytes;
};

bool ReadCodeRecord(const CodeRecord& record, const uint8_t* compressed, std::vector<uint32_t>& words) {
	words.resize(record.words);
	const auto bytes = words.size() * sizeof(uint32_t);
	return ZSTD_getFrameContentSize(compressed, record.compressed_bytes) == bytes &&
	       ZSTD_decompress(words.data(), bytes, compressed, record.compressed_bytes) == bytes;
}

// A shader binary ends with a 0x30-byte footer (shader.cpp, ShaderProgramSize()): its magic, a
// version and the sizes of the code, of the literal buffer after it and of the section between
// those and the footer. Calls `found` with the program (what ShaderMapUserData() hashes) of every
// footer in `data` whose program lies in it; returns how many.
template <typename Found>
uint32_t FindShaderPrograms(std::span<const uint8_t> data, Found&& found) {
	constexpr std::string_view Magic      = "barefoot";
	constexpr size_t           FooterSize = 0x30;
	const std::string_view     text(reinterpret_cast<const char*>(data.data()), data.size());
	uint32_t                   count = 0;
	for (size_t at = text.find(Magic); at != std::string_view::npos; at = text.find(Magic, at + 1)) {
		if (data.size() - at < FooterSize) {
			break;
		}
		std::array<uint32_t, 8> footer {};
		std::memcpy(footer.data(), data.data() + at, sizeof(footer));
		const auto program = Common::AlignUp(uint64_t {footer[5]}, 16) + footer[6];
		const auto before  = program + Common::AlignUp(uint64_t {footer[7]}, 16);
		if (footer[4] != 1 || footer[5] == 0 || program % 4 != 0 || before > at) {
			continue;
		}
		count++;
		found(data.subspan(at - before, program));
	}
	return count;
}

// LZ4 block format (what Unreal Engine compresses each shader with): sequences of literals and of
// copies from up to 64 KB back. False unless `src` fills `dst` exactly.
bool Lz4Decompress(std::span<const uint8_t> src, std::span<uint8_t> dst) {
	size_t     s      = 0;
	size_t     d      = 0;
	const auto length = [&](size_t& value) {
		for (uint8_t more = 255; more == 255;) {
			if (s == src.size()) {
				return false;
			}
			more = src[s++];
			value += more;
		}
		return true;
	};
	while (s < src.size()) {
		const auto token    = src[s++];
		size_t     literals = token >> 4u;
		if ((literals == 15 && !length(literals)) || literals > src.size() - s || literals > dst.size() - d) {
			return false;
		}
		std::memcpy(dst.data() + d, src.data() + s, literals);
		s += literals;
		d += literals;
		if (s == src.size()) {
			break; // The last sequence has only literals.
		}
		if (src.size() - s < 2) {
			return false;
		}
		const size_t offset = src[s] | (size_t {src[s + 1]} << 8u);
		s += 2;
		size_t match = token & 15u;
		if (offset == 0 || offset > d || (match == 15 && !length(match)) || match + 4 > dst.size() - d) {
			return false;
		}
		for (const auto end = d + match + 4; d < end; d++) {
			dst[d] = dst[d - offset]; // The copy can overlap what it writes.
		}
	}
	return d == dst.size();
}

// Shaders that Unreal Engine keeps in its packages (FShaderMapResourceCode), each one an array of
// its code compressed with LZ4 (the size, then the bytes), then its size and its stage. The
// footer's magic survives the compression in the literals near the end of the code: for every
// magic in `data` that is not a footer in place, the array whose bytes end a little past the footer
// (44 to 112 bytes after the magic, as found in the packages of GTA III), followed by a plausible size
// and stage, is decompressed into `shader` and `found` called with it.
template <typename Found>
void FindCompressedShaders(std::span<const uint8_t> data, std::vector<uint8_t>& shader, Found&& found) {
	constexpr std::string_view Magic      = "barefoot";
	constexpr size_t           FooterSize = 0x30;
	constexpr uint32_t         MaxSize    = 4u << 20u;
	constexpr uint64_t         MaxStored  = 256u << 10u;
	const std::string_view     text(reinterpret_cast<const char*>(data.data()), data.size());
	const auto                 word = [&](uint64_t at) {
		uint32_t value = 0;
		std::memcpy(&value, data.data() + at, sizeof(value));
		return value;
	};
	uint64_t done = 0; // Where the last array found ends.
	for (size_t at = text.find(Magic); at != std::string_view::npos; at = text.find(Magic, at + 1)) {
		if (at < done) {
			continue;
		}
		if (data.size() - at >= FooterSize) {
			const auto program = Common::AlignUp(uint64_t {word(at + 20)}, 16) + word(at + 24);
			const auto before  = program + Common::AlignUp(uint64_t {word(at + 28)}, 16);
			if (word(at + 16) == 1 && word(at + 20) != 0 && program % 4 == 0 && before <= at) {
				continue; // A footer in place.
			}
		}
		for (uint64_t end = at + FooterSize - 4; end <= at + FooterSize + 64 && end + 5 <= data.size(); end++) {
			const auto size = word(end);
			if (data[end + 4] >= 8 || size < 64 || size > MaxSize) {
				continue;
			}
			// The array's size, `stored` with stored <= size <= 16 * stored, just before its bytes.
			const auto most = std::min({uint64_t {size}, end - 4, MaxStored});
			for (uint64_t stored = std::max<uint64_t>(size / 16, end - at); stored <= most; stored++) {
				if (word(end - 4 - stored) != stored) {
					continue;
				}
				shader.resize(size);
				if (Lz4Decompress(data.subspan(end - stored, stored), shader)) {
					found(std::span<const uint8_t>(shader));
					done = end;
					break;
				}
			}
			if (done == end) {
				break;
			}
		}
	}
}

// Little-endian values and Unreal Engine strings from a span; `ok` turns false past its end.
struct SpanReader {
	std::span<const uint8_t> data;
	size_t                   at = 0;
	bool                     ok = true;

	bool Skip(uint64_t bytes) {
		if (data.size() - at < bytes) {
			ok = false;
			at = data.size();
		} else {
			at += bytes;
		}
		return ok;
	}
	template <typename T>
	T Get() {
		T value {};
		const auto from = at;
		if (Skip(sizeof(T))) {
			std::memcpy(&value, data.data() + from, sizeof(T));
		}
		return value;
	}
	// A length with the terminator, negative for UTF-16 (of which only the low bytes are kept).
	std::string String() {
		const auto  length = Get<int32_t>();
		const auto  units  = length < 0 ? -int64_t {length} : int64_t {length};
		const auto  unit   = length < 0 ? 2u : 1u;
		const auto  from   = at;
		std::string text;
		if (units > 0 && Skip(units * unit)) {
			for (int64_t i = 0; i + 1 < units; i++) {
				text.push_back(static_cast<char>(data[from + i * unit]));
			}
		}
		return text;
	}
};

// An Unreal Engine shader archive (.ushaderbytecode, version 2): hashes, shader maps, a table of
// shaders (offset, stored size, size, stage), preload entries and indices, then their code. Calls
// `found` with the stored bytes and the size of every shader.
template <typename Found>
void ForEachUnrealShader(std::span<const uint8_t> archive, Found&& found) {
	SpanReader reader {archive};
	const auto array = [&](uint32_t element) {
		const auto count = reader.Get<uint32_t>();
		return reader.Skip(uint64_t {count} * element) ? count : 0;
	};
	if (reader.Get<uint32_t>() != 2) {
		return;
	}
	array(20);
	array(20);
	array(16);
	const auto table   = reader.at + 4;
	const auto shaders = array(17);
	array(16);
	array(4);
	if (!reader.ok) {
		return;
	}
	const auto code = archive.subspan(reader.at);
	for (uint32_t i = 0; i < shaders; i++) {
		SpanReader entry {archive, table + i * 17};
		const auto offset = entry.Get<uint64_t>();
		const auto stored = entry.Get<uint32_t>();
		const auto size   = entry.Get<uint32_t>();
		if (offset <= code.size() && stored <= code.size() - offset) {
			found(code.subspan(offset, stored), size);
		}
	}
}

// What an Unreal Engine package (.pak, versions 3 to 7 and 10 to 11, index not encrypted) holds that
// has shaders: `read(offset, size, out)` reads the package, of `size` bytes. Calls `archive` with
// each shader archive (.ushaderbytecode) stored as it is, and `assets` with each asset (.uexp,
// .uasset, .umap) and each .bin (the global shaders) compressed with zlib, inflated, or stored as it
// is (assets up to 1 MB, the materials, and maps whole, with the materials of their level: these
// keep their shaders inline, the textures and meshes do not). Its shaders are found compressed by
// FindCompressedShaders(), but not inside the zlib streams. Returns whether the index was read.
template <typename Read, typename Archive, typename Assets>
bool ForEachUnrealEntry(uint64_t size, Read&& read, Archive&& archive, Assets&& assets) {
	// The footer: (from version 7, encryption key) whether the index is encrypted (from version
	// 4), the magic, the version, the index's offset and size, its hash and (from version 8) the
	// names of the compression methods.
	constexpr uint32_t   Magic    = 0x5A6F12E1u;
	constexpr uint64_t   MaxAsset = 1u << 20u;
	std::vector<uint8_t> data;
	const auto           tail = std::min<uint64_t>(size, 1024);
	if (!read(size - tail, tail, data)) {
		return false;
	}
	size_t magic = 0;
	for (size_t at = data.size() >= 24 ? data.size() - 24 : 0; at > 17 && magic == 0; at--) {
		if (std::memcmp(data.data() + at, &Magic, 4) == 0) {
			magic = at;
		}
	}
	SpanReader footer {data, magic + 4};
	const auto version      = footer.Get<uint32_t>();
	const auto index_offset = footer.Get<uint64_t>();
	const auto index_size   = footer.Get<uint64_t>();
	footer.Skip(20);
	// The compression methods: by index from 1 from version 8, flags before (zlib is 1).
	std::array<bool, 6> zlib {false, version < 8};
	for (size_t method = 1; version >= 8 && method < zlib.size() && footer.Skip(0) &&
	                        footer.data.size() - footer.at >= 32;
	     method++) {
		const auto* name = reinterpret_cast<const char*>(data.data() + footer.at);
		zlib[method]     = std::string_view(name, strnlen(name, 32)) == "Zlib";
		footer.Skip(32);
	}
	const bool legacy = version >= 3 && version <= 7;
	if (magic == 0 || (version >= 4 && data[magic - 1] != 0) || (!legacy && version != 10 && version != 11) ||
	    index_offset > size || index_size > size - index_offset || index_size > (256u << 20u) ||
	    !read(index_offset, index_size, data)) {
		return false;
	}
	std::vector<uint8_t> entry_data;
	std::vector<uint8_t> inflated;
	// An entry: its name, the offset of its header, its size, stored size, compression method
	// and number of blocks.
	const auto visit = [&](std::string name, uint64_t offset, uint64_t length, uint64_t stored, uint32_t method,
	                       uint64_t blocks) {
		std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const bool is_archive = name.ends_with(".ushaderbytecode");
		const bool is_global  = name.ends_with(".bin");
		const bool is_map     = name.ends_with(".umap");
		const bool is_asset   = is_map || name.ends_with(".uexp") || name.ends_with(".uasset");
		// The data follows the entry's header: offset, sizes, method, hash, the blocks (start
		// and end, from the header from version 5, from the package's start before) when
		// compressed, flags and block size.
		const uint64_t header = 8 + 8 + 8 + 4 + 20 + (method != 0 ? 4 + 16 * blocks : 0) + 1 + 4;
		const bool     stored_archive = is_archive && method == 0;
		const bool     readable_asset = (is_global || (is_asset && (method != 0 || is_map || length <= MaxAsset))) &&
		                            (method == 0 || (method < zlib.size() && zlib[method]));
		if ((!stored_archive && !readable_asset) || offset > size || stored > size - offset ||
		    header > size - offset - stored || length > (256u << 20u) || !read(offset, header + stored, entry_data)) {
			return;
		}
		SpanReader copy {entry_data, 8};
		if (copy.Get<uint64_t>() != stored || copy.Get<uint64_t>() != length) {
			return;
		}
		if (method == 0) {
			const auto stored_data = std::span<const uint8_t>(entry_data).subspan(header);
			is_archive ? archive(stored_data) : assets(stored_data);
			return;
		}
		copy.Skip(4 + 20 + 4);
		inflated.resize(length);
		const uint64_t base   = version < 5 ? offset : 0;
		uint64_t       filled = 0;
		for (uint64_t block = 0; block < blocks && copy.ok; block++) {
			const auto start = copy.Get<uint64_t>() - base;
			const auto end   = copy.Get<uint64_t>() - base;
			if (start < header || end < start || end > entry_data.size() || end - start > INT32_MAX) {
				break;
			}
			const auto out = stbi_zlib_decode_buffer(
			    reinterpret_cast<char*>(inflated.data() + filled), static_cast<int>(length - filled),
			    reinterpret_cast<const char*>(entry_data.data() + start), static_cast<int>(end - start));
			if (out <= 0) {
				break;
			}
			filled += static_cast<uint64_t>(out);
		}
		assets(std::span<const uint8_t>(inflated).first(filled));
	};
	SpanReader index {data};
	index.String();
	if (legacy) {
		// The index: mount point and entries, each its name, then as in its header (offset,
		// sizes, method, hash, the blocks when compressed, flags and block size).
		const auto entries = index.Get<uint32_t>();
		for (uint32_t entry = 0; entry < entries && index.ok; entry++) {
			auto           name   = index.String();
			const auto     offset = index.Get<uint64_t>();
			const auto     stored = index.Get<uint64_t>();
			const auto     length = index.Get<uint64_t>();
			const auto     method = index.Get<uint32_t>();
			uint64_t       blocks = 0;
			index.Skip(20);
			if (method != 0) {
				blocks = index.Get<uint32_t>();
				index.Skip(16 * blocks);
			}
			const bool encrypted = index.Get<uint8_t>() != 0;
			index.Skip(4);
			if (index.ok && !encrypted && method < zlib.size()) {
				visit(std::move(name), offset, length, stored, method, blocks);
			}
		}
		return index.ok;
	}
	// The index: mount point, entries, path hash index, full directory index (where the names
	// are), and the entries, encoded.
	index.Skip(4 + 8);
	if (index.Get<uint32_t>() != 0) {
		index.Skip(8 + 8 + 20);
	}
	if (index.Get<uint32_t>() == 0) {
		return false;
	}
	const auto directory_offset = index.Get<uint64_t>();
	const auto directory_size   = index.Get<uint64_t>();
	index.Skip(20);
	const auto           encoded_size = index.Get<uint32_t>();
	const auto           encoded_at   = index.at;
	std::vector<uint8_t> directory;
	if (!index.Skip(encoded_size) || directory_offset > size || directory_size > size - directory_offset ||
	    directory_size > (256u << 20u) || !read(directory_offset, directory_size, directory)) {
		return false;
	}
	const auto encoded = std::span<const uint8_t>(data).subspan(encoded_at, encoded_size);
	SpanReader names {directory};
	const auto folders = names.Get<uint32_t>();
	for (uint32_t folder = 0; folder < folders && names.ok; folder++) {
		names.String();
		const auto files = names.Get<uint32_t>();
		for (uint32_t file = 0; file < files && names.ok; file++) {
			auto       name  = names.String();
			const auto entry = names.Get<int32_t>();
			if (entry < 0) {
				continue;
			}
			// An encoded entry: flags (compression method, encryption, block count, and which
			// of the offset and sizes take 32 bits), the offset of the entry's header, the size
			// and, compressed, the stored size.
			SpanReader     reader {encoded, static_cast<size_t>(entry)};
			const auto     flags  = reader.Get<uint32_t>();
			const auto     method = (flags >> 23u) & 0x3fu;
			const uint64_t offset = (flags & (1u << 31u)) != 0 ? reader.Get<uint32_t>() : reader.Get<uint64_t>();
			const uint64_t length = (flags & (1u << 30u)) != 0 ? reader.Get<uint32_t>() : reader.Get<uint64_t>();
			uint64_t       stored = length;
			if (method != 0) {
				stored = (flags & (1u << 29u)) != 0 ? reader.Get<uint32_t>() : reader.Get<uint64_t>();
			}
			if (reader.ok && (flags & (1u << 22u)) == 0) {
				visit(std::move(name), offset, length, stored, method, (flags >> 6u) & 0xffffu);
			}
		}
	}
	return names.ok;
}

} // namespace

// The code of the listed shaders that neither the store nor the guest has given yet, from the
// game's files: every file is searched for shader binaries, whatever its format (a game keeps
// them in packages of its own, mostly as they are; those inside compressed packages are not
// found, and wait until the guest maps them, but for the shaders of Unreal Engine, each one
// compressed with LZ4 in an archive or an asset, which are read from the packages that hold them as
// they are or compressed with zlib, and zstd frames, which are decompressed where they are found).
// First, a kind of file (by extension) is left out
// after a sample in a row without shaders, and a file after as many bytes without one, so that a
// game's textures, audio and video, and what follows the shaders in a large package, are not read
// whole. Then, while listed code is still missing, what that left out is probed with a small
// window every few MB, and the stretch around a window with shaders is read whole: a game can
// keep its shaders in one package of a kind whose first packages hold none, or deep inside a
// package of several GB. A kind is not probed any more after many files in a row that gave no
// missing code (a game can have tens of thousands of small files of a kind, and its shader
// packages hold more shaders than the manifest lists). It ends as soon as nothing listed is
// missing. `append` stores each program found that a record waited for. False when there are no
// files to search.
bool PipelineCache::ScanGameFiles(const std::stop_token&                                          stop,
                                  const std::function<void(uint64_t, std::span<const uint32_t>)>& append) {
	auto& state = *m_manifest;
	// The game's folder is mounted after the renderer starts.
	std::filesystem::path root;
	for (uint32_t tries = 0; tries < 200 && !stop.stop_requested(); tries++) {
		root = LibKernel::FileSystem::GetRealFilename("/app0");
		if (!root.empty()) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	if (root.empty()) {
		return false;
	}
	// A kind of file is left out after this many files, or bytes, in a row without a shader.
	constexpr uint32_t SampleFiles = 32;
	constexpr uint64_t SampleBytes = 256ull << 20u;
	// Large files are read in pieces that overlap by more than a shader binary takes.
	constexpr uint32_t Piece   = 64u << 20u;
	constexpr uint32_t Overlap = 4u << 20u;
	// The probes: a window every so many bytes; around a window with shaders, pieces until this
	// many bytes hold none.
	constexpr uint32_t ProbeBytes  = 1u << 20u;
	constexpr uint64_t ProbeStride = 32ull << 20u;
	constexpr uint32_t RunPiece    = 8u << 20u;
	constexpr uint64_t RunQuiet    = 16ull << 20u;
	// A kind of file is not probed any more after this many files in a row that gave no missing
	// code.
	constexpr uint32_t ProbeFiles = 256;
	const auto         start      = std::chrono::steady_clock::now();
	size_t             listed     = 0;
	{
		std::scoped_lock lock(state.mutex);
		listed = state.code.size();
	}
	std::unordered_set<uint64_t> found;        // Every program found.
	std::unordered_set<uint64_t> found_listed; // Those of them the manifest lists.
	uint32_t                     provided = 0;
	uint32_t                     files    = 0;
	uint64_t                     bytes    = 0;
	std::vector<uint8_t>         data;
	std::vector<uint32_t>        code;
	// Searches `data`; returns how many shader binaries it holds.
	const auto search = [&](std::span<const uint8_t> chunk) {
		return FindShaderPrograms(chunk, [&](std::span<const uint8_t> program) {
			const auto hash = XXH3_64bits(program.data(), program.size());
			if (!found.insert(hash).second) {
				return;
			}
			{
				std::scoped_lock lock(state.mutex);
				const auto       entry = state.code.find(hash);
				if (entry == state.code.end()) {
					return;
				}
				found_listed.insert(hash);
				if (entry->second.mapped) {
					return;
				}
			}
			code.resize(program.size() / sizeof(uint32_t));
			std::memcpy(code.data(), program.data(), program.size());
			if (ProvideCode(hash, code, false)) {
				provided++;
				append(hash, code);
			}
		});
	};
	// Whether no listed code is missing any more (the guest maps code meanwhile too); counted
	// now and then.
	auto       next_count      = start;
	bool       missing         = true;
	const auto nothing_missing = [&] {
		if (const auto now = std::chrono::steady_clock::now(); now >= next_count) {
			next_count = now + std::chrono::milliseconds(250);
			std::scoped_lock lock(state.mutex);
			missing = std::ranges::any_of(state.code | std::views::values,
			                              [](const auto& entry) { return !entry.mapped; });
		}
		return !missing;
	};
	// Reads `file` from `from` in pieces and searches them. Stops at `end`, or past `reach` once
	// `quiet` bytes in a row held no shader (`quiet_after` once one was found, when not 0).
	struct Run {
		uint64_t end;
		uint32_t shaders;
		uint64_t silent; // Bytes at the end without a shader.
	};
	// The zstd frames that begin and end in `chunk` (a package can compress its entries with zstd, as
	// the .rpf packages of Red Dead Redemption do with its shaders), decompressed and searched.
	const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> frames(ZSTD_createDCtx(), &ZSTD_freeDCtx);
	std::vector<uint8_t>                                       frame;
	const auto search_frames = [&](std::span<const uint8_t> chunk) {
		constexpr std::string_view Magic("\x28\xb5\x2f\xfd", 4);
		constexpr uint64_t         MaxFrame = 64u << 20u;
		const std::string_view     text(reinterpret_cast<const char*>(chunk.data()), chunk.size());
		uint32_t                   shaders = 0;
		for (size_t at = text.find(Magic); at != std::string_view::npos && frames != nullptr;
		     at        = text.find(Magic, at + 1)) {
			const auto stored = ZSTD_findFrameCompressedSize(chunk.data() + at, chunk.size() - at);
			if (ZSTD_isError(stored) != 0) {
				continue;
			}
			const auto size = ZSTD_getFrameContentSize(chunk.data() + at, stored);
			if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR || size > MaxFrame) {
				continue;
			}
			frame.resize(size);
			const auto done = ZSTD_decompressDCtx(frames.get(), frame.data(), frame.size(), chunk.data() + at, stored);
			if (ZSTD_isError(done) == 0) {
				shaders += search(std::span<const uint8_t>(frame).first(done));
				at += stored - 1;
			}
		}
		return shaders;
	};
	const auto read_run = [&](Common::File& file, uint64_t from, uint64_t reach, uint64_t quiet,
	                          uint64_t end, uint32_t piece, uint64_t quiet_after) {
		Run    run {from, 0, 0};
		size_t kept = 0;
		if (!file.Seek(from)) {
			return run;
		}
		while (run.end < end && !stop.stop_requested()) {
			const auto size = static_cast<uint32_t>(std::min<uint64_t>(end - run.end, piece));
			data.resize(kept + size);
			uint32_t read = 0;
			file.Read(data.data() + kept, size, &read);
			if (read != size) {
				break;
			}
			run.end += size;
			bytes += size;
			const auto shaders = search(data) + search_frames(data);
			run.shaders += shaders;
			run.silent = shaders != 0 ? 0 : run.silent + size;
			if (shaders != 0 && quiet_after != 0) {
				quiet = quiet_after;
			}
			if (run.end >= reach && run.silent >= quiet) {
				break;
			}
			kept = std::min<size_t>(data.size(), Overlap);
			std::memmove(data.data(), data.data() + data.size() - kept, kept);
		}
		return run;
	};
	// Unreal Engine: the shader archives in a package, or a loose one.
	std::vector<uint8_t> shader;
	const auto           search_archive = [&](std::span<const uint8_t> archive) {
		ForEachUnrealShader(archive, [&](std::span<const uint8_t> stored, uint32_t size) {
			if (stored.size() == size) {
				search(stored);
				return;
			}
			if (size > (16u << 20u)) {
				return;
			}
			shader.resize(size);
			if (Lz4Decompress(stored, shader)) {
				search(shader);
			}
		});
	};
	// Whether the package's index (or the whole loose archive) was read.
	const auto search_unreal = [&](const std::filesystem::path& path, const std::string& extension) {
		Common::File file;
		if (!file.Open(path, Common::File::Mode::Read)) {
			return false;
		}
		const auto read = [&](uint64_t offset, uint64_t size, std::vector<uint8_t>& out) {
			uint32_t done = 0;
			if (size > (1u << 30u) || !file.Seek(offset)) {
				return false;
			}
			out.resize(size);
			file.Read(out.data(), static_cast<uint32_t>(size), &done);
			bytes += done;
			return done == size;
		};
		std::vector<uint8_t> archive;
		if (extension == ".pak") {
			return ForEachUnrealEntry(file.Size(), read, search_archive, [&](std::span<const uint8_t> asset) {
				search(asset);
				FindCompressedShaders(asset, archive, [&](std::span<const uint8_t> shader) { search(shader); });
			});
		}
		if (!read(0, file.Size(), archive)) {
			return false;
		}
		search_archive(archive);
		return true;
	};
	// Files and bytes of each kind in a row without a shader, and files probed in a row that gave
	// no missing code.
	struct Kind {
		uint32_t files  = 0;
		uint64_t bytes  = 0;
		uint32_t probed = 0;
	};
	std::unordered_map<std::string, Kind> kinds;
	std::vector<std::filesystem::path>    folders {root};
	// The files the sampling left out, and where it cut others short.
	struct Later {
		std::filesystem::path path;
		uint64_t              from;
		Kind*                 kind;
	};
	std::vector<Later> later;
	while (!folders.empty() && !stop.stop_requested() && !nothing_missing()) {
		const auto folder = std::move(folders.back());
		folders.pop_back();
		for (const auto& entry: Common::File::GetDirEntries(folder)) {
			if (stop.stop_requested() || nothing_missing()) {
				break;
			}
			if (entry.name == "." || entry.name == "..") {
				continue;
			}
			const auto path = folder / Common::PathFromUtf8(entry.name);
			if (!entry.is_file) {
				folders.push_back(path);
				continue;
			}
			auto extension = Common::PathToString(path.extension());
			std::ranges::transform(extension, extension.begin(),
			                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (extension == ".pak" || extension == ".ushaderbytecode") {
				search_unreal(path, extension);
			}
			auto& kind = kinds[extension];
			if (kind.files >= SampleFiles || kind.bytes >= SampleBytes) {
				later.push_back({path, 0, &kind});
				continue;
			}
			Common::File file;
			if (!file.Open(path, Common::File::Mode::Read)) {
				continue;
			}
			files++;
			// The end of the kind's sample ends the file too, and after a shader, as many bytes
			// without one.
			const auto size = file.Size();
			const auto run  = read_run(file, 0, 0, SampleBytes - kind.bytes, size, Piece, SampleBytes);
			if (run.shaders != 0) {
				kind.files = 0;
				kind.bytes = run.silent;
			} else {
				kind.files++;
				kind.bytes += run.end;
			}
			if (run.end < size) {
				later.push_back({path, run.end, &kind});
			}
		}
	}
	for (const auto& [path, from, kind]: later) {
		if (stop.stop_requested() || nothing_missing()) {
			break;
		}
		if (kind->probed >= ProbeFiles) {
			continue;
		}
		Common::File file;
		if (!file.Open(path, Common::File::Mode::Read)) {
			continue;
		}
		files += from == 0 ? 1 : 0;
		// The shaders around a window that holds some begin after the window before it.
		const auto size   = file.Size();
		const auto before = provided;
		uint64_t   read   = from;
		for (uint64_t at = from; at < size && !stop.stop_requested() && !nothing_missing();) {
			const auto window = std::min<uint64_t>(size, at + ProbeBytes);
			if (read_run(file, at, window, 0, window, ProbeBytes, 0).shaders == 0) {
				at += ProbeStride;
				continue;
			}
			const auto begin = std::max(read, at >= from + ProbeStride ? at - ProbeStride + ProbeBytes : from);
			read             = read_run(file, begin, window, RunQuiet, size, RunPiece, 0).end;
			at               = std::max(at + ProbeStride, read);
		}
		kind->probed = provided != before ? 0 : kind->probed + 1;
	}
	ManifestLog("[ShaderScan] game files: {} shaders found ({} of {} listed by the manifest, {} new), {:.1f} s "
	            "({} files, {:.0f} MB read)",
	            found.size(), found_listed.size(), listed, provided,
	            Seconds(std::chrono::steady_clock::now() - start), files,
	            static_cast<double>(bytes) / (1024.0 * 1024.0));
	return true;
}

void PipelineCache::CodeStoreThread(const std::stop_token& stop) {
	KYTY_PROFILER_THREAD("Thread_PipelineManifestCode");
	auto& state = *m_manifest;
	// The records of the file: those that do not read end it (a session that was killed while it
	// wrote one), and the next record is written in their place.
	std::unordered_set<uint64_t> stored;
	std::unordered_set<uint64_t> not_in_files;
	uint64_t                     valid = 0;
	{
		const auto bytes = ReadFile(state.code_path);
		uint64_t   magic = 0;
		if (bytes.size() >= sizeof(magic)) {
			std::memcpy(&magic, bytes.data(), sizeof(magic));
		}
		uint32_t              provided = 0;
		std::vector<uint32_t> code;
		for (size_t at = sizeof(magic); magic == CodeStoreMagic;) {
			CodeRecord record {};
			if (bytes.size() - at < sizeof(record)) {
				break;
			}
			std::memcpy(&record, bytes.data() + at, sizeof(record));
			if (record.words == 0 || bytes.size() - at - sizeof(record) < record.compressed_bytes) {
				break;
			}
			const auto* compressed = bytes.data() + at + sizeof(record);
			at += sizeof(record) + record.compressed_bytes;
			valid = at;
			if (record.hash == 0) {
				not_in_files.clear();
				if (ReadCodeRecord(record, compressed, code) && code.size() >= 2 && code[0] == ScanMark &&
				    code[1] == ScanVersion) {
					for (size_t i = 2; i + 1 < code.size(); i += 2) {
						not_in_files.insert(code[i] | (uint64_t {code[i + 1]} << 32u));
					}
				}
				continue;
			}
			stored.insert(record.hash);
			bool wanted = false;
			{
				std::scoped_lock lock(state.mutex);
				const auto       found = state.code.find(record.hash);
				wanted = found != state.code.end() && !found->second.mapped;
			}
			if (wanted && !stop.stop_requested() && ReadCodeRecord(record, compressed, code) &&
			    XXH3_64bits(code.data(), code.size() * sizeof(uint32_t)) == record.hash) {
				provided += ProvideCode(record.hash, code, false) ? 1u : 0u;
			}
		}
		if (provided != 0) {
			ManifestLog("Pipeline manifest: the code of {} shaders comes from {} ({} shaders)", provided,
			            Common::PathToString(state.code_path), stored.size());
		}
	}
	Common::File file;
	bool         opened = false;
	const auto   append = [&](uint64_t hash, std::span<const uint32_t> code) {
		if (hash != 0 && !stored.insert(hash).second) {
			return;
		}
		if (!opened) {
			opened = true;
			Common::File::CreateDirectories(state.code_path.parent_path());
			if (valid != 0 && file.Open(state.code_path, Common::File::Mode::ReadWrite)) {
				file.Truncate(valid);
				file.Seek(valid);
			} else if (file.Create(state.code_path)) {
				file.Write(&CodeStoreMagic, sizeof(CodeStoreMagic));
			}
		}
		if (file.IsInvalid()) {
			return;
		}
		std::vector<uint8_t> bytes(sizeof(CodeRecord) + ZSTD_compressBound(code.size_bytes()));
		const auto size = ZSTD_compress(bytes.data() + sizeof(CodeRecord), bytes.size() - sizeof(CodeRecord),
		                                code.data(), code.size_bytes(), 3);
		if (ZSTD_isError(size)) {
			return;
		}
		const CodeRecord record {.hash             = hash,
		                         .words            = static_cast<uint32_t>(code.size()),
		                         .compressed_bytes = static_cast<uint32_t>(size)};
		std::memcpy(bytes.data(), &record, sizeof(record));
		// Not flushed: a record a crash cuts short does not read and ends the file (above), and
		// flushing every record made a scan that finds thousands of shaders take minutes.
		file.Write(bytes.data(), static_cast<uint32_t>(sizeof(record) + size));
	};
	const auto listed_code = [&] {
		std::scoped_lock lock(state.mutex);
		return state.code.size();
	};
	// The listed code still missing: the game's files are searched for it, unless the last search
	// missed all of it already.
	const auto missing = [&] {
		std::vector<uint64_t> hashes;
		std::scoped_lock      lock(state.mutex);
		for (const auto& [hash, entry]: state.code) {
			if (!entry.mapped) {
				hashes.push_back(hash);
			}
		}
		std::ranges::sort(hashes);
		return hashes;
	};
	if (const auto still = missing();
	    std::ranges::any_of(still, [&](uint64_t hash) { return !not_in_files.contains(hash); })) {
		if (ScanGameFiles(stop, append) && !stop.stop_requested()) {
			std::vector<uint32_t> words {ScanMark, ScanVersion};
			for (const auto hash: missing()) {
				words.push_back(static_cast<uint32_t>(hash));
				words.push_back(static_cast<uint32_t>(hash >> 32u));
			}
			append(0, words);
		}
	} else if (still.empty()) {
		ManifestLog("[ShaderScan] game files: not searched (the code store has all {} shaders the manifest lists)",
		            listed_code());
	} else {
		ManifestLog("[ShaderScan] game files: not searched ({} of the {} shaders the manifest lists are missing, "
		            "and the last search did not find them)",
		            still.size(), listed_code());
	}
	state.scanning.store(false, std::memory_order_release);
	// From here on it only writes what the guest maps.
	Common::Thread::LowerCurrentPriority();
	for (;;) {
		std::pair<uint64_t, std::vector<uint32_t>> job;
		{
			std::unique_lock lock(state.code_mutex);
			// What is queued is written before the thread stops.
			state.code_available.wait(lock, stop, [&] { return !state.code_queue.empty(); });
			if (state.code_queue.empty()) {
				return;
			}
			job = std::move(state.code_queue.front());
			state.code_queue.pop_front();
		}
		append(job.first, job.second);
	}
}

void PipelineCache::ManifestWorker(const std::stop_token& stop) {
	KYTY_PROFILER_THREAD("Thread_PipelineManifest");
	Common::Thread::LowerCurrentPriority();
	auto& state = *m_manifest;
	for (;;) {
		std::shared_ptr<const std::vector<uint32_t>> code;
		std::shared_ptr<const std::vector<uint32_t>> back_code;
		uint32_t                                     index    = 0;
		bool                                         pipeline = false;
		{
			std::unique_lock lock(state.mutex);
			// What the game used first before goes first. Otherwise pipelines first: their
			// modules are ready, and they are what stalls a draw.
			if (!state.available.wait(lock, stop, [&] {
				    return !state.pipeline_jobs.empty() || !state.module_jobs.empty();
			    })) {
				return;
			}
			pipeline = !state.pipeline_jobs.empty() &&
			           (state.module_jobs.empty() ||
			            state.PipelineRank(state.pipeline_jobs.front()) <=
			                state.module_rank[state.module_jobs.front()]);
			auto& jobs = pipeline ? state.pipeline_jobs : state.module_jobs;
			index      = jobs.front();
			jobs.pop_front();
			if (!pipeline) {
				const auto& module = state.warm.modules[index];
				for (auto [hash, words]: {std::pair {module.code_hash, &code},
				                          std::pair {module.back_hash, &back_code}}) {
					if (hash == 0) {
						continue;
					}
					auto& entry = state.code.at(hash);
					*words      = entry.words;
					if (--entry.readers == 0) {
						entry.words = nullptr;
					}
				}
			}
			state.running++;
		}
		if (pipeline) {
			CreateManifestPipeline(index);
		} else {
			BuildManifestModule(index, *code,
			                    back_code ? std::span<const uint32_t>(*back_code)
			                              : std::span<const uint32_t> {});
		}
		{
			std::scoped_lock lock(state.mutex);
			state.running--;
		}
		ManifestProgress(false);
	}
}

void PipelineCache::BuildManifestModule(uint32_t index, std::span<const uint32_t> code,
                                        std::span<const uint32_t> back_code, bool guest_traversal) {
	auto&       state  = *m_manifest;
	const auto& record = state.warm.modules[index];
	const auto  start  = std::chrono::steady_clock::now();
	auto        input  = record.input;
	// As GetProgram() and RunCompileJob() set them; translation only reads the user data count.
	const std::vector<uint32_t>      user_data(record.user_data_count);
	ShaderRecompiler::CompileOptions options;
	ProgramKey                       key {.stage           = record.stage,
	                                      .hash            = record.ProgramHash(),
	                                      .user_data_count = record.user_data_count};
	std::visit(
	    [&]<typename InputInfo>(InputInfo& info) {
		    options.wave_size = info.wave_size;
		    if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			    options.stage             = info.logical_stage;
			    options.input_info.vertex = &info;
			    options.user_data_base    = 8;
			    if (options.stage == ShaderType::Mesh ||
			        options.stage == ShaderType::TessellationControl) {
				    options.user_data_base = 0;
			    }
			    if (options.stage == ShaderType::Mesh) {
				    options.wave_size = info.mesh.wave_size;
			    }
		    } else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			    options.stage            = ShaderType::Pixel;
			    options.input_info.pixel = &info;
		    } else {
			    options.stage              = ShaderType::Compute;
			    options.input_info.compute = &info;
		    }
		    BuildStageStaticKey(info, key.static_state);
	    },
	    input);
	// With native ray tracing on, dispatches look compute programs up as the compiler patches
	// them, under their own keys (AppendNativeRayTracingKey()); unpatched, the manifest's would
	// serve none, and each dispatch would translate its program and create its pipeline on the
	// execution thread. The guest traversal serves the dispatches without a TLAS
	// (GetComputeProgram()): `guest_traversal` builds that variant of a patched program.
	const bool native = NativeRayTracing::PatchShaders() && !guest_traversal;
	AppendNativeRayTracingKey(options.stage, key.static_state, native);
	options.shader_hash        = key.hash;
	options.user_data          = user_data;
	options.back_code          = back_code;
	options.dump_ir            = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
	options.emit_debug_info    = Config::GraphicsDebugDumpEnabled();
	options.native_ray_tracing = native;
	options.ray_stats          = native && Config::RayTracingStatsEnabled();
	// The SPIR-V of the module this thread builds (none for one another thread compiled).
	std::vector<uint32_t> spirv;
	std::vector<uint32_t> plain_spirv;

	const auto finish = [&](const ShaderModule* module, bool built) {
		if (guest_traversal) {
			if (module != nullptr && module->info.stage == ShaderType::Compute) {
				CreateManifestComputePipeline(*module, built ? spirv : std::vector<uint32_t> {});
			}
			return;
		}
		const auto elapsed =
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                              std::chrono::steady_clock::now() - start)
		                              .count());
		{
			std::scoped_lock lock(state.mutex);
			state.modules[index] = module;
			if (module == nullptr) {
				state.failed++;
			} else {
				(built ? state.built : state.found)++;
				state.build_ns += built ? elapsed : 0u;
				if (built) {
					state.module_time.Add(static_cast<double>(elapsed) / 1e9);
				}
				state.module_records[module->id] = {index, false};
				if (module->plain != nullptr) {
					state.module_records[module->plain->id] = {index, true};
				}
			}
			for (const auto pipeline: state.module_pipelines[index]) {
				if (module == nullptr) {
					state.missing_modules[pipeline] = UINT8_MAX;
				} else if (state.missing_modules[pipeline] != UINT8_MAX &&
				           --state.missing_modules[pipeline] == 0) {
					state.QueuePipeline(pipeline);
				}
			}
		}
		state.available.notify_all();
		if (module != nullptr && module->info.stage == ShaderType::Compute) {
			CreateManifestComputePipeline(*module, built ? spirv : std::vector<uint32_t> {});
		}
		if (native && record.stage == ShaderType::Compute &&
		    (module == nullptr || module->info.info.ray_query)) {
			BuildManifestModule(index, code, back_code, true);
		}
	};
	// The record's, or the one a program that reads its strides at run time has for it.
	auto                specialization = record.specialization;
	const ShaderModule* compiled       = nullptr;
	LockPrograms();
	compiled = FindCompiledModule(key, specialization, record.push_data_cursor);
	UnlockPrograms();
	if (compiled != nullptr) {
		finish(compiled, false);
		return;
	}
	// Programs that link the functions they call from guest memory are never listed.
	auto source = ShaderRecompiler::PrepareShaderSource(code, options);
	if (!source.calls.empty() || source.unsupported_call_pc != UINT32_MAX) {
		finish(nullptr, false);
		return;
	}
	auto translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
	// A record from a build that found other resources in the program: no dispatch or draw of
	// this build asks for its specialization.
	if (!ShaderRecompiler::IR::ResourceSpecializationFits(translated.program,
	                                                      record.specialization)) {
		finish(nullptr, false);
		return;
	}
	auto plan       = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
	// A program that reads its strides at run time may have records with strides, of sessions
	// that had them compiled in: they all are its one module's.
	if (record.stage == ShaderType::Compute && RuntimeStride(key.hash)) {
		ShaderRecompiler::IR::ReadStridesAtRunTime(plan, specialization);
		LockPrograms();
		compiled = FindCompiledModule(key, specialization, record.push_data_cursor);
		UnlockPrograms();
		if (compiled != nullptr) {
			finish(compiled, false);
			return;
		}
	}
	auto module = BuildModule(key, options, std::move(translated), specialization, {},
	                          record.push_data_cursor, spirv, plain_spirv);
	bool added  = false;
	{
		LockPrograms();
		auto entry = m_program_cache.find(key);
		if (entry == m_program_cache.end()) {
			entry =
			    m_program_cache.try_emplace(key, Program {.resource_plan = std::move(plan)}).first;
		}
		auto& program = entry->second;
		compiled      = FindCompiledModule(key, specialization, record.push_data_cursor);
		if (compiled != nullptr || program.skip_dispatch || program.call_source != nullptr) {
			DestroyModule(module);
		} else {
			auto& added_module = program.modules.emplace_back(std::move(module));
			added_module.plan  = &program.resource_plan;
			compiled           = &added_module;
			StoreModule(key, program, *compiled, spirv, plain_spirv);
			added = true;
		}
		UnlockPrograms();
	}
	finish(compiled, added);
}

void PipelineCache::CreateManifestPipeline(uint32_t index) {
	auto&                              state  = *m_manifest;
	const auto&                        record = state.warm.pipelines[index];
	std::array<const ShaderModule*, 4> stages {};
	auto                               key = record.key;
	{
		std::scoped_lock lock(state.mutex);
		for (size_t i = 0; i < stages.size(); i++) {
			if (record.stages[i] != ManifestPipeline::NoStage) {
				stages[i] = state.modules[record.stages[i]];
			}
		}
	}
	if (record.plain_pixel && stages[3] != nullptr) {
		stages[3] = stages[3]->plain.get();
	}
	if (stages[0] == nullptr ||
	    (record.stages[3] != ManifestPipeline::NoStage && stages[3] == nullptr)) {
		std::scoped_lock lock(state.mutex);
		state.pipelines_failed++;
		return;
	}
	for (size_t i = 0; i < stages.size(); i++) {
		key.stage_ids[i] = stages[i] != nullptr ? stages[i]->id : 0u;
	}
	{
		std::scoped_lock lock(m_async_mutex);
		if (!m_claimed_graphics.insert(key).second) {
			std::scoped_lock manifest_lock(state.mutex);
			state.pipelines_known++;
			return;
		}
		m_async_pending_graphics.insert(key);
	}
	const auto start = std::chrono::steady_clock::now();
	Pipeline   pipeline;
	CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
	const bool created = pipeline.pipeline != nullptr && pipeline.pipeline_layout != nullptr;
	if (created) {
		NoteCreatedPipeline(start);
	}
	{
		std::scoped_lock lock(m_async_mutex);
		if (created) {
			m_async_ready.try_emplace(key, pipeline);
		} else {
			m_async_failed_graphics.insert(key);
		}
		m_async_pending_graphics.erase(key);
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;
	std::scoped_lock lock(state.mutex);
	(created ? state.pipelines : state.pipelines_failed)++;
	state.pipeline_ns += static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
	if (created) {
		state.pipeline_time.Add(std::chrono::duration<double>(elapsed).count());
	}
}

void PipelineCache::CreateManifestComputePipeline(const ShaderModule&          module,
                                                  const std::vector<uint32_t>& spirv) {
	{
		std::scoped_lock lock(m_async_mutex);
		if (!m_claimed_compute.insert(module.id).second) {
			return;
		}
		m_async_pending_compute.insert(module.id);
	}
	// Helper processes compile it, side by side (see PipelineHelpers).
	if (CreateComputePipelineElsewhere(module, spirv, false)) {
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	Pipeline   pipeline;
	CreatePipelineInternal(m_graphics, pipeline, module, m_driver_cache);
	const bool created = pipeline.pipeline != nullptr && pipeline.pipeline_layout != nullptr;
	if (created) {
		NoteCreatedPipeline(start);
	}
	{
		std::scoped_lock lock(m_async_mutex);
		if (created) {
			m_async_ready_compute.try_emplace(module.id, pipeline);
		}
		m_async_pending_compute.erase(module.id);
	}
	auto&            state = *m_manifest;
	std::scoped_lock lock(state.mutex);
	state.compute += created ? 1u : 0u;
	state.pipeline_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                               std::chrono::steady_clock::now() - start)
	                                               .count());
}

PipelineCache::ManifestBacklog PipelineCache::GetManifestBacklog() const {
	ManifestBacklog backlog;
	if (m_manifest == nullptr) {
		return backlog;
	}
	auto&            state = *m_manifest;
	std::scoped_lock lock(state.mutex);
	backlog.modules   = static_cast<uint32_t>(state.module_jobs.size());
	backlog.pipelines = static_cast<uint32_t>(state.pipeline_jobs.size());
	backlog.running   = state.running;
	backlog.scanning  = state.scanning.load(std::memory_order_acquire);
	for (const auto index: state.module_jobs) {
		backlog.compute_modules += state.warm.modules[index].stage == ShaderType::Compute ? 1u : 0u;
	}
	backlog.pipelines_done   = state.pipelines + state.pipelines_known + state.pipelines_failed;
	backlog.threads          = static_cast<uint32_t>(state.threads.size());
	backlog.module_seconds   = state.module_time.Get();
	backlog.pipeline_seconds = state.pipeline_time.Get();
	return backlog;
}

void PipelineCache::ManifestProgress(bool force) {
	auto&            state = *m_manifest;
	std::scoped_lock lock(state.mutex);
	const auto       now = std::chrono::steady_clock::now();
	const bool       idle =
	    state.module_jobs.empty() && state.pipeline_jobs.empty() && state.running == 0;
	if (!force &&
	    (state.logged_idle || (!idle && now - state.last_log < std::chrono::seconds(2)))) {
		return;
	}
	state.last_log   = now;
	uint32_t waiting = 0;
	for (const auto missing: state.missing_code) {
		waiting += missing != 0 ? 1u : 0u;
	}
	const auto modules   = state.warm.modules.size();
	const auto pipelines = state.warm.pipelines.size();
	if (!idle && !force) {
		ManifestLog("Pipeline manifest: {}/{} modules, {}/{} pipelines ({} modules wait for "
		            "their shaders) at {:.1f} s",
		            state.built + state.found + state.failed, modules,
		            state.pipelines + state.pipelines_known + state.pipelines_failed, pipelines,
		            waiting, Seconds(now - state.start));
		return;
	}
	state.logged_idle = idle;
	ManifestLog("Pipeline manifest: {} at {:.1f} s: {} modules compiled ({:.0f} ms), {} compiled "
	            "already, {} failed, {} waiting for their shaders; {} pipelines and {} compute "
	            "pipelines created ({:.0f} ms), {} created already, {} failed",
	            idle ? "idle" : "stopped", Seconds(now - state.start), state.built,
	            Milliseconds(state.build_ns), state.found, state.failed, waiting, state.pipelines,
	            state.compute, Milliseconds(state.pipeline_ns), state.pipelines_known,
	            state.pipelines_failed);
}

void PipelineCache::RecordModule(const ProgramKey&                       key,
                                 const ShaderRecompiler::CompileOptions& options,
                                 std::span<const uint32_t> code, uint32_t push_data_cursor,
                                 const ShaderModule& module) {
	if (m_manifest == nullptr || m_manifest->record_path.empty()) {
		return;
	}
	ManifestModule record {.stage            = key.stage,
	                       .code_hash        = key.hash,
	                       .user_data_count  = key.user_data_count,
	                       .push_data_cursor = push_data_cursor,
	                       .specialization   = module.specialization};
	if (!options.back_code.empty()) {
		record.code_hash = XXH3_64bits(code.data(), code.size_bytes());
		record.back_hash = XXH3_64bits(options.back_code.data(), options.back_code.size_bytes());
		if (record.ProgramHash() != key.hash) {
			return;
		}
	}
	switch (key.stage) {
		case ShaderType::Pixel: record.input = *options.input_info.pixel; break;
		case ShaderType::Compute: record.input = *options.input_info.compute; break;
		default: record.input = *options.input_info.vertex; break;
	}
	NormalizeManifestInput(record);
	// A program the native ray tracing patches is recorded as asked for: the patch keeps the guest
	// walk and adds no resources (device addresses only), so the record's specialization is the
	// unpatched program's too. The manifest builds it as the host does, patched or not
	// (BuildManifestModule()). Its identity is the manifest's, without this host's static key
	// marks (AppendNativeRayTracingKey()), as the records loaded and built have it.
	auto host = record;
	SetHostInput(host);
	const auto       identity = ManifestModuleIdentity(record, StaticKey(host));
	auto&            state    = *m_manifest;
	std::scoped_lock lock(state.mutex);
	const auto [found, inserted] = state.module_index.try_emplace(
	    identity, static_cast<uint32_t>(state.recorded.modules.size()));
	if (inserted) {
		state.recorded.modules.push_back(std::move(record));
		state.added_modules++;
		state.dirty = true;
	}
	state.module_records[module.id] = {found->second, false};
	if (module.plain != nullptr) {
		state.module_records[module.plain->id] = {found->second, true};
	}
}

void PipelineCache::RecordPipeline(const GraphicsPipelineKey& key) {
	if (m_manifest == nullptr || m_manifest->record_path.empty()) {
		return;
	}
	auto&            state = *m_manifest;
	ManifestPipeline record {.key = key};
	record.key.stage_ids = {};
	std::scoped_lock lock(state.mutex);
	for (size_t i = 0; i < key.stage_ids.size(); i++) {
		if (key.stage_ids[i] == 0) {
			continue;
		}
		// Programs that are not recorded (they call guest functions) leave the pipeline out.
		const auto found = state.module_records.find(key.stage_ids[i]);
		if (found == state.module_records.end() || (found->second.second && i != 3)) {
			return;
		}
		record.stages[i]   = found->second.first;
		record.plain_pixel = record.plain_pixel || found->second.second;
	}
	if (state.pipeline_identities.insert(ManifestPipelineIdentity(record)).second) {
		state.recorded.pipelines.push_back(record);
		state.added_pipelines++;
		state.dirty = true;
	}
}

void PipelineCache::RecordLayout(uint64_t key, const ShaderTessellationInputInfo& layout) {
	if (m_manifest == nullptr || m_manifest->record_path.empty()) {
		return;
	}
	auto&            state = *m_manifest;
	std::scoped_lock lock(state.mutex);
	if (state.layout_keys.insert(key).second) {
		state.recorded.tessellation_layouts.emplace_back(key, layout);
		state.dirty = true;
	}
}

void PipelineCache::SaveManifestRecording() {
	auto& state = *m_manifest;
	if (state.record_path.empty()) {
		return;
	}
	PipelineManifest manifest;
	uint32_t         added_modules   = 0;
	uint32_t         added_pipelines = 0;
	{
		std::scoped_lock lock(state.mutex);
		if (!state.dirty) {
			return;
		}
		manifest        = state.recorded;
		added_modules   = state.added_modules;
		added_pipelines = state.added_pipelines;
		state.dirty     = false;
	}
	const auto bytes     = SaveManifest(manifest);
	auto       temporary = state.record_path;
	temporary += ".tmp";
	Common::File::CreateDirectories(state.record_path.parent_path());
	{
		Common::File file;
		if (!file.Create(temporary)) {
			return;
		}
		file.Write(bytes.data(), static_cast<uint32_t>(bytes.size()));
	}
	std::error_code error;
	std::filesystem::rename(temporary, state.record_path, error);
	ManifestLog(
	    "Pipeline manifest: wrote {} ({} modules, {} pipelines, {:.1f} KB; {} modules and {} "
	    "pipelines new in this session)",
	    Common::PathToString(state.record_path), manifest.modules.size(), manifest.pipelines.size(),
	    static_cast<double>(bytes.size()) / 1024.0, added_modules, added_pipelines);
}

} // namespace Libs::Graphics
