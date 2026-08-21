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

namespace ShaderRecompiler::IR {
struct ResourceSnapshot;
}

struct BufferView {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::DeviceSize range  = VK_WHOLE_SIZE;
};

struct TextureBinding {
	ImageId                    image_id;
	vk::ImageView              image_view = nullptr;
	TextureCache::ImageDesc    desc;
	vk::ImageLayout            layout = vk::ImageLayout::eUndefined;
	std::vector<vk::ImageView> mip_views;
};

struct NativeDescriptors {
	std::vector<BufferView>     buffers;
	std::vector<TextureBinding> images;
	std::vector<vk::Sampler>    samplers;
	std::vector<BufferView>     addresses;
	BufferView                  gds;
	BufferView                  flattened_srt;
	BufferView                  user_data;
};

struct PreparedBindings {
	const ShaderRecompiler::IR::Program*          program  = nullptr;
	const ShaderRecompiler::IR::ResourceSnapshot* snapshot = nullptr;
	NativeDescriptors                             resources;
	std::vector<BufferId>                         buffer_ids;
	std::vector<BufferId>                         address_ids;
	std::vector<uint32_t>                         flattened_srt;
	std::vector<uint32_t>                         user_data;
	bool                                          committed = false;
};

[[nodiscard]] vk::DescriptorType
NativeDescriptorType(ShaderRecompiler::IR::DescriptorBindingKind kind);
[[nodiscard]] uint32_t
NativeDescriptorCount(const ShaderRecompiler::IR::DescriptorBinding& binding);
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

struct TargetTextureViewInfo {
	vk::ImageViewType type        = static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM);
	uint32_t          base_layer  = 0;
	uint32_t          layer_count = 0;
};

[[nodiscard]] TargetTextureViewInfo
ResolveTargetTextureView(const ShaderRecompiler::IR::ImageResource& resource,
                         Prospero::ImageType type, uint32_t base_layer, uint32_t image_layers);

[[nodiscard]] bool IsSupportedDepthTargetDescriptor(const ShaderTextureResource& descriptor,
                                                    const Image& image, bool r128 = false);
[[nodiscard]] bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor,
                                                   const Image& image, bool r128 = false);
[[nodiscard]] bool
IsSupportedSampledVideoOutView(const ShaderRecompiler::IR::ImageResource& resource,
                               const ShaderTextureResource& descriptor, const Image& image);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
