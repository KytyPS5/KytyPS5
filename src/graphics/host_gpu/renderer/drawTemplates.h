#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DRAWTEMPLATES_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DRAWTEMPLATES_H_

#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;
struct ShaderVertexInputInfo;

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

// A stage of a draw template (see DrawTemplate).
struct DrawTemplateStage {
	const void* program = nullptr;
	// The T#s and S#s the bindings were derived from (compared again on use).
	std::vector<ShaderRecompiler::IR::DescriptorValue> image_values;
	std::vector<ShaderRecompiler::IR::DescriptorValue> sampler_values;
	// The image bindings as the draw committed them: views and read layouts.
	std::vector<TextureBinding> images;
	// The version of each image (TextureCache::ImageVersion()) after the commit.
	std::vector<uint32_t>    image_versions;
	std::vector<vk::Sampler> samplers;
	// The texture cache's image set epoch the images were last found current in.
	uint64_t image_set_epoch = 0;
	// The garbage collector tick the images were last touched in.
	uint64_t touched_tick = 0;
};

// Draw templates. Nearly every draw repeats the programs, T#s, S#s and buffer layouts of an
// earlier draw (GTA V: all but a handful a frame; Demon's Souls: 95 %); what changes from one
// draw to the next is where its constants and vertices are (V# base addresses, user data,
// flattened SRT dwords). A template keeps the image bindings and samplers the first draw of its
// kind committed, so the next draws of the kind bind them without looking any T# up, acquiring
// any view or transiting any image: they hold while every image's version is the one it had
// after that commit (the image is the same registered one, in the same read layout and state)
// and no image was registered over its range since.
struct DrawTemplate {
	uint64_t                         key         = 0;
	uint32_t                         stage_count = 0;
	// The tick a draw of the kind last failed to store the template in (0: none): it is not
	// stored again in that tick.
	uint64_t                         refused_tick = 0;
	std::array<DrawTemplateStage, 4> stages;

	// With state keys (KYTY_DRAW_TEMPLATES=2) the key also names every register the draw's
	// pipeline, targets and bindings derive from (DrawStateKey()) and the target images, so the
	// template keeps the pipeline (a PipelineCache::Pipeline; null: looked up by the draw).
	void* pipeline = nullptr;
	// The render targets as a draw acquired them from the target memo (AcquireRenderTargets()):
	// the rendering state and feedback aspects it gave, and the attachment layout, access and
	// version of each target image after its transition. While the draw reuses the memo's
	// targets, its stages bind the template's images and every target keeps its version, a draw
	// of the template acquires them the same way: it takes these instead.
	bool                                                       targets_valid = false;
	RenderState                                                rendering;
	vk::ImageAspectFlags                                       feedback_aspects;
	uint32_t                                                   target_count = 0;
	std::array<uint32_t, RENDER_COLOR_ATTACHMENTS_MAX + 1>     target_images {};
	std::array<uint32_t, RENDER_COLOR_ATTACHMENTS_MAX + 1>     target_versions {};
	std::array<vk::ImageLayout, RENDER_COLOR_ATTACHMENTS_MAX + 1>  target_layouts {};
	std::array<vk::AccessFlags2, RENDER_COLOR_ATTACHMENTS_MAX + 1> target_access {};

	// Template commits (push descriptors): the descriptors a draw of the template pushed, as the
	// data of an update template with the images and samplers in place. The buffer views change
	// from draw to draw: each slot names where a binding kind of a stage goes in the data. Valid
	// while the template holds its images, for the pipeline layout it was made for.
	struct CommitSlot {
		uint8_t  stage  = 0;
		uint8_t  kind   = 0;
		uint16_t count  = 0;
		uint32_t offset = 0;
	};
	bool                         commit_valid = false;
	vk::PipelineLayout           commit_layout;
	vk::DescriptorUpdateTemplate commit_template;
	std::vector<uint8_t>         commit_data;
	std::vector<CommitSlot>      commit_slots;
};

// The templates of the execution thread, by key: an open-addressed table that drops everything
// when it fills (templates are rebuilt by the next draw of their kind). Dropped templates keep
// their storage for the next ones (a slot with key 0 is free).
class DrawTemplateTable {
public:
	static constexpr uint32_t Size = 1u << 14u;

	DrawTemplateTable(): m_slots(Size) {}

	[[nodiscard]] DrawTemplate* Find(uint64_t key) {
		for (uint32_t i = Index(key), probes = 0; probes < MaxProbes; i = (i + 1) & (Size - 1), probes++) {
			auto* slot = m_slots[i].get();
			if (slot == nullptr || slot->key == 0) {
				return nullptr;
			}
			if (slot->key == key) {
				return slot;
			}
		}
		return nullptr;
	}

	// A template for `key`, reused or new (its contents are the caller's to fill).
	[[nodiscard]] DrawTemplate& Insert(uint64_t key) {
		if (m_count >= Size / 2) {
			Clear();
		}
		for (uint32_t i = Index(key), probes = 0; probes < MaxProbes; i = (i + 1) & (Size - 1), probes++) {
			auto& slot = m_slots[i];
			if (slot == nullptr || slot->key == 0) {
				if (slot == nullptr) {
					slot = std::make_unique<DrawTemplate>();
				}
				slot->key           = key;
				slot->stage_count   = 0;
				slot->pipeline      = nullptr;
				slot->targets_valid = false;
				slot->commit_valid  = false;
				m_count++;
				return *slot;
			}
			if (slot->key == key) {
				return *slot;
			}
		}
		Clear();
		return Insert(key);
	}

	void Clear() {
		for (auto& slot: m_slots) {
			if (slot != nullptr) {
				slot->key = 0;
			}
		}
		m_count = 0;
	}

private:
	static constexpr uint32_t MaxProbes = 64;

	[[nodiscard]] static uint32_t Index(uint64_t key) {
		return static_cast<uint32_t>((key * 0x9e3779b97f4a7c15ull) >> 50u) & (Size - 1);
	}

	std::vector<std::unique_ptr<DrawTemplate>> m_slots;
	uint32_t                                   m_count = 0;
};

// What a draw's image and sampler bindings derive from: the programs, T#s, S#s and bindless
// tables of its stages. 0 when there are no stages.
[[nodiscard]] uint64_t DrawResourceKey(std::span<const ShaderStageRuntime* const> stages);

// The context registers but those only dynamic state reads (viewports and scissors, blend color,
// depth bounds, depth bias values, stencil masks and references, user clip planes), hashed: draws
// that differ only in those have the same pipeline, targets and bindings.
[[nodiscard]] uint64_t DrawContextKey(const HW::Context& context);

// The user configuration registers but the index offset and object id (per-draw values the draw
// passes to its shaders), hashed.
[[nodiscard]] uint64_t DrawUserConfigKey(const HW::UserConfig& user_config);

// A draw's state key: its resource key, the context and user configuration keys (see
// DrawContextKey() and DrawUserConfigKey()), its shader stage registers but the user data
// addresses and its vertex input layout (strides, step rates and attribute offsets in their
// buffers).
[[nodiscard]] uint64_t DrawStateKey(uint64_t resource_key, uint64_t context_key, uint64_t user_config_key,
                                    const HW::Shader& shaders, const ShaderVertexInputInfo& vertex_input);

// KYTY_DRAW_TEMPLATES=1 turns draw templates on; =2 also keys them by the draw's registers and
// takes the pipeline and the render targets from them.
[[nodiscard]] bool DrawTemplatesEnabled();
[[nodiscard]] bool DrawStateTemplatesEnabled();

} // namespace Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DRAWTEMPLATES_H_
