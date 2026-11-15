#include "graphics/host_gpu/renderer/drawTemplates.h"

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/shader/recompiler/ir/ResourceSnapshot.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <xxhash.h>

namespace Libs::Graphics {

uint64_t DrawResourceKey(std::span<const ShaderStageRuntime* const> stages) {
	if (stages.empty()) {
		return 0;
	}
	// The words go through one buffer: a few hundred bytes a draw, hashed at once.
	thread_local std::vector<uint32_t> words;
	words.clear();
	const auto add_pointer = [&](const void* pointer) {
		const auto value = reinterpret_cast<uint64_t>(pointer);
		words.push_back(static_cast<uint32_t>(value));
		words.push_back(static_cast<uint32_t>(value >> 32u));
	};
	const auto add_values = [&](const std::vector<ShaderRecompiler::IR::DescriptorValue>& values) {
		words.push_back(static_cast<uint32_t>(values.size()));
		for (const auto& value: values) {
			words.push_back(value.dword_count);
			words.insert(words.end(), value.dwords.begin(), value.dwords.begin() + value.dword_count);
		}
	};
	for (const auto* stage: stages) {
		add_pointer(stage->module);
		const auto& snapshot = *stage->resources;
		add_values(snapshot.images);
		add_values(snapshot.samplers);
		words.push_back(static_cast<uint32_t>(snapshot.image_tables.size()));
		for (const auto& table: snapshot.image_tables) {
			words.push_back(static_cast<uint32_t>(table.slots_id));
			words.push_back(static_cast<uint32_t>(table.slots_id >> 32u));
		}
	}
	const auto key = XXH3_64bits(words.data(), words.size() * sizeof(uint32_t));
	return key != 0 ? key : 1;
}

uint64_t DrawContextKey(const HW::Context& context) {
	// A copy without the registers only dynamic state reads.
	thread_local HW::Context copy;
	copy = context;
	const auto clear = [&](const void* field, size_t size) {
		const auto offset = static_cast<const uint8_t*>(field) - reinterpret_cast<const uint8_t*>(&context);
		std::memset(reinterpret_cast<uint8_t*>(&copy) + offset, 0, size);
	};
	clear(&context.GetScreenViewport(), sizeof(HW::ScreenViewport));
	clear(&context.GetUserClipPlanes(), sizeof(context.GetUserClipPlanes()));
	copy.SetBlendColor({});
	copy.SetPolyOffset({});
	copy.SetStencilMask({});
	copy.SetDepthBoundsMin(0.0f);
	copy.SetDepthBoundsMax(0.0f);
	return XXH3_64bits(&copy, sizeof(copy));
}

uint64_t DrawUserConfigKey(const HW::UserConfig& user_config) {
	thread_local HW::UserConfig copy;
	copy = user_config;
	copy.SetIndexOffset(0);
	copy.SetObjectId(0);
	return XXH3_64bits(&copy, sizeof(copy));
}

uint64_t DrawStateKey(uint64_t resource_key, uint64_t context_key, uint64_t user_config_key,
                      const HW::Shader& shaders, const ShaderVertexInputInfo& vertex_input) {
	const auto& vs = shaders.GetVs();
	const auto& ps = shaders.GetPs();
	const std::array<uint64_t, 3> keys {resource_key, context_key, user_config_key};
	auto hash = XXH3_64bits(keys.data(), sizeof(keys));
	// The program addresses and resource registers, not the user data addresses (where the stages
	// read their user data from: a different place every frame).
	const std::array<uint64_t, 5> programs {vs.es_regs.data_addr, vs.ls_regs.data_addr, vs.hs_regs.data_addr,
	                                        vs.gs_regs.data_addr, ps.ps_regs.data_addr};
	hash = XXH3_64bits_withSeed(programs.data(), sizeof(programs), hash);
	hash = XXH3_64bits_withSeed(&vs.hs_regs.rsrc1, sizeof(vs.hs_regs.rsrc1), hash);
	hash = XXH3_64bits_withSeed(&vs.hs_regs.rsrc2, sizeof(vs.hs_regs.rsrc2), hash);
	hash = XXH3_64bits_withSeed(&vs.gs_regs.rsrc1, sizeof(vs.gs_regs.rsrc1), hash);
	hash = XXH3_64bits_withSeed(&vs.gs_regs.rsrc2, sizeof(vs.gs_regs.rsrc2), hash);
	hash = XXH3_64bits_withSeed(&ps.ps_regs.rsrc1, sizeof(ps.ps_regs.rsrc1), hash);
	hash = XXH3_64bits_withSeed(&ps.ps_regs.rsrc2, sizeof(ps.ps_regs.rsrc2), hash);
	// The vertex input layout, as the pipeline takes it.
	std::array<uint32_t, 2 + 2 * ShaderVertexInputInfo::RES_MAX * 2> layout {};
	size_t count = 0;
	const auto buffers    = std::clamp(vertex_input.buffers_num, 0, ShaderVertexInputInfo::RES_MAX);
	const auto attributes = std::clamp(vertex_input.resources_num, 0, ShaderVertexInputInfo::RES_MAX);
	layout[count++] = static_cast<uint32_t>(buffers);
	layout[count++] = static_cast<uint32_t>(attributes);
	for (int binding = 0; binding < buffers; binding++) {
		layout[count++] = vertex_input.buffers[binding].stride;
		layout[count++] = vertex_input.buffers[binding].fetch_index;
	}
	for (int attribute = 0; attribute < attributes; attribute++) {
		const auto binding = vertex_input.resources_dst[attribute].buffer_index;
		const auto base    = binding >= 0 && binding < buffers ? vertex_input.buffers[binding].addr : 0;
		layout[count++]    = static_cast<uint32_t>(vertex_input.resources[attribute].Base48() - base);
		layout[count++]    = static_cast<uint32_t>(binding);
	}
	hash = XXH3_64bits_withSeed(layout.data(), count * sizeof(uint32_t), hash);
	return hash != 0 ? hash : 1;
}

static int DrawTemplatesMode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_DRAW_TEMPLATES");
		return value == nullptr || value[0] == 0 || value[0] == '0' ? 0 : value[0] == '2' ? 2 : 1;
	}();
	return mode;
}

bool DrawTemplatesEnabled() {
	return DrawTemplatesMode() != 0;
}

bool DrawStateTemplatesEnabled() {
	return DrawTemplatesMode() == 2;
}

} // namespace Libs::Graphics
