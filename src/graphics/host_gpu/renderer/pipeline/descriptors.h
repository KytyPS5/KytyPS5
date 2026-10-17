#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;

struct TextureDescDerivation;

// The state of a cached image that its lookups and acquisitions depend on. While an image keeps
// it (and the image set around its range is unchanged), looking it up again and acquiring its
// views finds the same image and views and changes nothing.
struct ImageLookupState {
	ImageId  id;
	ImageId  depth_id;
	uint64_t track_addr      = 0;
	uint64_t track_addr_end  = 0;
	bool     cpu_dirty       = false;
	bool     buffer_modified = false;
	bool     gpu_modified    = false;
	bool     render_target   = false;

	bool operator==(const ImageLookupState&) const = default;

	[[nodiscard]] static ImageLookupState Of(ImageId id, const Image& image) {
		return {.id              = id,
		        .depth_id        = image.depth_id,
		        .track_addr      = image.track_addr,
		        .track_addr_end  = image.track_addr_end,
		        .cpu_dirty       = image.IsCpuDirty(),
		        .buffer_modified = image.IsBufferModified(),
		        .gpu_modified    = image.IsGpuModified(),
		        .render_target   = image.usage.render_target};
	}
};

// The image state a bindless table element's view was last acquired in: while the image keeps
// it, the view still holds and acquiring it again would only refresh the image's use.
struct TableViewMemo {
	ImageLookupState state;
	bool             valid = false;
};

struct TextureBinding {
	ImageId                    image_id;
	vk::ImageView              image_view = nullptr;
	TextureCache::ImageDesc    desc;
	vk::ImageLayout            layout = vk::ImageLayout::eUndefined;
	std::vector<vk::ImageView> mip_views;
	// The description whose last lookup this binding reuses (image_view is set) or records once
	// its view is acquired; see RenderExecutor::ResolveTexture().
	TextureDescDerivation* lookup = nullptr;
	bool                   reused = false;
};

struct PreparedBindings {
	struct BufferSource {
		uint64_t address = 0;
		uint64_t size    = 0;
		BufferId id;
	};

	// The draw owns the immutable compiled-program/runtime-snapshot association through commit.
	const ShaderStageRuntime* runtime = nullptr;
	// Keep the resolved guest range through cache preparation; only the host buffer ID may
	// become stale and need resolving again when bindings are rebound.
	std::vector<BufferSource>             buffer_sources;
	std::vector<vk::DescriptorBufferInfo> buffers;
	// The uniform views of the buffers that have a uniform_index, by that index.
	std::vector<vk::DescriptorBufferInfo> uniform_buffers;
	std::vector<TextureBinding>           images;
	// The distinct bindless table elements, with the T# and the root image each was resolved
	// for, and, for every slot of every table range in binding order, the element it binds.
	// GTA V's ray tracing tables have ~16k slots that name far fewer distinct textures. The
	// bindings live in the executor's table resolutions (see
	// RenderExecutor::TableElementResolution), which keep them until ResetBindings().
	std::vector<TextureBinding*>                        table_images;
	std::vector<TableViewMemo*>                         table_views;
	std::vector<ShaderRecompiler::IR::DescriptorValue> table_sources;
	std::vector<uint32_t>                               table_roots;
	// The change counter of each element (RenderExecutor::TableElementResolution::version_slot).
	std::vector<uint32_t>                               table_versions;
	std::vector<uint32_t>                               table_slots;
	// The program's table binding state (RenderExecutor::TableBindingState). When
	// `tables_reused`, the table vectors above stay empty: the tables bind what the state holds,
	// `table_visits` lists the state's elements this binding looks up again, and
	// `table_moved_slots` the slots whose T# changed to that of another element of the state.
	void*                                               table_state   = nullptr;
	bool                                                tables_reused = false;
	std::vector<uint32_t>                               table_visits;
	std::vector<uint32_t>                               table_moved_slots;
	std::vector<vk::Sampler>              samplers;
	vk::DescriptorBufferInfo              gds {nullptr, 0, VK_WHOLE_SIZE};
	vk::DescriptorBufferInfo              flattened_srt;
	vk::DescriptorBufferInfo              shader_data_buffer;
	vk::DescriptorBufferInfo              shared_memory;
	std::vector<uint32_t>                 shader_data;
	// What the previous draw of this stage bound: a stage that binds the same program with the
	// same T#s and S#s again (about half of them) skips the description lookups.
	const void*                                        last_program          = nullptr;
	uint64_t                                           last_descs_generation = 0;
	std::vector<ShaderRecompiler::IR::DescriptorValue> last_images;
	std::vector<ShaderRecompiler::IR::DescriptorValue> last_samplers;
	// The description each T# of `last_images` has (null when its lookups are not reused).
	std::vector<TextureDescDerivation*>                image_descs;
};

[[nodiscard]] vk::DescriptorType
NativeDescriptorType(ShaderRecompiler::IR::DescriptorBindingKind kind);
[[nodiscard]] vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture,
                                                    uint32_t              element = 0);

template <typename T>
[[nodiscard]] T DecodeNativeDescriptor(const ShaderRecompiler::IR::DescriptorValue& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	static_assert(sizeof(T) % sizeof(uint32_t) == 0);
	T result {};
	EXIT_IF(value.dword_count < sizeof(result) / sizeof(uint32_t));
	std::memcpy(&result, value.dwords.data(), sizeof(result));
	return result;
}

[[nodiscard]] bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor,
                                                   bool r128 = false);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
