#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEMANIFEST_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEMANIFEST_H_

#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// A title's pipeline manifest: how the shader modules and pipelines of earlier sessions were asked
// for, in the guest's terms (shader hashes, stage input state, resource specializations, pipeline
// state) and without any code, translated or not. A session that has one compiles them as soon as
// the guest maps their shaders, ahead of the draws that need them. It is text, one record per line
// with named fields (pipelineManifest.cpp), so that git keeps readable diffs of it. Its bytes do
// not depend on the emulator build or on the host, and the same contents always give the same
// bytes.
namespace Libs::Graphics {

// Changes only when a field that files hold changes meaning: a new field reads its default from
// older files. Files of another format are ignored.
inline constexpr uint32_t ManifestFormat = 1;

// A shader module: its program's key, the stage input state it was compiled with and its
// resource specialization.
struct ManifestModule {
	ShaderType stage = ShaderType::Unknown;
	// The hash of the mapped code (ShaderMapUserData) and of a GS back half (0 without one).
	uint64_t code_hash        = 0;
	uint64_t back_hash        = 0;
	uint32_t user_data_count  = 0;
	uint32_t push_data_cursor = 0;
	// By stage: Vertex, Local, Mesh, TessellationControl and TessellationEvaluation read the
	// vertex input.
	std::variant<ShaderVertexInputInfo, ShaderPixelInputInfo, ShaderComputeInputInfo> input;
	ShaderRecompiler::IR::ResourceSpecialization specialization;

	// The program hash of the module's cache key: the code hash, or both hashes' hash for a GS
	// front and back pair.
	[[nodiscard]] uint64_t ProgramHash() const;
};

// A graphics pipeline. Compute pipelines are not listed: every compute module has one.
struct ManifestPipeline {
	static constexpr uint32_t NoStage = UINT32_MAX;

	// Module indices in the order of GraphicsPipelineKey::stage_ids.
	std::array<uint32_t, 4> stages {NoStage, NoStage, NoStage, NoStage};
	// The pixel stage is its module's plain variant (ShaderModule::plain).
	bool plain_pixel = false;
	// The key with zero stage ids.
	GraphicsPipelineKey key {};
};

struct PipelineManifest {
	std::vector<ManifestModule>                                   modules;
	std::vector<ManifestPipeline>                                 pipelines;
	std::vector<std::pair<uint64_t, ShaderTessellationInputInfo>> tessellation_layouts;
};

// Clears what never reaches a module from a recorded stage input: guest addresses, per-dispatch
// sizes and host limits (PipelineCache sets those of the running host again).
void NormalizeManifestInput(ManifestModule& module);

// The file: a header line, then the records in their canonical order.
[[nodiscard]] std::vector<uint8_t> SaveManifest(const PipelineManifest& manifest);
// False, with `manifest` empty, for another format or damaged bytes.
// Records that do not read (or name modules that do not) are left out and counted in `skipped`,
// as are the pipelines that no draw creates from the modules they name (stages that a draw does
// not link, or a vertex input or patch size that is not their modules'): `unlinked` gets their
// stage fields and why.
bool LoadManifest(std::span<const uint8_t> bytes, PipelineManifest& manifest,
                  uint32_t* skipped = nullptr, std::vector<std::string>* unlinked = nullptr);

// Equal for the records of one module or of one pipeline, whatever stage input fields outside
// the static key they hold. `static_state` is the stage's static key on the running host.
[[nodiscard]] uint64_t ManifestModuleIdentity(const ManifestModule&     module,
                                              std::span<const uint32_t> static_state);
[[nodiscard]] uint64_t ManifestPipelineIdentity(const ManifestPipeline& pipeline);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEMANIFEST_H_
