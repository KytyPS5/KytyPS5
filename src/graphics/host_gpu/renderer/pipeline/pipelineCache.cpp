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
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ComputeExecution.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
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
#include <tuple>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

constexpr uint32_t DriverCacheCheckpointInterval = 16;

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

bool IsDriverCacheSignatureCompatible(std::string_view cached_signature,
                                      std::string_view expected_signature) {
	if (cached_signature == expected_signature) {
		return true;
	}

	// Vulkan keys cached entries by the full pipeline state. Keep the build fields for
	// provenance, but do not discard valid driver data merely because Kyty changed.
	const auto implementation_identity = [](std::string_view signature)
	    -> std::optional<std::string_view> {
		constexpr std::string_view prefix = "KytyPC2:";
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
		constexpr size_t identity_size = 8 + 1 + 8 + 1 + 8 + 1 + 32 + 1;
		if (identity.size() != identity_size || identity[8] != ':' ||
		    identity[17] != ':' || identity[26] != ':' || identity.back() != '\n' ||
		    !IsLowerHex(identity.substr(0, 8), 8) ||
		    !IsLowerHex(identity.substr(9, 8), 8) ||
		    !IsLowerHex(identity.substr(18, 8), 8) ||
		    !IsLowerHex(identity.substr(27, 32), 32)) {
			return std::nullopt;
		}
		return identity;
	};

	const auto cached_identity   = implementation_identity(cached_signature);
	const auto expected_identity = implementation_identity(expected_signature);
	return cached_identity.has_value() && expected_identity.has_value() &&
	       *cached_identity == *expected_identity;
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("KytyPC2:{}:{}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
	                   KYTY_GIT_WORKTREE_FINGERPRINT, properties.vendorID,
	                   properties.deviceID, properties.driverVersion, uuid);
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

bool ReadShaderBacking(void*, uint64_t address, uint32_t* value) {
	return value != nullptr &&
	       Libs::LibKernel::Memory::TryReadBacking(address, value, sizeof(*value));
}

bool ReadShaderGuestMemory(void*, uint64_t address, uint32_t* value) {
	return value != nullptr &&
	       Libs::LibKernel::Memory::TryReadGpuCleanBacking(address, value, sizeof(*value));
}

uint64_t ClampShaderGuestMemory(void*, uint64_t address, uint64_t size) {
	return Libs::LibKernel::Memory::TryClampRangeSize(address, size);
}

void CaptureDispatchedShader(const ShaderParams& params,
                             const ShaderRecompiler::CompileOptions& options,
                             std::span<const uint32_t> static_state,
                             std::optional<std::array<uint32_t, 3>> guest_workgroups) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	try {
		const char* stage = options.stage == ShaderType::Compute ? "compute"
		                    : options.stage == ShaderType::Vertex ? "vertex"
		                    : options.stage == ShaderType::Pixel ? "pixel" : "unknown";
		const auto content_hash = XXH3_64bits(params.code.data(), params.code.size_bytes());
		const auto state_hash = XXH3_64bits(static_state.data(), static_state.size_bytes());
		const auto stem = fmt::format("{}_{:016x}_{:016x}", stage, options.shader_hash, state_hash);
		const auto directory = Config::GetShaderLogFolder() / "dispatched";
		if (!Common::File::CreateDirectories(directory) && !Common::File::IsDirectoryExisting(directory)) {
			LOGF("Shader capture: cannot create directory %s\n", Common::PathToString(directory).c_str());
			return;
		}
		nlohmann::ordered_json metadata {
		    {"schema_version", 1},
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
		    {"scratch_dwords", options.scratch_dwords},
		    {"metadata_complete", false},
		    {"host_profile", {{"known", options.host_profile.known},
		                      {"float64", options.host_profile.float64},
		                      {"fma_float64", options.host_profile.fma_float64},
		                      {"rte_float64", options.host_profile.rte_float64},
		                      {"rte_float32", options.host_profile.rte_float32},
		                      {"signed_zero_inf_nan_preserve_float64",
		                       options.host_profile.signed_zero_inf_nan_preserve_float64}}},
		    {"static_state", std::vector<uint32_t>(static_state.begin(), static_state.end())},
		};
		if (options.stage == ShaderType::Compute && options.input_info.compute != nullptr) {
			const auto& input = *options.input_info.compute;
			metadata["metadata_complete"] = true;
			metadata["compute"] = {
			    {"initial_fp_state", {{"known", input.initial_fp_state.known},
			                          {"float_mode", input.initial_fp_state.float_mode},
			                          {"ieee_mode", input.initial_fp_state.ieee_mode},
			                          {"dx10_clamp", input.initial_fp_state.dx10_clamp}}},
			    {"threads_num", std::array {input.threads_num[0], input.threads_num[1], input.threads_num[2]}},
			    {"dispatch_threads_num", std::array {input.dispatch_threads_num[0], input.dispatch_threads_num[1], input.dispatch_threads_num[2]}},
			    {"lds_size_dwords", input.lds_size_dwords},
			    {"scratch_size_dwords", input.scratch_size_dwords},
			    {"group_id", std::array {input.group_id[0], input.group_id[1], input.group_id[2]}},
			    {"dispatch_thread_dimensions", input.dispatch_thread_dimensions},
			    {"needs_lds_barriers", input.needs_lds_barriers},
			    {"wave_size", input.wave_size},
			    {"thread_ids_num", input.thread_ids_num},
			    {"workgroup_register", input.workgroup_register},
			    {"tg_size_en", input.tg_size_en},
			};
			metadata["compute_workgroup_limits"] = {
			    {"max_size", options.compute_workgroup_limits.max_size},
			    {"max_invocations", options.compute_workgroup_limits.max_invocations},
			};
			if (guest_workgroups.has_value()) {
				metadata["compute"]["guest_workgroups"] = *guest_workgroups;
			}
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
		    !write(directory / (stem + ".json"), json.data(), json.size())) {
			LOGF("Shader capture: cannot write dispatched shader %s\n", stem.c_str());
		}
	} catch (const std::exception& error) {
		LOGF("Shader capture: cannot capture dispatched shader: %s\n", error.what());
	} catch (...) {
		LOGF("Shader capture: cannot capture dispatched shader: unknown exception\n");
	}
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

	template <ShaderType Stage>
	Permutation CompilePermutation(const ShaderParams&                          params,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword) {
		constexpr const char* stage_name = [] {
			if constexpr (Stage == ShaderType::Vertex) {
				return "vs";
			} else if constexpr (Stage == ShaderType::Pixel) {
				return "ps";
			} else {
				static_assert(Stage == ShaderType::Compute);
				return "cs";
			}
		}();
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		const char* optimization_trace = std::getenv("KYTY_SPIRV_OPTIMIZATION_TRACE");
		uint32_t    wave_partition_factor = 1;
		bool        cooperative_wave64    = false;
		if constexpr (Stage == ShaderType::Compute) {
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

		vk::ShaderModuleCreateInfo create_info {};
		create_info.sType       = vk::StructureType::eShaderModuleCreateInfo;
		create_info.codeSize    = result.spirv.size() * sizeof(uint32_t);
		create_info.pCode       = result.spirv.data();
		vk::ShaderModule module = nullptr;
		RequireVulkanSuccess(device.createShaderModule(&create_info, nullptr, &module),
		                     "create recompiled shader module");
		EXIT_IF(module == nullptr);
		SetVulkanObjectNameF(device, module, "{}", ShaderModuleDebugName(Stage, options.shader_hash));
		if (options.dump_ir) {
			if (!options.early_dump) {
				LOGF("%s decoded RDNA2:\n%s", options.dump_label, result.decoded_dump.c_str());
				LOGF("%s IR:\n%s", options.dump_label, result.ir_dump.c_str());
			}
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		auto compiled_info = std::move(result.program).TakeCompiledInfo();
		compiled_info.compute_wave_partition_factor = wave_partition_factor;
		compiled_info.compute_cooperative_wave64 = cooperative_wave64;
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(compiled_info),
		    .handle         = {.id = ++next_shader_id, .module = module},
		};
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor,
	                  std::optional<std::array<uint32_t, 3>> guest_workgroups = std::nullopt) {
		constexpr ShaderType stage = [] {
			if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
				return ShaderType::Vertex;
			} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
				return ShaderType::Pixel;
			} else {
				static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
				return ShaderType::Compute;
			}
		}();

		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = static_cast<uint32_t>(params.user_data.size());
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto                                         entry = programs.find(lookup_key);
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		const ShaderRecompiler::IR::SrtRuntime       runtime {
		    .user_data                  = params.user_data,
		    .shader_base                = params.Base(),
		    .read_memory                = ReadShaderBacking,
		    .read_specialization_memory = ReadShaderGuestMemory,
		    .compute_workgroups         = guest_workgroups,
		    .clamp_memory_range         = ClampShaderGuestMemory,
		};
		if (entry != programs.end()) {
			if (!ShaderRecompiler::IR::MaterializeResources(
			        entry->second.resource_plan, runtime, resources, specialization)) {
				const auto reason = ShaderRecompiler::IR::LastResourceSpecializationError();
				EXIT("shader resource rematerialization failed: stage=%u hash=0x%016" PRIx64
				     " reason=%.*s\n",
				     static_cast<uint32_t>(stage), params.hash, static_cast<int>(reason.size()),
				     reason.data());
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
		constexpr const char* label = [] {
			if constexpr (stage == ShaderType::Vertex) {
				return "ShaderRecompiler VS";
			} else if constexpr (stage == ShaderType::Pixel) {
				return "ShaderRecompiler PS";
			} else {
				return "ShaderRecompiler CS";
			}
		}();
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = params.user_data;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::ShaderLogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;
		options.host_profile = host_profile;
		// Native subgroup width also constrains graphics wave operations. Compute
		// consumes the remaining workgroup limits through the same host profile.
		options.compute_workgroup_limits = compute_workgroup_limits;
		if constexpr (stage == ShaderType::Vertex) {
			options.user_data_base = 8;
			options.scratch_dwords = input_info.scratch_size_dwords;
		} else if constexpr (stage == ShaderType::Pixel) {
			options.scratch_dwords = input_info.scratch_size_dwords;
		} else {
			options.scratch_dwords = input_info.scratch_size_dwords;
			options.wave_size      = input_info.wave_size;
		}
		CaptureDispatchedShader(params, options, lookup_key.static_state, guest_workgroups);
		auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
		if (entry == programs.end()) {
			auto resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(resource_plan, runtime, resources,
			                                                    specialization));
			entry = programs.try_emplace(lookup_key, std::move(resource_plan)).first;
		}
		entry->second.permutations.push_back(CompilePermutation<stage>(
		    params, options, std::move(translated), std::move(specialization), push_data_cursor));
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = std::move(resources)};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::printf("Num compiled %u shaders\n", ++num_compiled);
		return permutation.handle;
	}

	explicit ProgramCache(const GraphicContext& graphics): device(graphics.device) {
		host_profile = graphics.shader_host_profile;
		const auto& limits                       = graphics.GetPhysicalDeviceProperties().limits;
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
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	ProgramKey                                                  lookup_key;
	vk::Device                                                  device;
	ShaderRecompiler::ComputeWorkgroupLimits                    compute_workgroup_limits;
	ShaderRecompiler::ShaderHostProfile                          host_profile;
	uint32_t                                                    num_compiled   = 0;
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
	create.sType           = vk::StructureType::ePipelineCacheCreateInfo;
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 VulkanToString(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", VulkanToString(result));
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
		                 VulkanToString(result), size);
		return false;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
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
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
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

void PipelineCache::CheckpointDriverCacheLocked() {
	if (m_driver_cache == nullptr) {
		return;
	}
	if (++m_new_driver_pipelines < DriverCacheCheckpointInterval) {
		return;
	}
	m_new_driver_pipelines = 0;
	SaveDriverCacheLocked(true);
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
    bool pixel_active, ShaderVertexInputInfo& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const auto vertex_params = PrepareProgram(vertex_regs, sh, vertex_info);
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
	uint32_t          push_data_cursor = 0;
	GraphicsPrograms  result;
	vertex_info.linked_param_count = 0;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
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
			for (uint32_t i = 0; i < vertex_info.linked_param_count; ++i) {
				if (vertex_info.linked_param_sources[i] == source &&
				    vertex_info.linked_param_locations[i] == location) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate) {
				EXIT_IF(vertex_info.linked_param_count >= ShaderVertexInputInfo::PARAM_LINK_MAX);
				const auto link = vertex_info.linked_param_count++;
				vertex_info.linked_param_sources[link]   = source;
				vertex_info.linked_param_locations[link] = location;
			}
		}
	}
	result.vertex = m_program_cache->Get(vertex_params, vertex_info, push_data_cursor);
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info,
                                               const std::array<uint32_t, 3>& guest_workgroups) {
	input_info.needs_lds_barriers = !m_graphics.compute_wave64_supported;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor, guest_workgroups);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::GraphicsPipeline& PipelineCache::CreateGraphicsPipeline(
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

	uint32_t color_mask[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t i = 0; i < color_count; i++) {
		color_mask[i] =
		    (colors[i].image_id ? colors[i].export_mapping.ApplyMask(render_target_mask_slot(
		                              ctx.GetRenderTargetMask(), colors[i].target_slot))
		                        : 0);
	}
	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	PipelineStaticParameters static_params {};
	GraphicsPipeline         p {};
	p.ps_shader_id = ps_id;
	p.vs_shader_id = vs_id;

	static_params.color_count = color_count;
	PipelineRenderingState rendering {};
	rendering.color_count       = color_count;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		EXIT_IF(!colors[i].image_id || colors[i].format == vk::Format::eUndefined);
		rendering.color_formats[i] = colors[i].format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].samples;
		} else if (attachment_samples != colors[i].samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].samples);
		}
	}
	const bool with_depth =
	    depth.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects = ImageViewOps::DepthAspectMask(depth.format);
		rendering.depth_format =
		    aspects & vk::ImageAspectFlagBits::eDepth ? depth.format : vk::Format::eUndefined;
		rendering.stencil_format =
		    aspects & vk::ImageAspectFlagBits::eStencil ? depth.format : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.samples;
		} else if (attachment_samples != depth.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.samples);
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
	static_params.with_depth              = with_depth;
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	static_params.stencil_test_enable      = depth.stencil_test_enable;
	static_params.stencil_front            = depth.stencil_static_front;
	static_params.stencil_back             = depth.stencil_static_back;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		static_params.color_mask[i] = color_mask[i];
	}
	const bool rect_list     = topology == vk::PrimitiveTopology::ePatchList;
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;

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
	GraphicsPipelineKey key {};
	key.rendering     = rendering;
	key.vs_shader_id  = p.vs_shader_id;
	key.ps_shader_id  = p.ps_shader_id;
	key.static_params = static_params;
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

	auto cached = std::make_unique<GraphicsPipeline>(p);
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vs_input_info,
	                       vertex_program.module, ps_input_info, pixel_program.module,
	                       static_params, m_driver_cache);
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);
	CheckpointDriverCacheLocked();

	return *iter->second;
}

PipelineCache::ComputePipeline&
PipelineCache::CreateComputePipeline(ShaderComputeInputInfo& input_info,
                                     const ShaderProgram&    compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	Common::LockGuard lock(m_mutex);

	ComputePipeline p {};
	p.cs_shader_id = compute_program.id;

	ComputePipelineKey key {};
	key.cs_shader_id = p.cs_shader_id;

	if (auto iter = m_compute_pipelines.find(key); iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<ComputePipeline>(p);
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);
	CheckpointDriverCacheLocked();

	return *iter->second;
}
} // namespace Libs::Graphics
