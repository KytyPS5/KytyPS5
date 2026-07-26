#include "graphics/host_gpu/renderer/descriptors.h"

#include "graphics/host_gpu/guestMemoryWindow.h"

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
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderTargetBarriers.h"
#include "graphics/host_gpu/renderer/shaderResourceBarrier.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/displayBuffer.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <atomic>
#include <fmt/format.h>
#include <limits>
#include <span>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace Libs::Graphics {

using TextureVariant = DescriptorCache::TextureVariant;

static void BindNullStorageBuffer(CommandBuffer& cmd_buffer, BufferView& dst) {
	dst.buffer = &GetRenderContext().GetBufferCache().ObtainNullBuffer(cmd_buffer);
	dst.offset = 0;
	dst.range  = 16;
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

static int SampledArrayViewIndex(const VulkanImage& image, int view_index) {
	switch (view_index) {
		case VulkanImage::VIEW_DEFAULT: return VulkanImage::VIEW_DEFAULT_ARRAY;
		default: return view_index;
	}
}

static int SelectSampledTextureArrayView(const VulkanImage& image, int base_view) {
	const int array_view = SampledArrayViewIndex(image, base_view);
	if (image.image_view[array_view] == nullptr) {
		EXIT("missing sampled array image view: image_type=%s base_view=%d array_view=%d "
		     "layers=%u\n",
		     VulkanImageTypeName(image.type), base_view, array_view, image.layers);
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

static VulkanImage& GetDummySampledTexture(TextureVariant variant) {
	return GetRenderContext().GetTextureCache().GetDummySampledTexture(
	    TextureVariantIsUint(variant), TextureVariantIs3D(variant));
}

static VulkanImage& GetDummyStorageTexture(TextureVariant variant) {
	return GetRenderContext().GetTextureCache().GetDummyStorageTexture(
	    TextureVariantIsUint(variant), TextureVariantIs3D(variant));
}

static void CopyNativeDescriptor(const ShaderRecompiler::IR::DescriptorValue& source,
                                 std::span<uint32_t>                          destination) {
	EXIT_IF(source.dword_count != destination.size());
	std::copy_n(source.dwords.begin(), destination.size(), destination.begin());
}

static BufferView NativeStorageBuffer(uint64_t submit_id, CommandBuffer& command_buffer,
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
		BindNullStorageBuffer(command_buffer, result);
		return result;
	}
	const auto& graphics  = GetRenderContext().GetGraphics();
	const auto  alignment = graphics.StorageMinAlignment();
	if (alignment == 0 ||
	    size > graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange ||
	    BufferCache::CACHING_PAGE_SIZE % alignment != 0) {
		EXIT("storage buffer range or device alignment is unsupported\n");
	}
	(void)submit_id;
	auto binding = GetRenderContext().GetBufferCache().ObtainBuffer(
	    command_buffer, address, size, resource.written, resource.read, resource.formatted);
	if (binding.offset % alignment != 0) {
		EXIT("storage buffer binding is not device-aligned\n");
	}
	result.buffer = &binding.buffer;
	result.offset = binding.offset;
	result.range  = static_cast<vk::DeviceSize>(size);
	return result;
}

static BufferView NativeUpload(CommandBuffer& command_buffer, std::span<const uint32_t> data);

static BufferView
NativeAddressBuffer(uint64_t submit_id, CommandBuffer& command_buffer,
                    const ShaderRecompiler::IR::AddressResource&           resource,
                    const ShaderRecompiler::IR::ResourceSnapshot::Address& address) {
	BufferView result;
	// Guest-window resources are backed by the imported guest pages rather than anything the
	// buffer cache owns, so they bypass the guest-address lookup entirely.
	if (resource.guest_window_role == ShaderRecompiler::IR::GuestWindowRole::Chunk) {
		auto& window = GetGuestMemoryWindow();
		if (!window.IsAvailable() || resource.guest_window_index >= window.ChunkCount()) {
			BindNullStorageBuffer(command_buffer, result);
			return result;
		}
		const auto& chunk = window.Chunks()[resource.guest_window_index];
		result.buffer     = const_cast<VulkanBuffer*>(&chunk);
		result.offset     = 0;
		result.range      = chunk.buffer_size;
		return result;
	}
	if (resource.guest_window_role == ShaderRecompiler::IR::GuestWindowRole::RangeTable) {
		auto&      window = GetGuestMemoryWindow();
		const auto& ranges = window.Ranges();
		// A fixed-size table keeps the shader's unrolled search branchless; unused slots are zeroed
		// and never match because a zero size fails the containment test.
		std::array<uint32_t, ShaderRecompiler::IR::ShaderInfo::MaxGuestWindowRanges * 8u> table {};
		const auto count =
		    std::min<size_t>(ranges.size(), ShaderRecompiler::IR::ShaderInfo::MaxGuestWindowRanges);
		for (size_t i = 0; i < count; i++) {
			std::memcpy(table.data() + i * 8u, &ranges[i], sizeof(ranges[i]));
		}
		return NativeUpload(command_buffer, table);
	}
	if (address.binding_base == 0) {
		BindNullStorageBuffer(command_buffer, result);
		return result;
	}
	if (resource.written) {
		EXIT("writable address resources are unsupported\n");
	}
	const auto limit  = resource.kind == ShaderRecompiler::IR::ResourceKind::Flat
	                        ? ShaderRecompiler::IR::FlatAddressWindowSize
	                        : static_cast<uint64_t>(GetRenderContext()
	                                                    .GetGraphics()
	                                                    .GetPhysicalDeviceProperties()
	                                                    .limits.maxStorageBufferRange);
	uint64_t   size   = 0;
	const auto access = HostMemoryAccess::Mapped;
	if (!HostMemoryQueryRange(address.binding_base, limit, access, size)) {
		EXIT("address resource is not host-accessible: base=0x%016" PRIx64 "\n",
		     address.binding_base);
	}
	const auto& graphics  = GetRenderContext().GetGraphics();
	const auto  alignment = graphics.StorageMinAlignment();
	if (alignment == 0 ||
	    size > graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange ||
	    BufferCache::GetBufferOffset(address.binding_base) % alignment != 0) {
		EXIT("address resource range or alignment is unsupported\n");
	}
	(void)submit_id;
	auto binding  = GetRenderContext().GetBufferCache().ObtainBuffer(command_buffer,
	                                                                 address.binding_base, size);
	result.buffer = &binding.buffer;
	result.offset = binding.offset;
	result.range  = static_cast<vk::DeviceSize>(size);
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
	bool supported_dimension = false;
	switch (resource.dimension) {
		case ShaderRecompiler::Decoder::ImageDimension::Dim2D:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DArray:
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
		default: return {};
	}
}

bool IsSupportedSampledVideoOutView(const ShaderRecompiler::IR::ImageResource& resource,
                                    const ShaderTextureResource&               descriptor,
                                    const VulkanImage&                         image) {
	return image.type == VulkanImageType::VideoOut && image.layers == 1 &&
	       IsSupportedSampledColorResource(resource) &&
	       resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	       descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
	       descriptor.Depth() == 0 && descriptor.BaseArray5() == 0;
}

bool IsSupportedDepthTargetDescriptor(const ShaderTextureResource& descriptor,
                                      const VulkanImage&           image) {
	const auto width  = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto pitch  = TileGetTexturePitch(descriptor.Format(), width, 1, descriptor.TileMode());
	const auto type   = static_cast<Prospero::ImageType>(descriptor.Type());
	const bool stencil_view =
	    descriptor.Format() == Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt) &&
	    IsStencilAspectFormat(image.format);
	const bool supported_single_layer =
	    image.layers == 1 && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0 &&
	    (type == Prospero::ImageType::kColor2D || type == Prospero::ImageType::kColor2DArray);
	const bool supported_cube = type == Prospero::ImageType::kCube && width == height &&
	                            image.layers >= 6 && image.layers % 6u == 0 &&
	                            static_cast<uint32_t>(descriptor.Depth()) + 1u == image.layers &&
	                            descriptor.BaseArray5() == 0;
	return image.type == VulkanImageType::DepthStencil && width == image.extent.width &&
	       height == image.extent.height && (supported_single_layer || supported_cube) &&
	       descriptor.BaseLevel() == 0 && descriptor.LastLevel() == 0 && descriptor.MaxMip() == 0 &&
	       descriptor.MinLod() == 0 && descriptor.BaseArray5() == 0 &&
	       descriptor.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
	       descriptor.BCSwizzle() == 0 && !descriptor.MsaaDepth() && pitch >= width &&
	       (stencil_view || pitch == image.guest_pitch);
}

bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	constexpr uint32_t field3_common        = 0x01800000u;
	constexpr uint32_t field5_perf_mod_mask = 0x00700000u;
	const uint32_t     field3_expected =
	    (descriptor.Type() << 28u) | field3_common | descriptor.DstSelXYZW();
	const uint32_t field4_expected = descriptor.Depth() | (descriptor.BaseArray5() << 16u);
	return (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	       (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	       descriptor.fields[3] == field3_expected && descriptor.fields[4] == field4_expected &&
	       (descriptor.fields[5] & ~field5_perf_mod_mask) == 0 && descriptor.fields[6] == 0 &&
	       descriptor.fields[7] == 0;
}

static void ValidateDepthTargetBinding(const ShaderRecompiler::IR::ImageResource& resource,
                                       const ShaderTextureResource&               descriptor,
                                       const VulkanImage* image, vk::Format view_format,
                                       uint64_t size) {
	const bool stencil =
	    image != nullptr && IsSupportedSampledStencilFormat(image->format, descriptor.Format(),
	                                                        view_format);
	const bool resource_ok = stencil ? IsSupportedSampledDepthUintResource(resource)
	                                 : IsSupportedSampledDepthResource(resource);
	const bool descriptor_ok =
	    image != nullptr && IsSupportedDepthTargetDescriptor(descriptor, *image);
	const bool encoding_ok = IsSupportedDepthTextureEncoding(descriptor);
	const bool format_ok =
	    image != nullptr &&
	    (IsSupportedSampledDepthFormat(image->format, descriptor.Format(), view_format) || stencil);
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
	                      : static_cast<int>(image->format),
	     static_cast<int>(view_format), image == nullptr ? 0u : image->layers, descriptor.Type(),
	     descriptor.BaseArray5(), descriptor.Depth(), descriptor_pitch,
	     image == nullptr ? 0u : image->guest_pitch, descriptor.Base40(), size,
	     descriptor.fields[0], descriptor.fields[1], descriptor.fields[2], descriptor.fields[3],
	     descriptor.fields[4], descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

static bool IsSupportedStorageTextureDescriptor(const ShaderRecompiler::IR::ImageResource& resource,
                                                const ShaderTextureResource& descriptor) {
	const auto tile   = descriptor.TileMode();
	const auto width  = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto depth  = static_cast<uint32_t>(descriptor.Depth()) + 1u;
	// The decoder has no separate 1D dimension (ImageDimension only distinguishes 2D/3D/2D-array);
	// a guest 1D image storage access decodes as Dim2D, so a kColor1D descriptor is a legitimate
	// match for it here, not just kColor2D.
	const bool is_2d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	                   (descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) ||
	                    descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor1D)) &&
	                   descriptor.Depth() == 0;
	const bool is_2d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray &&
	    descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
	    descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_3d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D &&
	                   descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor3D);
	// A depth-tiled storage access can read the depth buffer's raw bits via a uint binding
	// (StorageImageUint) or its values directly via a float binding (StorageImage); ValidateStorageTexture
	// has already checked the resource kind and guest format agree (both uint or both float) before
	// this is reached, so accepting either kind here is safe.
	const bool supported_depth_tile =
	    tile == Prospero::GpuEnumValue(Prospero::TileMode::kDepth) && !resource.read &&
	    (resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint ||
	     resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImage) &&
	    IsSupportedStorageDepthTile(descriptor.Format(), descriptor.Type(), width, height, depth);
	const bool supported_tile = tile == Prospero::GpuEnumValue(Prospero::TileMode::kLinear) ||
	                            tile == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) ||
	                            supported_depth_tile;
	const bool supported_swizzle =
	    IsSupportedStorageSwizzle(descriptor.Format(), descriptor.DstSelXYZW()) &&
	    (descriptor.DstSelXYZW() == DstSel(4, 5, 6, 7) || !resource.read);
	const bool supported_mip_view = descriptor.BaseLevel() == 0 || is_2d;
	// MaxMip is not a hard upper bound for the explicitly selected view: tail-only storage
	// descriptors can select BaseLevel == LastLevel == MaxMip + 1. NativeTexture sizes the
	// backing from max(LastLevel, MaxMip), so requiring LastLevel <= MaxMip here would reject a
	// view the decoded allocation can represent.
	return (is_2d || is_2d_array || is_3d) && supported_tile && supported_mip_view &&
	       descriptor.BaseLevel() == descriptor.LastLevel() &&
	       descriptor.MinLod() == 0 && descriptor.BaseArray5() == 0 && supported_swizzle &&
	       descriptor.BCSwizzle() == 0 && !descriptor.MsaaDepth();
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
	// fields[6]/[7] carry DCC/CMASK compression metadata (address, block-size limits, etc.) on a
	// storage descriptor built from a compressed render target -- e.g. a post-process compute
	// pass writing back into the display buffer. This engine doesn't implement DCC (render
	// targets are always stored uncompressed; RenderColorTarget already just tolerates the
	// DCC/CMASK pipe-config flags rather than acting on them), and the ordinary sampled-texture
	// path never inspects these fields either, so they carry no information this validator needs.
	return (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	       (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	       descriptor.fields[3] == expected_field3 && descriptor.fields[4] == descriptor.Depth() &&
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
	const bool format_ok = Prospero::IsSupportedTextureFormat(format) &&
	                       uint_resource == Prospero::IsUintTextureFormat(format);
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	EXIT("unsupported storage texture: resource=%d descriptor=%d encoding=%d format=%d "
	     "kind=%u dimension=%u mip_mode=%u atomic=%d compare=%d "
	     "base_level=%u last_level=%u max_mip=%u min_lod=%u base_array=%u bc=%u msaa=%d "
	     "depth_tile_shape=%d swizzle_ok=%d "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " extent=%ux%ux%u type=%u format=%u tile=%u swizzle=0x%03x read=%d written=%d "
	     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok, static_cast<uint32_t>(resource.kind),
	     static_cast<uint32_t>(resource.dimension), static_cast<uint32_t>(resource.mip_mode),
	     resource.atomic, resource.depth_compare, descriptor.BaseLevel(), descriptor.LastLevel(),
	     descriptor.MaxMip(), descriptor.MinLod(), descriptor.BaseArray5(), descriptor.BCSwizzle(),
	     descriptor.MsaaDepth(),
	     IsSupportedStorageDepthTile(descriptor.Format(), descriptor.Type(),
	                                 static_cast<uint32_t>(descriptor.Width5()) + 1u,
	                                 static_cast<uint32_t>(descriptor.Height5()) + 1u,
	                                 static_cast<uint32_t>(descriptor.Depth()) + 1u),
	     IsSupportedStorageSwizzle(descriptor.Format(), descriptor.DstSelXYZW()),
	     descriptor.Base40(), size, static_cast<uint32_t>(descriptor.Width5()) + 1u,
	     static_cast<uint32_t>(descriptor.Height5()) + 1u,
	     static_cast<uint32_t>(descriptor.Depth()) + 1u, descriptor.Type(), format,
	     descriptor.TileMode(), descriptor.DstSelXYZW(), resource.read, resource.written,
	     descriptor.fields[0], descriptor.fields[1], descriptor.fields[2], descriptor.fields[3],
	     descriptor.fields[4], descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

void ValidateMetadataReuseTexture(const ShaderRecompiler::IR::ImageResource& resource,
                                  const ShaderTextureResource& descriptor, uint64_t size) {
	constexpr uint32_t field1_reserved = 0x200fff00u;
	constexpr uint32_t field2_reserved = 0xf0003000u;
	constexpr uint32_t field5_common   = 0x00700000u;
	const auto         format          = descriptor.Format();
	const bool         resource_ok     = IsSupportedSampledColorResource(resource);
	const bool         swizzle_ok      = IsValidSampledColorSwizzle(descriptor.DstSelXYZW());
	const bool         descriptor_ok =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	    descriptor.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
	    descriptor.Depth() == 0 && descriptor.BaseArray5() == 0 &&
	    descriptor.BaseLevel() <= descriptor.LastLevel() &&
	    descriptor.LastLevel() <= descriptor.MaxMip() && descriptor.MaxMip() < 15 &&
	    descriptor.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB) &&
	    swizzle_ok;
	const uint32_t field3_expected = descriptor.DstSelXYZW() |
	                                 (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	                                 (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	                                 (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                 (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t field4_expected = descriptor.Depth() | (descriptor.BaseArray5() << 16u);
	const uint32_t field5_expected =
	    field5_common | (static_cast<uint32_t>(descriptor.MaxMip()) << 4u);
	const bool encoding_ok =
	    (descriptor.fields[1] & field1_reserved) == 0 &&
	    (descriptor.fields[2] & field2_reserved) == 0 && descriptor.fields[3] == field3_expected &&
	    descriptor.fields[4] == field4_expected && descriptor.fields[5] == field5_expected &&
	    descriptor.fields[6] == 0 && descriptor.fields[7] == 0;
	const bool format_ok =
	    Prospero::IsSupportedTextureFormat(format) && !Prospero::IsUintTextureFormat(format);
	if (!resource_ok || !descriptor_ok || !encoding_ok || !format_ok || size == 0) {
		EXIT("unsupported metadata-reuse sampled texture: resource=%d descriptor=%d encoding=%d "
		     "format=%d "
		     "kind=%u dimension=%u mip_mode=%u read=%d written=%d atomic=%d compare=%d "
		     "base_level=%u last_level=%u max_mip=%u base_array=%u swizzle_ok=%d "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " extent=%ux%ux%u type=%u format=%u tile=%u swizzle=0x%03x "
		     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
		     resource_ok, descriptor_ok, encoding_ok, format_ok,
		     static_cast<uint32_t>(resource.kind), static_cast<uint32_t>(resource.dimension),
		     static_cast<uint32_t>(resource.mip_mode), resource.read, resource.written,
		     resource.atomic, resource.depth_compare, descriptor.BaseLevel(),
		     descriptor.LastLevel(), descriptor.MaxMip(), descriptor.BaseArray5(), swizzle_ok,
		     descriptor.Base40(), size, static_cast<uint32_t>(descriptor.Width5()) + 1u,
		     static_cast<uint32_t>(descriptor.Height5()) + 1u,
		     static_cast<uint32_t>(descriptor.Depth()) + 1u, descriptor.Type(), format,
		     descriptor.TileMode(), descriptor.DstSelXYZW(), descriptor.fields[0],
		     descriptor.fields[1], descriptor.fields[2], descriptor.fields[3], descriptor.fields[4],
		     descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
	}
}

static DescriptorCache::TextureBinding
NativeTexture(uint64_t submit_id, CommandBuffer& command_buffer,
              const ShaderRecompiler::IR::ImageResource&   resource,
              const ShaderRecompiler::IR::DescriptorValue& value) {
	ShaderTextureResource descriptor;
	CopyNativeDescriptor(value, descriptor.fields);
	const bool storage = resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
	                     resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint;
	const auto variant = NativeTextureVariant(resource);
	if (storage) {
		ValidateStorageImageResource(resource);
	}
	if (descriptor.IsNull()) {
		return storage ? DescriptorCache::TextureBinding {&GetDummyStorageTexture(variant),
		                                                  TextureVariantDefaultView(variant)}
		               : DescriptorCache::TextureBinding {&GetDummySampledTexture(variant),
		                                                  TextureVariantDefaultView(variant)};
	}
	const auto address    = descriptor.Base40();
	const auto width      = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height     = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto base_level = descriptor.BaseLevel();
	const auto last_level = descriptor.LastLevel();
	// LastLevel selects the end of this view, while MaxMip is an allocation/filtering hint and
	// can lag behind it on a tail-only descriptor. The backing must cover whichever field names
	// the highest mip. renderCompute's descriptor diagnostics use the same rule.
	const auto levels =
	    std::max(static_cast<uint32_t>(last_level),
	             static_cast<uint32_t>(descriptor.MaxMip())) +
	    1u;
	if (base_level > last_level || last_level >= levels) {
		EXIT("unsupported texture mip view: base=%u last=%u levels=%u\n", base_level, last_level,
		     levels);
	}
	const auto view_levels = last_level - base_level + 1u;
	const auto depth       = static_cast<uint32_t>(descriptor.Depth()) + 1u;
	const auto tile        = descriptor.TileMode();
	const auto format      = descriptor.Format();
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
	const auto view_format = TextureGetFormat(format);
	const auto type        = static_cast<Prospero::ImageType>(descriptor.Type());
	const auto target_view =
	    ResolveTargetTextureView(resource, type, descriptor.BaseArray5(), depth);
	const auto    pitch   = TileGetTexturePitch(format, width, levels, tile);
	const auto    swizzle = descriptor.DstSelXYZW();
	TileSizeAlign size;
	TileGetTextureTotalSize(format, width, height, depth, pitch, levels, tile,
	                        type == Prospero::ImageType::kColor3D, size);
	EXIT_NOT_IMPLEMENTED(size.size == 0 ||
	                     (address & (static_cast<uint64_t>(size.align) - 1u)) != 0);
	if (storage) {
		ValidateStorageTexture(resource, descriptor, size.size);
		GetRenderContext().GetBufferCache().ValidateGpuAccess(address, size.size, resource.read,
		                                                      resource.written);
	}

	VulkanImage*  image      = nullptr;
	int           view       = VulkanImage::VIEW_DEFAULT;
	vk::ImageView image_view = nullptr;
	const bool check_depth = static_cast<Prospero::TileMode>(tile) == Prospero::TileMode::kDepth ||
	                         descriptor.MsaaDepth();
	if (image == nullptr) {
		if (check_depth) {
			image = GetRenderContext().GetTextureCache().FindDepthTargetByRange(
			    command_buffer, address, size.size, true);
		} else {
			image = GetRenderContext().GetTextureCache().FindRenderTargetByRange(
			    command_buffer, address, size.size);
		}
		if (image != nullptr) {
			if (check_depth) {
				const bool uint_reinterpret =
				    IsSupportedSampledDepthUintResource(resource) &&
				    IsSupportedDepthTargetDescriptor(descriptor, *image) &&
				    IsSupportedDepthTextureEncoding(descriptor) &&
				    IsDepthUintTextureReinterpretation(image->format, descriptor.Format(),
				                                       view_format);
				const bool uint_storage_reinterpret =
				    storage && IsSupportedStorageImageResource(resource) &&
				    IsSupportedStorageTextureDescriptor(resource, descriptor) &&
				    IsSupportedStorageTextureEncoding(descriptor) &&
				    IsDepthUintTextureReinterpretation(image->format, descriptor.Format(),
				                                       view_format);
				const bool float_storage_direct =
				    storage && IsSupportedStorageImageResource(resource) &&
				    IsSupportedStorageTextureDescriptor(resource, descriptor) &&
				    IsSupportedStorageTextureEncoding(descriptor) &&
				    IsDepthFloatTextureDirectView(image->format, descriptor.Format(), view_format);
				if (uint_reinterpret || uint_storage_reinterpret || float_storage_direct) {
					image = nullptr;
				} else {
					const auto depth_view = ResolveTargetTextureView(
					    resource, type, descriptor.BaseArray5(), image->layers);
					ValidateDepthTargetBinding(resource, descriptor, image, view_format, size.size);
					if (depth_view.type ==
					    static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM)) {
						EXIT("unsupported sampled depth target view: dimension=%u "
						     "descriptor_type=%u "
						     "base_array=%u image_layers=%u\n",
						     static_cast<uint32_t>(resource.dimension), descriptor.Type(),
						     descriptor.BaseArray5(), image->layers);
					}
					image_view = GetRenderContext().GetTextureCache().GetDepthTargetSampledView(
					    *static_cast<DepthStencilVulkanImage*>(image), view_format, swizzle, 0, 1,
					    depth_view.type, depth_view.base_layer, depth_view.layer_count);
				}
			} else {
				if (!(storage ? IsSupportedStorageImageResource(resource)
				              : IsSupportedSampledColorResource(resource)) ||
				    image->type != VulkanImageType::RenderTexture || width != image->extent.width ||
				    height != image->extent.height ||
				    (storage ? levels != image->mip_levels || base_level != 0
				             : levels != image->mip_levels || base_level >= levels) ||
				    target_view.type ==
				        static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM) ||
				    target_view.base_layer >= image->layers ||
				    target_view.layer_count > image->layers - target_view.base_layer) {
					EXIT("unsupported cached render-target image view: storage=%d resource=%u "
					     "dimension=%u"
					     " image_type=%u layers=%u extent=%ux%u/%ux%u depth=%u"
					     " levels=%u/%u base_level=%u base_array=%u descriptor_type=%u\n",
					     storage, static_cast<uint32_t>(resource.kind),
					     static_cast<uint32_t>(resource.dimension),
					     static_cast<uint32_t>(image->type), image->layers, width, height,
					     image->extent.width, image->extent.height, depth, levels,
					     image->mip_levels, base_level, descriptor.BaseArray5(),
					     static_cast<uint32_t>(type));
				}
				if (storage) {
					view       = SelectStorageColorView(image->format, view_format, swizzle);
					image_view = GetRenderContext().GetTextureCache().GetRenderTargetStorageView(
					    *static_cast<RenderTextureVulkanImage*>(image), view_format, base_level,
					    view_levels, target_view.type, target_view.base_layer,
					    target_view.layer_count);
				} else {
					image_view = GetRenderContext().GetTextureCache().GetSampledColorView(
					    *image, view_format, swizzle, base_level, view_levels, target_view.type,
					    target_view.base_layer, target_view.layer_count);
				}
			}
			if (image != nullptr && image_view == nullptr && image->image_view[view] == nullptr) {
				EXIT("required cached texture image view is missing\n");
			}
			if (storage && image != nullptr) {
				GetRenderContext().GetTextureCache().MarkGpuWritten(*image);
			}
		}
	}
	if (image == nullptr) {
		const auto video = Presentation::DisplayBufferFind(address);
		if (video.image != nullptr) {
			if (storage) {
				const bool exact =
				    resource.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint &&
				    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
				    !resource.read && resource.written && !resource.atomic &&
				    format == Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UInt) &&
				    view_format == vk::Format::eR8G8B8A8Uint && swizzle == DstSel(6, 5, 4, 7) &&
				    width == video.image->extent.width && height == video.image->extent.height &&
				    depth == 1 && levels == 1 && base_level == 0 && view_levels == 1 &&
				    type == Prospero::ImageType::kColor2D && video.size == size.size &&
				    video.pitch == pitch &&
				    (video.image->format == vk::Format::eR8G8B8A8Srgb ||
				     video.image->format == vk::Format::eB8G8R8A8Srgb) &&
				    video.image->image_view[VulkanImage::VIEW_STORAGE] != nullptr;
				if (!exact) {
					EXIT("unsupported storage access to video-out surface: format=%u view=%d"
					     " extent=%ux%u size=0x%016" PRIx64 " pitch=%u\n",
					     format, static_cast<int>(view_format), width, height, size.size, pitch);
				}
				image = video.image;
				view  = VulkanImage::VIEW_STORAGE;
				GetRenderContext().GetTextureCache().MarkGpuWritten(*image);
			} else {
				const bool exact =
				    IsSupportedSampledVideoOutView(resource, descriptor, *video.image) &&
				    width == video.image->extent.width && height == video.image->extent.height &&
				    levels == 1 && base_level == 0 && view_levels == 1 && video.size == size.size &&
				    video.pitch == pitch;
				if (!exact) {
					EXIT("unsupported sampled access to video-out surface: resource=%u dimension=%u"
					     " image_format=%d view_format=%d swizzle=0x%03x extent=%ux%u/%ux%u"
					     " depth=%u levels=%u base=%u count=%u type=%u size=0x%016" PRIx64
					     "/0x%016" PRIx64 " pitch=%u/%u\n",
					     static_cast<uint32_t>(resource.kind),
					     static_cast<uint32_t>(resource.dimension),
					     static_cast<int>(video.image->format), static_cast<int>(view_format),
					     swizzle, width, height, video.image->extent.width,
					     video.image->extent.height, depth, levels, base_level, view_levels,
					     static_cast<uint32_t>(type), size.size, video.size, pitch, video.pitch);
				}
				image      = video.image;
				image_view = GetRenderContext().GetTextureCache().GetSampledColorView(
				    *video.image, view_format, swizzle, 0, 1, vk::ImageViewType::e2D, 0, 1);
			}
		}
	}
	if (image == nullptr) {
		auto&      texture_cache = GetRenderContext().GetTextureCache();
		const bool metadata_read = texture_cache.QueryRegion(address, size.size).metadata_pages;
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
			image      = &texture_cache.FindStorageTexture(command_buffer, info);
			view       = VulkanImage::VIEW_DEFAULT;
			image_view = texture_cache.GetStorageTextureStorageView(
			    *static_cast<StorageTextureVulkanImage*>(image), base_level);
		} else {
			image = &texture_cache.FindTexture(command_buffer, info, metadata_read);
			if (image->type == VulkanImageType::StorageTexture) {
				image_view = texture_cache.GetStorageTextureSampledView(
				    *static_cast<StorageTextureVulkanImage*>(image), info);
			}
		}
	}
	EXIT_NOT_IMPLEMENTED(image == nullptr);
	if (NeedsStaticSampledArrayView(resource.dimension ==
	                                    ShaderRecompiler::Decoder::ImageDimension::Dim2DArray,
	                                image_view != nullptr)) {
		view = SelectSampledTextureArrayView(*image, view);
	}
	return {image, view, image_view};
}

static vk::Sampler NativeSampler(const ShaderRecompiler::IR::Program& program, uint32_t index,
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
	return GetRenderContext().GetSamplerCache().GetSampler(descriptor);
}

static BufferView NativeUpload(CommandBuffer& command_buffer, std::span<const uint32_t> data) {
	EXIT_IF(data.empty());
	BufferView result;
	EXIT_IF(!GetRenderContext().GetBufferCache().UploadHostData(
	    command_buffer, data.data(), data.size_bytes(), 256, result.buffer, result.offset,
	    result.range));
	return result;
}

void BindDescriptors(uint64_t submit_id, CommandBuffer& buffer,
                     vk::PipelineBindPoint pipeline_bind_point, vk::PipelineLayout layout,
                     const ShaderStageRuntime& runtime, vk::ShaderStageFlags vk_stage,
                     DescriptorCache::Stage stage) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(!runtime);
	const auto& program  = *runtime.program;
	const auto& snapshot = *runtime.resources;
	std::string error;
	if (!ShaderRecompiler::IR::ValidateResourceSpecialization(program, snapshot, &error)) {
		EXIT("invalid native shader runtime snapshot: %s\n", error.c_str());
	}
	auto       vk_buffer     = buffer.Handle();
	const auto shader_stages = ShaderPipelineStages(vk_stage);

	DescriptorCache::NativeDescriptors descriptors;
	descriptors.buffers.reserve(program.info.buffers.size());
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		ShaderBufferResource descriptor;
		CopyNativeDescriptor(snapshot.buffers[i], descriptor.fields);
		descriptors.buffers.push_back(
		    NativeStorageBuffer(submit_id, buffer, descriptor, program.info.buffers[i]));
	}
	descriptors.images.reserve(program.info.images.size());
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto kind = program.info.images[i].kind;
		if ((kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
		     kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint) &&
		    vk_stage != vk::ShaderStageFlagBits::eCompute) {
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
	for (uint32_t i = 0; i < program.info.addresses.size(); i++) {
		descriptors.addresses.push_back(NativeAddressBuffer(
		    submit_id, buffer, program.info.addresses[i], snapshot.addresses[i]));
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
		descriptors.gds.buffer = &GetRenderContext().GetGdsBuffer().GetBuffer();
		const auto barrier     = MakeGdsDependency(*descriptors.gds.buffer);
		vk_buffer.pipelineBarrier(
		    vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eTransfer |
		        vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader,
		    shader_stages, vk::DependencyFlags {}, 0, nullptr, 1, &barrier, 0, nullptr);
	}

	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		auto*       image    = descriptors.images[i].image;
		const auto& resource = program.info.images[i];
		if (resource.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		    resource.kind == ShaderRecompiler::IR::ResourceKind::ImageUint) {
			switch (image->type) {
				case VulkanImageType::DepthStencil:
					GraphicsRenderDepthStencilBarrier(vk_buffer, *image);
					break;
				case VulkanImageType::RenderTexture:
				case VulkanImageType::StorageTexture:
					GraphicsRenderTextureBarrier(vk_buffer, *image);
					break;
				case VulkanImageType::VideoOut:
					GraphicsRenderColorImageBarrier(vk_buffer, *image, RENDER_COLOR_IMAGE_LAYOUT);
					break;
				default: break;
			}
		} else {
			const auto barrier =
			    MakeStorageImageDependency(*image, resource.read, resource.written);
			vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, shader_stages,
			                          vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &barrier);
			image->layout = vk::ImageLayout::eGeneral;
		}
	}

	if (!program.bindings.descriptors.empty()) {
		auto& set =
		    GetRenderContext().GetDescriptorCache().GetDescriptor(stage, program, descriptors);
		vk_buffer.bindDescriptorSets(pipeline_bind_point, layout, program.bindings.descriptor_set,
		                             1, &set.set, 0, nullptr);
		buffer.RecycleDescriptorAfterFence(set);
	}
	if (program.bindings.push_constant_size != 0) {
		EXIT_IF(program.bindings.push_constant_size != user_data.size() * sizeof(uint32_t));
		vk_buffer.pushConstants(layout, vk_stage, program.bindings.push_constant_offset,
		                        program.bindings.push_constant_size, user_data.data());
	}
}

} // namespace Libs::Graphics
