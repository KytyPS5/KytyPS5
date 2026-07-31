#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

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
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/BindingLayout.h"
#include "graphics/shader/recompiler/ir/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <atomic>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace Libs::Graphics {

static void BindNullStorageBuffer(RenderContext& context, BufferView& dst) {
	auto owner = context.GetBufferCache().ObtainNullBuffer();
	dst.buffer = owner->Handle();
	dst.owner  = std::move(owner);
	dst.offset = 0;
	dst.range  = 16;
}

static void CopyNativeDescriptor(const ShaderRecompiler::IR::DescriptorValue& source,
                                 std::span<uint32_t>                          destination) {
	EXIT_IF(source.dword_count != destination.size());
	std::copy_n(source.dwords.begin(), destination.size(), destination.begin());
}

static Prospero::ImageType TextureType(const ShaderTextureResource& descriptor) {
	const auto type = static_cast<Prospero::ImageType>(descriptor.Type());
	return type == Prospero::ImageType::kCube ? Prospero::ImageType::kColor2DArray : type;
}

static Prospero::ImageType TextureBaseType(Prospero::ImageType type) {
	switch (type) {
		case Prospero::ImageType::kColor1DArray: return Prospero::ImageType::kColor1D;
		case Prospero::ImageType::kColor2DArray:
		case Prospero::ImageType::kColor2DMsaa:
		case Prospero::ImageType::kColor2DMsaaArray: return Prospero::ImageType::kColor2D;
		default: return type;
	}
}

static bool IsMultisampledTexture(Prospero::ImageType type) {
	return type == Prospero::ImageType::kColor2DMsaa ||
	       type == Prospero::ImageType::kColor2DMsaaArray;
}

static BufferView NativeStorageBuffer(RenderContext& context, CommandBuffer& command_buffer,
                                      const ShaderBufferResource&                 descriptor,
                                      const ShaderRecompiler::IR::BufferResource& resource,
                                      uint32_t&                                   buffer_offset) {
	BufferView result;
	buffer_offset = 0;

	const auto address = descriptor.Base48();
	const auto stride  = descriptor.Stride();
	const auto records = descriptor.NumRecords();
	if (stride != 0 && records > UINT64_MAX / stride) {
		EXIT("storage buffer descriptor footprint overflow\n");
	}
	const auto size = stride != 0 ? static_cast<uint64_t>(stride) * records : records;
	if (address == 0 || size == 0) {
		BindNullStorageBuffer(context, result);
		return result;
	}
	const auto& graphics  = context.GetGraphics();
	const auto  alignment = graphics.StorageMinAlignment();
	if (alignment == 0 ||
	    size > graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange) {
		EXIT("storage buffer range or device alignment is unsupported\n");
	}
	auto binding = context.GetBufferCache().ObtainBuffer(
	    command_buffer, address, size, resource.written, resource.read, resource.formatted);
	const auto aligned_offset = binding.offset - binding.offset % alignment;
	const auto adjustment     = binding.offset - aligned_offset;
	const auto max_range      = graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange;
	if (adjustment % sizeof(uint32_t) != 0 || adjustment >= 256 || size > max_range - adjustment) {
		EXIT("storage buffer offset adjustment is unsupported\n");
	}
	buffer_offset = static_cast<uint32_t>(adjustment);
	result.owner  = std::move(binding.owner);
	result.buffer = binding.buffer;
	result.offset = aligned_offset;
	result.range  = static_cast<vk::DeviceSize>(size + adjustment);
	return result;
}

static BufferView
NativeAddressBuffer(RenderContext& context, CommandBuffer& command_buffer,
                    const ShaderRecompiler::IR::AddressResource&           resource,
                    const ShaderRecompiler::IR::ResourceSnapshot::Address& address) {
	BufferView result;
	if (address.binding_base == 0) {
		BindNullStorageBuffer(context, result);
		return result;
	}
	if (resource.written) {
		EXIT("writable address resources are unsupported\n");
	}
	const auto limit =
	    resource.kind == ShaderRecompiler::IR::ResourceKind::Flat
	        ? ShaderRecompiler::IR::FlatAddressWindowSize
	        : static_cast<uint64_t>(
	              context.GetGraphics().GetPhysicalDeviceProperties().limits.maxStorageBufferRange);
	uint64_t   size   = 0;
	const auto access = HostMemoryAccess::Mapped;
	if (!HostMemoryQueryRange(address.binding_base, limit, access, size)) {
		EXIT("address resource is not host-accessible: base=0x%016" PRIx64 "\n",
		     address.binding_base);
	}
	const auto& graphics  = context.GetGraphics();
	const auto  alignment = graphics.StorageMinAlignment();
	if (alignment == 0 ||
	    size > graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange ||
	    BufferCache::GetBufferOffset(address.binding_base) % alignment != 0) {
		EXIT("address resource range or alignment is unsupported\n");
	}
	auto binding =
	    context.GetBufferCache().ObtainBuffer(command_buffer, address.binding_base, size);
	result.owner  = std::move(binding.owner);
	result.buffer = binding.buffer;
	result.offset = binding.offset;
	result.range  = static_cast<vk::DeviceSize>(size);
	return result;
}

static bool IsSupportedSampledColorResource(const ShaderRecompiler::IR::ImageResource& resource) {
	bool supported_dimension = false;
	switch (resource.dimension) {
		case ShaderRecompiler::Decoder::ImageDimension::Dim1D:
		case ShaderRecompiler::Decoder::ImageDimension::Dim1DArray:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2D:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DArray:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaaArray:
			supported_dimension = true;
			break;
		default: break;
	}
	const bool sampled_kind = resource.kind == ShaderRecompiler::IR::ResourceKind::Image ||
	                          resource.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	return sampled_kind && supported_dimension &&
	       resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::None && resource.read &&
	       !resource.written && !resource.atomic && !resource.depth_compare;
}

TargetTextureViewInfo ResolveTargetTextureView(const ShaderRecompiler::IR::ImageResource& resource,
                                               Prospero::ImageType type, uint32_t base_layer,
                                               uint32_t image_layers) {
	switch (type) {
		case Prospero::ImageType::kColor2D:
			return resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
			               base_layer == 0 && image_layers == 1
			           ? TargetTextureViewInfo {vk::ImageViewType::e2D, 0, 1}
			           : TargetTextureViewInfo {};
		case Prospero::ImageType::kCube:
			if (resource.dimension != ShaderRecompiler::Decoder::ImageDimension::Dim2DArray ||
			    base_layer >= image_layers || (image_layers - base_layer) % 6u != 0) {
				return {};
			}
			return {vk::ImageViewType::e2DArray, base_layer, image_layers - base_layer};
		case Prospero::ImageType::kColor2DArray:
			if (resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
			    base_layer == 0 && image_layers == 1) {
				return {vk::ImageViewType::e2D, 0, 1};
			}
			return resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray &&
			               base_layer < image_layers
			           ? TargetTextureViewInfo {vk::ImageViewType::e2DArray, base_layer,
			                                    image_layers - base_layer}
			           : TargetTextureViewInfo {};
		case Prospero::ImageType::kColor2DMsaa:
			return resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa &&
			               base_layer == 0 && image_layers == 1
			           ? TargetTextureViewInfo {vk::ImageViewType::e2D, 0, 1}
			           : TargetTextureViewInfo {};
		case Prospero::ImageType::kColor2DMsaaArray:
			if (resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa &&
			    base_layer == 0 && image_layers == 1) {
				return {vk::ImageViewType::e2D, 0, 1};
			}
			return resource.dimension ==
			                   ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaaArray &&
			               base_layer < image_layers
			           ? TargetTextureViewInfo {vk::ImageViewType::e2DArray, base_layer,
			                                    image_layers - base_layer}
			           : TargetTextureViewInfo {};
		default: return {};
	}
}

bool IsSupportedSampledVideoOutView(const ShaderRecompiler::IR::ImageResource& resource,
                                    const ShaderTextureResource& descriptor, const Image& image) {
	return image.usage.video_out && image.info.resources.layers == 1 &&
	       IsSupportedSampledColorResource(resource) &&
	       resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	       descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
	       descriptor.Depth() == 0 && descriptor.BaseArray5() == 0;
}

bool IsSupportedDepthTargetDescriptor(const ShaderTextureResource& descriptor, const Image& image) {
	const auto width        = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height       = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto type         = static_cast<Prospero::ImageType>(descriptor.Type());
	const bool multisampled = IsMultisampledTexture(type);
	const auto samples      = multisampled ? 1u << descriptor.LastLevel() : 1u;
	const auto pitch =
	    multisampled ? TileGetDepthPitch(width, image.info.bytes_per_block, descriptor.LastLevel())
	                 : TileGetTexturePitch(descriptor.Format(), width, 1, descriptor.TileMode());
	const bool supported_2d    = type == Prospero::ImageType::kColor2D &&
	                             image.info.resources.layers == 1 && descriptor.Depth() == 0 &&
	                             descriptor.BaseArray5() == 0;
	const bool supported_array = type == Prospero::ImageType::kColor2DArray &&
	                             descriptor.BaseArray5() <= descriptor.Depth() &&
	                             descriptor.Depth() < image.info.resources.layers;
	const bool supported_cube =
	    type == Prospero::ImageType::kCube && width == height && image.info.resources.layers >= 6 &&
	    image.info.resources.layers % 6u == 0 &&
	    static_cast<uint32_t>(descriptor.Depth()) + 1u == image.info.resources.layers &&
	    descriptor.BaseArray5() == 0;
	const bool supported_msaa_2d    = type == Prospero::ImageType::kColor2DMsaa &&
	                                  image.info.resources.layers == 1 && descriptor.Depth() == 0 &&
	                                  descriptor.BaseArray5() == 0;
	const bool supported_msaa_array = type == Prospero::ImageType::kColor2DMsaaArray &&
	                                  descriptor.BaseArray5() <= descriptor.Depth() &&
	                                  descriptor.Depth() < image.info.resources.layers;
	const bool levels_ok =
	    multisampled
	        ? descriptor.BaseLevel() == 0 && descriptor.LastLevel() >= 1 &&
	              descriptor.LastLevel() <= 3 && descriptor.MaxMip() == descriptor.LastLevel() &&
	              image.info.resources.levels == 1 && image.info.samples == samples
	        : descriptor.BaseLevel() == 0 && descriptor.LastLevel() == 0 &&
	              descriptor.MaxMip() == 0 && image.info.samples == 1;
	return image.info.IsDepth() && width == image.info.extent.width &&
	       height == image.info.extent.height &&
	       (supported_2d || supported_array || supported_cube || supported_msaa_2d ||
	        supported_msaa_array) &&
	       levels_ok && descriptor.MinLod() == 0 &&
	       descriptor.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
	       descriptor.BCSwizzle() == 0 && descriptor.MsaaDepth() == multisampled &&
	       pitch >= width && pitch == image.info.pitch;
}

bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor, const Image& image) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	const uint32_t     field3_expected = descriptor.DstSelXYZW() |
	                                     (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	                                     (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	                                     (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t     field4_expected = descriptor.Depth() | (descriptor.BaseArray5() << 16u);
	const uint32_t     field5_expected =
	    0x00700000u | (static_cast<uint32_t>(descriptor.MaxMip()) << 4u);
	const bool common = (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	                    (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	                    descriptor.fields[3] == field3_expected &&
	                    descriptor.fields[4] == field4_expected &&
	                    descriptor.fields[5] == field5_expected;
	if (!common || (descriptor.fields[6] == 0 && descriptor.fields[7] != 0)) {
		return false;
	}
	if (descriptor.fields[6] == 0) {
		return true;
	}
	constexpr uint32_t htile_control = 0x00280000u;
	const uint32_t expected_control  = htile_control | (descriptor.MsaaDepth() ? (1u << 10u) : 0u);
	const auto     metadata_addr     = descriptor.MetaAddr() << 8u;
	return (descriptor.fields[6] & 0x00ffffffu) == expected_control && metadata_addr != 0 &&
	       descriptor.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
	       image.info.tile_mode == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
	       image.info.metadata.kind == ImageMetadataKind::Htile &&
	       image.info.metadata.range.Valid() && image.info.metadata.range.address == metadata_addr;
}

static void ValidateDepthTargetBinding(const ShaderRecompiler::IR::ImageResource& resource,
                                       const ShaderTextureResource& descriptor, const Image* image,
                                       vk::Format view_format, uint64_t size) {
	const bool resource_ok = IsSupportedSampledDepthResource(resource);
	const bool descriptor_ok =
	    image != nullptr && IsSupportedDepthTargetDescriptor(descriptor, *image);
	const bool encoding_ok =
	    image != nullptr && IsSupportedDepthTextureEncoding(descriptor, *image);
	const bool format_ok =
	    image != nullptr &&
	    IsSupportedSampledDepthFormat(image->info.pixel_format, descriptor.Format(), view_format);
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	const auto descriptor_pitch =
	    TileGetTexturePitch(descriptor.Format(), static_cast<uint32_t>(descriptor.Width5()) + 1u, 1,
	                        descriptor.TileMode());
	EXIT("unsupported sampled depth target: resource=%d descriptor=%d encoding=%d format=%d "
	     "kind=%u dimension=%u mip_mode=%u read=%d written=%d atomic=%d compare=%d "
	     "guest_format=%u swizzle=0x%03x image_format=%d view_format=%d image_layers=%u "
	     "descriptor_type=%u base_array=%u depth=%u descriptor_pitch=%u target_pitch=%u "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok, static_cast<uint32_t>(resource.kind),
	     static_cast<uint32_t>(resource.dimension), static_cast<uint32_t>(resource.mip_mode),
	     resource.read, resource.written, resource.atomic, resource.depth_compare,
	     descriptor.Format(), descriptor.DstSelXYZW(),
	     image == nullptr ? static_cast<int>(vk::Format::eUndefined)
	                      : static_cast<int>(image->info.pixel_format),
	     static_cast<int>(view_format), image == nullptr ? 0u : image->info.resources.layers,
	     descriptor.Type(), descriptor.BaseArray5(), descriptor.Depth(), descriptor_pitch,
	     image == nullptr ? 0u : image->info.pitch, descriptor.Base40(), size, descriptor.fields[0],
	     descriptor.fields[1], descriptor.fields[2], descriptor.fields[3], descriptor.fields[4],
	     descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

static bool IsSupportedStorageTextureDescriptor(const ShaderRecompiler::IR::ImageResource& resource,
                                                const ShaderTextureResource& descriptor) {
	const auto tile = descriptor.TileMode();
	const bool is_color_1d =
	    descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor1D);
	const bool is_color_1d_array =
	    descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor1DArray);
	const bool valid_1d_slice =
	    (is_color_1d && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0) ||
	    (is_color_1d_array && descriptor.BaseArray5() <= descriptor.Depth());
	const bool is_1d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1D &&
	                   descriptor.Height5() == 0 && valid_1d_slice;
	const bool is_1d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1DArray &&
	    is_color_1d_array && descriptor.Height5() == 0 &&
	    descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_color_2d =
	    descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
	const bool is_color_2d_array =
	    descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray);
	const bool valid_2d_slice =
	    (is_color_2d && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0) ||
	    (is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth());
	const bool is_2d =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D && valid_2d_slice;
	const bool is_2d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray &&
	    is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_3d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D &&
	                   descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor3D) &&
	                   descriptor.BaseArray5() == 0;
	TileBlockLayout depth_block {};
	const auto      depth_bpe = Prospero::RenderTargetBytesPerElement(descriptor.Format());
	const bool      supported_depth_tile =
	    tile == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) && !resource.read &&
	    !Prospero::IsFmaskTextureFormat(descriptor.Format()) && (is_2d || is_2d_array) &&
	    TileGetBlockLayout(TileBlockFamily::Depth64KB, depth_bpe, depth_block);
	const bool supported_standard_tile =
	    (tile == Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB) &&
	     TileIsStandard4KBTextureSupported(descriptor.Format())) ||
	    (tile == Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB) &&
	     TileIsStandard64KBTextureSupported(descriptor.Format()));
	const bool supported_tile = tile == Prospero::GpuEnumValue(Prospero::TileMode::kLinear) ||
	                            tile == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) ||
	                            supported_depth_tile || supported_standard_tile;
	const auto swizzle        = descriptor.DstSelXYZW();
	const bool supported_swizzle =
	    IsValidImageSwizzle(swizzle) &&
	    (swizzle == DstSel(4, 5, 6, 7) || !resource.read || resource.atomic);
	const bool supported_mip_view = descriptor.BaseLevel() == 0 || is_1d || is_2d;
	return (is_1d || is_1d_array || is_2d || is_2d_array || is_3d) && supported_tile &&
	       supported_mip_view && descriptor.BaseLevel() == descriptor.LastLevel() &&
	       descriptor.LastLevel() <= descriptor.MaxMip() && descriptor.MinLod() == 0 &&
	       supported_swizzle && descriptor.BCSwizzle() == 0 && !descriptor.MsaaDepth();
}

static bool IsSupportedStorageTextureEncoding(const ShaderTextureResource& descriptor) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	constexpr uint32_t field5_expected      = 0x00700000u;
	constexpr uint32_t field5_max_mip_mask  = 0x000000f0u;
	const uint32_t     expected_field3 = descriptor.DstSelXYZW() |
	                                     (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	                                     (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	                                     (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t     expected_field4 =
	    descriptor.Depth() | (static_cast<uint32_t>(descriptor.BaseArray5()) << 16u);
	return (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	       (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	       descriptor.fields[3] == expected_field3 && descriptor.fields[4] == expected_field4 &&
	       (descriptor.fields[5] & ~field5_max_mip_mask) == field5_expected;
}

void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size) {
	const auto format        = descriptor.Format();
	const bool resource_ok   = IsSupportedStorageImageResource(resource);
	const bool descriptor_ok = IsSupportedStorageTextureDescriptor(resource, descriptor);
	const bool encoding_ok   = IsSupportedStorageTextureEncoding(descriptor);
	const bool uint_resource =
	    resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	const bool format_ok =
	    Prospero::IsSupportedTextureFormat(format) &&
	    uint_resource == Prospero::IsUintTextureFormat(format) &&
	    (!resource.atomic || format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt));
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	EXIT("unsupported storage texture: resource=%d descriptor=%d encoding=%d format=%d "
	     "kind=%u dimension=%u mip_mode=%u atomic=%d compare=%d "
	     "base_level=%u last_level=%u max_mip=%u min_lod=%u base_array=%u bc=%u msaa=%d "
	     "depth_tile_bpe=%u swizzle_ok=%d "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " extent=%ux%ux%u type=%u format=%u tile=%u swizzle=0x%03x read=%d written=%d "
	     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok, static_cast<uint32_t>(resource.kind),
	     static_cast<uint32_t>(resource.dimension), static_cast<uint32_t>(resource.mip_mode),
	     resource.atomic, resource.depth_compare, descriptor.BaseLevel(), descriptor.LastLevel(),
	     descriptor.MaxMip(), descriptor.MinLod(), descriptor.BaseArray5(), descriptor.BCSwizzle(),
	     descriptor.MsaaDepth(), Prospero::RenderTargetBytesPerElement(format),
	     IsValidImageSwizzle(descriptor.DstSelXYZW()), descriptor.Base40(), size,
	     static_cast<uint32_t>(descriptor.Width5()) + 1u,
	     static_cast<uint32_t>(descriptor.Height5()) + 1u,
	     static_cast<uint32_t>(descriptor.Depth()) + 1u, descriptor.Type(), format,
	     descriptor.TileMode(), descriptor.DstSelXYZW(), resource.read, resource.written,
	     descriptor.fields[0], descriptor.fields[1], descriptor.fields[2], descriptor.fields[3],
	     descriptor.fields[4], descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

struct NullImageSpec {
	vk::Format format;
	uint32_t   guest_format;
};

static NullImageSpec NullTextureSpec(const ShaderRecompiler::IR::ImageResource& resource) {
	const bool uint_image = resource.kind == ShaderRecompiler::IR::ResourceKind::ImageUint ||
	                        resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	return uint_image ? NullImageSpec {vk::Format::eR32Uint,
	                                   Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt)}
	                  : NullImageSpec {vk::Format::eR32Sfloat,
	                                   Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float)};
}

static TextureCache::ImageDesc NullTextureDesc(const ShaderRecompiler::IR::ImageResource& resource,
                                               TextureCache::BindingType                  binding) {
	const auto              spec = NullTextureSpec(resource);
	TextureCache::ImageDesc desc {};
	desc.info.pixel_format    = spec.format;
	desc.info.guest_format    = spec.guest_format;
	desc.info.type            = Prospero::ImageType::kColor2D;
	desc.info.extent          = {1, 1, 1};
	desc.info.resources       = {1, 1};
	desc.info.bytes_per_block = 4;
	desc.info.samples         = 1;
	desc.info.mip_layout[0]   = {0, 0, 1, 1};
	desc.view_info.format     = desc.info.pixel_format;
	desc.view_info.type       = vk::ImageViewType::e2D;
	desc.view_info.aspect     = vk::ImageAspectFlagBits::eColor;
	desc.view_info.usage      = binding == TextureCache::BindingType::Storage
	                                ? vk::ImageUsageFlagBits::eStorage
	                                : vk::ImageUsageFlagBits::eSampled;
	desc.type                 = binding;
	return desc;
}

static void PopulateTextureMipLayout(ImageInfo& info) {
	if (info.IsVolume() && info.tile_mode != Prospero::GpuEnumValue(Prospero::TileMode::kLinear)) {
		TileVolumeLayout volume {};
		if (!TileGetTextureVolumeLayout(info.guest_format, info.extent.width, info.extent.height,
		                                info.extent.depth, info.resources.levels, info.tile_mode,
		                                volume)) {
			EXIT("unsupported normalized volume texture layout\n");
		}
		for (uint32_t level = 0; level < info.resources.levels; level++) {
			info.mip_layout[level] = {
			    volume.level_offsets[level],
			    volume.level_sizes[level],
			    volume.level_widths[level],
			    volume.level_heights[level],
			};
		}
		return;
	}

	TileSizeOffset levels[16] {};
	TilePaddedSize padded[16] {};
	TileGetTextureSize(info.guest_format, info.extent.width, info.extent.height, info.pitch,
	                   info.resources.levels, info.tile_mode, nullptr, levels, padded);
	for (uint32_t level = 0; level < info.resources.levels; level++) {
		const auto offset =
		    levels[level].src_size != 0 ? levels[level].src_offset : levels[level].offset;
		auto size = static_cast<uint64_t>(levels[level].src_size != 0 ? levels[level].src_size
		                                                              : levels[level].size);
		if (info.IsVolume()) {
			size *= std::max(info.extent.depth >> level, 1u);
		} else {
			size *= info.resources.layers;
		}
		info.mip_layout[level] = {
		    offset,
		    size,
		    padded[level].width != 0 ? padded[level].width : std::max(info.pitch >> level, 1u),
		    padded[level].height != 0 ? padded[level].height
		                              : std::max(info.extent.height >> level, 1u),
		};
	}
}

static ImageViewInfo TextureViewInfo(const ShaderRecompiler::IR::ImageResource& resource,
                                     const ShaderTextureResource& descriptor, vk::Format format,
                                     bool storage, uint32_t view_levels, uint32_t image_layers) {
	ImageViewInfo view {};
	view.format      = format;
	view.aspect      = vk::ImageAspectFlagBits::eColor;
	view.base_level  = descriptor.BaseLevel();
	view.level_count = view_levels;
	view.usage = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
	view.mapping =
	    storage ? vk::ComponentMapping {} : TextureGetComponentMapping(descriptor.DstSelXYZW());
	switch (resource.dimension) {
		case ShaderRecompiler::Decoder::ImageDimension::Dim1D:
			view.type       = vk::ImageViewType::e1D;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture base layer is out of bounds\n");
			}
			view.layer_count = 1;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim1DArray:
			view.type       = vk::ImageViewType::e1DArray;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture array base layer is out of bounds\n");
			}
			view.layer_count = image_layers - view.base_layer;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim3D:
			view.type        = vk::ImageViewType::e3D;
			view.base_layer  = 0;
			view.layer_count = 1;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DArray:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaaArray:
			view.type       = vk::ImageViewType::e2DArray;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture array base layer is out of bounds\n");
			}
			view.layer_count = image_layers - view.base_layer;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim2D:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa:
			view.type       = vk::ImageViewType::e2D;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture base layer is out of bounds\n");
			}
			view.layer_count = 1;
			break;
		default: EXIT("unsupported texture view dimension\n");
	}
	return view;
}

DescriptorCache::TextureBinding
RenderExecutor::ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
                               const ShaderRecompiler::IR::DescriptorValue& value) {
	ShaderTextureResource descriptor;
	CopyNativeDescriptor(value, descriptor.fields);
	const bool storage = resource.written;
	if (storage) {
		ValidateStorageImageResource(resource);
	}

	auto& texture_cache = m_context.GetTextureCache();
	if (descriptor.IsNull()) {
		auto       desc = NullTextureDesc(resource, storage ? TextureCache::BindingType::Storage
		                                                    : TextureCache::BindingType::Texture);
		const auto id   = texture_cache.FindImage(desc);
		return {id, nullptr, std::move(desc)};
	}

	const auto address      = descriptor.Base40();
	const auto width        = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height       = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto base_level   = descriptor.BaseLevel();
	const auto last_level   = descriptor.LastLevel();
	const auto type         = TextureType(descriptor);
	const bool multisampled = IsMultisampledTexture(type);
	const auto levels       = multisampled ? 1u : static_cast<uint32_t>(descriptor.MaxMip()) + 1u;
	const auto tile         = descriptor.TileMode();
	const bool msaa_tile =
	    tile == Prospero::GpuEnumValue(descriptor.MsaaDepth() ? Prospero::TileMode::kDepth
	                                                          : Prospero::TileMode::kRenderTarget);
	const bool msaa_array = type == Prospero::ImageType::kColor2DMsaaArray;
	if ((!multisampled && (base_level > last_level || last_level >= levels)) ||
	    (multisampled &&
	     (base_level != 0 || last_level == 0 || last_level > 3 ||
	      descriptor.MaxMip() != last_level || !msaa_tile ||
	      (!msaa_array && (descriptor.Depth() != 0 || descriptor.BaseArray5() != 0))))) {
		EXIT("unsupported texture mip view: base=%u last=%u levels=%u\n", base_level, last_level,
		     levels);
	}
	const auto samples = multisampled ? 1u << last_level : 1u;
	const auto view_levels =
	    multisampled ? 1u : static_cast<uint32_t>(last_level - base_level) + 1u;
	const auto depth  = static_cast<uint32_t>(descriptor.Depth()) + 1u;
	const auto format = descriptor.Format();
	const bool sampled_numeric_class =
	    storage || ((resource.kind == ShaderRecompiler::IR::ResourceKind::ImageUint) ==
	                Prospero::IsUintTextureFormat(format));
	if (!storage &&
	    (resource.kind == ShaderRecompiler::IR::ResourceKind::Image ||
	     resource.kind == ShaderRecompiler::IR::ResourceKind::ImageUint) &&
	    !sampled_numeric_class) {
		EXIT("sampled image numeric class mismatch: kind=%u format=%u addr=0x%016" PRIx64 "\n",
		     static_cast<uint32_t>(resource.kind), format, address);
	}

	const bool    volume       = type == Prospero::ImageType::kColor3D;
	const bool    layered      = type == Prospero::ImageType::kColor1DArray ||
	                             type == Prospero::ImageType::kColor2DArray ||
	                             type == Prospero::ImageType::kColor2DMsaaArray;
	const auto    image_layers = layered ? depth : 1u;
	uint32_t      pitch        = 0;
	TileSizeAlign size {};
	if (multisampled) {
		const auto bytes = Prospero::NumBytesPerElement(format);
		pitch            = TileGetRenderTargetPitch(width, bytes, last_level);
		if (pitch == 0 || !TileGetRenderTargetSize(width, height, pitch, bytes, size, last_level) ||
		    size.size > UINT32_MAX / image_layers) {
			EXIT("unsupported multisample texture layout\n");
		}
		size.size *= image_layers;
	} else {
		pitch = TileGetTexturePitch(format, width, levels, tile);
		TileGetTextureTotalSize(format, width, height, volume ? depth : image_layers, pitch, levels,
		                        tile, volume, size);
	}
	EXIT_NOT_IMPLEMENTED(size.size == 0 || size.align == 0 ||
	                     (address & (static_cast<uint64_t>(size.align) - 1u)) != 0);
	if (storage) {
		ValidateStorageTexture(resource, descriptor, size.size);
	}

	const auto              pixel_format        = TextureGetFormat(format);
	const auto              storage_view_format = SrgbStorageViewFormat(pixel_format);
	const auto              view_format = storage && storage_view_format != vk::Format::eUndefined
	                                          ? storage_view_format
	                                          : pixel_format;
	const auto              block_bytes = Prospero::BlockCompressedBytesPerBlock(format);
	TextureCache::ImageDesc desc {};
	desc.info.data         = {address, size.size};
	desc.info.pixel_format = pixel_format;
	desc.info.guest_format = format;
	desc.info.type         = TextureBaseType(type);
	desc.info.extent       = {width, height, volume ? depth : 1u};
	desc.info.resources    = {levels, image_layers};
	desc.info.pitch        = pitch;
	desc.info.bytes_per_block =
	    block_bytes != 0 ? block_bytes : Prospero::NumBytesPerElement(format);
	desc.info.samples   = samples;
	desc.info.tile_mode = tile;
	if (samples > 1) {
		desc.info.mip_layout[0] = {0, size.size, pitch, height};
	} else {
		PopulateTextureMipLayout(desc.info);
	}
	desc.view_info = TextureViewInfo(resource, descriptor, view_format, storage, view_levels,
	                                 desc.info.resources.layers);
	desc.type = storage ? TextureCache::BindingType::Storage : TextureCache::BindingType::Texture;

	auto       id                  = texture_cache.FindImage(desc);
	auto*      image               = &texture_cache.GetImage(id);
	const bool stencil_association = static_cast<bool>(image->depth_id);
	if (stencil_association) {
		id    = image->depth_id;
		image = &texture_cache.GetImage(id);
	} else if (image->info.IsDepth()) {
		if (storage) {
			EXIT("depth target cannot be bound as a storage image\n");
		}
		ValidateDepthTargetBinding(resource, descriptor, image, pixel_format, size.size);
		(void)SelectSampledDepthView(image->info.pixel_format, pixel_format,
		                             descriptor.DstSelXYZW());
	} else if (storage) {
		ValidateStorageColorView(image->info.pixel_format, view_format, descriptor.DstSelXYZW());
	} else {
		(void)SelectSampledColorView(image->info.pixel_format, pixel_format,
		                             descriptor.DstSelXYZW());
	}
	return {id, nullptr, std::move(desc)};
}

static vk::Sampler NativeSampler(RenderContext&                       context,
                                 const ShaderRecompiler::IR::Program& program, uint32_t index,
                                 const ShaderRecompiler::IR::DescriptorValue& value) {
	ShaderSamplerResource descriptor;
	CopyNativeDescriptor(value, descriptor.fields);
	const bool depth_compare = std::any_of(program.info.sampled_pairs.begin(),
	                                       program.info.sampled_pairs.end(), [&](const auto& pair) {
		                                       return pair.sampler == index &&
		                                              pair.image < program.info.images.size() &&
		                                              program.info.images[pair.image].depth_compare;
	                                       });
	if (!depth_compare) {
		descriptor.fields[0] &= ~(0x7u << 12u);
	}
	return context.GetSamplerCache().GetSampler(descriptor);
}

static BufferView NativeUpload(RenderContext& context, CommandBuffer& command_buffer,
                               std::span<const uint32_t> data) {
	EXIT_IF(data.empty());
	EXIT_IF(command_buffer.IsInvalid() || command_buffer.IsExecute());
	auto binding = context.GetBufferCache().UploadTransient(data.data(), data.size_bytes(), 256);
	return {.owner  = std::move(binding.owner),
	        .buffer = binding.buffer,
	        .offset = binding.offset,
	        .range  = data.size_bytes()};
}

void RenderExecutor::TrackImageBinding(ImageId id) {
	auto image = m_context.GetTextureCache().ResolveOwner(id);
	EXIT_IF(image == nullptr);
	if (std::ranges::find(m_bound_images, image) == m_bound_images.end()) {
		m_bound_images.push_back(std::move(image));
	}
}

void RenderExecutor::BindImage(ImageId id, bool storage) {
	auto& image = m_context.GetTextureCache().GetImage(id);
	if (image.info.data.Empty()) {
		return;
	}
	if (image.binding.is_bound) {
		image.binding.force_general |= storage;
	}
	image.binding.is_bound = true;
	TrackImageBinding(id);
}

void RenderExecutor::BindRenderTarget(ImageId id) {
	auto& image             = m_context.GetTextureCache().GetImage(id);
	image.binding.is_target = true;
	TrackImageBinding(id);
}

void RenderExecutor::ResetBindings() {
	for (const auto& image: m_bound_images) {
		image->binding = {};
	}
	m_bound_images.clear();
}

DescriptorCache::PreparedBindings RenderExecutor::PrepareBindings(CommandBuffer&            buffer,
                                                                  const ShaderStageRuntime& runtime,
                                                                  vk::ShaderStageFlags shader_stage,
                                                                  DescriptorCache::Stage stage) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(!runtime);
	const auto& program  = *runtime.program;
	const auto& snapshot = *runtime.resources;
	std::string error;
	if (!ShaderRecompiler::IR::ValidateResourceSpecialization(program, snapshot, &error)) {
		EXIT("invalid native shader runtime snapshot: %s\n", error.c_str());
	}

	DescriptorCache::PreparedBindings prepared;
	prepared.program      = runtime.program;
	prepared.snapshot     = runtime.resources;
	prepared.shader_stage = shader_stage;
	prepared.stage        = stage;
	auto& descriptors     = prepared.resources;
	descriptors.buffers.reserve(program.info.buffers.size());
	descriptors.images.reserve(program.info.images.size());
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		auto binding = ResolveTexture(program.info.images[i], snapshot.images[i]);
		BindImage(binding.image_id, binding.desc.type == TextureCache::BindingType::Storage);
		descriptors.images.push_back(binding);
	}
	descriptors.samplers.reserve(program.info.samplers.size());
	for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
		descriptors.samplers.push_back(NativeSampler(m_context, program, i, snapshot.samplers[i]));
	}
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::FlattenedSrt) !=
	    nullptr) {
		prepared.flattened_srt.assign(snapshot.flattened_srt.begin(), snapshot.flattened_srt.end());
	}

	prepared.user_data.reserve(program.bindings.ShaderDataDwords());
	for (const auto reg: program.bindings.user_data_registers) {
		prepared.user_data.push_back(snapshot.user_data[reg - program.user_data_base]);
	}
	prepared.user_data.resize(program.bindings.ShaderDataDwords());
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::Gds) != nullptr) {
		descriptors.gds.buffer = m_context.GetBufferCache().GetGdsBuffer().Handle();
	}
	return prepared;
}

void RenderExecutor::RebindBuffers(CommandBuffer&                     buffer,
                                   DescriptorCache::PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.program == nullptr || prepared.snapshot == nullptr);
	const auto& program   = *prepared.program;
	const auto& snapshot  = *prepared.snapshot;
	auto&       resources = prepared.resources;
	const auto& layout    = program.bindings;

	resources.buffers.clear();
	resources.buffers.reserve(program.info.buffers.size());
	EXIT_IF(prepared.user_data.size() != layout.ShaderDataDwords());
	std::fill(prepared.user_data.begin() + layout.buffer_offset_dword, prepared.user_data.end(), 0);
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		ShaderBufferResource descriptor;
		CopyNativeDescriptor(snapshot.buffers[i], descriptor.fields);
		uint32_t buffer_offset = 0;
		resources.buffers.push_back(NativeStorageBuffer(m_context, buffer, descriptor,
		                                                program.info.buffers[i], buffer_offset));
		const auto dword = layout.buffer_offset_dword + i / 4u;
		const auto shift = (i % 4u) * 8u;
		prepared.user_data[dword] |= buffer_offset << shift;
	}
	resources.addresses.clear();
	resources.addresses.reserve(program.info.addresses.size());
	for (uint32_t i = 0; i < program.info.addresses.size(); i++) {
		resources.addresses.push_back(NativeAddressBuffer(
		    m_context, buffer, program.info.addresses[i], snapshot.addresses[i]));
	}
}

void RenderExecutor::RebindImages(CommandBuffer&                     buffer,
                                  DescriptorCache::PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.program == nullptr || prepared.snapshot == nullptr);
	const auto& program  = *prepared.program;
	const auto& snapshot = *prepared.snapshot;
	auto&       images   = prepared.resources.images;
	EXIT_IF(images.size() != program.info.images.size());
	auto& texture_cache = m_context.GetTextureCache();
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto old_image = texture_cache.ResolveOwner(images[i].image_id);
		if (old_image == nullptr || (!old_image->registered && !old_image->info.data.Empty()) ||
		    old_image->binding.needs_rebind) {
			if (old_image != nullptr) {
				old_image->binding = {};
			}
			images[i] = ResolveTexture(program.info.images[i], snapshot.images[i]);
			BindImage(images[i].image_id,
			          images[i].desc.type == TextureCache::BindingType::Storage);
		}
		auto& binding      = images[i];
		binding.image_view = texture_cache.FindTexture(binding.image_id, binding.desc);
		auto&      image   = texture_cache.GetImage(binding.image_id);
		const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
		image.usage.storage |= storage;
		image.usage.texture |= !storage;
	}
}

RenderExecutor::GraphicsBindings
RenderExecutor::PrepareGraphicsBindings(CommandBuffer& buffer, const ShaderStageRuntime& vertex,
                                        const ShaderStageRuntime& pixel, bool pixel_active) {
	GraphicsBindings bindings {
	    .vertex = PrepareBindings(buffer, vertex, vk::ShaderStageFlagBits::eVertex,
	                              DescriptorCache::Stage::Vertex),
	};
	RebindBuffers(buffer, bindings.vertex);
	RebindImages(buffer, bindings.vertex);
	if (pixel_active) {
		bindings.pixel.emplace(PrepareBindings(buffer, pixel, vk::ShaderStageFlagBits::eFragment,
		                                       DescriptorCache::Stage::Pixel));
		RebindBuffers(buffer, *bindings.pixel);
		RebindImages(buffer, *bindings.pixel);
	}
	return bindings;
}

static void RetainBindings(CommandBuffer&                            buffer,
                           const DescriptorCache::NativeDescriptors& resources) {
	if (resources.flattened_srt.owner != nullptr) {
		buffer.RetainResourceUntilFence(resources.flattened_srt.owner);
	}
	if (resources.user_data.owner != nullptr) {
		buffer.RetainResourceUntilFence(resources.user_data.owner);
	}
	for (const auto& view: resources.buffers) {
		if (view.owner != nullptr) {
			buffer.RetainResourceUntilFence(view.owner);
		}
	}
	for (const auto& view: resources.addresses) {
		if (view.owner != nullptr) {
			buffer.RetainResourceUntilFence(view.owner);
		}
	}
}

void RenderExecutor::CommitBindings(CommandBuffer&                     buffer,
                                    vk::PipelineBindPoint              pipeline_bind_point,
                                    vk::PipelineLayout                 layout,
                                    DescriptorCache::PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.program == nullptr || prepared.stage == DescriptorCache::Stage::Unknown ||
	        prepared.committed);
	const auto& program     = *prepared.program;
	auto&       descriptors = prepared.resources;
	if (!prepared.flattened_srt.empty()) {
		descriptors.flattened_srt = NativeUpload(m_context, buffer, prepared.flattened_srt);
	}
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::UserData) != nullptr) {
		descriptors.user_data = NativeUpload(m_context, buffer, prepared.user_data);
	}
	RetainBindings(buffer, descriptors);

	auto       vk_buffer     = buffer.Handle();
	const auto shader_stages = ShaderPipelineStages(prepared.shader_stage);
	if (descriptors.gds.buffer != nullptr) {
		buffer.EndRendering();
		const auto barrier = MakeGdsDependency(descriptors.gds.buffer);
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eTransfer |
		        vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader,
		    shader_stages, vk::DependencyFlags {}, 0, nullptr, 1, &barrier, 0, nullptr);
	}

	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		auto&       image   = m_context.GetTextureCache().GetImage(descriptors.images[i].image_id);
		auto&       binding = descriptors.images[i];
		const auto& view    = binding.desc.view_info;
		const ImageSubresourceRange range {view.base_level, view.level_count, view.base_layer,
		                                   view.layer_count};
		const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
		if (image.info.data.Empty()) {
			image.Transit(vk::ImageLayout::eGeneral,
			              storage
			                  ? vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
			                  : vk::AccessFlagBits2::eShaderRead,
			              range, vk_buffer);
		} else if ((image.binding.force_general || image.binding.is_target) &&
		           !image.info.IsDepth()) {
			image.Transit(vk::ImageLayout::eGeneral,
			              vk::AccessFlagBits2::eShaderRead |
			                  vk::AccessFlagBits2::eColorAttachmentRead |
			                  vk::AccessFlagBits2::eColorAttachmentWrite,
			              {}, vk_buffer);
		} else if (storage) {
			image.Transit(vk::ImageLayout::eGeneral,
			              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
			              range, vk_buffer);
		} else {
			image.Transit(image.info.IsDepth() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
			                                   : vk::ImageLayout::eShaderReadOnlyOptimal,
			              vk::AccessFlagBits2::eShaderRead, range, vk_buffer);
		}
		binding.layout = image.backing.state.layout;
	}

	if (!program.bindings.descriptors.empty()) {
		auto& set =
		    m_context.GetDescriptorCache().GetDescriptor(prepared.stage, program, descriptors);
		vk_buffer.bindDescriptorSets(pipeline_bind_point, layout, program.bindings.descriptor_set,
		                             1, &set.set, 0, nullptr);
		buffer.RecycleDescriptorAfterFence(set);
	}
	if (program.bindings.push_constant_size != 0) {
		EXIT_IF(program.bindings.push_constant_size !=
		        prepared.user_data.size() * sizeof(uint32_t));
		vk_buffer.pushConstants(layout, prepared.shader_stage,
		                        program.bindings.push_constant_offset,
		                        program.bindings.push_constant_size, prepared.user_data.data());
	}
	prepared.committed = true;
}

} // namespace Libs::Graphics
