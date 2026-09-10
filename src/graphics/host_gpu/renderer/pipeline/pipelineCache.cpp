#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("KytyPC1:{}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

std::string ShaderCacheSignature() {
	// Independent of DriverCacheSignature's tag/format on purpose: a shader
	// disk cache entry is portable SPIR-V, not an opaque driver blob, so it
	// only needs to be rejected on an emulator/recompiler change (git
	// revision), not a driver or GPU change the way the pipeline cache does.
	return fmt::format("KytySC1:{}\n", KYTY_GIT_REVISION);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	if (Log::GetDirection() != Log::Direction::Console) {
		std::fwrite(message.data(), 1, message.size(), stdout);
		std::fflush(stdout);
	}
	Log::Write(message);
	Log::Flush();
}

bool ReadShaderGuestMemory(void*, uint64_t address, uint32_t* value) {
	return value != nullptr &&
	       Libs::LibKernel::Memory::TryReadGpuCleanBacking(address, value, sizeof(*value));
}

// The primary SRT evaluator's memory reader (SrtWalker.cpp's `EvaluateRawRead`, used for every
// ordinary scalar/constant-buffer read on every draw/dispatch), as opposed to
// ReadShaderGuestMemory above which is only wired to read_specialization_memory (branch-
// reachability evaluation at compile/specialization time, where refusing GPU-dirty ranges is
// correct). Session 22 (2026-09-10) crashed the whole process when `.read_memory` was left
// unset here: with no callback, SrtWalker.cpp's evaluator falls through to a bare, unvalidated
// `std::memcpy` from a guest-computed address, and one wrong/zeroed descriptor (e.g. from a
// non-invariant Phi) becomes a wild host pointer dereference (SIGSEGV, misattributed at the time
// to runtimeLinker.cpp:844 -- that line is only KytyExceptionHandler's `EXIT()` call site, which
// prints for *any* unhandled host fault; the actual fault was here, in the shader recompiler,
// confirmed by matching the crash dump's own code bytes against the built binary). Wiring a real
// reader turns that crash into a normal, hash-and-address-labelled materialization failure
// (logged once per hash by the `materialize_failure_logged` check below) instead.
bool ReadShaderGuestMemoryDirect(void*, uint64_t address, uint32_t* value) {
	return value != nullptr && Libs::LibKernel::Memory::TryReadBacking(address, value, sizeof(*value));
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code, const std::string& decoded_dump) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto base = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(base.parent_path());
	for (const auto& [suffix, data, size]: {
	         std::tuple {".bin", static_cast<const void*>(code.data()), code.size_bytes()},
	         std::tuple {".rdna2", static_cast<const void*>(decoded_dump.data()),
	                     decoded_dump.size()},
	     }) {
		if (size == 0) {
			// A zero-length ".rdna2" is not "no disassembly" -- it means the decode never ran
			// because --shader-log-direction is Silent (see options.dump_ir at the call site).
			// Say so instead of leaving a silently-absent file that looks identical to an
			// intentional skip.
			if (std::string_view {suffix} == ".rdna2") {
				static Log::RateLimit limiter {"DumpShaderOriginal:EmptyRdna2", 16};
				if (const auto hit = limiter.Hit()) {
					LOGF_COLOR(Log::Color::Yellow,
					           "DumpShaderOriginal[%llu]: %s_%016" PRIx64
					           ".rdna2 not written -- guest RDNA2 disassembly text is only "
					           "produced when --shader-log-direction is Console or File "
					           "(currently Silent)\n",
					           *hit, stage_name, shader_hash);
				}
			}
			continue;
		}
		auto path = base;
		path += suffix;
		Common::File file(path);
		if (file.IsInvalid()) {
			const auto path_text = Common::PathToString(path);
			LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		} else {
			file.Write(data, size);
		}
	}
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

// ── Shader disk cache serialization ──────────────────────────────────────
//
// Binary encoding for exactly the fields a cache hit needs: a ProgramKey +
// ResourceSpecialization pair (the lookup identity) and the resulting
// CompiledShaderInfo + SPIR-V (what a hit skips recompiling). Nothing here
// touches ShaderRecompiler::IR::Program's transient compile state (Block,
// Value, Inst) — that is never serialized, matching the same "the plan is
// not the whole IR" principle CompiledShaderInfo itself already applies.
//
// Read* functions never throw or assert on malformed input: a corrupt or
// foreign-version file must degrade to "cache miss, recompile", the same
// contract the driver pipeline cache above already gives a rejected blob.
class ByteWriter {
public:
	explicit ByteWriter(std::vector<uint8_t>& out) : m_out(out) {}

	template <typename T>
	    requires std::is_trivially_copyable_v<T>
	void Pod(const T& value) {
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		m_out.insert(m_out.end(), bytes, bytes + sizeof(T));
	}
	void U32(uint32_t v) { Pod(v); }
	void U64(uint64_t v) { Pod(v); }
	void I32(int32_t v) { Pod(v); }
	void Bool(bool v) { Pod(static_cast<uint8_t>(v ? 1 : 0)); }
	template <typename E>
	    requires std::is_enum_v<E>
	void Enum(E v) {
		Pod(static_cast<std::underlying_type_t<E>>(v));
	}
	void Str(const std::string& s) {
		U32(static_cast<uint32_t>(s.size()));
		m_out.insert(m_out.end(), s.begin(), s.end());
	}
	template <typename T>
	    requires std::is_trivially_copyable_v<T>
	void PodVector(const std::vector<T>& v) {
		U32(static_cast<uint32_t>(v.size()));
		if (!v.empty()) {
			const auto* bytes = reinterpret_cast<const uint8_t*>(v.data());
			m_out.insert(m_out.end(), bytes, bytes + v.size() * sizeof(T));
		}
	}
	template <typename T, typename F>
	void Vector(const std::vector<T>& v, F&& each) {
		U32(static_cast<uint32_t>(v.size()));
		for (const auto& item: v) {
			each(*this, item);
		}
	}

private:
	std::vector<uint8_t>& m_out;
};

class ByteReader {
public:
	ByteReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

	[[nodiscard]] bool Ok() const { return m_ok; }

	template <typename T>
	    requires std::is_trivially_copyable_v<T>
	T Pod() {
		T value {};
		if (m_pos + sizeof(T) > m_size) {
			m_ok = false;
			return value;
		}
		std::memcpy(&value, m_data + m_pos, sizeof(T));
		m_pos += sizeof(T);
		return value;
	}
	uint32_t U32() { return Pod<uint32_t>(); }
	uint64_t U64() { return Pod<uint64_t>(); }
	int32_t  I32() { return Pod<int32_t>(); }
	bool     Bool() { return Pod<uint8_t>() != 0; }
	template <typename E>
	    requires std::is_enum_v<E>
	E Enum() {
		return static_cast<E>(Pod<std::underlying_type_t<E>>());
	}
	std::string Str() {
		const auto len = U32();
		if (!m_ok || m_pos + len > m_size) {
			m_ok = false;
			return {};
		}
		std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
		m_pos += len;
		return s;
	}
	template <typename T>
	    requires std::is_trivially_copyable_v<T>
	std::vector<T> PodVector() {
		const auto    count = U32();
		std::vector<T> v;
		if (!m_ok || m_pos + static_cast<size_t>(count) * sizeof(T) > m_size) {
			m_ok = false;
			return v;
		}
		v.resize(count);
		if (count != 0) {
			std::memcpy(v.data(), m_data + m_pos, static_cast<size_t>(count) * sizeof(T));
		}
		m_pos += static_cast<size_t>(count) * sizeof(T);
		return v;
	}
	template <typename T, typename F>
	std::vector<T> Vector(F&& each) {
		const auto     count = U32();
		std::vector<T> v;
		if (!m_ok) {
			return v;
		}
		v.reserve(count);
		for (uint32_t i = 0; i < count && m_ok; i++) {
			v.push_back(each(*this));
		}
		return v;
	}

private:
	const uint8_t* m_data;
	size_t         m_size;
	size_t         m_pos = 0;
	bool           m_ok  = true;
};

void WriteBufferResource(ByteWriter& w, const ShaderRecompiler::IR::BufferResource& r) {
	w.U32(r.source);
	w.U32(r.first_use_pc);
	w.U32(r.max_byte_extent);
	w.U32(r.packed_stride);
	w.Enum(r.descriptor_format);
	w.U32(r.descriptor_swizzle);
	w.U32(r.image_alias);
	w.Bool(r.read);
	w.Bool(r.written);
	w.Bool(r.atomic);
	w.Bool(r.formatted);
	w.Bool(r.scalar);
}

ShaderRecompiler::IR::BufferResource ReadBufferResource(ByteReader& r) {
	ShaderRecompiler::IR::BufferResource v;
	v.source             = r.U32();
	v.first_use_pc       = r.U32();
	v.max_byte_extent    = r.U32();
	v.packed_stride      = r.U32();
	v.descriptor_format  = r.Enum<Prospero::BufferFormat>();
	v.descriptor_swizzle = r.U32();
	v.image_alias        = r.U32();
	v.read                = r.Bool();
	v.written              = r.Bool();
	v.atomic               = r.Bool();
	v.formatted            = r.Bool();
	v.scalar               = r.Bool();
	return v;
}

void WriteImageResource(ByteWriter& w, const ShaderRecompiler::IR::ImageResource& r) {
	w.U32(r.source);
	w.U32(r.first_use_pc);
	w.Enum(r.resource_class);
	w.Enum(r.numeric_class);
	w.Enum(r.dimension);
	w.Enum(r.mip_mode);
	w.U32(r.mip_count);
	w.Enum(r.conversion_format);
	w.U32(r.shader_swizzle);
	w.Bool(r.read);
	w.Bool(r.written);
	w.Bool(r.atomic);
	w.Bool(r.depth_compare);
	w.Bool(r.cube);
	w.Bool(r.r128);
	w.U32(r.indirect_root);
	w.U32(r.indirect_mapping_offset);
	w.U32(r.indirect_search_iterations);
	w.PodVector(r.indirect_resources);
}

ShaderRecompiler::IR::ImageResource ReadImageResource(ByteReader& r) {
	ShaderRecompiler::IR::ImageResource v;
	v.source         = r.U32();
	v.first_use_pc   = r.U32();
	v.resource_class = r.Enum<ShaderRecompiler::IR::ImageResourceClass>();
	v.numeric_class  = r.Enum<Prospero::TextureNumericClass>();
	v.dimension      = r.Enum<ShaderRecompiler::Decoder::ImageDimension>();
	v.mip_mode       = r.Enum<ShaderRecompiler::IR::ImageMipMode>();
	v.mip_count      = r.U32();
	v.conversion_format         = r.Enum<Prospero::BufferFormat>();
	v.shader_swizzle            = r.U32();
	v.read                      = r.Bool();
	v.written                   = r.Bool();
	v.atomic                    = r.Bool();
	v.depth_compare             = r.Bool();
	v.cube                      = r.Bool();
	v.r128                      = r.Bool();
	v.indirect_root             = r.U32();
	v.indirect_mapping_offset   = r.U32();
	v.indirect_search_iterations = r.U32();
	v.indirect_resources        = r.PodVector<uint32_t>();
	return v;
}

void WriteSamplerResource(ByteWriter& w, const ShaderRecompiler::IR::SamplerResource& r) {
	w.U32(r.source);
	w.U32(r.first_use_pc);
	w.Bool(r.force_point_filtering);
	w.Bool(r.depth_compare);
}

ShaderRecompiler::IR::SamplerResource ReadSamplerResource(ByteReader& r) {
	ShaderRecompiler::IR::SamplerResource v;
	v.source                = r.U32();
	v.first_use_pc          = r.U32();
	v.force_point_filtering = r.Bool();
	v.depth_compare         = r.Bool();
	return v;
}

void WriteSampledPair(ByteWriter& w, const ShaderRecompiler::IR::SampledResourcePair& r) {
	w.U32(r.image);
	w.U32(r.sampler);
	w.U32(r.first_use_pc);
}

ShaderRecompiler::IR::SampledResourcePair ReadSampledPair(ByteReader& r) {
	ShaderRecompiler::IR::SampledResourcePair v;
	v.image        = r.U32();
	v.sampler      = r.U32();
	v.first_use_pc = r.U32();
	return v;
}

void WriteStageInput(ByteWriter& w, const ShaderRecompiler::IR::StageInput& r) {
	w.Enum(r.kind);
	w.U32(r.location);
	w.U32(r.component_count);
	w.Str(r.debug_name);
	w.Bool(r.per_vertex);
}

ShaderRecompiler::IR::StageInput ReadStageInput(ByteReader& r) {
	ShaderRecompiler::IR::StageInput v;
	v.kind            = r.Enum<ShaderRecompiler::IR::StageInputKind>();
	v.location        = r.U32();
	v.component_count = r.U32();
	v.debug_name       = r.Str();
	v.per_vertex       = r.Bool();
	return v;
}

void WriteStageOutput(ByteWriter& w, const ShaderRecompiler::IR::StageOutput& r) {
	w.Enum(r.kind);
	w.U32(r.index);
	w.U32(r.location);
	w.Str(r.debug_name);
}

ShaderRecompiler::IR::StageOutput ReadStageOutput(ByteReader& r) {
	ShaderRecompiler::IR::StageOutput v;
	v.kind      = r.Enum<ShaderRecompiler::IR::StageOutputKind>();
	v.index     = r.U32();
	v.location  = r.U32();
	v.debug_name = r.Str();
	return v;
}

void WriteShaderInfo(ByteWriter& w, const ShaderRecompiler::IR::ShaderInfo& info) {
	w.Vector(info.buffers, [](ByteWriter& w2, const auto& v) { WriteBufferResource(w2, v); });
	w.Vector(info.images, [](ByteWriter& w2, const auto& v) { WriteImageResource(w2, v); });
	w.Vector(info.samplers, [](ByteWriter& w2, const auto& v) { WriteSamplerResource(w2, v); });
	w.Vector(info.sampled_pairs, [](ByteWriter& w2, const auto& v) { WriteSampledPair(w2, v); });
	w.Vector(info.inputs, [](ByteWriter& w2, const auto& v) { WriteStageInput(w2, v); });
	w.Vector(info.outputs, [](ByteWriter& w2, const auto& v) { WriteStageOutput(w2, v); });
	w.PodVector(std::vector<uint8_t>(info.vertex_fetch_components.begin(),
	                                 info.vertex_fetch_components.end()));
	w.I32(info.vertex_offset_sgpr);
	w.I32(info.instance_offset_sgpr);
	w.Bool(info.has_bitwise_xor);
	w.Bool(info.uses_dma);
}

bool ReadShaderInfo(ByteReader& r, ShaderRecompiler::IR::ShaderInfo& info) {
	info.buffers = r.Vector<ShaderRecompiler::IR::BufferResource>(
	    [](ByteReader& r2) { return ReadBufferResource(r2); });
	info.images = r.Vector<ShaderRecompiler::IR::ImageResource>(
	    [](ByteReader& r2) { return ReadImageResource(r2); });
	info.samplers = r.Vector<ShaderRecompiler::IR::SamplerResource>(
	    [](ByteReader& r2) { return ReadSamplerResource(r2); });
	info.sampled_pairs = r.Vector<ShaderRecompiler::IR::SampledResourcePair>(
	    [](ByteReader& r2) { return ReadSampledPair(r2); });
	info.inputs = r.Vector<ShaderRecompiler::IR::StageInput>(
	    [](ByteReader& r2) { return ReadStageInput(r2); });
	info.outputs = r.Vector<ShaderRecompiler::IR::StageOutput>(
	    [](ByteReader& r2) { return ReadStageOutput(r2); });
	const auto fetch_components = r.PodVector<uint8_t>();
	if (fetch_components.size() != info.vertex_fetch_components.size()) {
		return false;
	}
	std::ranges::copy(fetch_components, info.vertex_fetch_components.begin());
	info.vertex_offset_sgpr   = r.I32();
	info.instance_offset_sgpr = r.I32();
	info.has_bitwise_xor      = r.Bool();
	info.uses_dma             = r.Bool();
	return r.Ok();
}

void WriteDescriptorBinding(ByteWriter& w, const ShaderRecompiler::IR::DescriptorBinding& b) {
	w.Enum(b.kind);
	w.PodVector(b.resources);
}

ShaderRecompiler::IR::DescriptorBinding ReadDescriptorBinding(ByteReader& r) {
	ShaderRecompiler::IR::DescriptorBinding b;
	b.kind      = r.Enum<ShaderRecompiler::IR::DescriptorBindingKind>();
	b.resources = r.PodVector<uint32_t>();
	return b;
}

void WriteBindingLayout(ByteWriter& w, const ShaderRecompiler::IR::BindingLayout& b) {
	w.U32(b.push_data_start_dword);
	w.U32(b.memory_offset_dword);
	w.U32(b.memory_offset_count);
	w.PodVector(b.user_data_registers);
	w.Vector(b.descriptors, [](ByteWriter& w2, const auto& v) { WriteDescriptorBinding(w2, v); });
}

ShaderRecompiler::IR::BindingLayout ReadBindingLayout(ByteReader& r) {
	ShaderRecompiler::IR::BindingLayout b;
	b.push_data_start_dword = r.U32();
	b.memory_offset_dword   = r.U32();
	b.memory_offset_count   = r.U32();
	b.user_data_registers   = r.PodVector<uint32_t>();
	b.descriptors           = r.Vector<ShaderRecompiler::IR::DescriptorBinding>(
	    [](ByteReader& r2) { return ReadDescriptorBinding(r2); });
	return b;
}

void WriteCompiledShaderInfo(ByteWriter& w, const ShaderRecompiler::IR::CompiledShaderInfo& info) {
	w.Enum(info.stage);
	w.U64(info.shader_hash);
	w.U32(info.wave_size);
	w.U32(info.user_data_base);
	w.U32(info.user_data_count);
	w.U32(info.scratch_dwords);
	w.U32(info.param_export_mask);
	WriteShaderInfo(w, info.info);
	WriteBindingLayout(w, info.bindings);
}

bool ReadCompiledShaderInfo(ByteReader& r, ShaderRecompiler::IR::CompiledShaderInfo& info) {
	info.stage             = r.Enum<ShaderType>();
	info.shader_hash        = r.U64();
	info.wave_size          = r.U32();
	info.user_data_base     = r.U32();
	info.user_data_count    = r.U32();
	info.scratch_dwords     = r.U32();
	info.param_export_mask  = r.U32();
	if (!ReadShaderInfo(r, info.info)) {
		return false;
	}
	info.bindings = ReadBindingLayout(r);
	return r.Ok();
}

void WriteSpecialization(ByteWriter& w, const ShaderRecompiler::IR::ResourceSpecialization& s) {
	w.Vector(s.buffers, [](ByteWriter& w2, const auto& v) {
		w2.U32(v.packed_stride);
		w2.Enum(v.descriptor_format);
		w2.U32(v.descriptor_swizzle);
	});
	w.Vector(s.images, [](ByteWriter& w2, const auto& v) {
		w2.Enum(v.numeric_class);
		w2.Enum(v.dimension);
		w2.U32(v.mip_count);
		w2.Enum(v.conversion_format);
		w2.U32(v.shader_swizzle);
		w2.U32(v.indirect_root);
		w2.U32(v.indirect_mapping_offset);
		w2.U32(v.indirect_search_iterations);
		w2.Bool(v.cube);
		w2.Bool(v.fmask);
	});
}

ShaderRecompiler::IR::ResourceSpecialization ReadSpecialization(ByteReader& r) {
	ShaderRecompiler::IR::ResourceSpecialization s;
	s.buffers = r.Vector<ShaderRecompiler::IR::ResourceSpecialization::Buffer>([](ByteReader& r2) {
		ShaderRecompiler::IR::ResourceSpecialization::Buffer v;
		v.packed_stride      = r2.U32();
		v.descriptor_format  = r2.Enum<Prospero::BufferFormat>();
		v.descriptor_swizzle = r2.U32();
		return v;
	});
	s.images = r.Vector<ShaderRecompiler::IR::ResourceSpecialization::Image>([](ByteReader& r2) {
		ShaderRecompiler::IR::ResourceSpecialization::Image v;
		v.numeric_class  = r2.Enum<Prospero::TextureNumericClass>();
		v.dimension      = r2.Enum<ShaderRecompiler::Decoder::ImageDimension>();
		v.mip_count      = r2.U32();
		v.conversion_format          = r2.Enum<Prospero::BufferFormat>();
		v.shader_swizzle             = r2.U32();
		v.indirect_root              = r2.U32();
		v.indirect_mapping_offset    = r2.U32();
		v.indirect_search_iterations = r2.U32();
		v.cube                       = r2.Bool();
		v.fmask                      = r2.Bool();
		return v;
	});
	return s;
}

} // namespace

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
	};

	struct SourceEntry {
		explicit SourceEntry(ShaderRecompiler::IR::ResourcePlan plan)
		    : resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		ShaderRecompiler::IR::ResourcePlan resource_plan;
		std::vector<Permutation>           permutations;
		// Set once MaterializeResources fails with a STRUCTURAL reason (currently: "Phi is not
		// invariant across control flow" -- a resource descriptor whose value genuinely differs
		// per incoming control-flow edge in the shader's own IR). That failure depends only on
		// `resource_plan` (fixed once this SourceEntry exists, identical for every dispatch of
		// this shader+static_state) and NOT on the per-call `runtime`/user_data, unlike most
		// other MaterializeResources failure reasons -- so unlike those, retrying can never
		// succeed. Found investigating ASTRO's Playroom, 2026-09-10: without this flag, a shader
		// hitting this case gets a full MaterializeResources attempt on EVERY dispatch (thousands
		// per frame for a commonly-used compute shader), which was the actual cause of sustained
		// near-zero GPU utilization / pegged CPU / frozen frame progress observed repeatedly this
		// session (see workflow/astro_playroom_issues.md). Only this one specific, confirmed-
		// deterministic failure reason short-circuits -- every other reason still retries exactly
		// as before, since those CAN legitimately depend on runtime state that changes call to
		// call.
		bool structurally_unmaterializable = false;
	};

	// The disk-persisted twin of Permutation: everything LoadPermutationFromDisk
	// needs to rebuild one without ShaderRecompiler::CompileProgram. No
	// ShaderProgram handle (that is a live Vulkan module id, meaningless
	// across a process boundary) and no ResourcePlan (transient compiler
	// state that TranslateProgram + ExtractResourcePlan always rebuild
	// fresh, cache or not — see InitializeShaderDiskCache's comment).
	struct DiskEntry {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		std::vector<uint32_t>                        spirv;
	};

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing up to 429 words first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 13 + ShaderVertexInputInfo::RES_MAX * 13;

	Permutation CompilePermutation(const ShaderParams&                          params,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword,
	                               std::vector<uint32_t>* out_spirv = nullptr) {
		const char* stage_name = nullptr;
		switch (options.stage) {
			case ShaderType::Vertex: stage_name = "vs"; break;
			case ShaderType::Mesh: stage_name = "ms"; break;
			case ShaderType::Pixel: stage_name = "ps"; break;
			case ShaderType::Compute: stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		DumpShaderOriginal(stage_name, options.shader_hash, params.code, result.decoded_dump);
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
		if (out_spirv != nullptr) {
			*out_spirv = result.spirv;
		}

		vk::ShaderModuleCreateInfo create_info {};
		create_info.codeSize    = result.spirv.size() * sizeof(uint32_t);
		create_info.pCode       = result.spirv.data();
		vk::ShaderModule module = nullptr;
		RequireVulkanSuccess(device.createShaderModule(&create_info, nullptr, &module),
		                     "create recompiled shader module");
		EXIT_IF(module == nullptr);
		if (options.dump_ir) {
			if (!options.early_dump) {
				LOGF("%s decoded RDNA2:\n%s", options.dump_label, result.decoded_dump.c_str());
				LOGF("%s IR:\n%s", options.dump_label, result.ir_dump.c_str());
			}
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = ++next_shader_id, .module = module},
		};
	}

	// Builds a Permutation straight from cached SPIR-V, skipping
	// ShaderRecompiler::CompileProgram entirely. `entry` must already be a
	// disk_cache-owned entry (both call sites guarantee this — see
	// LoadDiskCache and Get()); this only creates the live Vulkan module a
	// serialized entry has no equivalent of.
	Permutation LoadPermutationFromDisk(const DiskEntry& entry) {
		vk::ShaderModuleCreateInfo create_info {};
		create_info.codeSize    = entry.spirv.size() * sizeof(uint32_t);
		create_info.pCode       = entry.spirv.data();
		vk::ShaderModule module = nullptr;
		RequireVulkanSuccess(device.createShaderModule(&create_info, nullptr, &module),
		                     "create cached shader module");
		EXIT_IF(module == nullptr);

		return {
		    .specialization = entry.specialization,
		    .program        = entry.program,
		    .handle         = {.id = ++next_shader_id, .module = module},
		};
	}

	// Loads every disk-cached permutation for this title into disk_cache
	// (no Vulkan calls here — modules are created lazily, only for an entry
	// a real Get() lookup actually hits this session). A missing file, a
	// signature mismatch, a truncated file, or a corrupt entry all mean the
	// same thing: fewer entries, no error — the recompiler behind this is
	// always correct, just slower to reach on a cold cache.
	void LoadDiskCache(const std::filesystem::path& path) {
		if (!Common::File::IsFileExisting(path)) {
			return;
		}
		Common::File file(path, Common::File::Mode::Read);
		if (file.IsInvalid()) {
			return;
		}
		const auto file_size = file.Size();
		std::vector<uint8_t> blob(file_size);
		uint32_t              read = 0;
		file.Read(blob.data(), static_cast<uint32_t>(blob.size()), &read);
		file.Close();
		if (read != blob.size()) {
			return;
		}

		const auto signature = ShaderCacheSignature();
		if (blob.size() < signature.size() ||
		    std::memcmp(blob.data(), signature.data(), signature.size()) != 0) {
			PipelineCacheLog("Shader disk cache: invalidating {} (emulator version mismatch)",
			                 Common::PathToString(path));
			return;
		}

		ByteReader reader(blob.data() + signature.size(), blob.size() - signature.size());
		uint32_t   loaded = 0;
		while (reader.Ok()) {
			const auto has_next = reader.Bool();
			if (!reader.Ok() || !has_next) {
				break;
			}

			ProgramKey key;
			key.stage           = reader.Enum<ShaderType>();
			key.hash            = reader.U64();
			key.user_data_count = reader.U32();
			key.code_size       = reader.U32();
			key.static_state    = reader.PodVector<uint32_t>();

			DiskEntry entry;
			entry.specialization = ReadSpecialization(reader);
			if (!ReadCompiledShaderInfo(reader, entry.program)) {
				break;
			}
			entry.spirv = reader.PodVector<uint32_t>();
			if (!reader.Ok() || entry.spirv.empty()) {
				break;
			}

			disk_cache[key].push_back(std::move(entry));
			loaded++;
		}
		// Cache hits skip CompilePermutation entirely, so --graphics-debug-dump/--draw-dump
		// produce zero .spv/.rdna2/.bin output for any shader loaded from here this run. Say so
		// up front rather than let a debug session silently see nothing for a warm cache.
		PipelineCacheLog(
		    "Shader disk cache: loaded {} permutation(s) from {}{}", loaded,
		    Common::PathToString(path),
		    loaded > 0 ? " (debug shader dumps are suppressed for cache hits -- delete this "
		                 "file before a --graphics-debug-dump run if fresh dumps are needed)"
		               : "");
	}

	// Writes every permutation this ProgramCache currently knows about
	// (loaded from disk this session, or freshly compiled) back to `path`
	// via a temp file + rename, so an interrupted exit never leaves a
	// half-written file for the next launch to read.
	void SaveDiskCache(const std::filesystem::path& path) const {
		if (disk_cache.empty()) {
			return;
		}

		std::vector<uint8_t> blob;
		const auto            signature = ShaderCacheSignature();
		blob.insert(blob.end(), signature.begin(), signature.end());

		ByteWriter writer(blob);
		size_t     count = 0;
		for (const auto& [key, entries]: disk_cache) {
			for (const auto& entry: entries) {
				writer.Bool(true);
				writer.Enum(key.stage);
				writer.U64(key.hash);
				writer.U32(key.user_data_count);
				writer.U32(key.code_size);
				writer.PodVector(key.static_state);
				WriteSpecialization(writer, entry.specialization);
				WriteCompiledShaderInfo(writer, entry.program);
				writer.PodVector(entry.spirv);
				count++;
			}
		}
		writer.Bool(false);

		if (!Common::File::CreateDirectories(path.parent_path())) {
			PipelineCacheLog("Shader disk cache: failed to create cache directory");
			return;
		}
		auto temp_path = path;
		temp_path += ".tmp";
		Common::File file;
		uint32_t     written = 0;
		if (file.Create(temp_path)) {
			file.Write(blob.data(), static_cast<uint32_t>(blob.size()), &written);
		}
		const bool flushed = !file.IsInvalid() && file.Flush();
		file.Close();
		if (written != blob.size() || !flushed || !Common::File::RenameFile(temp_path, path)) {
			PipelineCacheLog("Shader disk cache: failed to write {}", Common::PathToString(path));
			return;
		}
		PipelineCacheLog("Shader disk cache: saved {} permutation(s) ({} bytes) to {}", count,
		                 blob.size(), Common::PathToString(path));
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.mesh.threads_num[0] != 0 ? ShaderType::Mesh : ShaderType::Vertex;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = static_cast<uint32_t>(params.user_data.size());
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto entry = programs.find(lookup_key);
		// Temporary diagnostic (from the ASTRO's Playroom investigation, 2026-09-09): names why a shader is missing the
		// in-memory cache on every call instead of hitting after the first compile. Capped per
		// hash so a genuinely-looping miss cannot flood the log the way the unbounded version
		// would. Prints the static key words so two "missing" calls for the same hash can be
		// diffed by eye to see whether static_state itself is the thing changing.
		if (entry == programs.end()) {
			auto& miss_count = cache_miss_log_count[params.hash];
			if (miss_count < 8) {
				miss_count++;
				std::string words;
				for (const auto word: lookup_key.static_state) {
					words += fmt::format("{:08x} ", word);
				}
				PipelineCacheLog(
				    "shader cache miss #{} hash=0x{:016x} stage={} user_data_count={} "
				    "code_size={} static_state=[{}] programs.size()={}",
				    miss_count, params.hash, static_cast<uint32_t>(stage),
				    lookup_key.user_data_count, lookup_key.code_size, words, programs.size());
			}
		}
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		const ShaderRecompiler::IR::SrtRuntime       runtime {
		    .user_data                  = params.user_data,
		    .shader_base                = params.Base(),
		    .read_memory                = ReadShaderGuestMemoryDirect,
		    .read_specialization_memory = ReadShaderGuestMemory,
		};
		if (entry != programs.end()) {
			if (entry->second.structurally_unmaterializable) {
				// Already proven this shader's resource_plan can never materialize (see
				// SourceEntry::structurally_unmaterializable) -- skip straight to the same
				// "skip this draw/dispatch" outcome without re-running MaterializeResources.
				return {};
			}
			std::string materialize_fail_reason;
			if (!ShaderRecompiler::IR::MaterializeResources(entry->second.resource_plan, runtime,
			                                                resources, specialization,
			                                                &materialize_fail_reason)) {
				// Not fatal: skip this one draw/dispatch instead of aborting the emulator (see
				// commit 6aa6e39's identical "skip, don't abort" precedent for an unsupported
				// primitive type). Only cached as a PERMANENT failure when the reason is the
				// confirmed-structural "Phi is not invariant" case (see
				// SourceEntry::structurally_unmaterializable) -- every other reason leaves
				// `entry`/its resource_plan untouched, so a later call with different user data
				// still gets a full retry. `stage` returns falsy (ShaderProgram::operator
				// bool()), which every caller must check before dereferencing
				// input_info.stage.program.
				if (materialize_fail_reason.find("Phi is not invariant") != std::string::npos) {
					entry->second.structurally_unmaterializable = true;
				}
				if (materialize_failure_logged.insert(params.hash).second) {
					PipelineCacheLog(
					    "shader resource materialization failed, skipping this draw/dispatch: "
					    "hash=0x{:016x} stage={}: {}",
					    params.hash, static_cast<uint32_t>(stage), materialize_fail_reason);
				}
				return {};
			}
			if (const auto permutation = std::ranges::find_if(
			        entry->second.permutations, [&](const Permutation& candidate) {
				        const auto& layout = candidate.program.bindings;
				        return layout.push_data_start_dword ==
				                   ShaderRecompiler::IR::PushData::StartFor(
				                       push_data_cursor, layout.ShaderDataDwords()) &&
				               candidate.specialization == specialization;
			        });
			    permutation != entry->second.permutations.end()) {
				input_info.stage = {.program   = &permutation->program,
				                    .resources = std::move(resources)};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = params.user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::ShaderLogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;
		options.scratch_dwords = input_info.scratch_size_dwords;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			if (stage == ShaderType::Mesh) {
				options.user_data_base = 0;
				options.wave_size      = input_info.mesh.wave_size;
				options.scratch_dwords = input_info.mesh.scratch_size_dwords;
			} else {
				options.wave_size = input_info.wave_size;
			}
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			options.wave_size = input_info.wave_size;
		} else if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			options.wave_size = input_info.wave_size;
		}
		auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
		if (entry == programs.end()) {
			auto        resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			std::string materialize_fail_reason;
			const bool  materialized = ShaderRecompiler::IR::MaterializeResources(
			    resource_plan, runtime, resources, specialization, &materialize_fail_reason);
			// Cache `resource_plan` either way (found investigating ASTRO's Playroom, 2026-09-09).
			// TranslateProgram + ExtractResourcePlan are deterministic in `params.code` alone --
			// a failed MaterializeResources here does not mean a *different* resource_plan would
			// come out next time, only that `runtime` (user_data / guest memory) did not satisfy
			// this one. Previously `entry` was left == programs.end() on failure, so the very
			// next dispatch of this same shader re-ran the full decode/CFG/IR pipeline from raw
			// bytecode instead of the cheap MaterializeResources-only retry the cache-hit branch
			// above already does. For a shader materialization can never satisfy (the Astro's
			// Playroom case: a resource Phi that is structurally non-invariant, so retrying
			// buys nothing) and that is dispatched thousands of times a frame, that was a full
			// shader recompile on every single dispatch -- the actual cause of the sustained
			// near-zero GPU utilization, pegged CPU, and no forward frame progress a cold
			// "dirty build" run showed. The retry-every-dispatch *policy* is unchanged: this
			// only removes the redundant recompile, entry->second.resource_plan is still
			// re-materialized fresh on every call exactly as before.
			entry = programs.try_emplace(lookup_key, std::move(resource_plan)).first;
			if (!materialized) {
				if (materialize_fail_reason.find("Phi is not invariant") != std::string::npos) {
					entry->second.structurally_unmaterializable = true;
				}
				if (materialize_failure_logged.insert(params.hash).second) {
					PipelineCacheLog(
					    "shader resource materialization failed, skipping this draw/dispatch: "
					    "hash=0x{:016x} stage={}: {}",
					    params.hash, static_cast<uint32_t>(stage), materialize_fail_reason);
				}
				return {};
			}
		}

		// A disk hit skips only ShaderRecompiler::CompileProgram (the IR ->
		// SPIR-V emission just above, in CompilePermutation) — TranslateProgram
		// already ran unconditionally above it, exactly as it would with no
		// disk cache at all, because `specialization` has to exist before
		// either the in-memory or the disk lookup can happen.
		if (const auto disk_slot = disk_cache.find(lookup_key); disk_slot != disk_cache.end()) {
			const auto disk_hit = std::ranges::find_if(
			    disk_slot->second,
			    [&](const DiskEntry& e) { return e.specialization == specialization; });
			if (disk_hit != disk_slot->second.end()) {
				entry->second.permutations.push_back(LoadPermutationFromDisk(*disk_hit));
				const auto& permutation = entry->second.permutations.back();
				input_info.stage = {.program = &permutation.program, .resources = std::move(resources)};
				permutation.program.bindings.AdvancePushData(push_data_cursor);
				return permutation.handle;
			}
		}

		std::vector<uint32_t> fresh_spirv;
		entry->second.permutations.push_back(CompilePermutation(
		    params, options, std::move(translated), specialization, push_data_cursor, &fresh_spirv));
		disk_cache[lookup_key].push_back(
		    {.specialization = std::move(specialization),
		     .program        = entry->second.permutations.back().program,
		     .spirv          = std::move(fresh_spirv)});
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = std::move(resources)};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::Mesh) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)]);
		return permutation.handle;
	}

	explicit ProgramCache(vk::Device device): device(device) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash>           programs;
	std::unordered_map<ProgramKey, std::vector<DiskEntry>, ProgramKeyHash> disk_cache;
	ProgramKey                                                            lookup_key;
	// Shader hashes that have already logged a materialization failure (Get() returns a falsy
	// ShaderProgram for these instead of aborting -- see Get()'s two MaterializeResources call
	// sites). Guarded by the same m_mutex every Get() call already holds. Not cached as a
	// permanent failure: a later draw with different user data may still materialize fine, so
	// this only silences the repeat LOGF, it never skips retrying MaterializeResources itself.
	std::unordered_set<uint64_t>                                          materialize_failure_logged;
	// Temporary diagnostic counter (from the ASTRO's Playroom investigation, 2026-09-09), see its use in Get().
	std::unordered_map<uint64_t, int>                                     cache_miss_log_count;
	vk::Device                                                            device;
	uint64_t                                                              next_shader_id = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(graphics.device)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
	InitializeShaderDiskCache();
}

PipelineCache::~PipelineCache() {
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}
	const std::string_view git_hash     = KYTY_GIT_HASH;
	const std::string_view git_revision = KYTY_GIT_REVISION;
	if (git_hash == "unknown" || git_revision == "unknown") {
		PipelineCacheLog("Vulkan pipeline cache: disabled (unknown git revision)");
		return;
	}
	if (git_hash.ends_with("-dirty") && !Config::ForceShaderDiskCacheEnabled()) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (dirty build)");
		return;
	}
	if (git_hash.ends_with("-dirty")) {
		PipelineCacheLog(
		    "Vulkan pipeline cache: dirty build, but --force-shader-disk-cache overrides the "
		    "safety gate -- a cache entry may have been produced by different recompiler code "
		    "than this tree");
	}

	m_driver_cache_path     = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::InitializeShaderDiskCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Shader disk cache: disabled (non-Release build)");
		return;
	}
	const std::string_view git_hash     = KYTY_GIT_HASH;
	const std::string_view git_revision = KYTY_GIT_REVISION;
	if (git_hash == "unknown" || git_revision == "unknown") {
		PipelineCacheLog("Shader disk cache: disabled (unknown git revision)");
		return;
	}
	if (git_hash.ends_with("-dirty") && !Config::ForceShaderDiskCacheEnabled()) {
		PipelineCacheLog("Shader disk cache: disabled (dirty build)");
		return;
	}
	if (git_hash.ends_with("-dirty")) {
		PipelineCacheLog(
		    "Shader disk cache: dirty build, but --force-shader-disk-cache overrides the safety "
		    "gate -- a cache entry may have been produced by different recompiler code than "
		    "this tree");
	}

	m_shader_cache_path = std::filesystem::path("_ShaderCache") / (title_id + ".bin");
	m_program_cache->LoadDiskCache(m_shader_cache_path);
}

void PipelineCache::Save() {
	Common::LockGuard lock(m_mutex);

	if (!m_shader_cache_path.empty()) {
		m_program_cache->SaveDiskCache(m_shader_cache_path);
	}

	if (m_driver_cache == nullptr) {
		return;
	}

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    ShaderVertexInputInfo& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const auto vertex_params = PrepareProgram(vertex_regs, context, user_config, vertex_info);
	const bool mesh_active   = vertex_info.mesh.threads_num[0] != 0;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info.mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info.clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
	}
	result.vertex = m_program_cache->Get(vertex_params, vertex_info, push_data_cursor);
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline& PipelineCache::CreateGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    const ShaderVertexInputInfo& vs_input_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const ShaderProgram& vertex_program,
    const ShaderProgram& pixel_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	Common::LockGuard lock(m_mutex);
	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	key.vs_shader_id            = vs_id;
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = color_count;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[i] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[i] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static Log::RateLimit limiter {"PipelineExecOnNoopWithDepthTest", 16};
		if (limiter.Hit()) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	static_params.stencil_test_enable      = depth.stencil_test_enable;
	static_params.stencil_front            = depth.stencil_static_front;
	static_params.stencil_back             = depth.stencil_static_back;
	const bool rect_list     = topology == vk::PrimitiveTopology::ePatchList;
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	for (uint32_t i = 0; i < color_count; i++) {
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		static_params.color_srcblend[i]       = bc.color_srcblend;
		static_params.color_comb_fcn[i]       = bc.color_comb_fcn;
		static_params.color_destblend[i]      = bc.color_destblend;
		static_params.alpha_srcblend[i]       = bc.alpha_srcblend;
		static_params.alpha_comb_fcn[i]       = bc.alpha_comb_fcn;
		static_params.alpha_destblend[i]      = bc.alpha_destblend;
		static_params.separate_alpha_blend[i] = bc.separate_alpha_blend;
		static_params.blend_enable[i]         = bc.enable;
		static_params.blend_bypass[i]         = rt.info.blend_bypass;
	}
	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		uint32_t attributes_num          = 0;
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			EXIT_IF(buffer.attr_num < 0 || buffer.attr_num > ShaderVertexInputBuffer::ATTR_MAX);
			attributes_num += static_cast<uint32_t>(buffer.attr_num);
			EXIT_IF(attributes_num > static_cast<uint32_t>(vs_input_info.resources_num));
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
			for (int attribute = 0; attribute < buffer.attr_num; attribute++) {
				const auto index = buffer.attr_indices[attribute];
				EXIT_IF(index < 0 || index >= vs_input_info.resources_num);
				key.vertex_input.attributes[index] = {
				    .offset  = buffer.attr_offsets[attribute],
				    .binding = static_cast<uint8_t>(binding),
				};
			}
		}
		EXIT_IF(attributes_num != static_cast<uint32_t>(vs_input_info.resources_num));
	}

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vs_input_info,
	                       vertex_program, ps_input_info, pixel_program, static_params,
	                       m_driver_cache);
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}

PipelineCache::Pipeline&
PipelineCache::CreateComputePipeline(const ShaderComputeInputInfo& input_info,
                                     const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	Common::LockGuard lock(m_mutex);

	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}
} // namespace Libs::Graphics
