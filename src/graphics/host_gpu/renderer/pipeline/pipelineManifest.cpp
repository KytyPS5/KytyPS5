#include "graphics/host_gpu/renderer/pipeline/pipelineManifest.h"

#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/parallelFor.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/Serialize.h"
#include "graphics/shader/shaderCompiler.h"
#include "loader/systemContent.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
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
		uint32_t                                     readers = 0; // Records not started yet.
		bool                                         mapped  = false;
	};
	std::unordered_map<uint64_t, Code> code;
	std::vector<uint8_t>               missing_code;     // By module record.
	std::vector<const ShaderModule*>   modules;          // By module record, once compiled.
	std::vector<std::vector<uint32_t>> module_pipelines; // By module record.
	std::vector<uint8_t>               missing_modules;  // By pipeline record.
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

	std::vector<std::jthread> threads;
	std::jthread              saver;
};

void PipelineCache::ManifestStateDeleter::operator()(ManifestState* state) const {
	delete state;
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
	const bool record = Config::PipelineManifestRecord();
	const auto title  = PipelineCacheTitleId();
	if ((!warm && !record) || title.empty()) {
		return;
	}
	std::string version;
	Loader::SystemContentParamSfoGetString("APP_VER", &version);
	const auto name = SafeFileName(version.empty() ? title : title + "_" + version) + ".kpm";
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
	for (const auto& path: files) {
		PipelineManifest loaded;
		uint32_t         skipped = 0;
		const auto       start   = std::chrono::steady_clock::now();
		std::vector<std::string> unlinked;
		if (!LoadManifest(ReadFile(path), loaded, &skipped, &unlinked)) {
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
		std::scoped_lock lock(state.mutex);
		const auto       found = state.code.find(hash);
		if (found == state.code.end() || found->second.mapped) {
			return;
		}
		// The guest may unmap or reuse the memory later: the threads read a copy.
		auto& entry  = found->second;
		entry.words  = std::make_shared<const std::vector<uint32_t>>(code.begin(), code.end());
		entry.mapped = true;
		for (const auto index: entry.waiting) {
			if (--state.missing_code[index] == 0) {
				state.module_jobs.push_back(index);
			}
		}
		entry.waiting     = {};
		state.logged_idle = false;
	}
	state.available.notify_all();
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
			// Pipelines first: their modules are ready, and they are what stalls a draw.
			if (!state.available.wait(lock, stop, [&] {
				    return !state.pipeline_jobs.empty() || !state.module_jobs.empty();
			    })) {
				return;
			}
			pipeline   = !state.pipeline_jobs.empty();
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
                                        std::span<const uint32_t> back_code) {
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
	options.shader_hash     = key.hash;
	options.user_data       = user_data;
	options.back_code       = back_code;
	options.dump_ir         = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
	options.emit_debug_info = Config::GraphicsDebugDumpEnabled();

	const auto finish = [&](const ShaderModule* module, bool built) {
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
					state.pipeline_jobs.push_back(pipeline);
				}
			}
		}
		state.available.notify_all();
		if (module != nullptr && module->info.stage == ShaderType::Compute) {
			CreateManifestComputePipeline(*module);
		}
	};
	const ShaderModule* compiled = nullptr;
	LockPrograms();
	compiled = FindCompiledModule(key, record.specialization, record.push_data_cursor);
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
	auto                  translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
	auto                  plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
	std::vector<uint32_t> spirv;
	std::vector<uint32_t> plain_spirv;
	auto module = BuildModule(key, options, std::move(translated), record.specialization, {},
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
		compiled      = FindCompiledModule(key, record.specialization, record.push_data_cursor);
		if (compiled != nullptr || program.skip_dispatch || program.call_source != nullptr) {
			DestroyModule(module);
		} else {
			compiled = &program.modules.emplace_back(std::move(module));
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
	std::scoped_lock lock(state.mutex);
	(created ? state.pipelines : state.pipelines_failed)++;
	state.pipeline_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                               std::chrono::steady_clock::now() - start)
	                                               .count());
}

void PipelineCache::CreateManifestComputePipeline(const ShaderModule& module) {
	{
		std::scoped_lock lock(m_async_mutex);
		if (!m_claimed_compute.insert(module.id).second) {
			return;
		}
		m_async_pending_compute.insert(module.id);
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
	const auto       identity = ManifestModuleIdentity(record, key.static_state);
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
