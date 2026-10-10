#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/DescriptorBudget.h"

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
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ComputeExecution.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ShaderCaptureMetadata.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <nlohmann/json.hpp>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <string_view>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

uint8_t RemapSourceAlphaFactor(uint8_t factor) {
	switch (static_cast<Prospero::BlendFactor>(factor)) {
		case Prospero::BlendFactor::kSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Alpha);
		case Prospero::BlendFactor::kOneMinusSrcAlpha:
			return static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
		default: return factor;
	}
}

constexpr uint32_t DriverCacheCheckpointInterval = 16;
constexpr uint64_t DriverCacheExpensiveCreationMs = 5000;

bool IsLowerHex(std::string_view value, size_t expected_size) {
	return value.size() == expected_size &&
	       std::ranges::all_of(value, [](unsigned char c) {
		       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	       });
}

bool IsDriverCacheBuildIdentityUsable(std::string_view git_hash,
                                      std::string_view git_revision,
                                      std::string_view worktree_fingerprint) {
	return git_hash != "unknown" && IsLowerHex(git_revision, 40) &&
	       IsLowerHex(worktree_fingerprint, 64);
}

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

bool IsDriverCacheSignatureCompatible(std::string_view cached_signature,
                                      std::string_view expected_signature) {
	if (cached_signature == expected_signature) {
		return true;
	}

	// Vulkan keys cached entries by the full pipeline state. Keep the build fields for
	// provenance, but do not discard valid driver data merely because Kyty changed.
	// Validation mode (core vs GPUAV) is part of the driver-visible module identity:
	// mixing GPUAV-instrumented blobs into a core launch crashes NVIDIA's compiler.
	const auto implementation_identity = [](std::string_view signature)
	    -> std::optional<std::string_view> {
		constexpr std::string_view prefix = "KytyPC3:";
		if (!signature.starts_with(prefix) || !signature.ends_with('\n')) {
			return std::nullopt;
		}
		const auto revision_end = signature.find(':', prefix.size());
		if (revision_end == std::string_view::npos ||
		    !IsLowerHex(signature.substr(prefix.size(), revision_end - prefix.size()), 40)) {
			return std::nullopt;
		}
		const auto fingerprint_begin = revision_end + 1;
		const auto fingerprint_end   = signature.find(':', fingerprint_begin);
		if (fingerprint_end == std::string_view::npos ||
		    !IsLowerHex(signature.substr(fingerprint_begin,
		                                 fingerprint_end - fingerprint_begin),
		                64)) {
			return std::nullopt;
		}

		const auto identity = signature.substr(fingerprint_end + 1);
		// vendor:device:driver:uuid:vN\n  — fixed-width mode tag
		constexpr size_t identity_size = 8 + 1 + 8 + 1 + 8 + 1 + 32 + 1 + 2 + 1;
		if (identity.size() != identity_size || identity[8] != ':' ||
		    identity[17] != ':' || identity[26] != ':' || identity[59] != ':' ||
		    identity.back() != '\n' ||
		    !IsLowerHex(identity.substr(0, 8), 8) ||
		    !IsLowerHex(identity.substr(9, 8), 8) ||
		    !IsLowerHex(identity.substr(18, 8), 8) ||
		    !IsLowerHex(identity.substr(27, 32), 32) ||
		    (identity.substr(60, 2) != "v0" && identity.substr(60, 2) != "v2" &&
		     identity.substr(60, 2) != "v3")) {
			return std::nullopt;
		}
		return identity;
	};

	const auto cached_identity   = implementation_identity(cached_signature);
	const auto expected_identity = implementation_identity(expected_signature);
	return cached_identity.has_value() && expected_identity.has_value() &&
	       *cached_identity == *expected_identity;
}

std::string_view DriverCacheValidationModeTag(bool gpu_assisted_validation,
                                              bool shader_instrumentation) {
	return !gpu_assisted_validation ? "v0" : shader_instrumentation ? "v3" : "v2";
}

std::string DriverCacheFileName(std::string_view title_id, bool gpu_assisted_validation,
                                bool shader_instrumentation) {
	return fmt::format("{}-{}.bin", title_id,
	                   !gpu_assisted_validation ? "core"
	                   : shader_instrumentation ? "gpuav-instr"
	                                            : "gpuav-lite");
}

bool DriverCacheShaderInstrumentationEnabled() {
	if (!Config::GpuAssistedValidationEnabled()) {
		return false;
	}
	const auto* setting = std::getenv("VK_LAYER_GPUAV_SHADER_INSTRUMENTATION");
	// The validation layer instruments shaders by default. The launch scripts set
	// this variable explicitly when they request the lighter validation mode.
	return setting == nullptr || std::strcmp(setting, "0") != 0;
}

std::string FormatDriverCacheSignature(std::string_view git_revision,
                                       std::string_view worktree_fingerprint,
                                       uint32_t vendor_id, uint32_t device_id,
                                       uint32_t driver_version,
                                       std::string_view pipeline_cache_uuid_hex,
                                       bool gpu_assisted_validation, bool shader_instrumentation) {
	return fmt::format("KytyPC3:{}:{}:{:08x}:{:08x}:{:08x}:{}:{}\n", git_revision,
	                   worktree_fingerprint, vendor_id, device_id, driver_version,
	                   pipeline_cache_uuid_hex,
	                   DriverCacheValidationModeTag(gpu_assisted_validation,
	                                                shader_instrumentation));
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return FormatDriverCacheSignature(
	    KYTY_GIT_REVISION, KYTY_GIT_WORKTREE_FINGERPRINT, properties.vendorID,
	    properties.deviceID, properties.driverVersion, uuid,
	    Config::GpuAssistedValidationEnabled(), DriverCacheShaderInstrumentationEnabled());
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
	Log::WriteToConsoleAndLog(message);
}

bool ReadShaderBacking(void*, uint64_t address, std::span<uint32_t> values) {
 return !values.empty() && Libs::LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes());
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	// Specialization snapshots must observe GPU-published table bytes. Drain
	// dirty buffer ranges on the GPU thread; refuse texture-modified sources.
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadGpuCoherentBacking(address, values.data(),
	                                                         values.size_bytes());
}

uint64_t ClampShaderGuestMemory(void*, uint64_t address, uint64_t size) {
	return Libs::LibKernel::Memory::TryClampRangeSize(address, size);
}

void CaptureDispatchedShader(const ShaderParams& params,
                             const ShaderRecompiler::CompileOptions& options,
                             std::span<const uint32_t> static_state,
                             std::optional<std::array<uint32_t, 3>> guest_workgroups,
                             const nlohmann::ordered_json* runtime_capture = nullptr,
                             const std::filesystem::path& capture_directory = {}) {
	if (!Config::GraphicsDebugDumpEnabled() && capture_directory.empty()) {
		return;
	}
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	try {
#endif
		const char* stage = options.stage == ShaderType::Compute ? "compute"
		                    : options.stage == ShaderType::Vertex ? "vertex"
		                    : options.stage == ShaderType::Pixel ? "pixel" : "unknown";
		const auto state_hash = XXH3_64bits(static_state.data(), static_state.size_bytes());
		auto stem = fmt::format("{}_{:016x}_{:016x}", stage, options.shader_hash, state_hash);
		static std::atomic<uint32_t> runtime_sequence {0};
		if (runtime_capture) {
			const auto sequence = runtime_sequence.fetch_add(1);
			if (sequence >= 8u) return;
			stem += fmt::format("_runtime_{:04}", sequence);
		}
		const auto directory = capture_directory.empty() ? Config::GetShaderLogFolder() / "dispatched" : capture_directory;
		if (!Common::File::CreateDirectories(directory) && !Common::File::IsDirectoryExisting(directory)) {
			LOGF("Shader capture: cannot create directory %s\n", Common::PathToString(directory).c_str());
			return;
		}
		auto metadata = ShaderRecompiler::Capture::BuildCompileMetadata(
		    params, options, static_state, guest_workgroups);
		metadata["code_file"] = stem + ".bin";
		if (!options.back_code.empty()) metadata["back_code_file"] = stem + "_back.bin";
		if (runtime_capture) {
			metadata["runtime"] = *runtime_capture;
			metadata["runtime_resources_captured"] = runtime_capture->at("complete");
		}
		const auto json = metadata.dump(2) + '\n';
		const auto write = [&](const std::filesystem::path& path, const void* data, size_t size) {
			if (size > UINT32_MAX) {
				return false;
			}
			Common::File file(path);
			if (file.IsInvalid()) {
				return false;
			}
			uint32_t written = 0;
			file.Write(data, static_cast<uint32_t>(size), &written);
			return written == size && file.Flush();
		};
		// Write the JSON last so a new manifest never advertises an unfinished binary. This
		// capture precedes translation: even a fatal frontend error leaves a replayable record.
		if (!write(directory / (stem + ".bin"), params.code.data(), params.code.size_bytes()) ||
		    (!options.back_code.empty() &&
		     !write(directory / (stem + "_back.bin"), options.back_code.data(), options.back_code.size_bytes())) ||
		    !write(directory / (stem + ".json"), json.data(), json.size())) {
			LOGF("Shader capture: cannot write dispatched shader %s\n", stem.c_str());
		}
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
	} catch (const std::exception& error) {
		LOGF("Shader capture: cannot capture dispatched shader: %s\n", error.what());
	} catch (...) {
		LOGF("Shader capture: cannot capture dispatched shader: unknown exception\n");
	}
#endif
}

bool ShouldDumpShaderSpirv(uint64_t shader_hash) {
	if (Config::GraphicsDebugDumpEnabled() ||
	    std::getenv("KYTY_DUMP_SPIRV_BEFORE_VALIDATE") != nullptr) {
		return true;
	}
	const char* filter = std::getenv("KYTY_DUMP_SPIRV_HASH");
	if (filter == nullptr || *filter == '\0') {
		return false;
	}
	char*      end    = nullptr;
	const auto parsed = std::strtoull(filter, &end, 0);
	return end != filter && *end == '\0' && parsed == shader_hash;
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!ShouldDumpShaderSpirv(shader_hash)) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	if (std::getenv("KYTY_SPIRV_OPTIMIZATION_TRACE") != nullptr) {
		std::printf("DumpSpirvBegin: stage=%s hash=0x%016" PRIx64 " words=%zu path=%s\n",
		            stage_name, shader_hash, spirv.size(), Common::PathToString(path).c_str());
		std::fflush(stdout);
	}
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
	if (std::getenv("KYTY_SPIRV_OPTIMIZATION_TRACE") != nullptr) {
		std::printf("DumpSpirvEnd: stage=%s hash=0x%016" PRIx64 "\n", stage_name, shader_hash);
		std::fflush(stdout);
	}
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

std::vector<uint32_t> OptimizeShaderSpirv(
    const std::vector<uint32_t>& spirv, Config::ShaderOptimizationType optimization,
    bool cooperative_wave64 = false) {
	if (optimization == Config::ShaderOptimizationType::None || spirv.empty()) {
		return spirv;
	}

	spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
	std::string         messages;
	optimizer.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                         const spv_position_t& position,
	                                         const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column),
		                        static_cast<int>(position.index), message);
	});
	// The stock -O/-Os recipes include exhaustive inlining, scalar replacement and forced loop
	// unrolling. Generated dispatcher/cooperative modules can contain hundreds of thousands of
	// words, making those passes super-linear and stalling first launch for minutes. Cooperative
	// schedulers also make global redundancy analysis super-linear; keep their measured bounded
	// recipe to local simplification and block merging. Ordinary structured modules retain the
	// wider bounded recipe.
	if (cooperative_wave64) {
		optimizer.RegisterPass(spvtools::CreateSimplificationPass())
		    .RegisterPass(spvtools::CreateLocalRedundancyEliminationPass())
		    .RegisterPass(spvtools::CreateBlockMergePass())
		    .RegisterPass(spvtools::CreateCompactIdsPass());
	} else {
		optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass())
		    .RegisterPass(spvtools::CreateEliminateDeadFunctionsPass())
		    .RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass())
		    .RegisterPass(spvtools::CreateLocalSingleStoreElimPass())
		    .RegisterPass(spvtools::CreateAggressiveDCEPass(true))
		    .RegisterPass(spvtools::CreateSimplificationPass())
		    .RegisterPass(spvtools::CreateRedundancyEliminationPass())
		    .RegisterPass(spvtools::CreateBlockMergePass());
	}
	if (optimization == Config::ShaderOptimizationType::Size) {
		optimizer.RegisterPass(spvtools::CreateStripDebugInfoPass());
	}

	std::vector<uint32_t> optimized;
	if (!optimizer.Run(spirv.data(), spirv.size(), &optimized) || optimized.empty()) {
		LOGF("SPIR-V optimization failed; using generated module:\n%s", messages.c_str());
		return spirv;
	}
	return optimized;
}

bool ShouldOptimizeShaderSpirv(bool dispatcher_fallback, bool cooperative_wave64,
                               Config::ShaderOptimizationType optimization) {
	(void)cooperative_wave64;
	return optimization != Config::ShaderOptimizationType::None && !dispatcher_fallback;
}

std::string ShaderModuleDebugName(ShaderType stage, uint64_t shader_hash) {
	const char* stage_name = "unknown";
	switch (stage) {
		case ShaderType::Vertex: stage_name = "vs"; break;
		case ShaderType::Pixel: stage_name = "ps"; break;
		case ShaderType::Compute: stage_name = "cs"; break;
		default: break;
	}
	return fmt::format("kyty_shader_{}_{:016x}", stage_name, shader_hash);
}

} // namespace

// Test access to the exact production validation/configuration path. This does
// not create a Vulkan device or alter validation policy.
bool ValidateShaderSpirvForTest(const char* label, uint64_t shader_hash,
                               const std::vector<uint32_t>& spirv) {
	return ValidateShaderSpirv(label, shader_hash, spirv);
}

std::vector<uint32_t> OptimizeShaderSpirvForTest(
    const std::vector<uint32_t>& spirv, Config::ShaderOptimizationType optimization,
    bool cooperative_wave64) {
	return OptimizeShaderSpirv(spirv, optimization, cooperative_wave64);
}

bool ShouldOptimizeShaderSpirvForTest(bool dispatcher_fallback, bool cooperative_wave64,
                                      Config::ShaderOptimizationType optimization) {
	return ShouldOptimizeShaderSpirv(dispatcher_fallback, cooperative_wave64, optimization);
}

std::string ShaderModuleDebugNameForTest(ShaderType stage, uint64_t shader_hash) {
	return ShaderModuleDebugName(stage, shader_hash);
}

std::optional<uint64_t> FindReusableShaderProgramIdForTest(
    std::span<const uint64_t> existing_spirv_hashes, std::span<const uint64_t> existing_program_ids,
    uint64_t spirv_hash) {
	EXIT_IF(existing_spirv_hashes.size() != existing_program_ids.size());
	for (size_t index = 0; index < existing_spirv_hashes.size(); ++index) {
		if (existing_spirv_hashes[index] == spirv_hash) {
			return existing_program_ids[index];
		}
	}
	return std::nullopt;
}

static bool CanReuseCompiledSpecialization(
    const ShaderRecompiler::IR::ResourceSpecialization& prior,
    const ShaderRecompiler::IR::ResourceSpecialization& current,
    const ShaderRecompiler::IR::CompiledShaderInfo& compiled) {
	if (prior == current) return true;
	if (prior.bounded_srt_reads != current.bounded_srt_reads ||
	    prior.buffer_tables != current.buffer_tables || prior.buffer_origins != current.buffer_origins ||
	    prior.images != current.images || prior.sampler_origins != current.sampler_origins ||
	    prior.sampled_pairs != current.sampled_pairs ||
	    prior.sampler_depth_compare_funcs != current.sampler_depth_compare_funcs ||
	    prior.buffers.size() != current.buffers.size() ||
	    prior.buffers.size() != compiled.info.buffers.size()) return false;
	for (uint32_t i = 0; i < prior.buffers.size(); ++i) {
		if (prior.buffers[i] == current.buffers[i]) continue;
		const auto a = prior.buffers[i].packed_stride;
		const auto b = current.buffers[i].packed_stride;
		if ((a & 0x3fffu) == 0u || (b & 0x3fffu) == 0u ||
		    (a & ~0x3fffu) != (b & ~0x3fffu) ||
		    compiled.info.buffers[i].packed_stride != a ||
		    std::ranges::find(compiled.bindings.runtime_stride_resources, i) ==
		        compiled.bindings.runtime_stride_resources.end()) return false;
		auto normalized = current.buffers[i];
		normalized.packed_stride = a;
		if (normalized != prior.buffers[i]) return false;
	}
	return true;
}

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
		uint64_t                                     spirv_hash  = 0;
		bool                                         owns_module = true;
	};

	struct SourceEntry {
		explicit SourceEntry(ShaderRecompiler::IR::ResourcePlan plan)
		    : resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::vector<Permutation>                    permutations;
		bool                                        skip_dispatch = false;
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
			// exact state comparison needed on a stable hit without hashing up to 430 words first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords =
	    15 + ShaderVertexInputInfo::PARAM_LINK_MAX * 2 + ShaderVertexInputInfo::RES_MAX * 13;

	Permutation CompilePermutation(const ShaderParams&                  params,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword,
	                               const std::vector<Permutation>& siblings) {
		const char* stage_name = nullptr;
		switch (options.stage) {
			case ShaderType::Vertex: stage_name = "vs"; break;
			case ShaderType::Mesh: stage_name = "ms"; break;
			case ShaderType::Local: stage_name = "ls"; break;
			case ShaderType::TessellationControl: stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: stage_name = "ds"; break;
			case ShaderType::Pixel: stage_name = "ps"; break;
			case ShaderType::Compute: stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		const char* optimization_trace = std::getenv("KYTY_SPIRV_OPTIMIZATION_TRACE");
		uint32_t    wave_partition_factor = 1;
		bool        cooperative_wave64    = false;
		if (options.stage == ShaderType::Compute) {
			if (optimization_trace != nullptr && *optimization_trace != '\0') {
				std::printf("ComputePlanBegin: hash=0x%016" PRIx64 "\n", options.shader_hash);
				std::fflush(stdout);
			}
			// The renderer no longer owns the CFG after TakeCompiledInfo. Preserve
			// the exact dispatch geometry and generated-control-flow classification
			// selected by the same compiler planner.
			const auto execution = ShaderRecompiler::PlanComputeExecution(
			    result.program, options.input_info, options.compute_workgroup_limits);
			if (optimization_trace != nullptr && *optimization_trace != '\0') {
				std::printf("ComputePlanEnd: hash=0x%016" PRIx64 " error=%s\n", options.shader_hash,
				            execution.error.c_str());
				std::fflush(stdout);
			}
			if (!execution.error.empty()) {
				EXIT("compute execution plan failed: %s\n", execution.error.c_str());
			}
			wave_partition_factor = execution.wave_partition_factor;
			cooperative_wave64    = execution.IsCooperativeWave64();
		}
		const auto  optimization_start = std::chrono::steady_clock::now();
		const auto  original_words     = result.spirv.size();
		if (optimization_trace != nullptr && *optimization_trace != '\0') {
			std::printf("SpirvOptimizeBegin: stage=%s hash=0x%016" PRIx64 " words=%zu mode=%u\n",
			            stage_name, options.shader_hash, original_words,
			            static_cast<uint32_t>(Config::GetShaderOptimizationType()));
			std::fflush(stdout);
		}
		if (ShouldOptimizeShaderSpirv(result.program.dispatcher_fallback, cooperative_wave64,
		                              Config::GetShaderOptimizationType())) {
			result.spirv = OptimizeShaderSpirv(result.spirv, Config::GetShaderOptimizationType(),
			                                  cooperative_wave64);
		}
		if (optimization_trace != nullptr && *optimization_trace != '\0') {
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			                         std::chrono::steady_clock::now() - optimization_start)
			                         .count();
			std::printf("SpirvOptimizeEnd: stage=%s hash=0x%016" PRIx64
			            " words=%zu->%zu elapsed_ms=%" PRId64 "\n",
			            stage_name, options.shader_hash, original_words, result.spirv.size(), elapsed);
			std::fflush(stdout);
		}
		if (ShouldDumpShaderSpirv(options.shader_hash)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
		}
		if (optimization_trace != nullptr && *optimization_trace != '\0') {
			std::printf("CompilePermutationPostPlan: hash=0x%016" PRIx64 "\n", options.shader_hash);
			std::fflush(stdout);
		}
		DumpShaderOriginal(stage_name, options.shader_hash, params.code, result.decoded_dump);
		if (optimization_trace != nullptr && *optimization_trace != '\0') {
			std::printf("CompilePermutationPostOriginal: hash=0x%016" PRIx64 "\n", options.shader_hash);
			std::fflush(stdout);
		}
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		if (optimization_trace != nullptr && *optimization_trace != '\0') {
			std::printf("CompilePermutationPostValidation: hash=0x%016" PRIx64 "\n",
			            options.shader_hash);
			std::fflush(stdout);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);

		const uint64_t spirv_hash =
		    XXH3_64bits(result.spirv.data(), result.spirv.size() * sizeof(uint32_t));
		std::vector<uint64_t> sibling_hashes;
		std::vector<uint64_t> sibling_ids;
		sibling_hashes.reserve(siblings.size());
		sibling_ids.reserve(siblings.size());
		for (const auto& sibling: siblings) {
			sibling_hashes.push_back(sibling.spirv_hash);
			sibling_ids.push_back(sibling.handle.id);
		}
		const auto reused_id =
		    FindReusableShaderProgramIdForTest(sibling_hashes, sibling_ids, spirv_hash);
		ShaderProgram handle {};
		bool          owns_module = true;
		if (reused_id.has_value()) {
			const auto sibling = std::ranges::find_if(
			    siblings, [&](const Permutation& candidate) {
				    return candidate.handle.id == *reused_id;
			    });
			EXIT_IF(sibling == siblings.end());
			handle      = sibling->handle;
			owns_module = false;
			LOGF(
			    "SpirvReuse: stage=%s hash=0x%016" PRIx64
			    " words=%zu spirv_hash=0x%016" PRIx64 " program_id=%" PRIu64 " prior_perms=%zu\n",
			    stage_name, options.shader_hash, result.spirv.size(), spirv_hash, handle.id,
			    siblings.size());
		} else {
			handle.module = CompileSPV(result.spirv, device);
			EXIT_IF(handle.module == nullptr);
			handle.id = ++next_shader_id;
			SetVulkanObjectNameF(device, handle.module, "{}",
			                     ShaderModuleDebugName(options.stage, options.shader_hash));
		}
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		auto compiled_info = std::move(result.program).TakeCompiledInfo();
		compiled_info.compute_wave_partition_factor = wave_partition_factor;
		compiled_info.compute_cooperative_wave64 = cooperative_wave64;
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(compiled_info),
		    .handle         = handle,
		    .spirv_hash     = spirv_hash,
		    .owns_module    = owns_module,
		};
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor,
 std::optional<std::array<uint32_t, 3>> guest_workgroups = std::nullopt,
 bool compute_workgroups_trusted = true, uint64_t indirect_args_addr = 0,
 bool neighboring_stages_read_only = false) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto                                         entry = programs.find(lookup_key);
		if (entry == programs.end()) {
			for (const auto& [existing_key, existing_source]: programs) {
				if (existing_key.hash != params.hash || existing_key.stage != stage) {
					continue;
				}
				LOGF("ProgramKeySplit: stage=%u hash=0x%016" PRIx64
				     " user_data=%u/%u code_size=%u/%u static_words=%zu/%zu prior_perms=%zu\n",
				     static_cast<unsigned>(stage), params.hash, existing_key.user_data_count,
				     lookup_key.user_data_count, existing_key.code_size, lookup_key.code_size,
				     existing_key.static_state.size(), lookup_key.static_state.size(),
				     existing_source.permutations.size());
				static constexpr const char* kComputeStaticNames[] = {
				    "fp_state",           "workgroup_register", "wave_size",
				    "host_subgroup_size", "thread_ids_num",     "lds_size_dwords",
				    "scratch_size_dwords","dispatch_thread_dimensions",
				    "threads_num_x",      "group_id_x",         "threads_num_y",
				    "group_id_y",         "threads_num_z",      "group_id_z",
				    "tg_size_en",
				};
				const auto words =
				    std::min(existing_key.static_state.size(), lookup_key.static_state.size());
				for (size_t i = 0; i < words; ++i) {
					if (existing_key.static_state[i] == lookup_key.static_state[i]) {
						continue;
					}
					const char* name = (stage == ShaderType::Compute &&
					                    i < std::size(kComputeStaticNames))
					                       ? kComputeStaticNames[i]
					                       : "word";
					LOGF("ProgramKeySplitDiff: hash=0x%016" PRIx64
					     " %s[%zu]=0x%08" PRIx32 "->0x%08" PRIx32 "\n",
					     params.hash, name, i, existing_key.static_state[i],
					     lookup_key.static_state[i]);
				}
				if (existing_key.static_state.size() != lookup_key.static_state.size()) {
					LOGF("ProgramKeySplitDiff: hash=0x%016" PRIx64
					     " static_state size %zu->%zu\n",
					     params.hash, existing_key.static_state.size(),
					     lookup_key.static_state.size());
				}
				break;
			}
		}
		if (entry != programs.end() && entry->second.skip_dispatch) {
			return {};
		}
		ShaderRecompiler::IR::SrtRuntime       runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_memory                = ReadShaderBacking,
		    .read_specialization_memory = ReadShaderGuestMemory,
		    .compute_workgroups         = guest_workgroups,
		    .compute_workgroups_trusted = compute_workgroups_trusted,
		    .clamp_memory_range         = ClampShaderGuestMemory,
		    .max_dense_buffers          = dense_buffer_capacity,
		    .max_native_samplers        = native_sampler_capacity,
		    .max_dense_images           = dense_image_capacity,
		    .capture_scalar_selector_values = stage == ShaderType::Compute,
		    .neighboring_stages_read_only = stage == ShaderType::Compute || neighboring_stages_read_only,
		};
		const auto refresh_indirect_grid = [&](const ShaderRecompiler::IR::ResourcePlan& plan) {
			if (runtime.compute_workgroups_trusted || indirect_args_addr == 0 ||
			    !std::ranges::any_of(plan.bounded_srt_reads, [](const auto& read) {
				    return read.workgroup_axis != UINT32_MAX;
			    })) {
				return;
			}
			std::array<uint32_t, 3> groups {};
			if (Libs::LibKernel::Memory::TryReadGpuCoherentBacking(
			        indirect_args_addr, groups.data(), sizeof(groups))) {
				runtime.compute_workgroups = groups;
				runtime.compute_workgroups_trusted = true;
			}
		};
		const auto materialize = [&](const auto& plan, auto& resources, auto& specialization) {
			const auto* directory = std::getenv("KYTY_RESOURCE_CAPTURE_DIR");
			const auto* filter = std::getenv("KYTY_RESOURCE_CAPTURE_HASH");
			char* end = nullptr;
			const auto capture_hash = filter ? std::strtoull(filter, &end, 16) : 0;
			const bool selected = directory && *directory && filter && *filter && end &&
			                      *end == '\0' && capture_hash == params.hash;
			if (!selected)
				return ShaderRecompiler::IR::MaterializeResources(plan, runtime, resources, specialization);
			ShaderRecompiler::Capture::ResourceReadCapture capture(runtime);
			const auto observed = capture.Runtime();
			const bool ok = ShaderRecompiler::IR::MaterializeResources(plan, observed, resources, specialization);
			// Save before the caller's fatal error. No extra guest reads are made.
			if (!ok || std::getenv("KYTY_RESOURCE_CAPTURE_SUCCESS")) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
				try {
#endif
					ShaderRecompiler::CompileOptions captured;
					captured.stage = stage;
					captured.shader_hash = params.hash;
					captured.user_data = user_data;
					captured.back_code = params.back_code;
					captured.host_profile = host_profile;
					captured.compute_workgroup_limits = compute_workgroup_limits;
					captured.wave_size = input_info.wave_size;
					if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
						captured.input_info.vertex = &input_info;
						captured.user_data_base = stage == ShaderType::Mesh || stage == ShaderType::TessellationControl ? 0u : 8u;
						if (stage == ShaderType::Mesh) captured.wave_size = input_info.mesh.wave_size;
						if (stage == ShaderType::TessellationControl) captured.wave_size = 64u;
					} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
						captured.input_info.pixel = &input_info;
					} else captured.input_info.compute = &input_info;
					auto reads = ShaderRecompiler::Capture::BuildResourceReadMetadata(
					    runtime, capture, ok, ok ? std::string_view{} : ShaderRecompiler::IR::LastResourceSpecializationError());
					if (const auto* index = std::getenv("KYTY_RESOURCE_CAPTURE_TEXTURE_IMAGE")) {
						char* parsed_end = nullptr;
						const auto logical = std::strtoul(index, &parsed_end, 10);
						if (*index && parsed_end && !*parsed_end && logical <= UINT32_MAX)
							reads["additional_texture_inputs"] = ShaderRecompiler::Capture::CaptureTextureInputs(
							    plan, runtime, static_cast<uint32_t>(logical));
					}
					CaptureDispatchedShader(params, captured, lookup_key.static_state, runtime.compute_workgroups,
					                        &reads, std::filesystem::u8path(directory));
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
				} catch (const std::exception& error) {
					LOGF("Resource capture failed: %s\n", error.what());
				}
#endif
			}
			return ok;
		};
		if (entry != programs.end()) {
			refresh_indirect_grid(entry->second.resource_plan);
			if (!materialize(
			        entry->second.resource_plan, entry->second.resources,
			        entry->second.specialization)) {
				const auto reason = ShaderRecompiler::IR::LastResourceSpecializationError();
				EXIT("MaterializeResources failed for stage=%u hash=0x%016" PRIx64 ": %.*s\n",
				     static_cast<unsigned>(stage), params.hash, static_cast<int>(reason.size()),
				     reason.data());
			}
			if (const auto permutation = std::ranges::find_if(
			        entry->second.permutations, [&](const Permutation& candidate) {
				        const auto& layout = candidate.program.bindings;
				        return layout.push_data_start_dword ==
				                   ShaderRecompiler::IR::PushData::StartFor(
				                       push_data_cursor, layout.ShaderDataDwords()) &&
				               CanReuseCompiledSpecialization(candidate.specialization,
				                                              entry->second.specialization, candidate.program);
			        });
			    permutation != entry->second.permutations.end()) {
				input_info.stage = {.program   = &permutation->program,
				                    .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
			const auto& specialization = entry->second.specialization;
			LOGF("SpecializationMiss: stage=%u hash=0x%016" PRIx64
			     " buffers=%zu images=%zu sampled_pairs=%zu bounded_srt=%zu buffer_tables=%zu "
			     "prior_perms=%zu\n",
			     static_cast<unsigned>(stage), params.hash, specialization.buffers.size(),
			     specialization.images.size(), specialization.sampled_pairs.size(),
			     specialization.bounded_srt_reads.size(), specialization.buffer_tables.size(),
			     entry->second.permutations.size());
			if (!entry->second.permutations.empty()) {
				const auto& prior = entry->second.permutations.back().specialization;
				if (prior.buffers.size() != specialization.buffers.size()) {
					LOGF("SpecializationMissDiff: hash=0x%016" PRIx64 " buffers %zu->%zu\n",
					     params.hash, prior.buffers.size(), specialization.buffers.size());
				}
				if (prior.images.size() != specialization.images.size()) {
					LOGF("SpecializationMissDiff: hash=0x%016" PRIx64 " images %zu->%zu\n",
					     params.hash, prior.images.size(), specialization.images.size());
				}
				if (prior.sampled_pairs.size() != specialization.sampled_pairs.size()) {
					LOGF("SpecializationMissDiff: hash=0x%016" PRIx64 " sampled_pairs %zu->%zu\n",
					     params.hash, prior.sampled_pairs.size(),
					     specialization.sampled_pairs.size());
				}
				if (prior.bounded_srt_reads != specialization.bounded_srt_reads) {
					LOGF("SpecializationMissDiff: hash=0x%016" PRIx64
					     " bounded_srt layouts changed (prior=%zu now=%zu)\n",
					     params.hash, prior.bounded_srt_reads.size(),
					     specialization.bounded_srt_reads.size());
					const auto n = std::min(prior.bounded_srt_reads.size(),
					                        specialization.bounded_srt_reads.size());
					for (size_t i = 0; i < n; ++i) {
						if (prior.bounded_srt_reads[i] == specialization.bounded_srt_reads[i]) {
							continue;
						}
						LOGF("SpecializationMissDiff: hash=0x%016" PRIx64
						     " bounded_srt[%zu] count=%u->%u flat=%u->%u\n",
						     params.hash, i, prior.bounded_srt_reads[i].count,
						     specialization.bounded_srt_reads[i].count,
						     prior.bounded_srt_reads[i].flat_offset,
						     specialization.bounded_srt_reads[i].flat_offset);
					}
				}
				if (prior.buffer_tables != specialization.buffer_tables) {
					LOGF("SpecializationMissDiff: hash=0x%016" PRIx64
					     " buffer_tables changed (prior=%zu now=%zu)\n",
					     params.hash, prior.buffer_tables.size(),
					     specialization.buffer_tables.size());
					const auto n =
					    std::min(prior.buffer_tables.size(), specialization.buffer_tables.size());
					for (size_t i = 0; i < n; ++i) {
						const auto& a = prior.buffer_tables[i];
						const auto& b = specialization.buffer_tables[i];
						if (a == b) {
							continue;
						}
						LOGF("SpecializationMissDiff: hash=0x%016" PRIx64
						     " buffer_table[%zu] count=%u->%u resources=%zu->%zu\n",
						     params.hash, i, a.count, b.count, a.resources.size(),
						     b.resources.size());
					}
				}
				for (size_t i = 0; i < std::min(prior.images.size(), specialization.images.size());
				     ++i) {
					if (prior.images[i].indirect_search_iterations ==
					    specialization.images[i].indirect_search_iterations) {
						continue;
					}
					LOGF("SpecializationMissDiff: hash=0x%016" PRIx64
					     " image[%zu] indirect_search_iterations=%u->%u\n",
					     params.hash, i, prior.images[i].indirect_search_iterations,
					     specialization.images[i].indirect_search_iterations);
				}
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
		const char* stage_name = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; stage_name = "vs"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; stage_name = "ms"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; stage_name = "ls"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; stage_name = "ds"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; stage_name = "ps"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;
		options.host_profile = host_profile;
		options.compute_workgroup_limits = compute_workgroup_limits;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		CaptureDispatchedShader(params, options, lookup_key.static_state, guest_workgroups);
		auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
		if (translated.skip_dispatch) {
			entry = programs.try_emplace(lookup_key, ShaderRecompiler::IR::ResourcePlan {}).first;
			entry->second.skip_dispatch = true;
			return {};
		}
		if (entry == programs.end()) {
			entry = programs.try_emplace(lookup_key,
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program)).first;
			refresh_indirect_grid(entry->second.resource_plan);
			if (!materialize(
			        entry->second.resource_plan, entry->second.resources,
			        entry->second.specialization)) {
				const auto reason = ShaderRecompiler::IR::LastResourceSpecializationError();
				EXIT("MaterializeResources failed for stage=%u hash=0x%016" PRIx64 ": %.*s\n",
				     static_cast<unsigned>(stage), params.hash, static_cast<int>(reason.size()),
				     reason.data());
			}
			const auto& specialization = entry->second.specialization;
			LOGF("SpecializationCompile: stage=%u hash=0x%016" PRIx64
			     " buffers=%zu images=%zu sampled_pairs=%zu bounded_srt=%zu buffer_tables=%zu\n",
			     static_cast<unsigned>(stage), params.hash, specialization.buffers.size(),
			     specialization.images.size(), specialization.sampled_pairs.size(),
			     specialization.bounded_srt_reads.size(), specialization.buffer_tables.size());
			for (size_t i = 0; i < specialization.bounded_srt_reads.size(); ++i) {
				LOGF("SpecializationCompile: hash=0x%016" PRIx64
				     " bounded_srt[%zu] count=%u flat=%u\n",
				     params.hash, i, specialization.bounded_srt_reads[i].count,
				     specialization.bounded_srt_reads[i].flat_offset);
			}
			for (size_t i = 0; i < specialization.images.size(); ++i) {
				if (specialization.images[i].indirect_search_iterations == 0u) {
					continue;
				}
				LOGF("SpecializationCompile: hash=0x%016" PRIx64
				     " image[%zu] indirect_search_iterations=%u\n",
				     params.hash, i, specialization.images[i].indirect_search_iterations);
			}
		}
		entry->second.permutations.push_back(CompilePermutation(
		    params, options, std::move(translated), entry->second.specialization, push_data_cursor,
		    entry->second.permutations));
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		LOGF("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		     counts[static_cast<size_t>(ShaderType::Vertex)],
		     counts[static_cast<size_t>(ShaderType::Pixel)],
		     counts[static_cast<size_t>(ShaderType::Compute)],
		     counts[static_cast<size_t>(ShaderType::Mesh)],
		     counts[static_cast<size_t>(ShaderType::Local)],
		     counts[static_cast<size_t>(ShaderType::TessellationControl)],
		     counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return permutation.handle;
	}

	explicit ProgramCache(const GraphicContext& graphics): device(graphics.device) {
		host_profile = graphics.shader_host_profile;
		const auto& limits                       = graphics.GetPhysicalDeviceProperties().limits;
		dense_buffer_capacity = StorageBufferDescriptorCeiling(limits);
		native_sampler_capacity = SamplerDescriptorCeiling(limits);
		dense_image_capacity = ImageDescriptorCeiling(limits);
		compute_workgroup_limits.max_size        = {limits.maxComputeWorkGroupSize[0],
		                                            limits.maxComputeWorkGroupSize[1],
		                                            limits.maxComputeWorkGroupSize[2]};
		compute_workgroup_limits.max_invocations = limits.maxComputeWorkGroupInvocations;
		compute_workgroup_limits.max_shared_memory_bytes = limits.maxComputeSharedMemorySize;
		compute_workgroup_limits.native_subgroup_size = graphics.subgroup_size;
		compute_workgroup_limits.can_require_subgroup_size_64 =
		    graphics.compute_subgroup_size_control_enabled &&
		    graphics.min_subgroup_size <= 64u && graphics.max_subgroup_size >= 64u;
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				if (permutation.owns_module) {
					device.destroyShaderModule(permutation.handle.module, nullptr);
				}
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	ShaderRecompiler::ShaderHostProfile host_profile;
	ShaderRecompiler::ComputeWorkgroupLimits compute_workgroup_limits;
	uint32_t dense_buffer_capacity = 0;
	uint32_t native_sampler_capacity = 0;
	uint32_t dense_image_capacity = 0;
	ProgramKey                                                  lookup_key;
	vk::Device                                                  device;
	uint64_t                                                    next_shader_id = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(graphics)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
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

bool IsDriverCacheBuildIdentityUsableForTest(std::string_view git_hash,
                                             std::string_view git_revision,
                                             std::string_view worktree_fingerprint) {
	return IsDriverCacheBuildIdentityUsable(git_hash, git_revision, worktree_fingerprint);
}

bool IsDriverCacheSignatureCompatibleForTest(std::string_view cached_signature,
                                             std::string_view expected_signature) {
	return IsDriverCacheSignatureCompatible(cached_signature, expected_signature);
}

std::string DriverCacheFileNameForTest(std::string_view title_id,
                                       bool gpu_assisted_validation) {
	return DriverCacheFileName(title_id, gpu_assisted_validation, gpu_assisted_validation);
}

std::string DriverCacheFileNameForTest(std::string_view title_id,
                                       bool gpu_assisted_validation,
                                       bool shader_instrumentation) {
	return DriverCacheFileName(title_id, gpu_assisted_validation, shader_instrumentation);
}

std::string FormatDriverCacheSignatureForTest(
    std::string_view git_revision, std::string_view worktree_fingerprint, uint32_t vendor_id,
    uint32_t device_id, uint32_t driver_version, std::string_view pipeline_cache_uuid_hex,
    bool gpu_assisted_validation) {
	return FormatDriverCacheSignature(git_revision, worktree_fingerprint, vendor_id, device_id,
	                                  driver_version, pipeline_cache_uuid_hex,
	                                  gpu_assisted_validation, gpu_assisted_validation);
}

std::string FormatDriverCacheSignatureForTest(
    std::string_view git_revision, std::string_view worktree_fingerprint, uint32_t vendor_id,
    uint32_t device_id, uint32_t driver_version, std::string_view pipeline_cache_uuid_hex,
    bool gpu_assisted_validation, bool shader_instrumentation) {
	return FormatDriverCacheSignature(git_revision, worktree_fingerprint, vendor_id, device_id,
	                                  driver_version, pipeline_cache_uuid_hex,
	                                  gpu_assisted_validation, shader_instrumentation);
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
	const std::string_view worktree_fingerprint = KYTY_GIT_WORKTREE_FINGERPRINT;
	if (!IsDriverCacheBuildIdentityUsable(git_hash, git_revision, worktree_fingerprint)) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (incomplete build identity)");
		return;
	}

	m_driver_cache_path =
	    std::filesystem::path("_PipelineCache") /
	    DriverCacheFileName(title_id, Config::GpuAssistedValidationEnabled(),
	                        DriverCacheShaderInstrumentationEnabled());
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
			    payload_read != initial_data.size() ||
			    !IsDriverCacheSignatureCompatible(cached_signature, signature) ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, format, or data mismatch)",
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
		m_saved_driver_cache_hash     = XXH3_64bits(initial_data.data(), initial_data.size());
		m_has_saved_driver_cache_hash = true;
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::Save() {
	Common::LockGuard lock(m_mutex);
	if (SaveDriverCacheLocked(false)) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
		m_driver_cache = nullptr;
	}
}

bool PipelineCache::SaveDriverCacheLocked(bool checkpoint) {
	if (m_driver_cache == nullptr) {
		return false;
	}

	const auto save_begin = std::chrono::steady_clock::now();
	const bool trace_save = std::getenv("KYTY_PIPELINE_CACHE_PHASE_TRACE") != nullptr;
	const auto trace = [&](const char* phase, size_t bytes = 0) {
		if (!trace_save) return;
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now() - save_begin).count();
		std::printf("DriverCacheSavePhase: checkpoint=%u phase=%s bytes=%zu elapsed_ms=%" PRId64 "\n",
		            checkpoint ? 1u : 0u, phase, bytes, elapsed);
		std::fflush(stdout);
	};
	trace("begin");
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
		return false;
	}
	payload.resize(size);
	trace("driver-data", payload.size());
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	trace("hash", payload.size());
	if (m_has_saved_driver_cache_hash && payload_hash == m_saved_driver_cache_hash &&
	    Common::File::IsFileExisting(m_driver_cache_path)) {
		if (!checkpoint) {
			PipelineCacheLog("Vulkan pipeline cache: unchanged {} bytes in {}", payload.size(),
			                 Common::PathToString(m_driver_cache_path));
		}
		return true;
	}
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return false;
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
	trace("write", payload.size());
	const bool flushed = !file.IsInvalid() && file.Flush();
	trace("flush", payload.size());
	file.Close();
	const bool complete = prefix_written == prefix.size() && payload_written == payload.size() && flushed;
	const bool renamed = complete && Common::File::RenameFile(temp_path, m_driver_cache_path);
	trace("rename", payload.size());
	if (!renamed) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return false;
	}
	PipelineCacheLog("Vulkan pipeline cache: {} {} bytes to {}",
	                 checkpoint ? "checkpointed" : "saved", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	m_saved_driver_cache_hash     = payload_hash;
	m_has_saved_driver_cache_hash = true;
	return true;
}

bool DriverCacheCheckpointDue(uint32_t pending_pipelines, uint64_t creation_ms) {
	// Keep ordinary creations batched, but preserve expensive compiler work
	// before another pipeline can stall or a bounded run can end.
	return pending_pipelines >= DriverCacheCheckpointInterval ||
	       (pending_pipelines != 0 && creation_ms >= DriverCacheExpensiveCreationMs);
}

bool DriverCacheCheckpointDue(uint32_t pending_pipelines, uint64_t creation_ms,
                              uint64_t previous_save_ms, uint64_t elapsed_ms) {
	if (!DriverCacheCheckpointDue(pending_pipelines, creation_ms)) return false;
	// Intermediate persistence should consume at most one tenth of the command
	// lane. A new cache has no cost sample; genuinely expensive compilation can
	// still be protected when its value pays for the measured checkpoint cost.
	// Divide instead of multiplying durations so large clock values cannot wrap.
	return previous_save_ms == 0 || elapsed_ms / previous_save_ms >= 9u ||
	       creation_ms / previous_save_ms >= 10u;
}

void PipelineCache::CheckpointDriverCacheLocked(uint64_t creation_ms) {
	if (m_driver_cache == nullptr) {
		return;
	}
	if (m_new_driver_pipelines != UINT32_MAX) ++m_new_driver_pipelines;
	const auto begin = std::chrono::steady_clock::now();
	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    begin - m_checkpoint_finished).count();
	if (!DriverCacheCheckpointDue(m_new_driver_pipelines, creation_ms,
	                               m_previous_checkpoint_ms,
	                               static_cast<uint64_t>(elapsed_ms))) {
		return;
	}
	// Keep dirty work eligible after a failed write; a slow failure also needs
	// the same budget, rather than retrying expensive I/O on every creation.
	if (SaveDriverCacheLocked(true)) m_new_driver_pipelines = 0;
	m_checkpoint_finished = std::chrono::steady_clock::now();
	const auto cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    m_checkpoint_finished - begin).count();
	m_previous_checkpoint_ms = static_cast<uint64_t>(std::max<int64_t>(cost_ms, 1));
}

std::string ConfigurePixelTargetConversions(const HW::Context& context,
                                            ShaderPixelInputInfo& pixel_info) {
	std::array<Prospero::BufferFormat, 8> formats {};
	for (uint32_t slot = 0; slot < formats.size(); ++slot) {
		const auto& target = context.GetRenderTarget(slot);
		if (target.base.addr == 0 || pixel_info.target_output_mode[slot] == 0) continue;
		const auto logical_mask = ((context.GetRenderTargetMask() & context.GetShaderRegisters().m_cbShaderMask)
		                           >> (slot * 4u)) & 0xfu;
		if (logical_mask == 0) continue;
		const auto encoding = Prospero::ResolveRenderTargetFormat(target.info.format, target.info.channel_type);
		if (encoding.buffer_format != Prospero::BufferFormat::k11_11_10UNorm &&
		    encoding.buffer_format != Prospero::BufferFormat::k10_11_11UNorm) continue;
		if (context.GetBlendControl(slot).enable && !target.info.blend_bypass)
			return "packed normalized color blending is unsupported";
		if (target.info.round_mode)
			return "packed normalized color truncation is unsupported";
		if (target.attrib.num_samples != 0 || target.attrib.num_fragments != 0 || pixel_info.dual_source_blending)
			return "packed normalized multisample or dual-source color exports are unsupported";
		if (pixel_info.target_output_mode[slot] != 9u)
			return "packed normalized color export requires FP32 source components";
		const auto target_format = TextureGetRenderTargetFormat(target.info.format,
		    target.info.channel_type, target.info.channel_order);
		if ((target_format.export_mapping.ApplyMask(logical_mask) & 7u) != 7u)
			return "partial packed normalized color write masks are unsupported";
		formats[slot] = target_format.conversion_format;
	}
	pixel_info.target_conversion_format = formats;
	return {};
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
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
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		pixel_info.alpha_blend_source = ShaderAlphaBlendSource::None;
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; })) {
			switch (ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0])) {
				case BlendMappingSupport::SourceAlpha:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlpha;
					break;
				case BlendMappingSupport::SourceAlphaOne:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
					break;
				case BlendMappingSupport::SourceAlphaZero:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaZero;
					break;
				default: break;
			}
			if (pixel_info.alpha_blend_source != ShaderAlphaBlendSource::None) {
				pixel_info.dual_source_blending     = true;
				pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
				pixel_info.target_export_mapping[1] = {};
			}
		}
	}
	if (pixel_active) {
		const auto conversion_error = ConfigurePixelTargetConversions(context, pixel_info);
		if (!conversion_error.empty()) EXIT("%s\n", conversion_error.c_str());
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info[tess_active ? 2u : 0u].clip_space;
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
	std::array<bool, 3> vertex_read_only{};
	const uint32_t vertex_count = tess_active ? 3u : 1u;
	for (uint32_t i = 0; i < vertex_count; ++i) {
		vertex_read_only[i] = ShaderRecompiler::Decoder::IsExternalMemoryReadOnly(vertex_params[i].code) &&
		    (vertex_params[i].back_code.empty() ||
		     ShaderRecompiler::Decoder::IsExternalMemoryReadOnly(vertex_params[i].back_code));
	}
	vertex_info[tess_active ? 2u : 0u].linked_param_count = 0;
	if (pixel_active) {
		const bool producers_read_only = std::all_of(vertex_read_only.begin(),
		    vertex_read_only.begin() + vertex_count, [](bool value) { return value; });
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor,
		    std::nullopt, true, 0u, producers_read_only);
		std::vector<uint32_t> active_inputs;
		for (const auto& input: pixel_info.stage.program->info.inputs) {
			if (input.kind == ShaderRecompiler::IR::StageInputKind::Parameter) {
				active_inputs.push_back(input.location);
			}
		}
		for (const auto input: active_inputs) {
			const auto source = ShaderPixelParameterMappedLocation(pixel_info, input);
			const auto location = ShaderPixelParameterLocation(pixel_info, active_inputs, input);
			EXIT_IF(source >= 32u || location >= 32u);
			bool duplicate = false;
			for (uint32_t i = 0; i < vertex_info[tess_active ? 2u : 0u].linked_param_count; ++i) {
				if (vertex_info[tess_active ? 2u : 0u].linked_param_sources[i] == source &&
				    vertex_info[tess_active ? 2u : 0u].linked_param_locations[i] == location) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate) {
				EXIT_IF(vertex_info[tess_active ? 2u : 0u].linked_param_count >= ShaderVertexInputInfo::PARAM_LINK_MAX);
				const auto link = vertex_info[tess_active ? 2u : 0u].linked_param_count++;
				vertex_info[tess_active ? 2u : 0u].linked_param_sources[link]   = source;
				vertex_info[tess_active ? 2u : 0u].linked_param_locations[link] = location;
			}
		}
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		bool neighbors_read_only = !pixel_active ||
		    (!pixel_info.stage.program->info.writes_dma &&
		     !std::ranges::any_of(pixel_info.stage.program->info.buffers, &ShaderRecompiler::IR::BufferResource::written) &&
		     !std::ranges::any_of(pixel_info.stage.program->info.images, &ShaderRecompiler::IR::ImageResource::written));
		for (uint32_t other = 0; other < vertex_count; ++other)
			if (other != i) neighbors_read_only &= vertex_read_only[other];
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor,
		    std::nullopt, true, 0u, neighbors_read_only);
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info,
 std::optional<std::array<uint32_t, 3>> guest_workgroups,
 bool compute_workgroups_trusted, uint64_t indirect_args_addr) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor, guest_workgroups,
	                           compute_workgroups_trusted, indirect_args_addr);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
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
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		auto alpha_source = ShaderAlphaBlendSource::None;
		if (slot == 0 && ps_input_info != nullptr) {
			alpha_source = ps_input_info->alpha_blend_source;
		}
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && alpha_source == ShaderAlphaBlendSource::None &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			EXIT("unsupported color blend mapping: slot=%u mapping=0x%02x color=%u/%u alpha=%u/%u separate=%u\n",
			     slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
			     bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1u : 0u);
		}
		if (static_params.blend_enable[slot]) {
			auto blend = bc;
			switch (alpha_source) {
				case ShaderAlphaBlendSource::SourceAlpha:
					blend.color_srcblend  = RemapSourceAlphaFactor(blend.color_srcblend);
					blend.color_destblend = RemapSourceAlphaFactor(blend.color_destblend);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::SourceAlphaOne:
				case ShaderAlphaBlendSource::SourceAlphaZero:
					// The second source carries the mapped source factor; its alpha stays logical Sa.
					blend.color_srcblend = static_cast<uint8_t>(Prospero::BlendFactor::kSrc1Color);
					blend.color_destblend =
					    static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrc1Alpha);
					blend.separate_alpha_blend = false;
					break;
				case ShaderAlphaBlendSource::None: break;
			}
			static_params.color_srcblend[slot]       = blend.color_srcblend;
			static_params.color_comb_fcn[slot]       = blend.color_comb_fcn;
			static_params.color_destblend[slot]      = blend.color_destblend;
			static_params.separate_alpha_blend[slot] = blend.separate_alpha_blend;
			if (blend.separate_alpha_blend) {
				static_params.alpha_srcblend[slot]  = blend.alpha_srcblend;
				static_params.alpha_comb_fcn[slot]  = blend.alpha_comb_fcn;
				static_params.alpha_destblend[slot] = blend.alpha_destblend;
			}
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
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
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
	const bool rect_list = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

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
	const bool trace_pipeline = std::getenv("KYTY_PIPELINE_PHASE_TRACE") != nullptr;
	if (trace_pipeline) {
		std::printf("PipelineCreationPhase: kind=graphics phase=begin vs=%" PRIu64 " ps=%" PRIu64
		            " vs_hash=0x%016" PRIx64 " ps_hash=0x%016" PRIx64 "\n", vs_id, ps_id,
		            vertex_info[0].stage.program ? vertex_info[0].stage.program->shader_hash : 0,
		            ps_active && ps_input_info->stage.program ? ps_input_info->stage.program->shader_hash : 0);
		std::fflush(stdout);
	}
	const auto creation_begin = std::chrono::steady_clock::now();
	CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vertex_info,
	                       ps_input_info, programs, static_params, m_driver_cache);
	const auto creation_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - creation_begin).count();
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);
	if (trace_pipeline) {
		std::printf("PipelineCreationPhase: kind=graphics phase=done vs=%" PRIu64 " ps=%" PRIu64 " elapsed_ms=%" PRId64 "\n", vs_id, ps_id, creation_ms);
		std::fflush(stdout);
	}

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);
	CheckpointDriverCacheLocked(static_cast<uint64_t>(creation_ms));

	return *iter->second;
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
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
	const bool trace_pipeline = std::getenv("KYTY_PIPELINE_PHASE_TRACE") != nullptr;
	if (trace_pipeline) {
		std::printf("PipelineCreationPhase: kind=compute phase=begin id=%" PRIu64 " hash=0x%016" PRIx64 "\n", compute_program.id, input_info.stage.program->shader_hash);
		std::fflush(stdout);
	}
	const auto creation_begin = std::chrono::steady_clock::now();
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);
	const auto creation_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - creation_begin).count();

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	if (trace_pipeline) {
		std::printf("PipelineCreationPhase: kind=compute phase=done id=%" PRIu64 " elapsed_ms=%" PRId64 "\n", compute_program.id, creation_ms);
		std::fflush(stdout);
	}
	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);
	CheckpointDriverCacheLocked(static_cast<uint64_t>(creation_ms));

	return *iter->second;
}
} // namespace Libs::Graphics
