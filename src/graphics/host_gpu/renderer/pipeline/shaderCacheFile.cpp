#include "graphics/host_gpu/renderer/pipeline/shaderCacheFile.h"

#include "common/file.h"

#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <random>
#include <system_error>
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
	PutArray(out, recipe.specialization.buffers);
	PutArray(out, recipe.specialization.images);
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
	       reader.GetArray(recipe.specialization.buffers, buffers) &&
	       reader.GetArray(recipe.specialization.images, images) && reader.Remaining() == 0;
}

} // namespace

size_t ShaderCacheInputSize(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex:
		case ShaderType::Mesh:
		case ShaderType::Local:
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation: return sizeof(ShaderVertexInputInfo);
		case ShaderType::Pixel: return sizeof(ShaderPixelInputInfo);
		case ShaderType::Compute: return sizeof(ShaderComputeInputInfo);
		default: return 0;
	}
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
