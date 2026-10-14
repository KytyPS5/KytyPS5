#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PROGRAMLIST_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PROGRAMLIST_H_

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include "common/hash.h"
#include "common/stringUtils.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// What one translation of a guest shader was made from. With it, the same host program can be
// built again without the game: at the next start, on a worker thread, before the first draw
// needs it. The record holds inputs only, so it stays valid when the translator changes.
struct ProgramRecord {
	ShaderType            stage           = ShaderType::Unknown;
	uint64_t              hash            = 0;
	uint32_t              user_data_count = 0;
	uint32_t              push_data_start = 0;
	std::vector<uint32_t> code;
	std::vector<uint32_t> back_code;
	// The bytes of the stage's input info (vertex, pixel or compute), without its pointers to
	// the program that is built from it.
	std::vector<uint8_t>                         input_info;
	ShaderRecompiler::IR::ResourceSpecialization specialization;

	bool operator==(const ProgramRecord&) const = default;
};

namespace ProgramListDetail {

inline uint64_t Checksum(std::span<const uint8_t> bytes) {
	uint64_t hash = Common::FNV64_OFFSET;
	for (const auto byte: bytes) {
		hash = Common::Fnv64Step(hash, byte);
	}
	return hash;
}

class Out final {
public:
	explicit Out(std::vector<uint8_t>& bytes): m_bytes(bytes) {}
	template <typename T>
	void Value(T value) {
		const auto* raw = reinterpret_cast<const uint8_t*>(&value);
		m_bytes.insert(m_bytes.end(), raw, raw + sizeof(T));
	}
	template <typename T>
	void Array(std::span<const T> values) {
		Value(static_cast<uint32_t>(values.size()));
		const auto* raw = reinterpret_cast<const uint8_t*>(values.data());
		m_bytes.insert(m_bytes.end(), raw, raw + values.size_bytes());
	}

private:
	std::vector<uint8_t>& m_bytes;
};

class In final {
public:
	explicit In(std::span<const uint8_t> bytes): m_bytes(bytes) {}
	template <typename T>
	[[nodiscard]] bool Value(T& value) {
		if (m_bytes.size() < sizeof(T)) {
			return false;
		}
		std::memcpy(&value, m_bytes.data(), sizeof(T));
		m_bytes = m_bytes.subspan(sizeof(T));
		return true;
	}
	template <typename T>
	[[nodiscard]] bool Array(std::vector<T>& values) {
		uint32_t count = 0;
		if (!Value(count) || m_bytes.size() / sizeof(T) < count) {
			return false;
		}
		values.resize(count);
		std::memcpy(values.data(), m_bytes.data(), count * sizeof(T));
		m_bytes = m_bytes.subspan(count * sizeof(T));
		return true;
	}
	[[nodiscard]] bool Done() const { return m_bytes.empty(); }

private:
	std::span<const uint8_t> m_bytes;
};

} // namespace ProgramListDetail

// The offset of the last member of a struct that a record holds as bytes. With the struct's size
// it signs the layout: a field added before the last member moves it, also when the field goes
// into padding and the size stays the same. Not seen: a field in the padding after the last
// member, or two fields of one size that change places. Measured on an object, because offsetof
// needs a standard-layout struct and the input infos inherit members.
template <typename T, typename M>
[[nodiscard]] size_t RecordLastOffset(M T::* last) {
	static const T object {};
	return static_cast<size_t>(reinterpret_cast<const char*>(&(object.*last)) -
	                           reinterpret_cast<const char*>(&object));
}

// Appends the record to `out`: its length, its bytes, and a checksum of the bytes.
inline void AppendProgramRecord(std::vector<uint8_t>& out, const ProgramRecord& record) {
	std::vector<uint8_t>   body;
	ProgramListDetail::Out write(body);
	write.Value(static_cast<uint32_t>(record.stage));
	write.Value(record.hash);
	write.Value(record.user_data_count);
	write.Value(record.push_data_start);
	write.Array(std::span<const uint32_t>(record.code));
	write.Array(std::span<const uint32_t>(record.back_code));
	write.Array(std::span<const uint8_t>(record.input_info));
	// Field by field: the structs have padding, and equal records must give equal bytes.
	write.Value(static_cast<uint32_t>(record.specialization.buffers.size()));
	for (const auto& buffer: record.specialization.buffers) {
		write.Value(buffer.packed_stride);
		write.Value(static_cast<uint32_t>(buffer.descriptor_format));
		write.Value(buffer.descriptor_swizzle);
		write.Value(static_cast<uint8_t>(buffer.zero_stride_oob));
	}
	write.Value(static_cast<uint32_t>(record.specialization.images.size()));
	for (const auto& image: record.specialization.images) {
		write.Value(static_cast<uint32_t>(image.numeric_class));
		write.Value(static_cast<uint32_t>(image.dimension));
		write.Value(image.mip_count);
		write.Value(static_cast<uint32_t>(image.conversion_format));
		write.Value(image.shader_swizzle);
		write.Value(image.indirect_root);
		write.Value(image.indirect_mapping_offset);
		write.Value(image.indirect_search_iterations);
		write.Value(static_cast<uint8_t>(image.cube));
		write.Value(static_cast<uint8_t>(image.fmask));
	}
	ProgramListDetail::Out file(out);
	file.Value(static_cast<uint32_t>(body.size()));
	out.insert(out.end(), body.begin(), body.end());
	file.Value(ProgramListDetail::Checksum(body));
}

// The records in `bytes`, in order. Stops at the first record that is cut off or damaged: a
// file that was being written when the emulator ended keeps the records before that point.
inline std::vector<ProgramRecord> ReadProgramRecords(std::span<const uint8_t> bytes) {
	std::vector<ProgramRecord> records;
	for (;;) {
		ProgramListDetail::In file(bytes);
		uint32_t              size = 0;
		if (!file.Value(size) || bytes.size() - sizeof(size) < size + sizeof(uint64_t)) {
			break;
		}
		const auto body     = bytes.subspan(sizeof(size), size);
		uint64_t   checksum = 0;
		std::memcpy(&checksum, bytes.data() + sizeof(size) + size, sizeof(checksum));
		if (checksum != ProgramListDetail::Checksum(body)) {
			break;
		}
		ProgramRecord         record;
		ProgramListDetail::In read(body);
		uint32_t              stage = 0, buffers = 0, images = 0;
		bool ok      = read.Value(stage) && read.Value(record.hash) &&
		               read.Value(record.user_data_count) && read.Value(record.push_data_start) &&
		               read.Array(record.code) && read.Array(record.back_code) &&
		               read.Array(record.input_info) && read.Value(buffers);
		record.stage = static_cast<ShaderType>(stage);
		for (uint32_t i = 0; ok && i < buffers; i++) {
			auto&    buffer          = record.specialization.buffers.emplace_back();
			uint32_t format          = 0;
			uint8_t  oob             = 0;
			ok                       = read.Value(buffer.packed_stride) && read.Value(format) &&
			                           read.Value(buffer.descriptor_swizzle) && read.Value(oob);
			buffer.descriptor_format = static_cast<Prospero::BufferFormat>(format);
			buffer.zero_stride_oob   = oob != 0;
		}
		ok = ok && read.Value(images);
		for (uint32_t i = 0; ok && i < images; i++) {
			auto&    image         = record.specialization.images.emplace_back();
			uint32_t numeric_class = 0, dimension = 0, format = 0;
			uint8_t  cube = 0, fmask = 0;
			ok = read.Value(numeric_class) && read.Value(dimension) &&
			     read.Value(image.mip_count) && read.Value(format) &&
			     read.Value(image.shader_swizzle) && read.Value(image.indirect_root) &&
			     read.Value(image.indirect_mapping_offset) &&
			     read.Value(image.indirect_search_iterations) && read.Value(cube) &&
			     read.Value(fmask);
			image.numeric_class = static_cast<Prospero::TextureNumericClass>(numeric_class);
			image.dimension     = static_cast<ShaderRecompiler::Decoder::ImageDimension>(dimension);
			image.conversion_format = static_cast<Prospero::BufferFormat>(format);
			image.cube              = cube != 0;
			image.fmask             = fmask != 0;
		}
		if (!ok || !read.Done()) {
			break;
		}
		records.push_back(std::move(record));
		bytes = bytes.subspan(sizeof(size) + size + sizeof(checksum));
	}
	return records;
}

// A graphics pipeline a game used: the programs it was built from, by the identity each has on
// the program list, and the bytes of what else the build needs. Kept on disk for a game, so the
// next start builds the pipeline before a draw asks for it.
struct PipelineRecord {
	// Vertex stages 0..2, then the pixel program; 0 when the stage is absent.
	std::array<uint64_t, 4> programs {};
	// Rendering formats, vertex input layout and static parameters, as their bytes.
	std::vector<uint8_t> fixed_state;
	uint32_t             stage_count = 0;
	// The vertex stage input infos, as their bytes, `stage_count` of them.
	std::array<std::vector<uint8_t>, 3> stages;
	bool                                pixel_present = false;
	std::vector<uint8_t>                pixel;

	bool operator==(const PipelineRecord&) const = default;
};

inline void AppendPipelineRecord(std::vector<uint8_t>& out, const PipelineRecord& record) {
	std::vector<uint8_t>   body;
	ProgramListDetail::Out write(body);
	for (const auto program: record.programs) {
		write.Value(program);
	}
	write.Array(std::span<const uint8_t>(record.fixed_state));
	write.Value(record.stage_count);
	for (uint32_t i = 0; i < record.stage_count && i < record.stages.size(); i++) {
		write.Array(std::span<const uint8_t>(record.stages[i]));
	}
	write.Value(static_cast<uint8_t>(record.pixel_present));
	if (record.pixel_present) {
		write.Array(std::span<const uint8_t>(record.pixel));
	}
	ProgramListDetail::Out file(out);
	file.Value(static_cast<uint32_t>(body.size()));
	out.insert(out.end(), body.begin(), body.end());
	file.Value(ProgramListDetail::Checksum(body));
}

inline std::vector<PipelineRecord> ReadPipelineRecords(std::span<const uint8_t> bytes) {
	std::vector<PipelineRecord> records;
	for (;;) {
		ProgramListDetail::In file(bytes);
		uint32_t              size = 0;
		if (!file.Value(size) || bytes.size() - sizeof(size) < size + sizeof(uint64_t)) {
			break;
		}
		const auto body     = bytes.subspan(sizeof(size), size);
		uint64_t   checksum = 0;
		std::memcpy(&checksum, bytes.data() + sizeof(size) + size, sizeof(checksum));
		if (checksum != ProgramListDetail::Checksum(body)) {
			break;
		}
		PipelineRecord        record;
		ProgramListDetail::In read(body);
		bool                  ok = true;
		for (auto& program: record.programs) {
			ok = ok && read.Value(program);
		}
		ok = ok && read.Array(record.fixed_state) && read.Value(record.stage_count) &&
		     record.stage_count <= record.stages.size();
		for (uint32_t i = 0; ok && i < record.stage_count; i++) {
			ok = read.Array(record.stages[i]);
		}
		uint8_t pixel_present = 0;
		ok                    = ok && read.Value(pixel_present);
		record.pixel_present  = pixel_present != 0;
		if (ok && record.pixel_present) {
			ok = read.Array(record.pixel);
		}
		if (!ok || !read.Done()) {
			break;
		}
		records.push_back(std::move(record));
		bytes = bytes.subspan(sizeof(size) + size + sizeof(checksum));
	}
	return records;
}

// When the renderer looks again at the pipeline records whose programs were missing. A record
// can only change its answer when a program got a name the list knows (`names`, a count that
// only grows) or when the program list is done (`list_built`): then its missing programs never
// come. Scanning before every draw cost records x draws lookups per frame.
class ListedPipelineScan {
public:
	[[nodiscard]] bool Due(uint64_t names, bool list_built) noexcept {
		if (names == m_names && list_built == m_list_built) {
			return false;
		}
		m_names      = names;
		m_list_built = list_built;
		return true;
	}

private:
	uint64_t m_names      = UINT64_MAX;
	bool     m_list_built = false;
};

// The records to keep, each with its identity, in the order of first appearance. A record
// recorded again replaces the earlier one in its place: the later record wins. `identity_of`
// gives nothing for a record to drop.
template <typename Record, typename IdentityOf>
[[nodiscard]] std::vector<std::pair<uint64_t, Record>> LatestByIdentity(std::vector<Record> records,
                                                                        IdentityOf&& identity_of) {
	std::unordered_map<uint64_t, size_t>     index_of;
	std::vector<std::pair<uint64_t, Record>> kept;
	for (auto& record: records) {
		const std::optional<uint64_t> identity = identity_of(record);
		if (!identity) {
			continue;
		}
		const auto [it, inserted] = index_of.try_emplace(*identity, kept.size());
		if (inserted) {
			kept.emplace_back(*identity, std::move(record));
		} else {
			kept[it->second].second = std::move(record);
		}
	}
	return kept;
}

// Writes a list into a temporary file that then takes the list's place, so that a stop in
// between leaves the old list. Returns the list opened for appending, or null.
inline std::FILE* RewriteList(const std::filesystem::path& path,
                              std::span<const uint8_t>     bytes) {
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	auto temp_path = path;
	temp_path += ".tmp";
	bool written = false;
	if (std::FILE* file = std::fopen(Common::PathToString(temp_path).c_str(), "wb")) {
		written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
		written = std::fclose(file) == 0 && written;
	}
	if (written) {
		std::filesystem::rename(temp_path, path, error);
		written = !error;
	}
	if (!written) {
		std::filesystem::remove(temp_path, error);
		return nullptr;
	}
	return std::fopen(Common::PathToString(path).c_str(), "ab");
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PROGRAMLIST_H_
