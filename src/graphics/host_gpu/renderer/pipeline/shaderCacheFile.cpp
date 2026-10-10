#include "graphics/host_gpu/renderer/pipeline/shaderCacheFile.h"

#include "common/file.h"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <random>
#include <system_error>
#include <type_traits>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

using Buffer = ShaderRecompiler::IR::ResourceSpecialization::Buffer;
using Image  = ShaderRecompiler::IR::ResourceSpecialization::Image;

constexpr size_t MaxRecordBytes = 64u << 20u;

void PutBytes(std::vector<uint8_t>& out, const void* data, size_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	out.insert(out.end(), bytes, bytes + size);
}

template <typename T>
void Put(std::vector<uint8_t>& out, const T& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	PutBytes(out, &value, sizeof(value));
}

template <typename T>
void PutArray(std::vector<uint8_t>& out, const std::vector<T>& values) {
	static_assert(std::is_trivially_copyable_v<T>);
	PutBytes(out, values.data(), values.size() * sizeof(T));
}

class Reader {
public:
	explicit Reader(std::span<const uint8_t> data): m_data(data) {}

	[[nodiscard]] size_t Remaining() const { return m_data.size() - m_pos; }

	template <typename T>
	bool Get(T& value) {
		static_assert(std::is_trivially_copyable_v<T>);
		if (Remaining() < sizeof(T)) {
			return false;
		}
		std::memcpy(&value, m_data.data() + m_pos, sizeof(T));
		m_pos += sizeof(T);
		return true;
	}

	template <typename T>
	bool GetArray(std::vector<T>& values, uint32_t count) {
		static_assert(std::is_trivially_copyable_v<T>);
		if (Remaining() / sizeof(T) < count) {
			return false;
		}
		values.resize(count);
		std::memcpy(values.data(), m_data.data() + m_pos, count * sizeof(T));
		m_pos += count * sizeof(T);
		return true;
	}

	std::span<const uint8_t> Take(size_t size) {
		const auto result = m_data.subspan(m_pos, size);
		m_pos += size;
		return result;
	}

private:
	std::span<const uint8_t> m_data;
	size_t                   m_pos = 0;
};

// Fields are stored one by one in a fixed width (bools as one byte, enums as their underlying type)
// so that padding bytes and runtime pointers never reach the file.
template <typename T>
void PutField(std::vector<uint8_t>& out, const T& value) {
	if constexpr (std::is_same_v<T, bool>) {
		Put(out, static_cast<uint8_t>(value ? 1u : 0u));
	} else if constexpr (std::is_enum_v<T>) {
		Put(out, static_cast<std::underlying_type_t<T>>(value));
	} else {
		static_assert(std::is_arithmetic_v<T>);
		Put(out, value);
	}
}

template <typename T>
bool GetField(Reader& reader, T& value) {
	if constexpr (std::is_same_v<T, bool>) {
		uint8_t byte = 0;
		if (!reader.Get(byte) || byte > 1u) {
			return false;
		}
		value = byte != 0;
		return true;
	} else if constexpr (std::is_enum_v<T>) {
		std::underlying_type_t<T> raw {};
		if (!reader.Get(raw)) {
			return false;
		}
		value = static_cast<T>(raw);
		return true;
	} else {
		static_assert(std::is_arithmetic_v<T>);
		return reader.Get(value);
	}
}

template <typename Array, typename F>
void Each(Array& values, F& field) {
	for (auto& value: values) {
		field(value);
	}
}

template <typename Info, typename F>
void VisitWorkgroup(Info& info, F& field) {
	Each(info.threads_num, field);
	field(info.lds_size_dwords);
	field(info.scratch_size_dwords);
	field(info.host_subgroup_size);
	field(info.wave_size);
}

// Every field except the runtime `stage` pointers.
template <typename Info, typename F>
void VisitInput(Info& info, F& field) {
	using T = std::remove_const_t<Info>;
	if constexpr (std::is_same_v<T, ShaderVertexInputInfo>) {
		for (auto& resource: info.resources) {
			Each(resource.fields, field);
		}
		for (auto& destination: info.resources_dst) {
			field(destination.register_start);
			field(destination.registers_num);
			field(destination.attr_id);
			field(destination.fetch_index);
			field(destination.buffer_index);
		}
		for (auto& buffer: info.buffers) {
			field(buffer.addr);
			field(buffer.stride);
			field(buffer.num_records);
			field(buffer.fetch_index);
		}
		field(info.logical_stage);
		field(info.resources_num);
		field(info.fetch_attrib_reg);
		field(info.fetch_buffer_reg);
		field(info.buffers_num);
		field(info.wave_size);
		field(info.scratch_size_dwords);
		field(info.pa_cl_vs_out_cntl);
		Each(info.clip_space.scale, field);
		Each(info.clip_space.offset, field);
		Each(info.clip_space.half_extent, field);
		field(info.clip_space.enabled);
		VisitWorkgroup(info.mesh, field);
		field(info.mesh.input_primitive);
		field(info.mesh.primitives_per_group);
		field(info.mesh.vertices_per_group);
		field(info.mesh.max_vertices);
		field(info.mesh.max_primitives);
		field(info.mesh.provoking_vertex);
		field(info.mesh.fast_launch);
		field(info.tess.input_control_points);
		field(info.tess.output_control_points);
		field(info.tess.ls_stride);
		field(info.tess.hs_stride);
		field(info.tess.domain);
		field(info.tess.partitioning);
		field(info.tess.output_topology);
		field(info.fetch_external);
		field(info.fetch_embedded);
	} else if constexpr (std::is_same_v<T, ShaderPixelInputInfo>) {
		Each(info.interpolator_settings, field);
		field(info.input_num);
		field(info.wave_size);
		field(info.ps_system_input_base);
		field(info.custom_interpolation_mask);
		field(info.ps_perspective_center_vgpr);
		field(info.ps_perspective_sample_vgpr);
		field(info.ps_perspective_centroid_vgpr);
		Each(info.target_output_mode, field);
		field(info.target_shader_mask);
		for (auto& mapping: info.target_export_mapping) {
			field(mapping.packed);
		}
		field(info.scratch_size_dwords);
		field(info.ps_pos_x);
		field(info.ps_pos_y);
		field(info.ps_pos_z);
		field(info.ps_pos_w);
		field(info.ps_front_face);
		field(info.ps_ancillary);
		field(info.ps_no_perspective);
		field(info.parameter_mode);
		field(info.ps_pixel_kill_enable);
		field(info.ps_depth_export_enable);
		field(info.ps_sample_mask_export_enable);
		field(info.ps_sample_shading);
		field(info.dual_source_blending);
		field(info.alpha_blend_source);
		field(info.ps_early_z);
		field(info.ps_execute_on_noop);
	} else {
		static_assert(std::is_same_v<T, ShaderComputeInputInfo>);
		VisitWorkgroup(info, field);
		field(info.async_compute);
		field(info.float_mode);
		Each(info.dispatch_threads_num, field);
		Each(info.workgroup_counts, field);
		Each(info.group_id, field);
		field(info.dispatch_thread_dimensions);
		field(info.lds_storage);
		field(info.thread_ids_num);
		field(info.workgroup_register);
		field(info.tg_size_en);
	}
}

template <typename Info, typename F>
void VisitBuffer(Info& buffer, F& field) {
	field(buffer.packed_stride);
	field(buffer.descriptor_format);
	field(buffer.descriptor_swizzle);
	field(buffer.zero_stride_oob);
	field(buffer.indirect_root);
	field(buffer.indirect_mapping_offset);
	field(buffer.indirect_search_iterations);
}

template <typename Info, typename F>
void VisitImage(Info& image, F& field) {
	field(image.numeric_class);
	field(image.dimension);
	field(image.mip_count);
	field(image.conversion_format);
	field(image.shader_swizzle);
	field(image.indirect_root);
	field(image.indirect_mapping_offset);
	field(image.indirect_search_iterations);
	field(image.cube);
	field(image.fmask);
}

template <typename T>
void PutFields(std::vector<uint8_t>& out, const T& value) {
	auto put = [&out](const auto& field) { PutField(out, field); };
	if constexpr (std::is_same_v<T, Buffer>) {
		VisitBuffer(value, put);
	} else if constexpr (std::is_same_v<T, Image>) {
		VisitImage(value, put);
	} else {
		VisitInput(value, put);
	}
}

template <typename T>
bool GetFields(Reader& reader, T& value) {
	bool ok  = true;
	auto get = [&reader, &ok](auto& field) { ok = ok && GetField(reader, field); };
	if constexpr (std::is_same_v<T, Buffer>) {
		VisitBuffer(value, get);
	} else if constexpr (std::is_same_v<T, Image>) {
		VisitImage(value, get);
	} else {
		VisitInput(value, get);
	}
	return ok;
}

template <typename T>
size_t FieldsSize() {
	static const size_t size = [] {
		std::vector<uint8_t> out;
		PutFields(out, T {});
		return out.size();
	}();
	return size;
}

template <typename T>
void PutFieldsArray(std::vector<uint8_t>& out, const std::vector<T>& values) {
	for (const auto& value: values) {
		PutFields(out, value);
	}
}

template <typename T>
bool GetFieldsArray(Reader& reader, std::vector<T>& values, uint32_t count) {
	if (reader.Remaining() / FieldsSize<T>() < count) {
		return false;
	}
	values.resize(count);
	return std::ranges::all_of(values, [&reader](T& value) { return GetFields(reader, value); });
}

template <typename Info>
std::vector<uint8_t> EncodeInput(const Info& info) {
	std::vector<uint8_t> out;
	out.reserve(FieldsSize<Info>());
	PutFields(out, info);
	return out;
}

template <typename Info>
bool DecodeInput(std::span<const uint8_t> data, Info& info) {
	Reader reader(data);
	return GetFields(reader, info) && reader.Remaining() == 0;
}

std::vector<uint8_t> EncodeRecipe(const ShaderCacheRecipe& recipe) {
	std::vector<uint8_t> out;
	Put(out, static_cast<uint32_t>(recipe.stage));
	Put(out, recipe.user_data_count);
	Put(out, recipe.push_data_start);
	Put(out, recipe.hash);
	Put(out, static_cast<uint32_t>(recipe.code.size()));
	Put(out, static_cast<uint32_t>(recipe.back_code.size()));
	Put(out, static_cast<uint32_t>(recipe.input.size()));
	Put(out, static_cast<uint32_t>(recipe.specialization.buffers.size()));
	Put(out, static_cast<uint32_t>(recipe.specialization.images.size()));
	PutArray(out, recipe.code);
	PutArray(out, recipe.back_code);
	PutArray(out, recipe.input);
	PutFieldsArray(out, recipe.specialization.buffers);
	PutFieldsArray(out, recipe.specialization.images);
	return out;
}

bool DecodeRecipe(std::span<const uint8_t> payload, ShaderCacheRecipe& recipe) {
	Reader   reader(payload);
	uint32_t stage       = 0;
	uint32_t code_words  = 0;
	uint32_t back_words  = 0;
	uint32_t input_bytes = 0;
	uint32_t buffers     = 0;
	uint32_t images      = 0;
	if (!reader.Get(stage) || !reader.Get(recipe.user_data_count) ||
	    !reader.Get(recipe.push_data_start) || !reader.Get(recipe.hash) ||
	    !reader.Get(code_words) || !reader.Get(back_words) || !reader.Get(input_bytes) ||
	    !reader.Get(buffers) || !reader.Get(images)) {
		return false;
	}
	recipe.stage = static_cast<ShaderType>(stage);
	if (code_words == 0 || input_bytes == 0 || input_bytes != ShaderCacheInputSize(recipe.stage)) {
		return false;
	}
	return reader.GetArray(recipe.code, code_words) &&
	       reader.GetArray(recipe.back_code, back_words) &&
	       reader.GetArray(recipe.input, input_bytes) &&
	       GetFieldsArray(reader, recipe.specialization.buffers, buffers) &&
	       GetFieldsArray(reader, recipe.specialization.images, images) && reader.Remaining() == 0;
}

} // namespace

size_t ShaderCacheInputSize(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex:
		case ShaderType::Mesh:
		case ShaderType::Local:
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation: return FieldsSize<ShaderVertexInputInfo>();
		case ShaderType::Pixel: return FieldsSize<ShaderPixelInputInfo>();
		case ShaderType::Compute: return FieldsSize<ShaderComputeInputInfo>();
		default: return 0;
	}
}

std::vector<uint8_t> EncodeShaderCacheInput(const ShaderVertexInputInfo& info) {
	return EncodeInput(info);
}
std::vector<uint8_t> EncodeShaderCacheInput(const ShaderPixelInputInfo& info) {
	return EncodeInput(info);
}
std::vector<uint8_t> EncodeShaderCacheInput(const ShaderComputeInputInfo& info) {
	return EncodeInput(info);
}
bool DecodeShaderCacheInput(std::span<const uint8_t> data, ShaderVertexInputInfo& info) {
	return DecodeInput(data, info);
}
bool DecodeShaderCacheInput(std::span<const uint8_t> data, ShaderPixelInputInfo& info) {
	return DecodeInput(data, info);
}
bool DecodeShaderCacheInput(std::span<const uint8_t> data, ShaderComputeInputInfo& info) {
	return DecodeInput(data, info);
}

std::vector<uint8_t> SerializeShaderCache(std::string_view          signature,
                                          const ShaderCacheRecipes& recipes) {
	std::vector<uint8_t> out(signature.begin(), signature.end());
	for (const auto& recipe: recipes) {
		const auto payload = EncodeRecipe(*recipe);
		Put(out, static_cast<uint32_t>(payload.size()));
		Put(out, XXH3_64bits(payload.data(), payload.size()));
		PutArray(out, payload);
	}
	return out;
}

bool ParseShaderCache(std::span<const uint8_t> data, std::string_view signature,
                      ShaderCacheRecipes& recipes, size_t* parsed_bytes) {
	if (parsed_bytes != nullptr) {
		*parsed_bytes = 0;
	}
	if (data.size() < signature.size() ||
	    std::memcmp(data.data(), signature.data(), signature.size()) != 0) {
		return false;
	}
	Reader reader(data.subspan(signature.size()));
	for (;;) {
		if (parsed_bytes != nullptr) {
			*parsed_bytes = data.size() - reader.Remaining();
		}
		uint32_t size = 0;
		uint64_t hash = 0;
		if (!reader.Get(size) || !reader.Get(hash) || size > reader.Remaining() ||
		    size > MaxRecordBytes) {
			break;
		}
		const auto payload = reader.Take(size);
		auto       recipe  = std::make_shared<ShaderCacheRecipe>();
		if (XXH3_64bits(payload.data(), payload.size()) != hash ||
		    !DecodeRecipe(payload, *recipe)) {
			break;
		}
		recipes.push_back(std::move(recipe));
	}
	return true;
}

bool ReadShaderCacheFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
	data.clear();
	if (!Common::File::IsFileExisting(path)) {
		return false;
	}
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid() || file.Size() > std::numeric_limits<uint32_t>::max()) {
		return false;
	}
	data.resize(file.Size());
	uint32_t read = 0;
	file.Read(data.data(), static_cast<uint32_t>(data.size()), &read);
	file.Close();
	return read == data.size();
}

bool WriteShaderCacheFile(const std::filesystem::path& path, std::span<const uint8_t> data) {
	if (data.size() > std::numeric_limits<uint32_t>::max() ||
	    !Common::File::CreateDirectories(path.parent_path())) {
		return false;
	}
	// Concurrent writers each fill their own file; the last rename wins with a complete file.
	std::random_device entropy;
	auto               temp_path = path;
	temp_path += fmt::format(".{:08x}{:08x}.tmp", entropy(), entropy());
	Common::File file;
	uint32_t     written = 0;
	if (file.Create(temp_path)) {
		file.Write(data.data(), static_cast<uint32_t>(data.size()), &written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	std::error_code error;
	if (written == data.size() && flushed) {
		std::filesystem::rename(temp_path, path, error);
		if (!error) {
			return true;
		}
	}
	std::filesystem::remove(temp_path, error);
	return false;
}

} // namespace Libs::Graphics
