#include "graphics/host_gpu/renderer/descriptors.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/host_gpu/renderer/shaderResourceBarrier.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/presentation/displayBuffer.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <vulkan/vk_enum_string_helper.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace Libs::Graphics {

using TextureVariant = DescriptorCache::TextureVariant;

static void BindNullStorageBuffer(CommandBuffer* cmd_buffer, BufferView* dst) {
	EXIT_IF(cmd_buffer == nullptr || dst == nullptr);

	dst->buffer =
	    g_render_ctx->GetBufferCache()->ObtainNullBuffer(cmd_buffer, g_render_ctx->GetGraphicCtx());
	dst->offset = 0;
	dst->range  = 16;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static const char* VulkanImageTypeName(VulkanImageType type) {
	switch (type) {
		case VulkanImageType::VideoOut: return "VideoOut";
		case VulkanImageType::DepthStencil: return "DepthStencil";
		case VulkanImageType::Texture: return "Texture";
		case VulkanImageType::StorageTexture: return "StorageTexture";
		case VulkanImageType::RenderTexture: return "RenderTexture";
		case VulkanImageType::Unknown:
		default: return "Unknown";
	}
}

VkImageAspectFlags DepthStencilAspectMask(VkFormat format) {
	auto ret = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT);
	if (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
		ret |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	return ret;
}

static int SampledArrayViewIndex(const VulkanImage* image, int view_index) {
	EXIT_IF(image == nullptr);

	switch (view_index) {
		case VulkanImage::VIEW_DEFAULT: return VulkanImage::VIEW_DEFAULT_ARRAY;
		case VulkanImage::VIEW_BGRA: return VulkanImage::VIEW_BGRA_ARRAY;
		case VulkanImage::VIEW_DEPTH_TEXTURE: return VulkanImage::VIEW_DEPTH_TEXTURE_ARRAY;
		case VulkanImage::VIEW_STENCIL_TEXTURE: return VulkanImage::VIEW_STENCIL_TEXTURE_ARRAY;
		case VulkanImage::VIEW_R001: return VulkanImage::VIEW_R001_ARRAY;
		case VulkanImage::VIEW_RGB1: return VulkanImage::VIEW_RGB1_ARRAY;
		case VulkanImage::VIEW_R000: return VulkanImage::VIEW_R000_ARRAY;
		case VulkanImage::VIEW_RG01: return VulkanImage::VIEW_RG01_ARRAY;
		case VulkanImage::VIEW_000R: return VulkanImage::VIEW_000R_ARRAY;
		default: return view_index;
	}
}

static int SelectSampledTextureArrayView(const VulkanImage* image, int base_view) {
	const int array_view = SampledArrayViewIndex(image, base_view);
	if (image->image_view[array_view] == nullptr) {
		EXIT("missing sampled array image view: image_type=%s base_view=%d array_view=%d "
		     "layers=%u\n",
		     VulkanImageTypeName(image->type), base_view, array_view, image->layers);
	}
	return array_view;
}

static bool TextureVariantIsUint(TextureVariant variant) {
	return variant == TextureVariant::Uint2D || variant == TextureVariant::UintArray ||
	       variant == TextureVariant::Uint3D;
}

static bool TextureVariantIsArray(TextureVariant variant) {
	return variant == TextureVariant::FloatArray || variant == TextureVariant::UintArray;
}

static bool TextureVariantIs3D(TextureVariant variant) {
	return variant == TextureVariant::Float3D || variant == TextureVariant::Uint3D;
}

static int TextureVariantDefaultView(TextureVariant variant) {
	return TextureVariantIsArray(variant) ? VulkanImage::VIEW_DEFAULT_ARRAY
	                                      : VulkanImage::VIEW_DEFAULT;
}

static VulkanImage* GetDummySampledTexture(TextureVariant variant) {
	return g_render_ctx->GetTextureCache()->GetDummySampledTexture(TextureVariantIsUint(variant),
	                                                               TextureVariantIs3D(variant));
}

static VulkanImage* GetDummyStorageTexture(TextureVariant variant) {
	return g_render_ctx->GetTextureCache()->GetDummyStorageTexture(TextureVariantIsUint(variant),
	                                                               TextureVariantIs3D(variant));
}

static ShaderBufferResource
NativeBufferDescriptor(const ShaderRecompiler::IR::DescriptorValue& descriptor) {
	ShaderBufferResource result;
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return result;
}

static ShaderTextureResource
NativeTextureDescriptor(const ShaderRecompiler::IR::DescriptorValue& descriptor) {
	ShaderTextureResource result;
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return result;
}

static ShaderSamplerResource
NativeSamplerDescriptor(const ShaderRecompiler::IR::DescriptorValue& descriptor) {
	ShaderSamplerResource result;
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return result;
}

static BufferView NativeStorageBuffer(uint64_t submit_id, CommandBuffer* command_buffer,
                                      const ShaderBufferResource&                 descriptor,
                                      const ShaderRecompiler::IR::BufferResource& resource) {
	BufferView result;
	const auto address = descriptor.Base48();
	const auto stride  = descriptor.Stride();
	const auto records = descriptor.NumRecords();
	if (stride != 0 && records > UINT64_MAX / stride) {
		EXIT("storage buffer descriptor footprint overflow\n");
	}
	const auto size = stride != 0 ? static_cast<uint64_t>(stride) * records : records;
	if (address == 0 || size == 0) {
		BindNullStorageBuffer(command_buffer, &result);
		return result;
	}
	auto* const ctx       = g_render_ctx->GetGraphicCtx();
	const auto  alignment = ctx->StorageMinAlignment();
	if (alignment == 0 || size > ctx->GetPhysicalDeviceProperties().limits.maxStorageBufferRange ||
	    BufferCache::CACHING_PAGE_SIZE % alignment != 0) {
		EXIT("storage buffer range or device alignment is unsupported\n");
	}
	(void)submit_id;
	auto binding = g_render_ctx->GetBufferCache()->ObtainBuffer(
	    command_buffer, ctx, address, size, resource.written, resource.read, resource.formatted);
	if (binding.second % alignment != 0) {
		EXIT("storage buffer binding is not device-aligned\n");
	}
	result.buffer = binding.first;
	result.offset = binding.second;
	result.range  = static_cast<VkDeviceSize>(size);
	return result;
}

static BufferView
NativeAddressBuffer(uint64_t submit_id, CommandBuffer* command_buffer,
                    const ShaderRecompiler::IR::AddressResource&           resource,
                    const ShaderRecompiler::IR::ResourceSnapshot::Address& address) {
	BufferView result;
	if (address.binding_base == 0) {
		BindNullStorageBuffer(command_buffer, &result);
		return result;
	}
	if (resource.written) {
		EXIT("writable address resources are unsupported\n");
	}
	const auto limit  = resource.kind == ShaderRecompiler::IR::ResourceKind::Flat
	                        ? ShaderRecompiler::IR::FlatAddressWindowSize
	                        : static_cast<uint64_t>(g_render_ctx->GetGraphicCtx()
	                                                    ->GetPhysicalDeviceProperties()
	                                                    .limits.maxStorageBufferRange);
	uint64_t   size   = 0;
	const auto access = HostMemoryAccess::Read;
	if (!HostMemoryQueryRange(address.binding_base, limit, access, &size)) {
		EXIT("address resource is not host-accessible: base=0x%016" PRIx64 "\n",
		     address.binding_base);
	}
	auto* const ctx       = g_render_ctx->GetGraphicCtx();
	const auto  alignment = ctx->StorageMinAlignment();
	if (alignment == 0 || size > ctx->GetPhysicalDeviceProperties().limits.maxStorageBufferRange ||
	    BufferCache::GetBufferOffset(address.binding_base) % alignment != 0) {
		EXIT("address resource range or alignment is unsupported\n");
	}
	(void)submit_id;
	auto binding  = g_render_ctx->GetBufferCache()->ObtainBuffer(command_buffer, ctx,
	                                                             address.binding_base, size);
	result.buffer = binding.first;
	result.offset = binding.second;
	result.range  = static_cast<VkDeviceSize>(size);
	return result;
}

static TextureVariant NativeTextureVariant(const ShaderRecompiler::IR::ImageResource& resource) {
	const bool uint_image = resource.kind == ShaderRecompiler::IR::ResourceKind::ImageUint ||
	                        resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	switch (resource.dimension) {
		case ShaderRecompiler::Decoder::ImageDimension::Dim3D:
			return uint_image ? TextureVariant::Uint3D : TextureVariant::Float3D;
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DArray:
			return uint_image ? TextureVariant::UintArray : TextureVariant::FloatArray;
		default: return uint_image ? TextureVariant::Uint2D : TextureVariant::Float2D;
	}
}

static bool IsSupportedSampledColorResource(const ShaderRecompiler::IR::ImageResource& resource) {
	return resource.kind == ShaderRecompiler::IR::ResourceKind::Image &&
	       resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	       resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::None && resource.read &&
	       !resource.written && !resource.atomic && !resource.depth_compare;
}

static bool IsSupportedSampledDepthFormat(VkFormat image_format, uint32_t guest_format,
                                          VkFormat view_format) {
	const bool d16 = image_format == VK_FORMAT_D16_UNORM &&
	                 guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm) &&
	                 view_format == VK_FORMAT_R16_UNORM;
	const bool d32 =
	    (image_format == VK_FORMAT_D32_SFLOAT || image_format == VK_FORMAT_D32_SFLOAT_S8_UINT) &&
	    guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
	    view_format == VK_FORMAT_R32_SFLOAT;
	return d16 || d32;
}

bool IsSupportedDepthTargetDescriptor(const ShaderTextureResource& descriptor,
                                      const VulkanImage&           image) {
	const auto width  = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto pitch  = TileGetTexturePitch(descriptor.Format(), width, 1, descriptor.TileMode());
	return image.type == VulkanImageType::DepthStencil && image.layers == 1 &&
	       width == image.extent.width && height == image.extent.height &&
	       descriptor.Depth() == 0 && descriptor.BaseLevel() == 0 && descriptor.LastLevel() == 0 &&
	       descriptor.MaxMip() == 0 && descriptor.MinLod() == 0 && descriptor.BaseArray5() == 0 &&
	       descriptor.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
	       descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
	       descriptor.BCSwizzle() == 0 && !descriptor.MsaaDepth() && pitch >= width &&
	       pitch == image.guest_pitch;
}

static bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	constexpr uint32_t field3_base          = 0x91800000u;
	constexpr uint32_t field5_expected      = 0x00700000u;
	return (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	       (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	       descriptor.fields[3] == (field3_base | descriptor.DstSelXYZW()) &&
	       descriptor.fields[4] == 0 && descriptor.fields[5] == field5_expected &&
	       descriptor.fields[6] == 0 && descriptor.fields[7] == 0;
}

static void ValidateDepthTargetBinding(const ShaderRecompiler::IR::ImageResource& resource,
                                       const ShaderTextureResource&               descriptor,
                                       const VulkanImage* image, VkFormat view_format,
                                       uint64_t size) {
	const bool resource_ok = IsSupportedSampledDepthResource(resource);
	const bool descriptor_ok =
	    image != nullptr && IsSupportedDepthTargetDescriptor(descriptor, *image);
	const bool encoding_ok = IsSupportedDepthTextureEncoding(descriptor);
	const bool format_ok   = image != nullptr && IsSupportedSampledDepthFormat(
	                                                 image->format, descriptor.Format(), view_format);
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	const auto descriptor_pitch =
	    TileGetTexturePitch(descriptor.Format(), static_cast<uint32_t>(descriptor.Width5()) + 1u, 1,
	                        descriptor.TileMode());
	EXIT("unsupported sampled depth target: resource=%d descriptor=%d encoding=%d format=%d "
	     "image_format=%d view_format=%d descriptor_pitch=%u target_pitch=%u "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok,
	     image == nullptr ? static_cast<int>(VK_FORMAT_UNDEFINED) : static_cast<int>(image->format),
	     static_cast<int>(view_format), descriptor_pitch,
	     image == nullptr ? 0u : image->guest_pitch, descriptor.Base40(), size);
}

static bool IsSupportedStorageTextureDescriptor(const ShaderRecompiler::IR::ImageResource& resource,
                                                const ShaderTextureResource& descriptor) {
	const auto tile  = descriptor.TileMode();
	const bool is_2d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	                   descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
	                   descriptor.Depth() == 0;
	const bool is_2d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray &&
	    descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
	    descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_3d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D &&
	                   descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor3D);
	const bool supported_tile = tile == Prospero::GpuEnumValue(Prospero::TileMode::kLinear) ||
	                            tile == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
	const bool supported_swizzle =
	    IsSupportedStorageSwizzle(descriptor.Format(), descriptor.DstSelXYZW()) &&
	    (descriptor.DstSelXYZW() == DstSel(4, 5, 6, 7) || !resource.read);
	return (is_2d || is_2d_array || is_3d) && supported_tile && descriptor.BaseLevel() == 0 &&
	       descriptor.LastLevel() == 0 && descriptor.MaxMip() == 0 && descriptor.MinLod() == 0 &&
	       descriptor.BaseArray5() == 0 && supported_swizzle && descriptor.BCSwizzle() == 0 &&
	       !descriptor.MsaaDepth();
}

static bool IsSupportedStorageTextureEncoding(const ShaderTextureResource& descriptor) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	constexpr uint32_t field5_expected      = 0x00700000u;
	const uint32_t     expected_field3 = descriptor.DstSelXYZW() |
	                                     (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.Type()) << 28u);
	return (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	       (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	       descriptor.fields[3] == expected_field3 && descriptor.fields[4] == descriptor.Depth() &&
	       descriptor.fields[5] == field5_expected && descriptor.fields[6] == 0 &&
	       descriptor.fields[7] == 0;
}

void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size) {
	const auto format        = descriptor.Format();
	const bool resource_ok   = IsSupportedStorageImageResource(resource);
	const bool descriptor_ok = IsSupportedStorageTextureDescriptor(resource, descriptor);
	const bool encoding_ok   = IsSupportedStorageTextureEncoding(descriptor);
	const bool uint_resource =
	    resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	const bool format_ok = Prospero::IsSupportedTextureFormat(format) &&
	                       uint_resource == Prospero::IsUintTextureFormat(format);
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	EXIT("unsupported storage texture: resource=%d descriptor=%d encoding=%d format=%d "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " extent=%ux%ux%u type=%u format=%u tile=%u swizzle=0x%03x read=%d written=%d\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok, descriptor.Base40(), size,
	     static_cast<uint32_t>(descriptor.Width5()) + 1u,
	     static_cast<uint32_t>(descriptor.Height5()) + 1u,
	     static_cast<uint32_t>(descriptor.Depth()) + 1u, descriptor.Type(), format,
	     descriptor.TileMode(), descriptor.DstSelXYZW(), resource.read, resource.written);
}

void ValidateMetadataReuseTexture(const ShaderRecompiler::IR::ImageResource& resource,
                                  const ShaderTextureResource& descriptor, uint64_t size) {
	constexpr uint32_t field1_reserved = 0x200fff00u;
	constexpr uint32_t field2_reserved = 0xf0003000u;
	const auto         format          = descriptor.Format();
	if (!IsSupportedSampledColorResource(resource) || size == 0 ||
	    (descriptor.fields[1] & field1_reserved) != 0 ||
	    (descriptor.fields[2] & field2_reserved) != 0 || descriptor.fields[3] != 0x90500facu ||
	    descriptor.fields[4] != 0 || descriptor.fields[5] != 0x00700000u ||
	    descriptor.fields[6] != 0 || descriptor.fields[7] != 0 ||
	    !Prospero::IsSupportedTextureFormat(format) || Prospero::IsUintTextureFormat(format)) {
		EXIT("unsupported storage texture descriptor encoding\n");
	}
}

static DescriptorCache::TextureBinding
NativeTexture(uint64_t submit_id, CommandBuffer* command_buffer,
              const ShaderRecompiler::IR::ImageResource&   resource,
              const ShaderRecompiler::IR::DescriptorValue& value) {
	auto       descriptor = NativeTextureDescriptor(value);
	const bool storage    = resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
	                        resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	const auto variant    = NativeTextureVariant(resource);
	if (storage) {
		ValidateStorageImageResource(resource);
	}
	if (descriptor.IsNull()) {
		return storage ? DescriptorCache::TextureBinding {GetDummyStorageTexture(variant),
		                                                  TextureVariantDefaultView(variant)}
		               : DescriptorCache::TextureBinding {GetDummySampledTexture(variant),
		                                                  TextureVariantDefaultView(variant)};
	}
	const auto address    = descriptor.Base40();
	const auto width      = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height     = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto base_level = descriptor.BaseLevel();
	const auto last_level = descriptor.LastLevel();
	const auto levels     = static_cast<uint32_t>(descriptor.MaxMip()) + 1u;
	if (base_level > last_level || last_level >= levels) {
		EXIT("unsupported texture mip view: base=%u last=%u levels=%u\n", base_level, last_level,
		     levels);
	}
	const auto    view_levels = last_level - base_level + 1u;
	const auto    depth       = static_cast<uint32_t>(descriptor.Depth()) + 1u;
	const auto    tile        = descriptor.TileMode();
	const auto    format      = descriptor.Format();
	const auto    view_format = TextureGetFormat(format, storage ? TextureFormatUsage::Storage
	                                                             : TextureFormatUsage::Sampled);
	const auto    type        = static_cast<Prospero::ImageType>(descriptor.Type());
	const auto    pitch       = TileGetTexturePitch(format, width, levels, tile);
	const auto    swizzle     = descriptor.DstSelXYZW();
	TileSizeAlign size;
	TileGetTextureTotalSize(format, width, height, depth, pitch, levels, tile,
	                        type == Prospero::ImageType::kColor3D, &size);
	EXIT_NOT_IMPLEMENTED(size.size == 0 ||
	                     (address & (static_cast<uint64_t>(size.align) - 1u)) != 0);
	if (storage) {
		ValidateStorageTexture(resource, descriptor, size.size);
		g_render_ctx->GetBufferCache()->ValidateGpuAccess(address, size.size, resource.read,
		                                                  resource.written);
	}

	VulkanImage* image      = nullptr;
	int          view       = VulkanImage::VIEW_DEFAULT;
	VkImageView  image_view = nullptr;
	const bool check_depth  = static_cast<Prospero::TileMode>(tile) == Prospero::TileMode::kDepth ||
	                          descriptor.MsaaDepth();
	if (image == nullptr) {
		if (check_depth) {
			image = g_render_ctx->GetTextureCache()->FindDepthTargetByRange(address, size.size);
		} else {
			image = g_render_ctx->GetTextureCache()->FindRenderTargetByRange(command_buffer,
			                                                                 address, size.size);
		}
		if (image != nullptr) {
			if (check_depth) {
				ValidateDepthTargetBinding(resource, descriptor, image, view_format, size.size);
				view = SelectSampledDepthView(image->format, view_format, swizzle);
				if (graphics_debug_dump_enabled()) {
					static std::atomic_uint log_count {0};
					if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
						LOGF("SampledDepthTarget: bind addr=0x%016" PRIx64 " size=0x%016" PRIx64
						     " format=%d view=%d\n",
						     address, size.size, static_cast<int>(image->format), view);
					}
				}
			} else {
				if (!(storage ? IsSupportedStorageImageResource(resource)
				              : IsSupportedSampledColorResource(resource)) ||
				    image->type != VulkanImageType::RenderTexture || image->layers != 1 ||
				    width != image->extent.width || height != image->extent.height || depth != 1 ||
				    (storage ? levels != 1 || base_level != 0
				             : levels != image->mip_levels || base_level >= levels) ||
				    descriptor.BaseArray5() != 0 || type != Prospero::ImageType::kColor2D) {
					EXIT("unsupported cached render-target image view: storage=%d resource=%u"
					     " image_type=%u layers=%u extent=%ux%u/%ux%u depth=%u"
					     " levels=%u/%u base_level=%u base_array=%u descriptor_type=%u\n",
					     storage, static_cast<uint32_t>(resource.kind),
					     static_cast<uint32_t>(image->type), image->layers, width, height,
					     image->extent.width, image->extent.height, depth, levels,
					     image->mip_levels, base_level, descriptor.BaseArray5(),
					     static_cast<uint32_t>(type));
				}
				view = storage ? SelectStorageColorView(image->format, view_format, swizzle)
				               : SelectSampledColorView(image->format, view_format, swizzle);
				if (storage) {
					image_view = g_render_ctx->GetTextureCache()->GetRenderTargetStorageView(
					    g_render_ctx->GetGraphicCtx(),
					    static_cast<RenderTextureVulkanImage*>(image), view_format, base_level,
					    view_levels);
				} else {
					image_view = g_render_ctx->GetTextureCache()->GetRenderTargetSampledView(
					    g_render_ctx->GetGraphicCtx(),
					    static_cast<RenderTextureVulkanImage*>(image), view_format, view,
					    base_level, view_levels);
				}
			}
			if (image_view == nullptr && image->image_view[view] == nullptr) {
				EXIT("required cached texture image view is missing\n");
			}
			if (storage) {
				g_render_ctx->GetTextureCache()->MarkGpuWritten(image);
			}
		}
	}
	if (image == nullptr) {
		const auto video = Presentation::DisplayBufferFind(address);
		if (video.image != nullptr) {
			if (storage) {
				EXIT("storage access to a video-out surface is unsupported\n");
			}
			image = video.image;
			if (!storage && swizzle == DstSel(6, 5, 4, 7)) {
				view = VulkanImage::VIEW_BGRA;
			}
		}
	}
	if (image == nullptr) {
		auto*      texture_cache = g_render_ctx->GetTextureCache();
		const bool metadata_read = texture_cache->HasMetaOverlap(address, size.size);
		if (storage && metadata_read) {
			EXIT("storage texture overlaps surface metadata\n");
		}
		if (!storage && metadata_read) {
			ValidateMetadataReuseTexture(resource, descriptor, size.size);
		}
		(void)submit_id;
		(void)command_buffer;
		ImageInfo info {};
		info.address     = address;
		info.size        = size.size;
		info.format      = format;
		info.width       = width;
		info.height      = height;
		info.pitch       = pitch;
		info.base_level  = base_level;
		info.levels      = levels;
		info.view_levels = view_levels;
		info.tile        = tile;
		info.swizzle     = swizzle;
		info.depth       = depth;
		info.type        = descriptor.Type();
		info.base_array  = descriptor.BaseArray5();
		if (storage) {
			image = texture_cache->FindStorageTexture(command_buffer, g_render_ctx->GetGraphicCtx(),
			                                          info);
			view  = VulkanImage::VIEW_DEFAULT;
		} else {
			image = texture_cache->FindTexture(command_buffer, g_render_ctx->GetGraphicCtx(), info,
			                                   metadata_read);
			if (image->type == VulkanImageType::StorageTexture) {
				image_view = texture_cache->GetStorageTextureSampledView(
				    g_render_ctx->GetGraphicCtx(), static_cast<StorageTextureVulkanImage*>(image),
				    info);
			}
		}
	}
	EXIT_NOT_IMPLEMENTED(image == nullptr);
	if (resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray) {
		view = SelectSampledTextureArrayView(image, view);
	}
	return {image, view, image_view};
}

static VkSampler NativeSampler(const ShaderRecompiler::IR::Program& program, uint32_t index,
                               const ShaderRecompiler::IR::DescriptorValue& value) {
	auto       descriptor    = NativeSamplerDescriptor(value);
	const bool depth_compare = std::any_of(program.info.sampled_pairs.begin(),
	                                       program.info.sampled_pairs.end(), [&](const auto& pair) {
		                                       return pair.sampler == index &&
		                                              pair.image < program.info.images.size() &&
		                                              program.info.images[pair.image].depth_compare;
	                                       });
	if (!depth_compare) {
		descriptor.fields[0] &= ~(0x7u << 12u);
	}
	return g_render_ctx->GetSamplerCache()->GetSampler(descriptor);
}

static BufferView NativeUpload(CommandBuffer* command_buffer, std::span<const uint32_t> data) {
	EXIT_IF(data.empty());
	BufferView result;
	EXIT_IF(!g_render_ctx->GetBufferCache()->UploadHostData(
	    command_buffer, g_render_ctx->GetGraphicCtx(), data.data(), data.size_bytes(), 256,
	    &result.buffer, &result.offset, &result.range));
	return result;
}

std::vector<ShaderAddressWriteRange>
BindDescriptors(uint64_t submit_id, CommandBuffer* buffer, VkPipelineBindPoint pipeline_bind_point,
                VkPipelineLayout layout, const ShaderStageRuntime& runtime,
                VkShaderStageFlags vk_stage, DescriptorCache::Stage stage) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer == nullptr || !runtime);
	const auto& program  = *runtime.program;
	const auto& snapshot = *runtime.resources;
	std::string error;
	if (!ShaderRecompiler::IR::ValidateResourceSpecialization(program, snapshot, &error)) {
		EXIT("invalid native shader runtime snapshot: %s\n", error.c_str());
	}
	auto*      vk_buffer     = buffer->GetPool()->buffers[buffer->GetIndex()];
	const auto shader_stages = ShaderPipelineStages(vk_stage);

	DescriptorCache::NativeDescriptors descriptors;
	descriptors.buffers.reserve(program.info.buffers.size());
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		descriptors.buffers.push_back(
		    NativeStorageBuffer(submit_id, buffer, NativeBufferDescriptor(snapshot.buffers[i]),
		                        program.info.buffers[i]));
	}
	descriptors.images.reserve(program.info.images.size());
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto kind = program.info.images[i].kind;
		if ((kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
		     kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint) &&
		    vk_stage != VK_SHADER_STAGE_COMPUTE_BIT) {
			EXIT("storage images are unsupported outside compute shaders\n");
		}
		descriptors.images.push_back(
		    NativeTexture(submit_id, buffer, program.info.images[i], snapshot.images[i]));
	}
	descriptors.samplers.reserve(program.info.samplers.size());
	for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
		descriptors.samplers.push_back(NativeSampler(program, i, snapshot.samplers[i]));
	}
	descriptors.addresses.reserve(program.info.addresses.size());
	std::vector<ShaderAddressWriteRange> address_writes;
	for (uint32_t i = 0; i < program.info.addresses.size(); i++) {
		auto view = NativeAddressBuffer(submit_id, buffer, program.info.addresses[i],
		                                snapshot.addresses[i]);
		descriptors.addresses.push_back(view);
		if (program.info.addresses[i].written && snapshot.addresses[i].binding_base != 0) {
			EXIT_IF(view.buffer == nullptr || view.offset != 0 || view.range == VK_WHOLE_SIZE ||
			        view.range == 0);
			address_writes.push_back({view.buffer, snapshot.addresses[i].binding_base,
			                          static_cast<uint64_t>(view.range)});
		}
	}
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::FlattenedSrt) !=
	    nullptr) {
		descriptors.flattened_srt = NativeUpload(buffer, snapshot.flattened_srt);
	}

	std::vector<uint32_t> user_data;
	user_data.reserve(program.bindings.user_data_registers.size());
	for (const auto reg: program.bindings.user_data_registers) {
		user_data.push_back(snapshot.user_data[reg - program.user_data_base]);
	}
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::UserData) != nullptr) {
		descriptors.user_data = NativeUpload(buffer, user_data);
	}
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::Gds) != nullptr) {
		descriptors.gds.buffer =
		    g_render_ctx->GetGdsBuffer()->GetBuffer(g_render_ctx->GetGraphicCtx());
		const auto barrier = MakeGdsDependency(*descriptors.gds.buffer);
		vkCmdPipelineBarrier(vk_buffer,
		                     VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
		                         VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
		                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     shader_stages, 0, 0, nullptr, 1, &barrier, 0, nullptr);
	}

	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		auto*       image    = descriptors.images[i].image;
		const auto& resource = program.info.images[i];
		if (resource.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		    resource.kind == ShaderRecompiler::IR::ResourceKind::ImageUint) {
			switch (image->type) {
				case VulkanImageType::DepthStencil:
					GraphicsRenderDepthStencilBarrier(vk_buffer, image);
					break;
				case VulkanImageType::RenderTexture:
				case VulkanImageType::StorageTexture:
					GraphicsRenderTextureBarrier(vk_buffer, image);
					break;
				case VulkanImageType::VideoOut:
					GraphicsRenderColorImageBarrier(vk_buffer, image, RENDER_COLOR_IMAGE_LAYOUT);
					break;
				default: break;
			}
		} else {
			const auto barrier =
			    MakeStorageImageDependency(*image, resource.read, resource.written);
			vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, shader_stages, 0, 0,
			                     nullptr, 0, nullptr, 1, &barrier);
			image->layout = VK_IMAGE_LAYOUT_GENERAL;
		}
	}

	if (!program.bindings.descriptors.empty()) {
		auto* set = g_render_ctx->GetDescriptorCache()->GetDescriptor(stage, program, descriptors);
		EXIT_IF(set == nullptr);
		vkCmdBindDescriptorSets(vk_buffer, pipeline_bind_point, layout,
		                        program.bindings.descriptor_set, 1, &set->set, 0, nullptr);
		buffer->RecycleDescriptorAfterFence(set);
	}
	if (program.bindings.push_constant_size != 0) {
		EXIT_IF(program.bindings.push_constant_size != user_data.size() * sizeof(uint32_t));
		vkCmdPushConstants(vk_buffer, layout, vk_stage, program.bindings.push_constant_offset,
		                   program.bindings.push_constant_size, user_data.data());
	}
	return address_writes;
}

} // namespace Libs::Graphics
