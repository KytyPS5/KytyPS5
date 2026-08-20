#include "graphics/shader/shaderDiskCache.h"

#include "common/logging/log.h"
#include "kytyGitVersion.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

namespace IR = ShaderRecompiler::IR;

constexpr char     CACHE_MAGIC[8]       = {'K', 'Y', 'T', 'Y', 'S', 'H', 'C', '\0'};
constexpr uint32_t CACHE_FORMAT_VERSION = 1;

// A cache entry is only meaningful to the exact recompiler that wrote it. Two
// independent guards, because getting this wrong shows up as a wrong frame
// rather than a crash:
//
//   - the git version, which changes whenever anything in the tree does;
//   - the sizes of every IR struct the plan serializes, which catches a locally
//     modified build that forgot to rebuild the version header.
//
// Being too strict costs one cold run after an update, which is what a driver
// pipeline cache does too. Being too loose costs correctness, so this leans
// strict on purpose.
uint64_t ComputeBuildId() {
	const uint32_t sizes[] = {
	    static_cast<uint32_t>(sizeof(IR::BufferResource)),
	    static_cast<uint32_t>(sizeof(IR::AddressResource)),
	    static_cast<uint32_t>(sizeof(IR::ImageResource)),
	    static_cast<uint32_t>(sizeof(IR::SamplerResource)),
	    static_cast<uint32_t>(sizeof(IR::SampledResourcePair)),
	    static_cast<uint32_t>(sizeof(IR::StageInput)),
	    static_cast<uint32_t>(sizeof(IR::StageOutput)),
	    static_cast<uint32_t>(sizeof(IR::DescriptorBinding)),
	    static_cast<uint32_t>(sizeof(IR::BindingLayout)),
	    static_cast<uint32_t>(sizeof(IR::ShaderInfo)),
	    static_cast<uint32_t>(sizeof(IR::SpirvRequirements)),
	    static_cast<uint32_t>(IR::DescriptorBindingKind::Count),
	    CACHE_FORMAT_VERSION,
	};

	XXH3_state_t* state = XXH3_createState();
	XXH3_64bits_reset(state);
	XXH3_64bits_update(state, sizes, sizeof(sizes));
	const std::string_view git = KYTY_GIT_VERSION;
	XXH3_64bits_update(state, git.data(), git.size());
	const auto id = XXH3_64bits_digest(state);
	XXH3_freeState(state);
	return id;
}

// ── byte writer / reader ────────────────────────────────────────────────
//
// Everything is written little-endian and length-prefixed. The reader never
// trusts a length: Take() checks it against what is actually left, so a
// truncated or malicious file makes Ok() false instead of reading past the end.

class Writer {
public:
	void U8(uint8_t value) { m_bytes.push_back(value); }
	void U32(uint32_t value) {
		for (uint32_t i = 0; i < 4; i++) {
			m_bytes.push_back(static_cast<uint8_t>((value >> (i * 8U)) & 0xFFU));
		}
	}
	void U64(uint64_t value) {
		for (uint32_t i = 0; i < 8; i++) {
			m_bytes.push_back(static_cast<uint8_t>((value >> (i * 8U)) & 0xFFU));
		}
	}
	void I32(int32_t value) { U32(static_cast<uint32_t>(value)); }
	void Bool(bool value) { U8(value ? 1U : 0U); }
	void Bytes(const void* data, size_t size) {
		const auto* p = static_cast<const uint8_t*>(data);
		m_bytes.insert(m_bytes.end(), p, p + size);
	}
	void String(const std::string& value) {
		U32(static_cast<uint32_t>(value.size()));
		Bytes(value.data(), value.size());
	}
	void U32Vector(const std::vector<uint32_t>& values) {
		U32(static_cast<uint32_t>(values.size()));
		for (auto value: values) {
			U32(value);
		}
	}

	[[nodiscard]] const std::vector<uint8_t>& Bytes() const { return m_bytes; }

private:
	std::vector<uint8_t> m_bytes;
};

class Reader {
public:
	Reader(const uint8_t* data, size_t size): m_data(data), m_size(size) {}

	[[nodiscard]] bool Ok() const { return m_ok; }
	[[nodiscard]] bool Done() const { return m_ok && m_pos == m_size; }

	// True when `size` more bytes are actually available. Checked before every
	// allocation sized from the file, so a corrupt length cannot ask for a
	// gigabyte.
	[[nodiscard]] bool CanHold(uint64_t size) const {
		return m_ok && size <= static_cast<uint64_t>(m_size - m_pos);
	}

	const uint8_t* Skip(uint64_t size) {
		if (size > static_cast<uint64_t>(m_size)) {
			m_ok = false;
			return nullptr;
		}
		return Take(static_cast<size_t>(size));
	}

	uint8_t U8() {
		const auto* p = Take(1);
		return p == nullptr ? 0 : *p;
	}
	uint32_t U32() {
		const auto* p = Take(4);
		if (p == nullptr) {
			return 0;
		}
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
		       (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
	}
	uint64_t U64() {
		const uint64_t low  = U32();
		const uint64_t high = U32();
		return low | (high << 32U);
	}
	int32_t I32() { return static_cast<int32_t>(U32()); }
	bool    Bool() { return U8() != 0; }

	std::string String() {
		const auto  size = U32();
		const auto* p    = Take(size);
		if (p == nullptr) {
			return {};
		}
		return std::string(reinterpret_cast<const char*>(p), size);
	}

	std::vector<uint32_t> U32Vector() {
		const auto count = U32();
		// A count is 4 bytes of payload each; refuse anything the buffer cannot
		// hold before allocating for it.
		if (!m_ok || static_cast<uint64_t>(count) * 4ULL > (m_size - m_pos)) {
			m_ok = false;
			return {};
		}
		std::vector<uint32_t> values(count);
		for (auto& value: values) {
			value = U32();
		}
		return values;
	}

	// Reads an enum by value, rejecting anything outside the range this build
	// knows. A cache from a build with more enumerators must not silently
	// become a different enumerator here.
	template <class E>
	E Enum(uint32_t count) {
		const auto value = U32();
		if (value >= count) {
			m_ok = false;
			return static_cast<E>(0);
		}
		return static_cast<E>(value);
	}

private:
	const uint8_t* Take(size_t size) {
		if (!m_ok || size > m_size - m_pos) {
			m_ok = false;
			return nullptr;
		}
		const auto* p = m_data + m_pos;
		m_pos += size;
		return p;
	}

	const uint8_t* m_data = nullptr;
	size_t         m_size = 0;
	size_t         m_pos  = 0;
	bool           m_ok   = true;
};

// ── IR plan serialization ───────────────────────────────────────────────

void WriteBuffer(Writer& w, const IR::BufferResource& r) {
	w.U32(r.source);
	w.U32(r.first_use_pc);
	w.U32(r.max_byte_extent);
	w.U32(r.packed_stride);
	w.U32(static_cast<uint32_t>(r.descriptor_format));
	w.U32(r.descriptor_swizzle);
	w.U32(r.image_alias);
	w.Bool(r.read);
	w.Bool(r.written);
	w.Bool(r.atomic);
	w.Bool(r.formatted);
	w.Bool(r.scalar);
}

IR::BufferResource ReadBuffer(Reader& r) {
	IR::BufferResource value {};
	value.source             = r.U32();
	value.first_use_pc       = r.U32();
	value.max_byte_extent    = r.U32();
	value.packed_stride      = r.U32();
	value.descriptor_format  = static_cast<Prospero::BufferFormat>(r.U32());
	value.descriptor_swizzle = r.U32();
	value.image_alias        = r.U32();
	value.read               = r.Bool();
	value.written            = r.Bool();
	value.atomic             = r.Bool();
	value.formatted          = r.Bool();
	value.scalar             = r.Bool();
	return value;
}

void WriteAddress(Writer& w, const IR::AddressResource& r) {
	w.U32(r.source);
	w.U32(r.first_use_pc);
	w.U32(static_cast<uint32_t>(r.kind));
	w.I32(r.min_offset);
	w.U64(r.specialized_base);
	w.Bool(r.unbased);
	w.Bool(r.read);
	w.Bool(r.written);
	w.Bool(r.atomic);
}

IR::AddressResource ReadAddress(Reader& r) {
	IR::AddressResource value {};
	value.source           = r.U32();
	value.first_use_pc     = r.U32();
	value.kind             = static_cast<IR::ResourceKind>(r.U32());
	value.min_offset       = r.I32();
	value.specialized_base = r.U64();
	value.unbased          = r.Bool();
	value.read             = r.Bool();
	value.written          = r.Bool();
	value.atomic           = r.Bool();
	return value;
}

void WriteImage(Writer& w, const IR::ImageResource& r) {
	w.U32(r.source);
	w.U32(r.first_use_pc);
	w.U32(static_cast<uint32_t>(r.kind));
	w.U32(static_cast<uint32_t>(r.dimension));
	w.U32(static_cast<uint32_t>(r.mip_mode));
	w.U32(r.mip_count);
	w.U32(r.storage_swizzle);
	w.Bool(r.read);
	w.Bool(r.written);
	w.Bool(r.atomic);
	w.Bool(r.depth_compare);
	w.Bool(r.cube);
	w.U32(r.indirect_root);
	w.U32(r.indirect_mapping_offset);
	w.U32(r.indirect_mapping_capacity);
	w.U32Vector(r.indirect_resources);
}

IR::ImageResource ReadImage(Reader& r) {
	IR::ImageResource value {};
	value.source                    = r.U32();
	value.first_use_pc              = r.U32();
	value.kind                      = static_cast<IR::ResourceKind>(r.U32());
	value.dimension = static_cast<ShaderRecompiler::Decoder::ImageDimension>(r.U32());
	value.mip_mode                  = static_cast<IR::ImageMipMode>(r.U32());
	value.mip_count                 = r.U32();
	value.storage_swizzle           = r.U32();
	value.read                      = r.Bool();
	value.written                   = r.Bool();
	value.atomic                    = r.Bool();
	value.depth_compare             = r.Bool();
	value.cube                      = r.Bool();
	value.indirect_root             = r.U32();
	value.indirect_mapping_offset   = r.U32();
	value.indirect_mapping_capacity = r.U32();
	value.indirect_resources        = r.U32Vector();
	return value;
}

void WriteShaderInfo(Writer& w, const IR::ShaderInfo& info) {
	w.U32(static_cast<uint32_t>(info.buffers.size()));
	for (const auto& value: info.buffers) {
		WriteBuffer(w, value);
	}
	w.U32(static_cast<uint32_t>(info.addresses.size()));
	for (const auto& value: info.addresses) {
		WriteAddress(w, value);
	}
	w.U32(static_cast<uint32_t>(info.images.size()));
	for (const auto& value: info.images) {
		WriteImage(w, value);
	}
	w.U32(static_cast<uint32_t>(info.samplers.size()));
	for (const auto& value: info.samplers) {
		w.U32(value.source);
		w.U32(value.first_use_pc);
	}
	w.U32(static_cast<uint32_t>(info.sampled_pairs.size()));
	for (const auto& value: info.sampled_pairs) {
		w.U32(value.image);
		w.U32(value.sampler);
		w.U32(value.first_use_pc);
	}
	w.U32(static_cast<uint32_t>(info.inputs.size()));
	for (const auto& value: info.inputs) {
		w.U32(static_cast<uint32_t>(value.kind));
		w.U32(value.location);
		w.U32(value.component_count);
		w.String(value.debug_name);
	}
	w.U32(static_cast<uint32_t>(info.outputs.size()));
	for (const auto& value: info.outputs) {
		w.U32(static_cast<uint32_t>(value.kind));
		w.U32(value.index);
		w.U32(value.location);
		w.String(value.debug_name);
	}
	w.I32(info.vertex_offset_sgpr);
	w.Bool(info.has_bitwise_xor);
}

// Every vector length is read through a helper that refuses a count the buffer
// cannot hold, so a corrupt length cannot turn into a huge allocation.
template <class T, class F>
bool ReadVector(Reader& r, std::vector<T>& out, size_t min_bytes_each, F&& read_one) {
	const auto count = r.U32();
	if (!r.Ok() || !r.CanHold(static_cast<uint64_t>(count) * min_bytes_each)) {
		return false;
	}
	out.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		out.push_back(read_one(r));
		if (!r.Ok()) {
			return false;
		}
	}
	return true;
}

bool ReadShaderInfo(Reader& r, IR::ShaderInfo& info) {
	if (!ReadVector(r, info.buffers, 29, ReadBuffer) ||
	    !ReadVector(r, info.addresses, 28, ReadAddress) ||
	    !ReadVector(r, info.images, 47, ReadImage)) {
		return false;
	}
	if (!ReadVector(r, info.samplers, 8,
	                [](Reader& in) {
		                IR::SamplerResource value {};
		                value.source       = in.U32();
		                value.first_use_pc = in.U32();
		                return value;
	                }) ||
	    !ReadVector(r, info.sampled_pairs, 12, [](Reader& in) {
		    IR::SampledResourcePair value {};
		    value.image        = in.U32();
		    value.sampler      = in.U32();
		    value.first_use_pc = in.U32();
		    return value;
	    })) {
		return false;
	}
	if (!ReadVector(r, info.inputs, 16,
	                [](Reader& in) {
		                IR::StageInput value {};
		                value.kind            = static_cast<IR::StageInputKind>(in.U32());
		                value.location        = in.U32();
		                value.component_count = in.U32();
		                value.debug_name      = in.String();
		                return value;
	                }) ||
	    !ReadVector(r, info.outputs, 16, [](Reader& in) {
		    IR::StageOutput value {};
		    value.kind       = static_cast<IR::StageOutputKind>(in.U32());
		    value.index      = in.U32();
		    value.location   = in.U32();
		    value.debug_name = in.String();
		    return value;
	    })) {
		return false;
	}
	info.vertex_offset_sgpr = r.I32();
	info.has_bitwise_xor    = r.Bool();
	return r.Ok();
}

void WriteBindings(Writer& w, const IR::BindingLayout& bindings) {
	w.U32(bindings.descriptor_set);
	w.U32(bindings.push_constant_offset);
	w.U32(bindings.push_constant_size);
	w.U32(bindings.buffer_offset_dword);
	w.U32(bindings.buffer_offset_count);
	w.U32Vector(bindings.user_data_registers);
	w.U32(static_cast<uint32_t>(bindings.descriptors.size()));
	for (const auto& descriptor: bindings.descriptors) {
		w.U32(static_cast<uint32_t>(descriptor.kind));
		w.U32(descriptor.binding);
		w.U32Vector(descriptor.resources);
	}
}

bool ReadBindings(Reader& r, IR::BindingLayout& bindings) {
	bindings.descriptor_set       = r.U32();
	bindings.push_constant_offset = r.U32();
	bindings.push_constant_size   = r.U32();
	bindings.buffer_offset_dword  = r.U32();
	bindings.buffer_offset_count  = r.U32();
	bindings.user_data_registers  = r.U32Vector();
	return ReadVector(r, bindings.descriptors, 12, [](Reader& in) {
		IR::DescriptorBinding value {};
		value.kind      = in.Enum<IR::DescriptorBindingKind>(
            static_cast<uint32_t>(IR::DescriptorBindingKind::Count));
		value.binding   = in.U32();
		value.resources = in.U32Vector();
		return value;
	});
}

void WriteProgram(Writer& w, const IR::Program& program) {
	w.U32(static_cast<uint32_t>(program.stage));
	w.U32(static_cast<uint32_t>(program.lane_mask_mode));
	w.U64(program.shader_hash);
	w.U32(program.wave_size);
	w.U32(program.user_data_base);
	w.U32(program.user_data_count);
	w.Bool(program.srt_plan_complete);
	w.Bool(program.resource_tracking_complete);
	w.Bool(program.shader_info_complete);
	w.Bool(program.binding_layout_complete);
	WriteShaderInfo(w, program.info);
	WriteBindings(w, program.bindings);

	w.Bool(program.spirv_requirements.has_value());
	if (program.spirv_requirements.has_value()) {
		const auto& req = *program.spirv_requirements;
		w.Bool(req.requires_exact_subgroup);
		w.Bool(req.subgroup_ballot);
		w.Bool(req.subgroup_shuffle);
		w.Bool(req.subgroup_local_invocation_id);
		w.Bool(req.compute_derivatives);
		w.Bool(req.image_gather_extended);
		w.Bool(req.function_lds);
		w.Bool(req.pixel_valid_mask);
	}
}

bool ReadProgram(Reader& r, IR::Program& program) {
	program.stage          = r.Enum<ShaderType>(5);
	program.lane_mask_mode = r.Enum<ShaderLaneMaskMode>(2);
	program.shader_hash    = r.U64();
	program.wave_size      = r.U32();
	program.user_data_base = r.U32();
	program.user_data_count            = r.U32();
	program.srt_plan_complete          = r.Bool();
	program.resource_tracking_complete = r.Bool();
	program.shader_info_complete       = r.Bool();
	program.binding_layout_complete    = r.Bool();

	if (!r.Ok() || !ReadShaderInfo(r, program.info) || !ReadBindings(r, program.bindings)) {
		return false;
	}

	if (r.Bool()) {
		IR::SpirvRequirements req {};
		req.requires_exact_subgroup      = r.Bool();
		req.subgroup_ballot              = r.Bool();
		req.subgroup_shuffle             = r.Bool();
		req.subgroup_local_invocation_id = r.Bool();
		req.compute_derivatives          = r.Bool();
		req.image_gather_extended        = r.Bool();
		req.function_lds                 = r.Bool();
		req.pixel_valid_mask             = r.Bool();
		program.spirv_requirements       = req;
	}
	return r.Ok();
}

std::vector<uint8_t> EncodeEntry(const ShaderDiskCacheEntry& entry) {
	Writer w;
	w.U32(static_cast<uint32_t>(entry.stage));
	w.U32(static_cast<uint32_t>(entry.lane_mask_mode));
	w.U64(entry.shader_hash);
	w.U32(entry.program_id.hash0);
	w.U32(entry.program_id.crc32);
	w.U32Vector(entry.program_id.ids);
	w.U32(static_cast<uint32_t>(entry.optimization_type));
	WriteProgram(w, *entry.program);
	w.U32Vector(entry.spirv);
	return w.Bytes();
}

bool DecodeEntry(const uint8_t* data, size_t size, ShaderDiskCacheEntry& entry) {
	Reader r(data, size);
	entry.stage             = r.Enum<ShaderType>(5);
	entry.lane_mask_mode    = r.Enum<ShaderLaneMaskMode>(2);
	entry.shader_hash       = r.U64();
	entry.program_id.hash0  = r.U32();
	entry.program_id.crc32  = r.U32();
	entry.program_id.ids    = r.U32Vector();
	entry.optimization_type = r.Enum<Config::ShaderOptimizationType>(3);
	if (!r.Ok()) {
		return false;
	}

	auto program = std::make_shared<IR::Program>();
	if (!ReadProgram(r, *program)) {
		return false;
	}
	entry.spirv = r.U32Vector();

	// A SPIR-V module is at least a header, and the whole payload must have been
	// consumed - trailing bytes mean this is not the record it claims to be.
	if (!r.Done() || entry.spirv.size() < 5 || entry.spirv[0] != 0x07230203) {
		return false;
	}

	entry.program = std::move(program);
	return true;
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path) {
	std::error_code ec;
	if (!std::filesystem::exists(path, ec) || ec) {
		return {};
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return {};
	}
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

uint64_t ShaderDiskCacheBuildId() {
	static const uint64_t id = ComputeBuildId();
	return id;
}

std::vector<ShaderDiskCacheEntry> ShaderDiskCacheLoad(const std::filesystem::path& path) {
	if (path.empty()) {
		return {};
	}

	const auto blob = ReadWholeFile(path);
	if (blob.empty()) {
		return {}; // first run
	}

	Reader header(blob.data(), blob.size());
	char   magic[sizeof(CACHE_MAGIC)] = {};
	for (char& c: magic) {
		c = static_cast<char>(header.U8());
	}
	const auto format   = header.U32();
	const auto build_id = header.U64();
	const auto count    = header.U32();

	if (!header.Ok() || std::memcmp(magic, CACHE_MAGIC, sizeof(CACHE_MAGIC)) != 0) {
		LOGF("Shader cache: not a shader cache file, ignoring\n");
		return {};
	}
	if (format != CACHE_FORMAT_VERSION || build_id != ShaderDiskCacheBuildId()) {
		LOGF("Shader cache: written by a different build, starting cold\n");
		return {};
	}

	std::vector<ShaderDiskCacheEntry> entries;
	entries.reserve(count);

	uint32_t rejected = 0;
	for (uint32_t i = 0; i < count; i++) {
		const auto size     = header.U64();
		const auto checksum = header.U64();
		if (!header.Ok() || !header.CanHold(size)) {
			break; // truncated file: keep what was read before this point
		}
		const auto* payload = header.Skip(size);
		if (payload == nullptr) {
			break;
		}
		if (XXH3_64bits(payload, size) != checksum) {
			rejected++;
			continue;
		}
		ShaderDiskCacheEntry entry;
		if (!DecodeEntry(payload, size, entry)) {
			rejected++;
			continue;
		}
		entries.push_back(std::move(entry));
	}

	LOGF("Shader cache: %zu entries loaded from %s%s\n", entries.size(),
	     path.string().c_str(),
	     rejected != 0 ? " (some entries were rejected and will be recompiled)" : "");
	return entries;
}

bool ShaderDiskCacheSave(const std::filesystem::path&             path,
                         const std::vector<ShaderDiskCacheEntry>& entries) {
	if (path.empty()) {
		return false;
	}

	Writer header;
	for (char c: CACHE_MAGIC) {
		header.U8(static_cast<uint8_t>(c));
	}
	header.U32(CACHE_FORMAT_VERSION);
	header.U64(ShaderDiskCacheBuildId());

	std::vector<std::vector<uint8_t>> payloads;
	payloads.reserve(entries.size());
	for (const auto& entry: entries) {
		if (entry.program == nullptr || entry.spirv.empty()) {
			continue;
		}
		payloads.push_back(EncodeEntry(entry));
	}
	header.U32(static_cast<uint32_t>(payloads.size()));

	std::error_code ec;
	auto            tmp = path;
	tmp += ".tmp";

	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) {
			return false;
		}
		out.write(reinterpret_cast<const char*>(header.Bytes().data()),
		          static_cast<std::streamsize>(header.Bytes().size()));
		for (const auto& payload: payloads) {
			Writer entry_header;
			entry_header.U64(payload.size());
			entry_header.U64(XXH3_64bits(payload.data(), payload.size()));
			out.write(reinterpret_cast<const char*>(entry_header.Bytes().data()),
			          static_cast<std::streamsize>(entry_header.Bytes().size()));
			out.write(reinterpret_cast<const char*>(payload.data()),
			          static_cast<std::streamsize>(payload.size()));
		}
		if (!out) {
			std::filesystem::remove(tmp, ec);
			return false;
		}
	}

	// Temp file plus rename: an exit interrupted here leaves the previous cache
	// intact rather than a half-written one.
	std::filesystem::rename(tmp, path, ec);
	if (ec) {
		std::filesystem::remove(tmp, ec);
		return false;
	}

	LOGF("Shader cache: %zu entries written to %s\n", payloads.size(), path.string().c_str());
	return true;
}

} // namespace Libs::Graphics
