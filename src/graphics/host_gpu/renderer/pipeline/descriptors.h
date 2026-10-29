#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "common/assert.h"
#include "common/hash.h"
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

// What a texture descriptor decodes to before the texture cache looks up its image (see
// RenderExecutor::DecodeTexture). It depends only on the descriptor and on how the shader uses the
// texture, so a frame that binds the same texture thousands of times decodes it once.
struct DecodedTexture {
	TextureCache::ImageDesc desc;
	bool                    null_descriptor   = false;
	// Rebased onto the mips that have backing (a PRT texture streams): decoded again when guest
	// backing changes (kernel BackingGeneration).
	bool                    sparse            = false;
	uint64_t                backing_generation = 0;
	uint64_t                probe_address     = 0; // the first page after the backed ones
	bool                    shader_conversion = false;
	vk::Format              pixel_format      = vk::Format::eUndefined;
	vk::Format              view_format       = vk::Format::eUndefined;
	// The image a plain backing match found for the description (TextureCache::FindImage), and
	// the image bound for it (the depth owner of a stencil plane): kept until an image over the
	// description's range changes (TextureCache::RangeUnchangedSince from image_since).
	mutable ImageId  found_image;
	mutable ImageId  bound_image;
	mutable uint64_t image_since = 0;
};

struct TextureBinding {
	ImageId                    image_id;
	vk::ImageView              image_view = nullptr;
	// The decoded texture, which RenderExecutor keeps until the frame ends, and its view.
	const DecodedTexture*      texture = nullptr;
	ImageViewInfo              view_info;
	TextureCache::BindingType  type   = TextureCache::BindingType::Texture;
	vk::ImageLayout            layout = vk::ImageLayout::eUndefined;
	std::vector<vk::ImageView> mip_views;

	// The whole description: the decoded texture's, with the binding's view.
	[[nodiscard]] TextureCache::ImageDesc Desc() const {
		EXIT_IF(texture == nullptr);
		auto desc      = texture->desc;
		desc.view_info = view_info;
		desc.type      = type;
		return desc;
	}
};

struct TextureDecodeKey {
	ShaderRecompiler::IR::DescriptorValue value;
	ShaderRecompiler::IR::ImageResource   resource;

	bool operator==(const TextureDecodeKey& other) const = default;
};

// A lookup of the table by reference: the key, with its resource's vectors, is copied only when
// it is added.
struct TextureDecodeView {
	const ShaderRecompiler::IR::DescriptorValue& value;
	const ShaderRecompiler::IR::ImageResource&   resource;
};

struct TextureDecodeKeyHash {
	using is_transparent = void;

	size_t operator()(const TextureDecodeKey& key) const noexcept {
		return (*this)(TextureDecodeView {key.value, key.resource});
	}
	size_t operator()(const TextureDecodeView& key) const noexcept {
		uint64_t hash = key.value.dword_count;
		for (const auto dword: key.value.dwords) {
			hash = Common::Fnv64Step(hash, dword);
		}
		// Every field the keys are compared by: one texture that many shaders read (each its
		// own first use, descriptor index) would otherwise fill a single bucket.
		const auto& r = key.resource;
		hash = Common::Fnv64Step(hash, r.source);
		hash = Common::Fnv64Step(hash, r.descriptor_index);
		hash = Common::Fnv64Step(hash, r.first_use_pc);
		hash = Common::Fnv64Step(hash, static_cast<uint32_t>(r.resource_class) |
		                                   static_cast<uint32_t>(r.numeric_class) << 8u |
		                                   static_cast<uint32_t>(r.dimension) << 16u |
		                                   static_cast<uint32_t>(r.mip_mode) << 24u);
		hash = Common::Fnv64Step(hash, r.mip_count);
		hash = Common::Fnv64Step(hash, static_cast<uint32_t>(r.conversion_format));
		hash = Common::Fnv64Step(hash, r.shader_swizzle);
		hash = Common::Fnv64Step(hash, uint32_t {r.read} | uint32_t {r.written} << 1u |
		                                   uint32_t {r.atomic} << 2u | uint32_t {r.atomic64} << 3u |
		                                   uint32_t {r.unfiltered_access_only} << 4u |
		                                   uint32_t {r.depth_compare} << 5u |
		                                   uint32_t {r.cube} << 6u | uint32_t {r.r128} << 7u);
		hash = Common::Fnv64Step(hash, r.indirect_root);
		hash = Common::Fnv64Step(hash, r.indirect_mapping_offset);
		hash = Common::Fnv64Step(hash, r.indirect_search_iterations);
		for (const auto index: r.indirect_resources) {
			hash = Common::Fnv64Step(hash, index);
		}
		return static_cast<size_t>(hash);
	}
};

struct TextureDecodeKeyEqual {
	using is_transparent = void;

	bool operator()(const TextureDecodeKey& a, const TextureDecodeKey& b) const { return a == b; }
	bool operator()(const TextureDecodeView& a, const TextureDecodeKey& b) const {
		return a.value == b.value && a.resource == b.resource;
	}
	bool operator()(const TextureDecodeKey& a, const TextureDecodeView& b) const {
		return (*this)(b, a);
	}
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
	std::vector<TextureBinding>           images;
	std::vector<vk::Sampler>              samplers;
	vk::DescriptorBufferInfo              gds {nullptr, 0, VK_WHOLE_SIZE};
	vk::DescriptorBufferInfo              flattened_srt;
	vk::DescriptorBufferInfo              shader_data_buffer;
	vk::DescriptorBufferInfo              shared_memory;
	std::vector<uint32_t>                 shader_data;
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
