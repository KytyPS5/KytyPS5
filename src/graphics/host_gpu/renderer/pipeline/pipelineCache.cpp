#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/fileLock.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/pipeline/programList.h"
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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fmt/format.h>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

// Converts to any field type, to count an aggregate's fields.
struct AnyField {
	template <typename T>
	operator T() const;
};

// The number of fields of an aggregate of scalars: the most initializers it takes.
template <typename T, typename... Fields>
consteval size_t FieldCount() {
	if constexpr (requires { T {Fields {}..., AnyField {}}; }) {
		return FieldCount<T, Fields..., AnyField>();
	} else {
		return sizeof...(Fields);
	}
}

} // namespace

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
	// The file is tied to the device and its driver, not to the emulator build. The driver finds
	// an entry by the shader code and state a pipeline is created from, so an entry that a newer
	// emulator build no longer asks for is only unused. With the build in the signature, every
	// update threw the file away, and the next session compiled each pipeline again at its first
	// use: in a 3D scene 500 pipelines took 66 s that way, against 1 to 2 s with the file kept.
	return fmt::format("KytyPC2:{:08x}:{:08x}:{:08x}:{}\n", properties.vendorID,
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
	Log::WriteToConsoleAndLog(message);
}


bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads.
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
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
                        std::span<const uint32_t> code) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
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

} // namespace

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
	for (const auto id: key.vertex_shader_ids) {
		PipelineKeyHash::Mix(hash, id);
	}
	PipelineKeyHash::Mix(hash, key.ps_shader_id);
	PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
	for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
	}
	PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
	for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
	}
	PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
	return hash;
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
		std::vector<uint32_t>                       function_code;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
		// The name the program has on the lists of the game: the same at every start.
		uint64_t identity = 0;
	};

	struct SourceEntry {
		SourceEntry(ShaderRecompiler::IR::ResourcePlan plan,
		            std::unique_ptr<ShaderRecompiler::ShaderSource> call_source)
		    : call_source(std::move(call_source)), resource_plan(std::move(plan)) {}

		std::span<const uint32_t> FunctionCode() const {
			return call_source
			    ? std::span<const uint32_t>(call_source->linked->code).subspan(call_source->code.size())
			    : std::span<const uint32_t>{};
		}

		bool RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime) {
			const auto revision = call_source->revision;
			ShaderRecompiler::RefreshShaderSource(*call_source, runtime);
			resource_plan.source_reads = call_source->reads;
			return revision != call_source->revision;
		}

		template <bool HasCalls>
		Permutation* FindPermutation(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                             uint32_t push_data_cursor) {
			{
				KYTY_PROFILER_BLOCK("MaterializeResources");
				EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
				    resource_plan, runtime, resources, specialization));
			}
			for (auto& candidate: permutations) {
				const auto& layout = candidate.program.bindings;
				if (layout.push_data_start_dword != ShaderRecompiler::IR::PushData::StartFor(
				        push_data_cursor, layout.ShaderDataDwords()) ||
				    candidate.specialization != specialization) {
					continue;
				}
				if constexpr (HasCalls) {
					if (!std::ranges::equal(candidate.function_code, FunctionCode())) continue;
				}
				return &candidate;
			}
			return nullptr;
		}

		std::unique_ptr<ShaderRecompiler::ShaderSource> call_source;
		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		// A deque: a worker that builds a pipeline points to a permutation, and a new one must
		// not move those that exist.
		std::deque<Permutation> permutations;
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
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	// Translated IR to SPIR-V, checked. Has no state of its own: a worker thread can run it.
	static ShaderRecompiler::CompileResult
	CompileChecked(const char* stage_name, const ShaderRecompiler::CompileOptions& options,
	               ShaderRecompiler::TranslateResult                   translated,
	               const ShaderRecompiler::IR::ResourceSpecialization& specialization,
	               uint32_t                                            push_data_start_dword) {
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
		if (!result.mesh_vertex_spirv.empty()) {
			if (!ValidateShaderSpirv(options.dump_label, options.shader_hash,
			                         result.mesh_vertex_spirv)) {
				DumpShaderSpirv("mesh_vs", options.shader_hash, result.mesh_vertex_spirv);
				EXIT("%s failed hash=0x%016" PRIx64 ": emulated mesh vertex shader is invalid\n",
				     options.dump_label, options.shader_hash);
			}
			DumpShaderSpirv("mesh_vs", options.shader_hash, result.mesh_vertex_spirv);
		}
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return result;
	}

	// Renderer thread: the host shader modules and the handle of a compiled program.
	Permutation MakePermutation(ShaderRecompiler::CompileResult              result,
	                            ShaderRecompiler::IR::ResourceSpecialization specialization,
	                            std::span<const uint32_t>                    function_code = {}) {
		const auto module = CompileSPV(result.spirv, device);
		EXIT_IF(module == nullptr);
		vk::ShaderModule mesh_vertex_module = nullptr;
		if (!result.mesh_vertex_spirv.empty()) {
			mesh_vertex_module = CompileSPV(result.mesh_vertex_spirv, device);
			EXIT_IF(mesh_vertex_module == nullptr);
		}
		return {
		    .function_code  = {function_code.begin(), function_code.end()},
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id                 = ++next_shader_id,
		                       .module             = module,
		                       .mesh_vertex_module = mesh_vertex_module,
		                       .mesh_slot_words    = result.mesh_slot_words},
		};
	}

	struct StageNames {
		const char* label      = nullptr;
		const char* stage_name = nullptr;
	};

	static StageNames NamesOf(ShaderType stage) {
		switch (stage) {
			case ShaderType::Vertex: return {"ShaderRecompiler VS", "vs"};
			case ShaderType::Mesh: return {"ShaderRecompiler MS", "ms"};
			case ShaderType::Local: return {"ShaderRecompiler LS", "ls"};
			case ShaderType::TessellationControl: return {"ShaderRecompiler HS", "hs"};
			case ShaderType::TessellationEvaluation: return {"ShaderRecompiler DS", "ds"};
			case ShaderType::Pixel: return {"ShaderRecompiler PS", "ps"};
			case ShaderType::Compute: return {"ShaderRecompiler CS", "cs"};
			default: EXIT("invalid pipeline shader stage\n");
		}
	}

	// The translator's options for one stage. `user_data`: only its size matters here; the
	// values select the specialization, which the caller passes on its own.
	template <typename InputInfo>
	ShaderRecompiler::CompileOptions
	MakeOptions(ShaderType stage, uint64_t hash, std::span<const uint32_t> user_data,
	            std::span<const uint32_t> back_code, InputInfo& input_info) const {
		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = hash;
		options.user_data   = user_data;
		options.back_code   = back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = NamesOf(stage).label;
		options.input_info  = stage_input;
		options.host        = host;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size      = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		return options;
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		KYTY_PROFILER_BLOCK("ProgramCache::Get");
		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto                                         entry = programs.find(lookup_key);
		ShaderRecompiler::IR::SrtRuntime             runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_specialization_memory = ReadShaderGuestMemory,
		};
		if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			runtime.workgroup_counts = input_info.workgroup_counts;
		}
		bool source_changed = false;
		if (entry != programs.end()) {
			auto& cached = entry->second;
			Permutation* permutation = nullptr;
			if (cached.call_source) {
				source_changed = cached.RefreshCallSource(runtime);
				if (!source_changed) permutation = cached.FindPermutation<true>(runtime, push_data_cursor);
			} else {
				permutation = cached.FindPermutation<false>(runtime, push_data_cursor);
			}
			if (permutation != nullptr) {
				input_info.stage = {.program = &permutation->program, .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		const auto options =
		    MakeOptions(stage, params.hash, user_data, params.back_code, input_info);
		const char* stage_name = NamesOf(stage).stage_name;
		DumpShaderOriginal(stage_name, options.shader_hash, params.code);
		ShaderRecompiler::ShaderSource source;
		if (entry == programs.end()) source = ShaderRecompiler::PrepareShaderSource(params.code, options);
		auto* current = entry == programs.end() ? &source : entry->second.call_source.get();
		ShaderRecompiler::TranslateResult translated;
		if (current != nullptr) {
			if (current->call) {
				if (entry == programs.end()) ShaderRecompiler::RefreshShaderSource(*current, runtime);
				translated = ShaderRecompiler::TranslateProgram(current->linked->decoded, options);
				translated.program.source_reads = current->reads;
			} else {
				translated = ShaderRecompiler::TranslateProgram(current->decoded, options);
			}
		} else {
			translated = ShaderRecompiler::TranslateProgram(params.code, options);
		}
		if (entry == programs.end()) {
			auto retained = source.call ? std::make_unique<ShaderRecompiler::ShaderSource>(std::move(source)) : nullptr;
			entry = programs.try_emplace(lookup_key,
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program), std::move(retained)).first;
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
			    entry->second.resource_plan, runtime, entry->second.resources,
			    entry->second.specialization));
		} else if (source_changed) {
			entry->second.resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			if (const auto* permutation = entry->second.FindPermutation<true>(runtime, push_data_cursor)) {
				input_info.stage = {.program = &permutation->program, .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}
		auto compiled = CompileChecked(stage_name, options, std::move(translated),
		                               entry->second.specialization, push_data_cursor);
		// After the compile: a program that fails to compile stops the emulator, and it must not
		// stop every later start from the list. Zero without lists for this game, and for a
		// program that calls a guest function chosen at run time: a record has no function code.
		const auto identity =
		    entry->second.call_source
		        ? 0u
		        : Remember(stage, params, input_info, entry->second.specialization, push_data_cursor);
		entry->second.permutations.push_back(MakePermutation(
		    std::move(compiled), entry->second.specialization, entry->second.FunctionCode()));
		auto& permutation = entry->second.permutations.back();
		Name(entry->second, permutation, identity);
		input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return permutation.handle;
	}

	// ---- The program list: what each program was translated from, kept on disk for a game.
	// At the next start worker threads translate the listed programs again, so a draw finds its
	// program ready and the renderer does not stop to translate it.

	// A program a worker built from a record. It has no host objects yet.
	struct Prebuilt {
		ProgramKey                                   key;
		ShaderRecompiler::IR::ResourcePlan           plan;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::CompileResult              result;
		uint64_t                                     identity = 0;
		// The record's specialization does not fit what this translator finds in the shader
		// (it came from another build). Nothing was built; the record has to be made again.
		bool stale = false;
	};

	// What a program on the list is, once built: for the pipeline list, which names programs by
	// their identity.
	struct ListedProgram {
		ShaderStageRuntime stage;
		ShaderProgram      handle;
	};
	[[nodiscard]] std::optional<ListedProgram> Listed(uint64_t identity) const {
		const auto it = m_by_identity.find(identity);
		if (it == m_by_identity.end()) {
			return std::nullopt;
		}
		const auto& [entry, permutation] = it->second;
		return ListedProgram {.stage  = {.program   = &permutation->program,
		                                 .resources = &entry->resources},
		                      .handle = permutation->handle};
	}
	[[nodiscard]] uint64_t IdentityOf(const ShaderProgram& program) const {
		const auto it = m_identity_of_id.find(program.id);
		return it == m_identity_of_id.end() ? 0 : it->second;
	}
	// Also for a second identity of the same permutation: two records can name one program when
	// their push data cursors differ but land on the same start.
	void Name(SourceEntry& entry, Permutation& permutation, uint64_t identity) {
		if (identity == 0) {
			return;
		}
		if (permutation.identity == 0) {
			permutation.identity = identity;
			m_identity_of_id.emplace(permutation.handle.id, identity);
		}
		if (m_by_identity.try_emplace(identity, &entry, &permutation).second) {
			m_names++;
		}
	}

	// Grows each time a program gets a name the list knows (see ListedPipelineScan).
	[[nodiscard]] uint64_t NameCount() const { return m_names; }

	template <typename InputInfo>
	static bool ReadInputInfo(const ProgramRecord& record, InputInfo& info) {
		static_assert(std::is_trivially_copyable_v<InputInfo>);
		if (record.input_info.size() != sizeof(InputInfo)) {
			return false;
		}
		std::memcpy(static_cast<void*>(&info), record.input_info.data(), sizeof(InputInfo));
		info.stage = {};
		return true;
	}

	// Names a record for "is it on the list already". The input info is left out for its key
	// words: its raw bytes have padding, and equal inputs must give an equal name.
	static uint64_t Identity(const ProgramRecord& record, std::span<const uint32_t> static_state) {
		ProgramRecord named = record;
		named.code          = {static_cast<uint32_t>(record.code.size())};
		named.input_info.assign(reinterpret_cast<const uint8_t*>(static_state.data()),
		                        reinterpret_cast<const uint8_t*>(static_state.data()) +
		                            static_state.size_bytes());
		std::vector<uint8_t> bytes;
		AppendProgramRecord(bytes, named);
		return XXH3_64bits(bytes.data(), bytes.size());
	}

	bool StaticStateOf(const ProgramRecord& record, std::vector<uint32_t>& static_state) const {
		const auto build = [&]<typename InputInfo>(InputInfo info) {
			if (!ReadInputInfo(record, info)) {
				return false;
			}
			BuildStageStaticKey(info, static_state);
			return true;
		};
		switch (record.stage) {
			case ShaderType::Vertex:
			case ShaderType::Mesh:
			case ShaderType::Local:
			case ShaderType::TessellationControl:
			case ShaderType::TessellationEvaluation: return build(ShaderVertexInputInfo {});
			case ShaderType::Pixel: return build(ShaderPixelInputInfo {});
			case ShaderType::Compute: return build(ShaderComputeInputInfo {});
			default: return false;
		}
	}

	// The host as the list's signature sees it: every field by value, never the object's bytes,
	// which have padding. A new field must be added here, or programs built for another host
	// would be taken from the list. The field count catches a new field also where the size
	// does not change (a bool in padding).
	uint64_t HostWords() const {
		const auto& c = host.capabilities;
		const auto& f = host.faults;
		const uint32_t words[] = {
		    c.image_view_min_lod,
		    c.cull_distance,
		    c.buffer_int64_atomics,
		    c.shared_int64_atomics,
		    c.image_int64_atomics,
		    c.float64,
		    c.fragment_barycentric,
		    c.depth_bounds,
		    c.depth_clamp,
		    c.depth_clip_enable,
		    c.color_write_enable,
		    c.mesh_shader,
		    c.compute_derivatives,
		    c.push_descriptors,
		    c.packed_scaled_vertex_input,
		    c.provoking_vertex_last,
		    c.attachment_feedback_loop,
		    c.compute_subgroup_size_control,
		    c.block_texel_view,
		    c.depth_color_copy,
		    c.subgroup_supported_stages,
		    c.subgroup_supported_operations,
		    f.pushed_buffers_have_no_size,
		    f.no_per_vertex_inputs,
		    f.no_centroid_barycentric,
		    f.volatile_loads_are_reused,
		    f.no_contraction_is_slow,
		    f.pipeline_cache_load_is_slow,
		    f.pipeline_cache_serializes_builds,
		    f.vertex_subgroups_unreported,
		};
		static_assert(std::size(words) == FieldCount<HostCapabilities>() + FieldCount<DriverFaults>(),
		              "a host field was added: add it to the list signature");
		return XXH3_64bits(words, sizeof(words));
	}

	// With the layout of what a record holds and the host's capabilities, not with the build: a
	// record is what the translator was given, and the program built from it is filed under the
	// key the live path computes from the same inputs. That key holds every input that changes
	// the translation (the live cache relies on it), so a record from another build either
	// meets a draw with the same inputs, and the current translator builds what the draw would,
	// or it meets no draw and the build is wasted. Lists survive new builds of the emulator.
	// The number in the tag counts format changes that the layouts below do not show.
	std::string ListSignature() const {
		return fmt::format("KytyPL5:{}:{:016x}\n", InputInfoLayouts(), HostWords());
	}

	// Each input info's size and the offset of its last member (see RecordLastOffset).
	static std::string InputInfoLayouts() {
		return fmt::format("{}/{}:{}/{}:{}/{}", sizeof(ShaderVertexInputInfo),
		                   RecordLastOffset(&ShaderVertexInputInfo::param_copy_location),
		                   sizeof(ShaderPixelInputInfo),
		                   RecordLastOffset(&ShaderPixelInputInfo::stage),
		                   sizeof(ShaderComputeInputInfo),
		                   RecordLastOffset(&ShaderComputeInputInfo::stage));
	}

	// Worker thread. Builds what the live path builds for the same inputs.
	template <typename InputInfo>
	bool Prebuild(const ProgramRecord& record, Prebuilt& out) const {
		InputInfo info;
		if (!ReadInputInfo(record, info)) {
			return false;
		}
		out.key.stage           = record.stage;
		out.key.hash            = record.hash;
		out.key.user_data_count = record.user_data_count;
		out.key.code_size       = static_cast<uint32_t>(record.code.size());
		BuildStageStaticKey(info, out.key.static_state);
		out.identity = Identity(record, out.key.static_state);
		// The translator reads only how many user data words there are. Their values matter to
		// the resources alone, and the record holds the specialization they gave on the live path.
		const std::vector<uint32_t> user_data(record.user_data_count, 0u);
		const auto                  options =
		    MakeOptions(record.stage, record.hash, user_data, record.back_code, info);
		const auto source = ShaderRecompiler::PrepareShaderSource(record.code, options);
		// A program that calls a guest function chosen at run time is built by the draw that
		// finds the function (see Get). Lists of older builds can name one.
		if (source.call) {
			out.stale = true;
			m_stale_records.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		auto translated = ShaderRecompiler::TranslateProgram(source.decoded, options);
		if (!ShaderRecompiler::IR::ResourceSpecializationFits(translated.program.info,
		                                                      record.specialization)) {
			out.stale = true;
			m_stale_records.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		out.plan           = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
		out.specialization = record.specialization;
		out.result =
		    CompileChecked(NamesOf(record.stage).stage_name, options, std::move(translated),
		                   record.specialization, record.push_data_start);
		return true;
	}

	bool Prebuild(const ProgramRecord& record, Prebuilt& out) const {
		switch (record.stage) {
			case ShaderType::Vertex:
			case ShaderType::Mesh:
			case ShaderType::Local:
			case ShaderType::TessellationControl:
			case ShaderType::TessellationEvaluation:
				return Prebuild<ShaderVertexInputInfo>(record, out);
			case ShaderType::Pixel: return Prebuild<ShaderPixelInputInfo>(record, out);
			case ShaderType::Compute: return Prebuild<ShaderComputeInputInfo>(record, out);
			default: return false;
		}
	}

	// Main thread, once. Reads the list of the game and starts the workers on it. `writable`:
	// this emulator keeps the list; otherwise it only builds ahead from it.
	void OpenProgramList(const std::filesystem::path& path, bool writable) {
		const auto           signature = ListSignature();
		std::vector<uint8_t> bytes;
		if (std::FILE* file = std::fopen(Common::PathToString(path).c_str(), "rb")) {
			std::fseek(file, 0, SEEK_END);
			const auto size = std::ftell(file);
			std::fseek(file, 0, SEEK_SET);
			bytes.resize(size > 0 ? static_cast<size_t>(size) : 0);
			if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
				bytes.clear();
			}
			std::fclose(file);
		}
		const bool current = bytes.size() >= signature.size() &&
		                     std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
		if (current) {
			// A program recorded again (its first record was stale, see Prebuilt) has the later
			// record.
			std::vector<uint32_t> static_state;
			const auto            identity_of =
			    [&](const ProgramRecord& record) -> std::optional<uint64_t> {
				if (!StaticStateOf(record, static_state)) {
					return std::nullopt;
				}
				return Identity(record, static_state);
			};
			for (auto& [identity, record]: LatestByIdentity(
			         ReadProgramRecords(std::span(bytes).subspan(signature.size())), identity_of)) {
				m_listed.insert(identity);
				m_records.push_back(std::move(record));
			}
		}
		if (writable) {
			// A list from another layout or device is started again; a damaged tail is cut off.
			std::vector<uint8_t> kept(signature.begin(), signature.end());
			for (const auto& record: m_records) {
				AppendProgramRecord(kept, record);
			}
			m_list_file = RewriteList(path, kept);
			if (m_list_file == nullptr) {
				// The programs that were read are still built ahead; new ones are not remembered.
				PipelineCacheLog("Program list: cannot write {}", Common::PathToString(path));
			}
		}
		PipelineCacheLog("Program list: {} program(s) to build ahead from {}{}", m_records.size(),
		                 Common::PathToString(path), writable ? "" : " (read only)");
		if (m_records.empty()) {
			return;
		}
		// These threads come on top of the pipeline builders (PipelineBuilderCount) until the
		// list is translated, so at a start the two together can use the cores the builders
		// leave free. Not tuned together: the builder count was measured alone.
		const auto threads = std::clamp(std::thread::hardware_concurrency() / 4u, 1u, 4u);
		m_workers_running  = threads;
		for (uint32_t i = 0; i < threads; i++) {
			m_workers.emplace_back([this, started = std::chrono::steady_clock::now()](
			                           std::stop_token stop) {
				for (;;) {
					const auto index = m_next_record.fetch_add(1);
					if (stop.stop_requested() || index >= m_records.size()) {
						if (m_workers_running.fetch_sub(1) == 1 && !stop.stop_requested()) {
							PipelineCacheLog("Program list: {} program(s) built ahead in {} ms, {} "
							                 "stale record(s) left out",
							                 m_records.size() - m_stale_records.load(),
							                 std::chrono::duration_cast<std::chrono::milliseconds>(
							                     std::chrono::steady_clock::now() - started)
							                     .count(),
							                 m_stale_records.load());
						}
						return;
					}
					Prebuilt built;
					if (Prebuild(m_records[index], built)) {
						std::lock_guard lock(m_built_mutex);
						m_built.push_back(std::move(built));
						m_built_waiting.store(true, std::memory_order_release);
					}
				}
			});
		}
	}

	// True when no program will come from the list any more: every worker is done and what they
	// built was taken over. A pipeline record whose programs are still missing then never gets
	// them.
	[[nodiscard]] bool ListBuilt() const {
		// The workers set m_built_waiting before they count themselves out.
		return m_workers_running.load(std::memory_order_acquire) == 0 &&
		       !m_built_waiting.load(std::memory_order_acquire);
	}

	// Renderer thread, before the programs of a draw or dispatch are looked up. Takes over what
	// the workers have finished. Not later in a draw: adding a permutation can move the ones a
	// stage of the same draw already points to.
	void Adopt() {
		if (!m_built_waiting.load(std::memory_order_acquire)) {
			return;
		}
		std::vector<Prebuilt> built;
		{
			std::lock_guard lock(m_built_mutex);
			built.swap(m_built);
			m_built_waiting.store(false, std::memory_order_release);
		}
		for (auto& item: built) {
			if (item.stale) {
				// The next draw that translates the program puts it on the list again.
				m_listed.erase(item.identity);
				continue;
			}
			auto entry =
			    programs.try_emplace(std::move(item.key), std::move(item.plan), nullptr).first;
			const auto start = item.result.program.bindings.push_data_start_dword;
			if (const auto known = std::ranges::find_if(
			        entry->second.permutations,
			        [&](const Permutation& candidate) {
				        return candidate.program.bindings.push_data_start_dword == start &&
				               candidate.specialization == item.specialization;
			        });
			    known != entry->second.permutations.end()) {
				// A draw needed it before the worker was done; the record's name still has to
				// find it.
				Name(entry->second, *known, item.identity);
				continue;
			}
			entry->second.permutations.push_back(
			    MakePermutation(std::move(item.result), std::move(item.specialization)));
			Name(entry->second, entry->second.permutations.back(), item.identity);
			m_adopted++;
		}
	}

	// Renderer thread: a program was translated for a draw. Puts it on the list, once, and
	// returns its identity.
	template <typename InputInfo>
	uint64_t Remember(ShaderType stage, const ShaderParams& params, const InputInfo& input_info,
	                  const ShaderRecompiler::IR::ResourceSpecialization& specialization,
	                  uint32_t                                            push_data_cursor) {
		static_assert(std::is_trivially_copyable_v<InputInfo>);
		if (m_list_file == nullptr) {
			return 0; // no lists for this game: nothing to name
		}
		ProgramRecord record;
		record.stage           = stage;
		record.hash            = params.hash;
		record.user_data_count = params.user_data_count;
		record.push_data_start = push_data_cursor;
		record.code.assign(params.code.begin(), params.code.end());
		record.back_code.assign(params.back_code.begin(), params.back_code.end());
		record.specialization = specialization;
		InputInfo plain       = input_info;
		plain.stage           = {};
		record.input_info.resize(sizeof(InputInfo));
		std::memcpy(record.input_info.data(), static_cast<const void*>(&plain), sizeof(InputInfo));
		const auto identity = Identity(record, lookup_key.static_state);
		if (!m_listed.insert(identity).second) {
			return identity; // on the list, and built here before its worker was done
		}
		std::vector<uint8_t> bytes;
		AppendProgramRecord(bytes, record);
		std::fwrite(bytes.data(), 1, bytes.size(), m_list_file);
		std::fflush(m_list_file);
		return identity;
	}

	ProgramCache(vk::Device device, const HostGpu& host): device(device), host(host) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (auto& worker: m_workers) {
			worker.request_stop();
		}
		m_workers.clear();
		if (m_list_file != nullptr) {
			std::fclose(m_list_file);
		}
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
				if (permutation.handle.mesh_vertex_module != nullptr) {
					device.destroyShaderModule(permutation.handle.mesh_vertex_module, nullptr);
				}
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	ProgramKey                                                  lookup_key;
	std::vector<ProgramRecord>                                  m_records;
	std::atomic<size_t>                                         m_next_record {0};
	std::vector<std::jthread>                                   m_workers;
	std::mutex                                                  m_built_mutex;
	std::vector<Prebuilt>                                       m_built;
	std::atomic<bool>                                           m_built_waiting {false};
	std::atomic<uint32_t>                                       m_workers_running {0};
	mutable std::atomic<size_t>                                 m_stale_records {0};
	std::unordered_set<uint64_t>                                m_listed;
	std::unordered_map<uint64_t, std::pair<SourceEntry*, Permutation*>> m_by_identity;
	std::unordered_map<uint64_t, uint64_t>                      m_identity_of_id;
	std::FILE*                                                  m_list_file = nullptr;
	uint64_t                                                    m_adopted   = 0;
	uint64_t                                                    m_names     = 0;
	vk::Device                                                  device;
	HostGpu                                                     host;
	uint64_t                                                    next_shader_id = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics),
      m_program_cache(std::make_unique<ProgramCache>(graphics.device, graphics.host)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
	m_always_wait       = Config::PipelineWaitEnabled();
	const auto builders = PipelineBuilderCount(std::thread::hardware_concurrency());
	if (m_graphics.host.faults.pipeline_cache_serializes_builds) {
		m_builder_caches.resize(builders);
		for (auto& cache: m_builder_caches) {
			vk::PipelineCacheCreateInfo create {};
			EXIT_NOT_IMPLEMENTED(m_graphics.device.createPipelineCache(&create, nullptr, &cache) !=
			                     vk::Result::eSuccess);
		}
	}
	for (uint32_t i = 0; i < builders; i++) {
		m_builders.emplace_back([this, i](std::stop_token stop) { BuildPipelines(stop, i); });
	}
	if (const auto title_id = PipelineCacheTitleId(); !title_id.empty()) {
		const auto folder = Config::GetPipelineCacheFolder();
		// The folder is shared by every copy of the emulator: one at a time keeps a game's lists.
		// Another one running the same game builds ahead from them and leaves the files alone.
		m_lists_writable = m_list_lock.TryAcquire(folder / (title_id + ".lock"));
		if (!m_lists_writable) {
			PipelineCacheLog("Pipeline lists: another emulator keeps them for this game, or the "
		                 "lock file cannot be opened; read only");
		}
		m_program_cache->OpenProgramList(folder / (title_id + ".programs"), m_lists_writable);
		OpenPipelineList(folder / (title_id + ".pipelines"), m_lists_writable);
	}
}

// Signed like the program list: with the layout of what a record holds and the host's
// capabilities, not with the build. A record names its programs and fixed state; the pipeline
// built from it is filed under the key a draw computes from the same state, so a record that no
// draw matches is only a wasted build.
static std::string PipelineListSignature(std::string_view input_info_layouts,
                                         uint64_t         host_words) {
	return fmt::format("KytyPP5:{}/{}:{}/{}:{}:{}:{:016x}\n", sizeof(PipelineRenderingState),
	                   RecordLastOffset(&PipelineRenderingState::color_count),
	                   sizeof(PipelineVertexInputState),
	                   RecordLastOffset(&PipelineVertexInputState::attribute_count),
	                   sizeof(PipelineStaticParameters), input_info_layouts, host_words);
}

void PipelineCache::OpenPipelineList(const std::filesystem::path& path, bool writable) {
	const auto           signature = PipelineListSignature(m_program_cache->InputInfoLayouts(),
	                                                       m_program_cache->HostWords());
	std::vector<uint8_t> bytes;
	if (std::FILE* file = std::fopen(Common::PathToString(path).c_str(), "rb")) {
		std::fseek(file, 0, SEEK_END);
		const auto size = std::ftell(file);
		if (size > 0) {
			bytes.resize(static_cast<size_t>(size));
			std::fseek(file, 0, SEEK_SET);
			bytes.resize(std::fread(bytes.data(), 1, bytes.size(), file));
		}
		std::fclose(file);
	}
	const bool signed_by_this_build =
	    bytes.size() >= signature.size() &&
	    std::equal(signature.begin(), signature.end(), bytes.begin());
	if (signed_by_this_build) {
		m_listed_pipelines = ReadPipelineRecords(
		    std::span<const uint8_t>(bytes).subspan(signature.size()));
	}
	PipelineCacheLog("Pipeline list: {} pipeline(s) to build ahead from {}",
	                 m_listed_pipelines.size(), Common::PathToString(path));
	m_built_ahead.Listed(static_cast<uint32_t>(m_listed_pipelines.size()));
	// The list is written again without what was dropped (another build, a damaged tail), into
	// a temporary file that then takes the list's place: a stop in between leaves the old list.
	// New records are appended to it.
	std::vector<uint8_t> kept(signature.begin(), signature.end());
	for (const auto& record: m_listed_pipelines) {
		std::vector<uint8_t> bytes;
		AppendPipelineRecord(bytes, record);
		// Known, so a draw that needs it before its programs are built ahead does not add it again.
		m_pipelines_on_list.insert(XXH3_64bits(bytes.data(), bytes.size()));
		kept.insert(kept.end(), bytes.begin(), bytes.end());
	}
	if (!writable) {
		// Another emulator keeps the list: the pipelines read are built ahead, nothing is added.
		return;
	}
	m_pipeline_list = RewriteList(path, kept);
	if (m_pipeline_list == nullptr) {
		// The pipelines that were read are still built ahead; new ones are not remembered.
		PipelineCacheLog("Pipeline list: cannot write {}", Common::PathToString(path));
	}
}

template <typename T>
static void AppendBytes(std::vector<uint8_t>& out, const T& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	const auto* raw = reinterpret_cast<const uint8_t*>(&value);
	out.insert(out.end(), raw, raw + sizeof(T));
}

template <typename T>
static bool ReadBytes(std::span<const uint8_t>& in, T& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	if (in.size() < sizeof(T)) {
		return false;
	}
	std::memcpy(&value, in.data(), sizeof(T));
	in = in.subspan(sizeof(T));
	return true;
}

// Renderer thread: a pipeline was queued for a draw. Puts it on the list, once.
void PipelineCache::RememberPipeline(const GraphicsPipelineKey& key, const PipelineJob& job) {
	if (m_pipeline_list == nullptr) {
		return;
	}
	PipelineRecord record;
	for (uint32_t i = 0; i < job.vertex_count; i++) {
		record.programs[i] = m_program_cache->IdentityOf(job.programs.vertex[i]);
		if (record.programs[i] == 0) {
			return; // a program the list does not know cannot be named
		}
	}
	if (job.pixel) {
		record.programs[3] = m_program_cache->IdentityOf(job.programs.pixel);
		if (record.programs[3] == 0) {
			return;
		}
	}
	AppendBytes(record.fixed_state, key.rendering);
	AppendBytes(record.fixed_state, key.vertex_input);
	AppendBytes(record.fixed_state, key.static_params);
	record.stage_count = job.vertex_count;
	for (uint32_t i = 0; i < job.vertex_count; i++) {
		auto plain  = job.vertex_info[i];
		plain.stage = {};
		AppendBytes(record.stages[i], plain);
	}
	record.pixel_present = job.pixel.has_value();
	if (job.pixel) {
		auto plain  = *job.pixel;
		plain.stage = {};
		AppendBytes(record.pixel, plain);
	}
	WriteRecord(record);
}

void PipelineCache::RememberComputePipeline(const PipelineJob& job) {
	if (m_pipeline_list == nullptr) {
		return;
	}
	PipelineRecord record;
	record.programs[0] = m_program_cache->IdentityOf(job.programs.pixel);
	if (record.programs[0] == 0) {
		return;
	}
	auto plain  = *job.compute;
	plain.stage = {};
	AppendBytes(record.fixed_state, plain);
	WriteRecord(record);
}

void PipelineCache::WriteRecord(const PipelineRecord& record) {
	std::vector<uint8_t> bytes;
	AppendPipelineRecord(bytes, record);
	if (!m_pipelines_on_list.insert(XXH3_64bits(bytes.data(), bytes.size())).second) {
		return;
	}
	std::fwrite(bytes.data(), 1, bytes.size(), m_pipeline_list);
	std::fflush(m_pipeline_list);
}

// Renderer thread, before the programs of a draw are looked up (right after the program list
// workers' results were adopted). Queues every listed pipeline whose programs are all there.
void PipelineCache::QueueListedPipelines() {
	if (m_listed_pipelines.empty()) {
		return;
	}
	const bool list_built = m_program_cache->ListBuilt();
	if (!m_listed_scan.Due(m_program_cache->NameCount(), list_built)) {
		return;
	}
	std::erase_if(m_listed_pipelines, [this, list_built](const PipelineRecord& record) {
		PipelineJob         job;
		GraphicsPipelineKey key;
		// Dropped or already built: done as far as the title's count goes.
		const auto done = [this] {
			m_built_ahead.Built();
			return true;
		};
		// A program that is not built yet is asked for again at the next draw. Once the program
		// list is built, it never comes: the record is dropped, and a draw that needs the pipeline
		// builds it.
		const auto missing = [&] { return list_built ? done() : false; };
		if (record.stage_count == 0 && !record.pixel_present) {
			const auto program = m_program_cache->Listed(record.programs[0]);
			if (!program) {
				return missing();
			}
			auto bytes = std::span<const uint8_t>(record.fixed_state);
			job.compute.emplace();
			if (!ReadBytes(bytes, *job.compute) || !bytes.empty()) {
				return done();
			}
			if (m_compute_pipelines.contains(program->handle.id)) {
				return done();
			}
			job.compute->stage = program->stage;
			job.programs.pixel = program->handle;
			job.listed         = true;
			QueueComputeBuild(program->handle.id, std::move(job));
			return true;
		}
		for (uint32_t i = 0; i < record.stage_count; i++) {
			const auto program = m_program_cache->Listed(record.programs[i]);
			if (!program) {
				return missing();
			}
			auto bytes = std::span<const uint8_t>(record.stages[i]);
			if (!ReadBytes(bytes, job.vertex_info[i]) || !bytes.empty()) {
				return done(); // not from this layout: dropped
			}
			job.vertex_info[i].stage   = program->stage;
			job.programs.vertex[i]     = program->handle;
			key.vertex_shader_ids[i]   = program->handle.id;
		}
		job.vertex_count = record.stage_count;
		if (record.pixel_present) {
			const auto program = m_program_cache->Listed(record.programs[3]);
			if (!program) {
				return missing();
			}
			auto bytes = std::span<const uint8_t>(record.pixel);
			job.pixel.emplace();
			if (!ReadBytes(bytes, *job.pixel) || !bytes.empty()) {
				return done();
			}
			job.pixel->stage    = program->stage;
			job.programs.pixel  = program->handle;
			key.ps_shader_id    = program->handle.id;
		}
		auto fixed = std::span<const uint8_t>(record.fixed_state);
		if (!ReadBytes(fixed, key.rendering) || !ReadBytes(fixed, key.vertex_input) ||
		    !ReadBytes(fixed, key.static_params) || !fixed.empty()) {
			return done();
		}
		if (m_graphics_pipelines.contains(key)) {
			return done(); // a draw needed it before its programs were built ahead
		}
		job.listed        = true;
		job.rendering     = key.rendering;
		job.vertex_input  = key.vertex_input;
		job.static_params = key.static_params;
		QueueBuild(std::move(key), std::move(job));
		return true;
	});
}

void PipelineCache::Enqueue(PipelineJob job) {
	{
		std::lock_guard lock(m_jobs_mutex);
		m_jobs.push_back(std::move(job));
	}
	m_job_added.notify_one();
}

void PipelineCache::QueueBuild(GraphicsPipelineKey key, PipelineJob job) {
	auto cached = std::make_unique<PipelineEntry>();
	job.target  = cached.get();
	Enqueue(std::move(job));
	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);
}

void PipelineCache::QueueComputeBuild(uint64_t program_id, PipelineJob job) {
	auto cached = std::make_unique<PipelineEntry>();
	job.target  = cached.get();
	Enqueue(std::move(job));
	auto [iter, inserted] = m_compute_pipelines.emplace(program_id, std::move(cached));
	EXIT_IF(!inserted);
}

void PipelineCache::Promote(const PipelineEntry& entry) {
	std::lock_guard lock(m_jobs_mutex);
	const auto      it = std::ranges::find_if(
        m_jobs, [&entry](const PipelineJob& job) { return job.target == &entry; });
	if (it != m_jobs.end() && it != m_jobs.begin()) {
		auto job = std::move(*it);
		m_jobs.erase(it);
		m_jobs.push_front(std::move(job));
	}
}

PipelineCache::~PipelineCache() {
	// The workers first: a pipeline that is still queued is not built any more, and one that is
	// being built is finished before anything is destroyed.
	for (auto& builder: m_builders) {
		builder.request_stop();
	}
	m_builders.clear();
	if (m_builds != 0) {
		PipelineCacheLog("Pipelines: {} built on worker threads, {} ms each on average, the "
		                 "longest {} ms; {} draw(s) left out while their pipeline was built, {} "
		                 "waited within the frame's budget because their target was cleared or "
		                 "overwritten in that frame",
		                 m_builds, m_build_us / m_builds / 1000, m_longest_us / 1000,
		                 m_left_out_draws, m_reset_target_waits);
	}
	Save();
	if (m_pipeline_list != nullptr) {
		std::fclose(m_pipeline_list);
	}
	auto destroy = [this](const Pipeline& pipeline) {
		m_graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
		m_graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
		if (pipeline.mesh_compute != nullptr) {
			m_graphics.device.destroyPipeline(pipeline.mesh_compute, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline.mesh_compute_layout, nullptr);
		}
		m_graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
	};
	for (const auto& [key, entry]: m_graphics_pipelines) {
		(void)key;
		// Queued and never built: nothing to destroy.
		if (entry->ready.load(std::memory_order_acquire)) {
			destroy(entry->pipeline);
		}
	}
	for (const auto& [id, entry]: m_compute_pipelines) {
		(void)id;
		if (entry->ready.load(std::memory_order_acquire)) {
			destroy(entry->pipeline);
		}
	}
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
	for (const auto cache: m_builder_caches) {
		m_graphics.device.destroyPipelineCache(cache, nullptr);
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
	if (git_hash.ends_with("-dirty")) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (dirty build)");
		return;
	}
	if (m_graphics.host.faults.pipeline_cache_serializes_builds) {
		PipelineCacheLog("Vulkan pipeline cache: one in memory for each worker thread (the driver "
		                 "builds the pipelines of a cache one at a time; the game's pipeline list "
		                 "is built ahead instead)");
		return;
	}
	if (m_graphics.host.faults.pipeline_cache_load_is_slow) {
		// Measured: 76 s to load 75 MB. The in-memory cache still serves this session.
		PipelineCacheLog("Vulkan pipeline cache: not kept on disk (loading it would hold the "
		                 "start of the game; the game's pipeline list is built ahead instead)");
		vk::PipelineCacheCreateInfo create {};
		EXIT_NOT_IMPLEMENTED(m_graphics.device.createPipelineCache(&create, nullptr,
		                                                           &m_driver_cache) !=
		                     vk::Result::eSuccess);
		return;
	}

	m_driver_cache_path     = Config::GetPipelineCacheFolder() / (title_id + ".bin");
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

void PipelineCache::Save() {
	// The cache stays alive: worker threads may still build with it until the destructor stops
	// them.
	if (m_driver_cache == nullptr || m_driver_cache_saved || m_driver_cache_path.empty() ||
	    !m_lists_writable) {
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
	m_driver_cache_saved = true;
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const ShaderColorTarget, 8> color_targets, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	KYTY_PROFILER_FUNCTION();
	m_program_cache->Adopt();
	QueueListedPipelines();
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (mesh_active) {
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		// Without mesh shaders the guest shader runs as a compute shader (see
		// capturedVertexLayout.h).
		mesh.emulated = !m_graphics.host.capabilities.mesh_shader;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		bool within_limits = false;
		if (mesh.emulated) {
			const auto& limits = m_graphics.GetPhysicalDeviceProperties().limits;
			// Besides the guest LDS, the shader keeps the layers and the allocation in shared
			// memory.
			within_limits = host_threads <= limits.maxComputeWorkGroupInvocations &&
			                host_threads <= limits.maxComputeWorkGroupSize[0] &&
			                (mesh.lds_size_dwords + mesh.max_vertices + 2u) * sizeof(uint32_t) <=
			                    limits.maxComputeSharedMemorySize;
		} else {
			const auto& limits = m_graphics.mesh_shader_properties;
			within_limits =
			    host_threads <= limits.maxMeshWorkGroupInvocations &&
			    host_threads <= limits.maxMeshWorkGroupSize[0] &&
			    mesh.max_vertices <= limits.maxMeshOutputVertices &&
			    mesh.max_primitives <= limits.maxMeshOutputPrimitives &&
			    mesh.lds_size_dwords * sizeof(uint32_t) <= limits.maxMeshSharedMemorySize;
		}
		if (!within_limits) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params      = PrepareProgram(pixel_regs, sh, color_targets, pixel_info);
		if (!mesh_active && !tess_active && Prospero::IsRectList(user_config.GetPrimType())) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::Rectangle;
		} else if (context.GetModeControl().provoking_vtx_last) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::LastVertex;
		} else {
			pixel_info.parameter_mode = ShaderPixelParameterMode::FirstVertex;
		}
		pixel_info.flat_aliases_copied = m_graphics.host.faults.no_per_vertex_inputs;
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
			pixel_info.target_number_class[1]   = pixel_info.target_number_class[0];
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
				pixel_info.target_number_class[1]   = pixel_info.target_number_class[0];
			}
		}
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
	uint32_t push_data_cursor = !mesh_active ? 0
	                            : vertex_info[0].mesh.emulated
	                                ? ShaderRecompiler::IR::PushData::MeshEmulatedDrawDwordCount
	                                : ShaderRecompiler::IR::PushData::MeshDrawDwordCount;
	GraphicsPrograms  result;
	if (pixel_active && !m_graphics.host.capabilities.depth_bounds &&
	    context.GetDepthControl().depth_bounds_enable) {
		// The host has no depth bounds test: the pixel shader applies it to a copy of the depth
		// buffer (see EmitDepthBoundsTest).
		switch (context.GetDepthRenderTarget().z_info.format) {
			case Prospero::DepthFormat::kZ32F: pixel_info.ps_depth_bounds_format = 1; break;
			case Prospero::DepthFormat::kZ16: pixel_info.ps_depth_bounds_format = 2; break;
			default: EXIT("depth bounds test on an unsupported depth format\n");
		}
		pixel_info.ps_depth_bounds_dword = push_data_cursor;
		push_data_cursor += ShaderRecompiler::IR::PushData::DepthBoundsDwordCount;
	}
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
		// What the pixel shader reads both flat and interpolated, the last vertex stage writes
		// twice.
		std::vector<uint32_t> active_inputs;
		for (const auto& input: pixel_info.stage.program->info.inputs) {
			if (input.kind == ShaderRecompiler::IR::StageInputKind::Parameter) {
				active_inputs.push_back(input.location);
			}
		}
		auto& last_stage               = vertex_info[tess_active ? 2u : 0u];
		last_stage.param_copy_location = ShaderVertexParameterCopies(pixel_info, active_inputs);
		// A mesh program writes its parameters to records, which have one place for each.
		EXIT_NOT_IMPLEMENTED(mesh_active &&
		                     std::ranges::any_of(last_stage.param_copy_location,
		                                         [](uint8_t location) { return location != 0; }));
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	m_program_cache->Adopt();
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	input_info.lds_storage = input_info.lds_size_dwords * 4u >
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize;
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline* PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs, const DrawEffects& effects,
    uint64_t frame) {
	KYTY_PROFILER_FUNCTION();
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

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
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
				    bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1 : 0));
			}
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
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return WhenReady(*iter->second, effects, frame);
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

	LogPipelineTrace("CreatePipelineInternal queued", vs_id, ps_id);
	PipelineJob job;
	job.rendering    = rendering;
	job.vertex_input = key.vertex_input;
	EXIT_IF(vertex_info.size() > job.vertex_info.size());
	std::copy(vertex_info.begin(), vertex_info.end(), job.vertex_info.begin());
	job.vertex_count = static_cast<uint32_t>(vertex_info.size());
	if (ps_input_info != nullptr) {
		job.pixel = *ps_input_info;
	}
	job.programs      = programs;
	job.static_params = static_params;
	RememberPipeline(key, job);
	QueueBuild(key, std::move(job));
	return WhenReady(*m_graphics_pipelines.find(key)->second, effects, frame);
}

void PipelineCache::BuildPipelines(std::stop_token stop, uint32_t builder) {
	const auto driver_cache = m_builder_caches.empty() ? m_driver_cache : m_builder_caches[builder];
	for (;;) {
		PipelineJob job;
		{
			std::unique_lock lock(m_jobs_mutex);
			if (!m_job_added.wait(lock, stop, [this] { return !m_jobs.empty(); }) ||
			    stop.stop_requested()) {
				return;
			}
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
		}
		const auto begin = std::chrono::steady_clock::now();
		if (job.compute) {
			CreatePipelineInternal(m_graphics, job.target->pipeline, *job.compute,
			                       job.programs.pixel.module, driver_cache);
		} else {
			CreatePipelineInternal(m_graphics, job.target->pipeline, job.rendering, job.vertex_input,
			                       std::span {job.vertex_info.data(), job.vertex_count},
			                       job.pixel ? &*job.pixel : nullptr, job.programs,
			                       job.static_params, driver_cache);
		}
		EXIT_NOT_IMPLEMENTED(job.target->pipeline.pipeline == nullptr);
		EXIT_NOT_IMPLEMENTED(job.target->pipeline.pipeline_layout == nullptr);
		const auto took = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		                                            std::chrono::steady_clock::now() - begin)
		                                            .count());
		{
			std::lock_guard lock(m_jobs_mutex);
			m_builds++;
			m_build_us += took;
			m_longest_us = std::max(m_longest_us, took);
			job.target->ready.store(true, std::memory_order_release);
		}
		if (job.listed) {
			m_built_ahead.Built();
		}
		m_job_done.notify_all();
	}
}

PipelineCache::Pipeline* PipelineCache::WhenReady(PipelineEntry& entry, const DrawEffects& effects,
                                                  uint64_t frame) {
	const auto ready = [&entry] { return entry.ready.load(std::memory_order_acquire); };
	const auto leave_out = [this] {
		if (m_left_out_draws++ == 0) {
			PipelineCacheLog("Pipelines: a draw is left out until its pipeline is built "
			                 "(--pipeline-wait true waits instead)");
		}
		return nullptr;
	};
	// A short wait first (see GRACE_WAIT_US): once per pipeline and frame, within the frame's
	// budget.
	const auto grace_wait = [&]() -> Pipeline* {
		Promote(entry);
		const auto wait_us =
		    GraceWaitUs(entry.grace_wait_frame, frame, m_grace_wait_budget.Left(frame));
		if (wait_us == 0) {
			return leave_out();
		}
		entry.grace_wait_frame = frame;
		const auto begin       = std::chrono::steady_clock::now();
		bool       built       = false;
		{
			std::unique_lock lock(m_jobs_mutex);
			built = m_job_done.wait_for(lock, std::chrono::microseconds(wait_us), ready);
		}
		m_grace_wait_budget.Spend(
		    frame, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		                                     std::chrono::steady_clock::now() - begin)
		                                     .count()));
		return built ? &entry.pipeline : leave_out();
	};
	switch (PlanPipelineUse(effects, ready(), m_always_wait)) {
		case PipelineUse::Draw: return &entry.pipeline;
		case PipelineUse::Wait: {
			Promote(entry);
			std::unique_lock lock(m_jobs_mutex);
			m_job_done.wait(lock, ready);
			return &entry.pipeline;
		}
		case PipelineUse::WaitWithinBudget: {
			const auto left = m_reset_target_budget.Left(frame);
			if (left == 0) {
				// The frame's budget is spent: no worse off than a draw into a target not reset.
				return grace_wait();
			}
			m_reset_target_waits++;
			Promote(entry);
			const auto begin = std::chrono::steady_clock::now();
			bool       built = false;
			{
				std::unique_lock lock(m_jobs_mutex);
				built = m_job_done.wait_for(lock, std::chrono::microseconds(left), ready);
			}
			m_reset_target_budget.Spend(
			    frame, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			                                     std::chrono::steady_clock::now() - begin)
			                                     .count()));
			if (!built) {
				return grace_wait();
			}
			return &entry.pipeline;
		}
		case PipelineUse::LeaveOut: return grace_wait();
	}
	return nullptr;
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	// A dispatch that is left out would change the game's data: it always waits. The build runs
	// on a worker all the same, so it is in parallel with others, and the game's list builds it
	// ahead at the next start.
	const auto wait = [this](PipelineEntry& entry) -> Pipeline& {
		return *WhenReady(entry, DrawEffects {.writes_memory = true}, 0);
	};
	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		return wait(*iter->second);
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}
	PipelineJob job;
	job.compute         = input_info;
	job.programs.pixel  = compute_program;
	RememberComputePipeline(job);
	QueueComputeBuild(compute_program.id, std::move(job));
	return wait(*m_compute_pipelines.find(compute_program.id)->second);
}
} // namespace Libs::Graphics
