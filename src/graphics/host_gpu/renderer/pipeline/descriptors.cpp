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
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
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

namespace {

using BindingKind = ShaderRecompiler::IR::DescriptorBindingKind;

} // namespace

vk::DescriptorType NativeDescriptorType(BindingKind kind) {
	const auto image_class = ShaderRecompiler::IR::ImageBindingResourceClass(kind);
	if (image_class == ShaderRecompiler::IR::ImageResourceClass::Sampled) {
		return vk::DescriptorType::eSampledImage;
	}
	if (image_class == ShaderRecompiler::IR::ImageResourceClass::Storage) {
		return vk::DescriptorType::eStorageImage;
	}
	switch (kind) {
		case BindingKind::Samplers: return vk::DescriptorType::eSampler;
		case BindingKind::Buffers:
		case BindingKind::Gds:
		case BindingKind::BdaPagetable:
		case BindingKind::FaultBuffer:
		case BindingKind::FlattenedSrt:
		case BindingKind::ShaderData: return vk::DescriptorType::eStorageBuffer;
		case BindingKind::Count: EXIT("invalid native descriptor binding kind");
	}
	EXIT("invalid native descriptor binding kind");
}

uint32_t NativeDescriptorCount(const ShaderRecompiler::IR::DescriptorBinding& binding) {
	return binding.resources.empty() ? 1u : static_cast<uint32_t>(binding.resources.size());
}

vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture, uint32_t element) {
	const auto view =
	    texture.mip_views.empty()
	        ? (element == 0u ? texture.image_view : vk::ImageView {})
	        : (element < texture.mip_views.size() ? texture.mip_views[element] : vk::ImageView {});
	EXIT_IF(!texture.image_id || view == nullptr || texture.layout == vk::ImageLayout::eUndefined);
	return {nullptr, view, texture.layout};
}

static const char* ShaderStageResourceName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "Vertex";
		case ShaderType::Pixel: return "Pixel";
		case ShaderType::Compute: return "Compute";
		default: return "Unknown";
	}
}

static vk::ShaderStageFlags NativeShaderStage(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return vk::ShaderStageFlagBits::eVertex;
		case ShaderType::Pixel: return vk::ShaderStageFlagBits::eFragment;
		case ShaderType::Compute: return vk::ShaderStageFlagBits::eCompute;
		default: EXIT("unknown native shader stage\n");
	}
}

static void BindNullStorageBuffer(RenderContext& context, BufferView& dst) {
	dst.buffer = context.GetBufferCache().GetBuffer(NULL_BUFFER_ID).Handle();
	dst.offset = 0;
	dst.range  = 16;
}

static Prospero::ImageType TextureType(const ShaderTextureResource& descriptor) {
	const auto type = descriptor.Type();
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

// Descriptor-formatted byte components accept any byte alignment without a
// two-word load/store. The proof covers every use of the
// resource; one formatted use must not authorize a neighboring raw/typed access.
static bool SupportsByteStorageOffset(const ShaderBufferResource& descriptor,
                                      const ShaderRecompiler::IR::BufferResource& resource) {
	if (!resource.descriptor_formatted_only || !resource.formatted || resource.scalar ||
	    resource.atomic) return false;
	const auto format = ShaderRecompiler::Format::GetFormatInfo(descriptor.Format());
	if (format.type == ShaderRecompiler::Format::ComponentType::Unknown ||
	    format.packed_bitfield || format.component_count == 0u) return false;
	for (uint32_t component = 0; component < format.component_count; ++component) {
		if (format.component_bits[component] != 8u) return false;
	}
	return true;
}

static BufferView NativeStorageBuffer(RenderContext&                              context,
                                      const ShaderBufferResource&                 descriptor,
                                      const ShaderRecompiler::IR::BufferResource& resource,
                                      ShaderType stage, uint32_t slot, uint32_t& buffer_offset,
                                      BufferId id) {
	BufferView result;
	buffer_offset = 0;

	const auto address = descriptor.Base48();
	const auto stride  = descriptor.Stride();
	const auto records = descriptor.NumRecords();
	if (stride != 0 && records > UINT64_MAX / stride) {
		EXIT("storage buffer descriptor footprint overflow\n");
	}
	const auto requested_size = stride != 0 ? static_cast<uint64_t>(stride) * records : records;
	if (address == 0 || requested_size == 0) {
		BindNullStorageBuffer(context, result);
		return result;
	}
	const auto  size      = Libs::LibKernel::Memory::ClampRangeSize(address, requested_size);
	const auto& graphics  = context.GetGraphics();
	const auto  alignment = graphics.StorageMinAlignment();
	if (alignment == 0 ||
	    size > graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange) {
		EXIT("storage buffer range or device alignment is unsupported\n");
	}
	auto [buffer, offset] = context.GetBufferCache().ObtainBuffer(address, size, resource.written,
	                                                              resource.formatted, id);
	const auto aligned_offset = offset - offset % alignment;
	const auto adjustment     = offset - aligned_offset;
	const auto max_range      = graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange;
	const bool byte_adjustment = adjustment % sizeof(uint32_t) != 0;
	if ((byte_adjustment && !SupportsByteStorageOffset(descriptor, resource)) ||
	    adjustment >= 256 || adjustment > max_range || size > max_range - adjustment) {
		EXIT("storage buffer offset adjustment is unsupported\n");
	}
	// Runtime-array length counts complete DWORDs. A partial last DWORD needs
	// a separate guest-byte bound before its host backing can be rounded up.
	if (byte_adjustment && (size + adjustment) % sizeof(uint32_t) != 0) {
		EXIT("storage buffer offset adjustment is unsupported: partial DWORD tail\n");
	}
	buffer_offset = static_cast<uint32_t>(adjustment);
	result.buffer = buffer->Handle();
	result.offset = aligned_offset;
	result.range  = static_cast<vk::DeviceSize>(size + adjustment);
	if (resource.formatted && resource.written) {
		context.GetTextureCache().InvalidateMemoryFromGPU(address, size);
	}
	SetVulkanObjectNameF(
	    graphics.device, result.buffer,
	    "Kyty.{}.StorageBuffer[slot={} guest=0x{:016x} size=0x{:x} access={} formatted={}]",
	    ShaderStageResourceName(stage), slot, address, size,
	    resource.written ? (resource.read ? "ReadWrite" : "Write") : "Read", resource.formatted);
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
	return resource.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled &&
	       resource.numeric_class != Prospero::TextureNumericClass::Unsupported &&
	       supported_dimension && resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::None &&
	       resource.read && !resource.written && !resource.atomic && !resource.depth_compare;
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
	       descriptor.Type() == Prospero::ImageType::kColor2D && descriptor.Depth() == 0 &&
	       descriptor.BaseArray5() == 0;
}

bool IsSupportedDepthTargetDescriptor(const ShaderTextureResource& descriptor, const Image& image,
                                      bool r128) {
	const auto width        = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height       = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto type         = descriptor.Type();
	const bool multisampled = IsMultisampledTexture(type);
	const auto samples      = multisampled ? 1u << descriptor.LastLevel() : 1u;
	const auto pitch =
	    multisampled ? TileGetDepthPitch(width, image.info.bytes_per_block, descriptor.LastLevel())
		             : TileGetTexturePitch(descriptor.Format(), width, descriptor.TileMode());
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
	    multisampled ? descriptor.BaseLevel() == 0 && descriptor.LastLevel() >= 1 &&
	                       descriptor.LastLevel() <= 3 &&
	                       (r128 || descriptor.MaxMip() == descriptor.LastLevel()) &&
	                       image.info.resources.levels == 1 && image.info.samples == samples
		             : descriptor.BaseLevel() == 0 && descriptor.LastLevel() == 0 &&
		                   (r128 || descriptor.MaxMip() == 0) && image.info.samples == 1;
	return image.info.IsDepth() && width == image.info.extent.width &&
	       height == image.info.extent.height &&
	       (supported_2d || supported_array || supported_cube || supported_msaa_2d ||
	        supported_msaa_array) &&
	       levels_ok && descriptor.MinLod() == 0 &&
	       descriptor.TileMode() == Prospero::TileMode::kDepth && descriptor.BCSwizzle() == 0 &&
	       (!descriptor.MsaaDepth() || multisampled) && pitch >= width && pitch == image.info.pitch;
}

bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor, const Image& image,
                                     bool r128) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	const uint32_t     field3_expected = descriptor.DstSelXYZW() |
	                                     (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	                                     (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	                                     (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t     field4_expected = descriptor.Depth() | (descriptor.BaseArray5() << 16u);
	const uint32_t     field5_expected = (static_cast<uint32_t>(descriptor.PerfMod5()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.MaxMip()) << 4u);
	const bool         common          = (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	                                     (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	                                     descriptor.fields[3] == field3_expected;
	if (r128) {
		return common && descriptor.fields[4] == 0 && descriptor.fields[5] == 0 &&
		       descriptor.fields[6] == 0 && descriptor.fields[7] == 0;
	}
	const bool full = common && descriptor.fields[4] == field4_expected &&
	                  descriptor.fields[5] == field5_expected;
	if (!full || (descriptor.fields[6] == 0 && descriptor.fields[7] != 0)) {
		return false;
	}
	if (descriptor.fields[6] == 0) {
		return true;
	}
	constexpr uint32_t htile_control = 0x00280000u;
	const uint32_t expected_control  = htile_control | (descriptor.MsaaDepth() ? (1u << 10u) : 0u);
	const auto     metadata_addr     = descriptor.MetaAddr() << 8u;
	return (descriptor.fields[6] & 0x00ffffffu) == expected_control && metadata_addr != 0 &&
	       descriptor.TileMode() == Prospero::TileMode::kDepth &&
	       image.info.tile_mode == Prospero::TileMode::kDepth &&
	       image.info.metadata.kind == ImageMetadataKind::Htile &&
	       image.info.metadata.range.Valid() && image.info.metadata.range.address == metadata_addr;
}

static void ValidateDepthTargetBinding(const ShaderRecompiler::IR::ImageResource& resource,
                                       const ShaderTextureResource& descriptor, const Image* image,
                                       vk::Format view_format, uint64_t size) {
	const bool resource_ok = IsSupportedSampledDepthResource(resource);
	const bool descriptor_ok =
	    image != nullptr && IsSupportedDepthTargetDescriptor(descriptor, *image, resource.r128);
	const bool encoding_ok =
	    image != nullptr && IsSupportedDepthTextureEncoding(descriptor, *image, resource.r128);
	const bool format_ok =
	    image != nullptr && IsSupportedSampledDepthFormat(image->info.pixel_format, view_format);
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	const auto descriptor_pitch =
	    TileGetTexturePitch(descriptor.Format(), static_cast<uint32_t>(descriptor.Width5()) + 1u,
		                    descriptor.TileMode());
	EXIT("unsupported sampled depth target: resource=%d descriptor=%d encoding=%d format=%d "
	     "class=%u numeric=%u dimension=%u mip_mode=%u read=%d written=%d atomic=%d compare=%d "
	     "guest_format=%u swizzle=0x%03x image_format=%d view_format=%d image_layers=%u "
	     "descriptor_type=%u base_array=%u depth=%u descriptor_pitch=%u target_pitch=%u "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok,
	     static_cast<uint32_t>(resource.resource_class),
	     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(resource.dimension),
	     static_cast<uint32_t>(resource.mip_mode), resource.read, resource.written, resource.atomic,
	     resource.depth_compare, static_cast<uint32_t>(descriptor.Format()),
	     descriptor.DstSelXYZW(),
	     image == nullptr ? static_cast<int>(vk::Format::eUndefined)
	                      : static_cast<int>(image->info.pixel_format),
	     static_cast<int>(view_format), image == nullptr ? 0u : image->info.resources.layers,
	     static_cast<uint32_t>(descriptor.Type()), descriptor.BaseArray5(), descriptor.Depth(),
	     descriptor_pitch, image == nullptr ? 0u : image->info.pitch, descriptor.Base40(), size,
	     descriptor.fields[0], descriptor.fields[1], descriptor.fields[2], descriptor.fields[3],
	     descriptor.fields[4], descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

static bool IsSupportedStorageTextureDescriptor(const ShaderRecompiler::IR::ImageResource& resource,
                                                const ShaderTextureResource& descriptor) {
	const auto tile              = descriptor.TileMode();
	const bool is_color_1d       = descriptor.Type() == Prospero::ImageType::kColor1D;
	const bool is_color_1d_array = descriptor.Type() == Prospero::ImageType::kColor1DArray;
	const bool valid_1d_slice =
	    (is_color_1d && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0) ||
	    (is_color_1d_array && descriptor.BaseArray5() <= descriptor.Depth());
	const bool is_1d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1D &&
	                   descriptor.Height5() == 0 && valid_1d_slice;
	const bool is_1d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1DArray &&
	    is_color_1d_array && descriptor.Height5() == 0 &&
	    descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_color_2d       = descriptor.Type() == Prospero::ImageType::kColor2D;
	const bool is_color_2d_array = descriptor.Type() == Prospero::ImageType::kColor2DArray;
	const bool valid_2d_slice =
	    (is_color_2d && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0) ||
	    (is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth());
	const bool is_2d =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D && valid_2d_slice;
	const bool is_cube = resource.cube && descriptor.Type() == Prospero::ImageType::kCube &&
	                     descriptor.Width5() == descriptor.Height5() &&
	                     descriptor.BaseArray5() <= descriptor.Depth() &&
	                     (descriptor.Depth() - descriptor.BaseArray5() + 1u) % 6u == 0;
	const bool is_2d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray &&
	    ((!resource.cube && is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth()) ||
		 is_cube);
	const bool is_3d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D &&
	                   descriptor.Type() == Prospero::ImageType::kColor3D &&
	                   descriptor.BaseArray5() == 0;
	TileTextureBlockLayout tile_layout {};
	bool                   supported_tile = false;
	switch (tile) {
		case Prospero::TileMode::kLinear: supported_tile = true; break;
		case Prospero::TileMode::kDepth:
			supported_tile =
			    !resource.read && !Prospero::IsFmaskTextureFormat(descriptor.Format()) &&
			    (is_2d || is_2d_array) &&
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		case Prospero::TileMode::kStandard256B:
			supported_tile =
			    (is_2d || is_2d_array) &&
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		case Prospero::TileMode::kStandard4KB:
		case Prospero::TileMode::kStandard64KB:
			supported_tile =
			    TileGetTextureBlockLayout(descriptor.Format(), tile, is_3d, tile_layout);
			break;
		case Prospero::TileMode::kRenderTarget:
			supported_tile =
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		default: break;
	}
	const auto swizzle = descriptor.DstSelXYZW();
	const bool supported_swizzle =
	    IsValidImageSwizzle(swizzle) &&
	    (swizzle == DstSel(4, 5, 6, 7) || !resource.read || resource.atomic);
	const auto max_mip = resource.r128 ? descriptor.LastLevel() : descriptor.MaxMip();
	const auto view_last_level =
	    resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::DynamicStorage
	        ? descriptor.LastLevel()
	        : std::min(descriptor.LastLevel(), max_mip);
	return (is_1d || is_1d_array || is_2d || is_2d_array || is_3d) && supported_tile &&
	       descriptor.BaseLevel() <= view_last_level && view_last_level <= max_mip &&
	       descriptor.MinLod() == 0 && supported_swizzle && descriptor.BCSwizzle() == 0 &&
	       !descriptor.MsaaDepth();
}

static bool IsSupportedStorageTextureEncoding(const ShaderRecompiler::IR::ImageResource& resource,
                                              const ShaderTextureResource& descriptor) {
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
	const bool common = (descriptor.fields[1] & field1_reserved_mask) == 0 &&
	                    (descriptor.fields[2] & field2_reserved_mask) == 0 &&
	                    descriptor.fields[3] == expected_field3;
	if (resource.r128) {
		return common && descriptor.fields[4] == 0 && descriptor.fields[5] == 0 &&
		       descriptor.fields[6] == 0 && descriptor.fields[7] == 0;
	}
	return common && descriptor.fields[4] == expected_field4 &&
	       (descriptor.fields[5] & ~field5_max_mip_mask) == field5_expected;
}

void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size) {
	const auto format           = descriptor.Format();
	const bool resource_ok      = IsSupportedStorageImageResource(resource);
	const bool descriptor_ok    = IsSupportedStorageTextureDescriptor(resource, descriptor);
	const bool encoding_ok      = IsSupportedStorageTextureEncoding(resource, descriptor);
	const bool uint_resource    = resource.numeric_class == Prospero::TextureNumericClass::Uint;
	const bool raw_sint_storage = format == Prospero::BufferFormat::k32SInt && uint_resource &&
	                              resource.written && !resource.read && !resource.atomic;
	const auto numeric_class    = Prospero::SampledTextureNumericClass(format);
	const bool format_ok =
	    raw_sint_storage ||
	    (numeric_class != Prospero::TextureNumericClass::Unsupported &&
	     numeric_class != Prospero::TextureNumericClass::Sint &&
	     (resource.atomic || uint_resource == (numeric_class == Prospero::TextureNumericClass::Uint)) &&
	     (!resource.atomic || format == Prospero::BufferFormat::k32UInt ||
	     format == Prospero::BufferFormat::k32Float));
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	EXIT("unsupported storage texture: resource=%d descriptor=%d encoding=%d format=%d "
	     "class=%u numeric=%u dimension=%u mip_mode=%u atomic=%d compare=%d "
	     "base_level=%u last_level=%u max_mip=%u min_lod=%u base_array=%u bc=%u msaa=%d "
	     "depth_tile_bpe=%u swizzle_ok=%d "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " extent=%ux%ux%u type=%u format=%u tile=%u swizzle=0x%03x read=%d written=%d "
	     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok,
	     static_cast<uint32_t>(resource.resource_class),
	     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(resource.dimension),
	     static_cast<uint32_t>(resource.mip_mode), resource.atomic, resource.depth_compare,
	     descriptor.BaseLevel(), descriptor.LastLevel(), descriptor.MaxMip(), descriptor.MinLod(),
	     descriptor.BaseArray5(), descriptor.BCSwizzle(), descriptor.MsaaDepth(),
	     Prospero::RenderTargetBytesPerElement(format),
	     IsValidImageSwizzle(descriptor.DstSelXYZW()), descriptor.Base40(), size,
	     static_cast<uint32_t>(descriptor.Width5()) + 1u,
	     static_cast<uint32_t>(descriptor.Height5()) + 1u,
	     static_cast<uint32_t>(descriptor.Depth()) + 1u, static_cast<uint32_t>(descriptor.Type()),
	     static_cast<uint32_t>(format), static_cast<uint32_t>(descriptor.TileMode()),
	     descriptor.DstSelXYZW(), resource.read, resource.written, descriptor.fields[0],
	     descriptor.fields[1], descriptor.fields[2], descriptor.fields[3], descriptor.fields[4],
	     descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

struct NullImageSpec {
	vk::Format             format;
	Prospero::BufferFormat guest_format;
};

static NullImageSpec NullTextureSpec(const ShaderRecompiler::IR::ImageResource& resource) {
	switch (resource.numeric_class) {
		case Prospero::TextureNumericClass::Float:
			return {vk::Format::eR32Sfloat, Prospero::BufferFormat::k32Float};
		case Prospero::TextureNumericClass::Uint:
			return {vk::Format::eR32Uint, Prospero::BufferFormat::k32UInt};
		case Prospero::TextureNumericClass::Sint:
			return {vk::Format::eR32Sint, Prospero::BufferFormat::k32SInt};
		case Prospero::TextureNumericClass::Unsupported: break;
	}
	EXIT("null image has unsupported numeric class\n");
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
	if (info.IsVolume() && info.tile_mode != Prospero::TileMode::kLinear) {
		TileSurfaceLayout            surface {};
		const TileSurfaceDescription description {
		    info.guest_format,  info.tile_mode,    TileSurfaceDimension::Dim3D, info.extent.width,
		    info.extent.height, info.extent.depth, info.resources.levels,       1};
		if (!TileGetTiledTextureLayout(description, surface)) {
			EXIT("unsupported normalized volume texture layout\n");
		}
		for (uint32_t level = 0; level < info.resources.levels; level++) {
			const auto& mip        = surface.mips[level];
			info.mip_layout[level] = {
			    mip.offset,
			    mip.size,
			    mip.padded_width,
			    mip.padded_height,
			};
		}
		return;
	}

	TileSizeOffset levels[16] {};
	TilePaddedSize padded[16] {};
	TileGetTextureSize(info.guest_format, info.extent.width, info.extent.height,
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
                                     bool shader_conversion, bool storage, uint32_t view_levels,
                                     uint32_t image_layers) {
	ImageViewInfo view {};
	view.format      = format;
	view.aspect      = DepthAspectTransferFormat(format) != vk::Format::eUndefined
	                       ? vk::ImageAspectFlagBits::eDepth
	                       : vk::ImageAspectFlagBits::eColor;
	view.base_level  = descriptor.BaseLevel();
	view.level_count = view_levels;
	view.usage   = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
	view.mapping = storage || shader_conversion
	                   ? vk::ComponentMapping {}
	                   : TextureGetComponentMapping(descriptor.DstSelXYZW());
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

// Pure descriptor interpretation shared by alias admission and cache acquisition.
// It reads descriptor values only: no guest-memory access, image/view creation,
// ownership changes or GPU commands are allowed in this function.
struct NormalizedTextureDescriptor {
	ShaderTextureResource   descriptor;
	TextureCache::ImageDesc desc;
	bool                   shader_conversion = false;
};

static NormalizedTextureDescriptor
NormalizeTextureDescriptor(const ShaderRecompiler::IR::ImageResource& resource,
                           const ShaderRecompiler::IR::DescriptorValue& value) {
	const auto descriptor = DecodeNativeDescriptor<ShaderTextureResource>(value);
	const bool storage = resource.written;
	if (storage) {
		ValidateStorageImageResource(resource);
	}
	if (descriptor.IsNull()) {
		return {descriptor,
		        NullTextureDesc(resource, storage ? TextureCache::BindingType::Storage
		                                          : TextureCache::BindingType::Texture),
		        false};
	}

	const auto address      = descriptor.Base40();
	const auto width        = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height       = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto base_level   = descriptor.BaseLevel();
	const auto last_level   = descriptor.LastLevel();
	const auto type         = TextureType(descriptor);
	const bool multisampled = IsMultisampledTexture(type);
	const auto max_mip      = resource.r128 ? last_level : descriptor.MaxMip();
	const auto levels       = multisampled ? 1u : static_cast<uint32_t>(max_mip) + 1u;
	const bool dynamic_storage =
	    storage && resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::DynamicStorage;
	const auto view_last_level =
	    !multisampled && !dynamic_storage ? std::min(last_level, max_mip) : last_level;
	const auto tile       = descriptor.TileMode();
	const bool depth_tile = tile == Prospero::TileMode::kDepth;
	const bool msaa_tile  = depth_tile || tile == Prospero::TileMode::kRenderTarget;
	const bool msaa_array = type == Prospero::ImageType::kColor2DMsaaArray;
	if ((!multisampled && (base_level > view_last_level || view_last_level >= levels)) ||
	    (multisampled &&
	     (base_level != 0 || last_level == 0 || last_level > 3 || max_mip != last_level ||
	      !msaa_tile || (descriptor.MsaaDepth() && !depth_tile) ||
	      (!msaa_array && (descriptor.Depth() != 0 || descriptor.BaseArray5() != 0))))) {
		EXIT("unsupported texture mip view: base=%u last=%u levels=%u max=%u type=%u tile=%u "
		     "class=%u numeric=%u dimension=%u mip_mode=%u read=%d written=%d "
		     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
		     base_level, last_level, levels, descriptor.MaxMip(),
		     static_cast<uint32_t>(descriptor.Type()), static_cast<uint32_t>(tile),
		     static_cast<uint32_t>(resource.resource_class),
		     static_cast<uint32_t>(resource.numeric_class),
		     static_cast<uint32_t>(resource.dimension), static_cast<uint32_t>(resource.mip_mode),
		     resource.read, resource.written, descriptor.fields[0], descriptor.fields[1],
		     descriptor.fields[2], descriptor.fields[3], descriptor.fields[4], descriptor.fields[5],
		     descriptor.fields[6], descriptor.fields[7]);
	}
	const auto samples = multisampled ? 1u << last_level : 1u;
	const auto view_levels =
	    multisampled ? 1u : static_cast<uint32_t>(view_last_level - base_level) + 1u;
	const auto depth          = static_cast<uint32_t>(descriptor.Depth()) + 1u;
	const auto format         = descriptor.Format();
	const auto surface_format = TextureGetSurfaceFormatInfo(format);
	const bool shader_conversion =
	    surface_format.conversion_format != Prospero::BufferFormat::kInvalid;
	const bool sampled_numeric_class =
	    storage || resource.numeric_class == Prospero::SampledTextureNumericClass(format);
	if (!storage && resource.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled &&
	    !sampled_numeric_class) {
		EXIT("sampled image numeric class mismatch: numeric=%u format=%u addr=0x%016" PRIx64 "\n",
		     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(format), address);
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
		pitch            = depth_tile ? TileGetDepthPitch(width, bytes, last_level)
		                              : TileGetRenderTargetPitch(width, bytes, last_level);
		if (pitch == 0 || !TileGetRenderTargetSize(width, height, pitch, bytes, size, last_level) ||
		    size.size > UINT32_MAX / image_layers) {
			EXIT("unsupported multisample texture layout\n");
		}
		size.size *= image_layers;
	} else {
		pitch = TileGetTexturePitch(format, width, tile);
		TileGetTextureTotalSize(format, width, height, volume ? depth : image_layers, levels, tile,
		                        volume, size);
	}
	EXIT_NOT_IMPLEMENTED(size.size == 0 || size.align == 0 ||
	                     (address & (static_cast<uint64_t>(size.align) - 1u)) != 0);
	if (storage) {
		ValidateStorageTexture(resource, descriptor, size.size);
	}

	const auto pixel_format        = surface_format.vk_format;
	const auto storage_view_format = storage && (format == Prospero::BufferFormat::k32SInt || resource.atomic)
	                                     ? vk::Format::eR32Uint
	                                     : SrgbStorageViewFormat(pixel_format);
	const auto view_format         = storage && storage_view_format != vk::Format::eUndefined
	                                     ? storage_view_format
	                                     : pixel_format;
	const auto block_bytes         = Prospero::BlockCompressedBytesPerBlock(format);
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
	desc.view_info = TextureViewInfo(resource, descriptor, view_format, shader_conversion, storage,
	                                 view_levels, desc.info.resources.layers);
	desc.type = storage ? TextureCache::BindingType::Storage : TextureCache::BindingType::Texture;
	return {descriptor, std::move(desc), shader_conversion};
}


// Pure HTile interpretation shared by preflight and actual sampled acquisition.
// This recognizes Float reads regardless of whether the shader uses Dref.
static bool NormalizeSampledHtileRead(
    const ShaderRecompiler::IR::ImageResource& resource, NormalizedTextureDescriptor& normalized) {
	const auto& descriptor = normalized.descriptor;
	if (descriptor.IsNull() || !descriptor.MetaCompress() ||
	    descriptor.TileMode() != Prospero::TileMode::kDepth ||
	    descriptor.Format() != Prospero::BufferFormat::k32Float ||
	    resource.numeric_class != Prospero::TextureNumericClass::Float ||
	    !IsSupportedSampledDepthResource(resource)) {
		return false;
	}
	const uint32_t field3 = descriptor.DstSelXYZW() |
	    (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	    (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	    (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	    (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t field4 = descriptor.Depth() | (descriptor.BaseArray5() << 16u);
	const uint32_t field5 = (static_cast<uint32_t>(descriptor.PerfMod5()) << 20u) |
	    (static_cast<uint32_t>(descriptor.MaxMip()) << 4u);
	const uint32_t control = 0x00280000u | (descriptor.MsaaDepth() ? (1u << 10u) : 0u);
	if (resource.r128 || (descriptor.fields[1] & 0x200fff00u) != 0 ||
	    (descriptor.fields[2] & 0xf0003000u) != 0 || descriptor.fields[3] != field3 ||
	    descriptor.fields[4] != field4 || descriptor.fields[5] != field5 ||
	    (descriptor.fields[6] & 0x00ffffffu) != control) {
		EXIT("unsupported sampled HTile descriptor encoding\n");
	}
	auto& info = normalized.desc.info;
	const bool multisampled = IsMultisampledTexture(descriptor.Type());
	if (descriptor.BaseLevel() != 0 || info.resources.levels != 1 ||
	    (!multisampled && (descriptor.LastLevel() != 0 || descriptor.MaxMip() != 0 ||
	                      descriptor.MsaaDepth())) ||
	    (multisampled && descriptor.MaxMip() != descriptor.LastLevel())) {
		EXIT("unsupported sampled HTile mip layout\n");
	}
	TileSizeAlign stencil_size {}, htile_size {}, depth_size {};
	if (!TileGetDepthSize(info.extent.width, info.extent.height, 0,
	                      Prospero::DepthFormat::kZ32F, Prospero::StencilFormat::kInvalid,
	                      true, stencil_size, htile_size, depth_size,
	                      multisampled ? descriptor.LastLevel() : 0) ||
	    info.resources.layers == 0 ||
	    static_cast<uint64_t>(depth_size.size) * info.resources.layers != info.data.size) {
		EXIT("unsupported sampled HTile allocation footprint\n");
	}
	const GuestRange metadata {descriptor.MetaAddr() << 8u,
	                           static_cast<uint64_t>(htile_size.size) * info.resources.layers};
	if (!metadata.Valid() || (metadata.address & (htile_size.align - 1u)) != 0 ||
	    (info.data.address < metadata.End() && metadata.address < info.data.End())) {
		EXIT("invalid sampled HTile metadata range\n");
	}
	info.pixel_format = vk::Format::eD32Sfloat;
	info.metadata.kind = ImageMetadataKind::Htile;
	info.metadata.range = metadata;
	info.metadata.control = control;
	return true;
}

// Indexed SRT values are frozen before dispatch. Check every source byte
// against the complete declared write footprint before any cache acquisition,
// ownership change, upload or binding can begin for any active stage.
static void ValidateImmutableSrtWriteAliases(
    std::span<const ShaderStageRuntime* const> stages) {
	std::vector<GuestRange> reads;
	for (const auto* stage: stages) {
		EXIT_IF(stage == nullptr || !*stage);
		for (const auto& source: stage->resources.immutable_srt_ranges) {
			const GuestRange range {source.address, source.size};
			if (!range.Valid()) {
				EXIT("immutable SRT snapshot has an invalid range: address=0x%016" PRIx64
				     " size=0x%016" PRIx64 "\n", range.address, range.size);
			}
			reads.push_back(range);
		}
	}
	if (reads.empty()) {
		return;
	}
	// Read-only DMA does not mutate the snapshotted source. Dynamic DMA writes
	// have no bounded footprint to compare, so keep rejecting them here.
	for (const auto* stage: stages) {
		const auto& info = stage->program->info;
		if (stage->program->stage != ShaderType::Compute || info.writes_dma) {
			EXIT("immutable SRT snapshot requires compute without DMA writes: stage=%u dma_write=%d\n",
			     static_cast<uint32_t>(stage->program->stage), info.writes_dma);
		}
		if (stage->resources.buffers.size() != info.buffers.size() ||
		    stage->resources.images.size() != info.images.size()) {
			EXIT("immutable SRT snapshot resource counts disagree\n");
		}
	}
	const auto validate_write = [&](GuestRange written, ShaderType stage,
	                                const char* kind, uint32_t index) {
		if (!written.Valid()) {
			EXIT("immutable SRT writable resource has an invalid range: stage=%u kind=%s index=%u "
			     "address=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     static_cast<uint32_t>(stage), kind, index, written.address, written.size);
		}
		for (const auto source: reads) {
			if (written.address < source.End() && source.address < written.End()) {
				EXIT("immutable SRT snapshot overlaps writable resource: stage=%u kind=%s index=%u "
				     "source=0x%016" PRIx64 "+0x%016" PRIx64 " writer=0x%016" PRIx64 "+0x%016" PRIx64 "\n",
				     static_cast<uint32_t>(stage), kind, index, source.address, source.size,
				     written.address, written.size);
			}
		}
	};
	for (const auto* stage: stages) {
		const auto& info = stage->program->info;
		for (uint32_t index = 0; index < info.images.size(); ++index) {
			auto resource = info.images[index];
			if (!resource.written && !resource.atomic) {
				continue;
			}
			resource.written = true; // An atomic access has the storage write footprint too.
			const auto normalized = NormalizeTextureDescriptor(resource, stage->resources.images[index]);
			if (!normalized.descriptor.IsNull()) {
				// This is the same full padded allocation (all mips/layers/depth)
				// ResolveTexture will expose, not merely its selected view texels.
				validate_write(normalized.desc.info.data, stage->program->stage, "image", index);
			}
		}
		for (uint32_t index = 0; index < info.buffers.size(); ++index) {
			const auto& resource = info.buffers[index];
			if (!resource.written && !resource.atomic) {
				continue;
			}
			const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(stage->resources.buffers[index]);
			const uint64_t address = descriptor.Base48();
			const uint64_t stride = descriptor.Stride();
			const uint64_t records = descriptor.NumRecords();
			if (stride != 0 && records > UINT64_MAX / stride) {
				EXIT("immutable SRT writable buffer footprint overflow\n");
			}
			const uint64_t size = stride == 0 ? records : stride * records;
			// Match NativeStorageBuffer's explicit null/empty descriptor behavior.
			if (address != 0 && size != 0) {
				validate_write({address, size}, stage->program->stage, "buffer", index);
			}
		}
	}
}

static void ValidateSampledHtileWriteAliases(
    std::span<const ShaderStageRuntime* const> stages) {
	struct ReadDependency { GuestRange data; GuestRange metadata; };
	std::vector<ReadDependency> reads;
	for (const auto* stage: stages) {
		EXIT_IF(stage == nullptr || !*stage);
		const auto& images = stage->program->info.images;
		EXIT_IF(stage->resources.images.size() != images.size());
		for (uint32_t index = 0; index < images.size(); ++index) {
			const auto& image = images[index];
			if (!image.read || image.written || image.atomic ||
			    image.numeric_class != Prospero::TextureNumericClass::Float) {
				continue;
			}
			auto normalized = NormalizeTextureDescriptor(image, stage->resources.images[index]);
			if (NormalizeSampledHtileRead(image, normalized)) {
				reads.push_back({normalized.desc.info.data, normalized.desc.info.metadata.range});
			}
		}
	}
	if (reads.empty()) {
		return;
	}
	const auto validate_write = [&](GuestRange written) {
		if (!written.Valid()) {
			EXIT("invalid writable resource range during sampled HTile admission\n");
		}
		for (const auto& read: reads) {
			for (const auto dependency: {read.data, read.metadata}) {
				if (written.address < dependency.End() && dependency.address < written.End()) {
					EXIT("simultaneous sampled HTile and writable resource overlap\n");
				}
			}
		}
	};
	for (const auto* stage: stages) {
		const auto& info = stage->program->info;
		if (info.writes_dma) {
			EXIT("sampled HTile admission cannot prove DMA write dependencies\n");
		}
		for (uint32_t index = 0; index < info.images.size(); ++index) {
			const auto& image = info.images[index];
			if (!image.written && !image.atomic) {
				continue;
			}
			const auto normalized = NormalizeTextureDescriptor(image, stage->resources.images[index]);
			if (!normalized.descriptor.IsNull()) {
				validate_write(normalized.desc.info.data);
			}
		}
		EXIT_IF(stage->resources.buffers.size() != info.buffers.size());
		for (uint32_t index = 0; index < info.buffers.size(); ++index) {
			if (!info.buffers[index].written && !info.buffers[index].atomic) {
				continue;
			}
			const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(
			    stage->resources.buffers[index]);
			const uint64_t address = descriptor.Base48();
			const uint64_t records = descriptor.NumRecords();
			const uint64_t stride = descriptor.Stride();
			if (stride != 0 && records > UINT64_MAX / stride) {
				EXIT("sampled HTile writable buffer footprint overflow\n");
			}
			const uint64_t size = stride == 0 ? records : records * stride;
			if (address != 0 && size != 0) {
				validate_write({address, size});
			}
		}
	}
}

static void ValidateComparisonStorageAliases(
    std::span<const ShaderStageRuntime* const> stages) {
	bool has_comparison = false;
	bool has_write = false;
	for (const auto* stage: stages) {
		EXIT_IF(stage == nullptr || !*stage);
		const auto& images = stage->program->info.images;
		EXIT_IF(stage->resources.images.size() != images.size());
		for (const auto& image: images) {
			has_comparison |= image.depth_compare;
			has_write |= image.written || image.atomic;
		}
	}
	if (!has_comparison || !has_write) {
		return;
	}

	struct Access {
		GuestRange range;
		ShaderType stage;
		uint32_t index;
		bool comparison;
		bool write;
	};
	std::vector<Access> accesses;
	for (const auto* stage: stages) {
		const auto& images = stage->program->info.images;
		for (uint32_t index = 0; index < images.size(); ++index) {
			const auto& image = images[index];
			const bool written = image.written || image.atomic;
			if (!image.depth_compare && !written) {
				continue;
			}
			// This is the exact same full padded allocation (all levels/layers)
			// that ResolveTexture will hand to the cache. Do not compare only
			// base-address equality or the shader's selected view rectangle.
			const auto normalized = NormalizeTextureDescriptor(
			    image, stage->resources.images[index]);
			if (normalized.descriptor.IsNull()) {
				continue;
			}
			const auto range = normalized.desc.info.data;
			if (!range.Valid()) {
				EXIT("invalid shader image allocation during comparison/storage admission\n");
			}
			accesses.push_back({range, stage->program->stage, index,
			                    image.depth_compare, written});
		}
	}
	for (const auto& comparison: accesses) {
		if (!comparison.comparison) {
			continue;
		}
		for (const auto& storage: accesses) {
			if (storage.write && comparison.range.address < storage.range.End() &&
			    storage.range.address < comparison.range.End()) {
				EXIT("simultaneous depth comparison and storage image overlap: "
				     "compare_stage=%u compare_image=%u storage_stage=%u storage_image=%u\n",
				     static_cast<uint32_t>(comparison.stage), comparison.index,
				     static_cast<uint32_t>(storage.stage), storage.index);
			}
		}
	}
}

TextureBinding RenderExecutor::ResolveTexture(const ShaderRecompiler::IR::ImageResource& resource,
                                              const ShaderRecompiler::IR::DescriptorValue& value) {
	auto normalized = NormalizeTextureDescriptor(resource, value);
	const auto& descriptor = normalized.descriptor;
	auto& desc = normalized.desc;
	const bool shader_conversion = normalized.shader_conversion;
	const bool storage = resource.written;
	auto& texture_cache = m_context.GetTextureCache();
	if (descriptor.IsNull()) {
		const auto id = texture_cache.FindImage(desc);
		return {id, nullptr, std::move(desc)};
	}
	const auto pixel_format = desc.info.pixel_format;
	const bool sampled_htile = NormalizeSampledHtileRead(resource, normalized);
	const auto view_format = desc.view_info.format;
	const auto type = TextureType(descriptor);
	const bool depth_tile = descriptor.TileMode() == Prospero::TileMode::kDepth;
	const auto format = descriptor.Format();
	const auto samples = desc.info.samples;
	const auto levels = desc.info.resources.levels;
	// A depth-tiled R32 intermediate can be written as color and subsequently
	// compared as depth. Ask the cache for a separate native depth image so its
	// existing overlap/copy path preserves current GPU contents. Vulkan does not
	// permit a D32 view of the same R32 image. Unrestricted depth copies preserve
	// arbitrary guest float values rather than silently clamping them to [0,1].
	const bool promote_comparison = resource.depth_compare &&
	    IsSupportedSampledDepthResource(resource) &&
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D &&
	    type == Prospero::ImageType::kColor2D && depth_tile &&
	    format == Prospero::BufferFormat::k32Float && samples == 1 && levels == 1 &&
	    descriptor.BaseArray5() == 0 && descriptor.Depth() == 0 &&
	    !descriptor.MsaaDepth() && descriptor.fields[6] == 0 && descriptor.fields[7] == 0;
	if (promote_comparison && m_context.GetGraphics().depth_range_unrestricted_enabled) {
		desc.info.pixel_format = vk::Format::eD32Sfloat;
	}

	auto       id                  = sampled_htile ? texture_cache.FindSampledHtileImage(desc)
	                                                : texture_cache.FindImage(desc, shader_conversion);
	auto*      image               = &texture_cache.GetImage(id);
	const bool stencil_association = static_cast<bool>(image->depth_id);
	if (stencil_association) {
		id    = image->depth_id;
		image = &texture_cache.GetImage(id);
	} else if (image->info.IsDepth()) {
		if (storage) {
			EXIT("depth target cannot be bound as a storage image\n");
		}
		ValidateDepthTargetBinding(resource, descriptor, image, pixel_format, desc.info.data.size);
		(void)SelectSampledDepthView(image->info.pixel_format, pixel_format,
		                             descriptor.DstSelXYZW());
	} else if (storage) {
		ValidateStorageColorView(image->info.pixel_format, view_format, descriptor.DstSelXYZW());
	} else {
		if (resource.depth_compare) {
			EXIT("color depth comparison requires a supported depth image representation "
			     "and enabled VK_EXT_depth_range_unrestricted: extension=%d eligible=%d "
			     "source=%u class=%u numeric=%u dimension=%u mip=%u read=%d write=%d atomic=%d "
			     "addr=0x%016" PRIx64 " format=%u type=%u tile=%u "
			     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
			     m_context.GetGraphics().depth_range_unrestricted_enabled, promote_comparison,
			     resource.source, static_cast<uint32_t>(resource.resource_class),
			     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(resource.dimension),
			     static_cast<uint32_t>(resource.mip_mode), resource.read, resource.written, resource.atomic,
			     descriptor.Base40(), static_cast<uint32_t>(format), static_cast<uint32_t>(type),
			     static_cast<uint32_t>(descriptor.TileMode()), descriptor.fields[0], descriptor.fields[1],
			     descriptor.fields[2], descriptor.fields[3], descriptor.fields[4], descriptor.fields[5],
			     descriptor.fields[6], descriptor.fields[7]);
		}
		(void)SelectSampledColorView(image->info.pixel_format, pixel_format,
		                             descriptor.DstSelXYZW());
	}
	return {id, nullptr, std::move(desc)};
}

static vk::Sampler NativeSampler(RenderContext&                                  context,
                                 const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                 uint32_t                                        index,
                                 const ShaderRecompiler::IR::DescriptorValue&    value) {
	auto descriptor = DecodeNativeDescriptor<ShaderSamplerResource>(value);
	if (!program.info.samplers[index].depth_compare) {
		descriptor.fields[0] &= ~(0x7u << 12u);
	}
	if (program.info.samplers[index].force_point_filtering) {
		descriptor.SetPointFiltering();
	}
	return context.GetSamplerCache().GetSampler(descriptor);
}

static BufferView NativeUpload(RenderContext& context, std::span<const uint32_t> data) {
	EXIT_IF(data.empty());
	auto& command_buffer = context.GetCommandScheduler().Current();
	EXIT_IF(command_buffer.IsInvalid());
	auto&      buffer = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
	const auto offset = buffer.Copy(data.data(), data.size_bytes(), 256);
	return {.buffer = buffer.Handle(), .offset = offset, .range = data.size_bytes()};
}

void RenderExecutor::TrackImageBinding(ImageId id) {
	EXIT_IF(m_context.GetTextureCache().m_slot_images.try_get(id) == nullptr);
	if (std::ranges::find(m_bound_images, id) == m_bound_images.end()) {
		m_bound_images.push_back(id);
	}
}

void RenderExecutor::BindImage(ImageId id, bool storage) {
	auto& image = m_context.GetTextureCache().GetImage(id);
	if (image.info.data.Empty()) {
		return;
	}
	if (image.binding.is_bound) {
		image.binding.force_general |= image.binding.shader_write != storage;
	}
	image.binding.is_bound = true;
	image.binding.shader_write |= storage;
	TrackImageBinding(id);
}

void RenderExecutor::BindRenderTarget(ImageId id) {
	auto& image             = m_context.GetTextureCache().GetImage(id);
	image.binding.is_target = true;
	TrackImageBinding(id);
}

void RenderExecutor::ResetBindings() {
	for (const auto id: m_bound_images) {
		if (auto* image = m_context.GetTextureCache().m_slot_images.try_get(id); image != nullptr) {
			image->binding = {};
		}
	}
	m_bound_images.clear();
}

PreparedBindings RenderExecutor::PrepareBindings(const ShaderStageRuntime& runtime) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(!runtime);
	const ShaderStageRuntime* stage = &runtime;
	ValidateImmutableSrtWriteAliases(std::span<const ShaderStageRuntime* const>{&stage, 1u});
	ValidateComparisonStorageAliases(std::span<const ShaderStageRuntime* const>{&stage, 1u});
	ValidateSampledHtileWriteAliases(std::span<const ShaderStageRuntime* const>{&stage, 1u});
	const auto& program  = *runtime.program;
	const auto& snapshot = runtime.resources;
	PreparedBindings prepared;
	prepared.program  = runtime.program;
	prepared.snapshot = &runtime.resources;
	auto& descriptors = prepared.resources;
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
	prepared.shader_data.reserve(program.bindings.ShaderDataDwords());
	for (const auto reg: program.bindings.user_data_registers) {
		prepared.shader_data.push_back(snapshot.user_data[reg - program.user_data_base]);
	}
	prepared.shader_data.resize(program.bindings.ShaderDataDwords());
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::Gds) != nullptr) {
		descriptors.gds.buffer = m_context.GetBufferCache().GetGdsBuffer()->Handle();
	}
	return prepared;
}

void RenderExecutor::FindBuffers(PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.program == nullptr || prepared.snapshot == nullptr);
	const auto& program  = *prepared.program;
	const auto& snapshot = *prepared.snapshot;
	auto&       cache    = m_context.GetBufferCache();

	prepared.buffer_sources.clear();
	prepared.buffer_sources.reserve(program.info.buffers.size());
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		auto       descriptor = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[i]);
		const auto address    = descriptor.Base48();
		const auto stride     = descriptor.Stride();
		const auto records    = descriptor.NumRecords();
		EXIT_IF(stride != 0 && records > UINT64_MAX / stride);
		const auto requested_size = stride != 0 ? static_cast<uint64_t>(stride) * records : records;
		if (address == 0 || requested_size == 0) {
			prepared.buffer_sources.emplace_back(descriptor, BufferId {});
			continue;
		}
		const auto size = Libs::LibKernel::Memory::ClampRangeSize(address, requested_size);
		prepared.buffer_sources.emplace_back(descriptor, cache.FindBuffer(address, size));
	}
}

void RenderExecutor::RebindBuffers(PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.program == nullptr || prepared.snapshot == nullptr);
	const auto& program   = *prepared.program;
	const auto& snapshot  = *prepared.snapshot;
	auto&       resources = prepared.resources;
	const auto& layout    = program.bindings;
	EXIT_IF(prepared.buffer_sources.size() != program.info.buffers.size());

	resources.buffers.clear();
	resources.buffers.reserve(program.info.buffers.size());
	EXIT_IF(prepared.shader_data.size() != layout.ShaderDataDwords());
	std::fill(prepared.shader_data.begin() + layout.memory_offset_dword, prepared.shader_data.end(),
	          0);
	auto pack_memory_offset = [&](uint32_t index, uint32_t offset) {
		const auto dword = layout.memory_offset_dword + index / 4u;
		const auto shift = (index % 4u) * 8u;
		prepared.shader_data[dword] |= offset << shift;
	};
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& [descriptor, buffer_id] = prepared.buffer_sources[i];
		uint32_t buffer_offset              = 0;
		resources.buffers.push_back(NativeStorageBuffer(m_context, descriptor,
		                                                program.info.buffers[i], program.stage, i,
		                                                buffer_offset, buffer_id));
		pack_memory_offset(i, buffer_offset);
	}
	if (ShaderRecompiler::IR::FindBinding(
	        layout, ShaderRecompiler::IR::DescriptorBindingKind::FlattenedSrt) != nullptr) {
		resources.flattened_srt = NativeUpload(m_context, snapshot.flattened_srt);
	}
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::ShaderData) != nullptr) {
		resources.shader_data = NativeUpload(m_context, prepared.shader_data);
	}
}

void RenderExecutor::RebindImages(PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.program == nullptr || prepared.snapshot == nullptr);
	const auto& program  = *prepared.program;
	const auto& snapshot = *prepared.snapshot;
	auto&       images   = prepared.resources.images;
	EXIT_IF(images.size() != program.info.images.size());
	auto& texture_cache = m_context.GetTextureCache();
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto old_image = texture_cache.m_slot_images.try_get(images[i].image_id);
		if (old_image == nullptr || (!old_image->registered && !old_image->info.data.Empty()) ||
		    old_image->binding.needs_rebind) {
			if (old_image != nullptr) {
				old_image->binding = {};
			}
			images[i] = ResolveTexture(program.info.images[i], snapshot.images[i]);
			BindImage(images[i].image_id,
			          images[i].desc.type == TextureCache::BindingType::Storage);
		}
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		auto& binding = images[i];
		binding.mip_views.clear();
		const auto& resource = program.info.images[i];
		if (resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::DynamicStorage) {
			EXIT_IF(resource.mip_count == 0u ||
			        resource.mip_count != binding.desc.view_info.level_count);
			binding.mip_views.reserve(resource.mip_count);
			for (uint32_t mip = 0; mip < resource.mip_count; mip++) {
				auto desc = binding.desc;
				desc.view_info.base_level += mip;
				desc.view_info.level_count = 1;
				binding.mip_views.push_back(texture_cache.FindTexture(binding.image_id, desc));
			}
			binding.image_view = binding.mip_views.front();
		} else {
			auto desc = binding.desc;
			if (desc.type == TextureCache::BindingType::Storage) {
				desc.view_info.level_count = 1;
			}
			binding.image_view = texture_cache.FindTexture(binding.image_id, desc);
		}
		auto&      image   = texture_cache.GetImage(binding.image_id);
		const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
		image.usage.storage |= storage;
		image.usage.texture |= !storage;
	}
}

RenderExecutor::GraphicsBindings
RenderExecutor::PrepareGraphicsBindings(const ShaderStageRuntime& vertex,
                                        const ShaderStageRuntime& pixel, bool pixel_active) {
	// Validate the complete draw before preparing either stage: preparing the
	// vertex stage first could already replace an image needed by the pixel stage.
	const std::array<const ShaderStageRuntime*, 2> stages {&vertex, &pixel};
	ValidateImmutableSrtWriteAliases(std::span<const ShaderStageRuntime* const>{
	    stages.data(), pixel_active ? 2u : 1u});
	ValidateComparisonStorageAliases(std::span<const ShaderStageRuntime* const>{
	    stages.data(), pixel_active ? 2u : 1u});
	ValidateSampledHtileWriteAliases(std::span<const ShaderStageRuntime* const>{
	    stages.data(), pixel_active ? 2u : 1u});
	GraphicsBindings bindings {
	    .vertex = PrepareBindings(vertex),
	};
	if (pixel_active) {
		bindings.pixel.emplace(PrepareBindings(pixel));
	}
	FindBuffers(bindings.vertex);
	if (bindings.pixel) {
		FindBuffers(*bindings.pixel);
	}
	if (bindings.vertex.program->info.uses_dma ||
	    (bindings.pixel && bindings.pixel->program->info.uses_dma)) {
		m_context.GetGpuResources().PrepareBda();
	}
	RebindBuffers(bindings.vertex);
	if (bindings.pixel) {
		RebindBuffers(*bindings.pixel);
	}
	RebindImages(bindings.vertex);
	if (bindings.pixel) {
		RebindImages(*bindings.pixel);
	}
	return bindings;
}

void RenderExecutor::CommitBindings(CommandBuffer&                     buffer,
                                    vk::PipelineBindPoint              pipeline_bind_point,
                                    const PipelineCache::Pipeline&     pipeline,
                                    std::span<PreparedBindings* const> prepared_bindings) {
	KYTY_PROFILER_FUNCTION();
	auto                           vk_buffer        = buffer.Handle();
	size_t                         descriptor_count = 0;
	size_t                         write_count      = 0;
	ShaderRecompiler::IR::PushData push_data;
	bool                           has_push_data = false;
	constexpr auto                 GraphicsStages =
	    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
	for (const auto* prepared: prepared_bindings) {
		EXIT_IF(prepared == nullptr || prepared->program == nullptr ||
		        prepared->snapshot == nullptr);
		write_count += prepared->program->bindings.descriptors.size();
		for (const auto& binding: prepared->program->bindings.descriptors) {
			descriptor_count += NativeDescriptorCount(binding);
		}
		const auto shader_stage = NativeShaderStage(prepared->program->stage);
		EXIT_IF((pipeline_bind_point == vk::PipelineBindPoint::eGraphics &&
		         (shader_stage & GraphicsStages) == vk::ShaderStageFlags {}) ||
		        (pipeline_bind_point == vk::PipelineBindPoint::eCompute &&
		         shader_stage != vk::ShaderStageFlagBits::eCompute));
	}
	m_descriptor_buffers.clear();
	m_descriptor_images.clear();
	m_descriptor_writes.clear();
	m_descriptor_buffers.reserve(descriptor_count);
	m_descriptor_images.reserve(descriptor_count);
	m_descriptor_writes.reserve(write_count);

	for (auto* prepared: prepared_bindings) {
		const auto& program       = *prepared->program;
		auto&       descriptors   = prepared->resources;
		const auto  shader_stage  = NativeShaderStage(program.stage);
		const auto  shader_stages = ShaderPipelineStages(shader_stage);
		if (descriptors.gds.buffer != nullptr) {
			buffer.EndRendering();
			const auto barrier = MakeGdsDependency(descriptors.gds.buffer);
			vk_buffer.pipelineBarrier(
			    vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eTransfer |
			        vk::PipelineStageFlagBits::eAllGraphics |
			        vk::PipelineStageFlagBits::eComputeShader,
			    shader_stages, vk::DependencyFlags {}, 0, nullptr, 1, &barrier, 0, nullptr);
		}

		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			auto& image   = m_context.GetTextureCache().GetImage(descriptors.images[i].image_id);
			auto& binding = descriptors.images[i];
			const auto&                 view = binding.desc.view_info;
			const ImageSubresourceRange range {view.base_level, view.level_count, view.base_layer,
			                                   view.layer_count};
			const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
			if (image.info.data.Empty()) {
				image.Transit(vk::ImageLayout::eGeneral,
				              storage ? vk::AccessFlagBits2::eShaderRead |
				                            vk::AccessFlagBits2::eShaderWrite
				                      : vk::AccessFlagBits2::eShaderRead,
				              range, vk_buffer);
			} else if ((image.binding.force_general || image.binding.is_target) &&
			           !image.info.IsDepth()) {
				const vk::AccessFlags2 storage_access = image.binding.shader_write
				                                            ? vk::AccessFlagBits2::eShaderWrite
				                                            : vk::AccessFlags2 {};
				image.Transit(vk::ImageLayout::eGeneral,
				              vk::AccessFlagBits2::eShaderRead | storage_access |
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

		m_image_occurrences.assign(descriptors.images.size(), 0);
		for (const auto& binding: program.bindings.descriptors) {
			vk::WriteDescriptorSet write {};
			write.sType          = vk::StructureType::eWriteDescriptorSet;
			write.dstBinding     = ShaderRecompiler::IR::NativeBinding(program.stage, binding.kind);
			write.descriptorType = NativeDescriptorType(binding.kind);
			write.descriptorCount   = NativeDescriptorCount(binding);
			const auto buffer_start = m_descriptor_buffers.size();
			const auto image_start  = m_descriptor_images.size();
			if (ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) !=
			    ShaderRecompiler::IR::ImageResourceClass::None) {
				for (const auto resource: binding.resources) {
					m_descriptor_images.push_back(MakeImageInfo(
					    descriptors.images.at(resource), m_image_occurrences.at(resource)++));
				}
			} else {
				switch (binding.kind) {
					case BindingKind::Buffers:
						for (const auto resource: binding.resources) {
							const auto& view = descriptors.buffers.at(resource);
							EXIT_IF(view.buffer == nullptr);
							m_descriptor_buffers.emplace_back(view.buffer, view.offset, view.range);
						}
						break;
					case BindingKind::BdaPagetable:
					case BindingKind::FaultBuffer: {
						auto&       cache      = m_context.GetBufferCache();
						const auto* bda_buffer = binding.kind == BindingKind::BdaPagetable
						                             ? cache.GetBdaPageTableBuffer()
						                             : cache.GetFaultBuffer();
						m_descriptor_buffers.emplace_back(bda_buffer->Handle(), 0,
						                                  bda_buffer->Size());
						break;
					}
					case BindingKind::FlattenedSrt:
					case BindingKind::ShaderData:
					case BindingKind::Gds: {
						const BufferView* view = &descriptors.gds;
						if (binding.kind == BindingKind::FlattenedSrt) {
							view = &descriptors.flattened_srt;
						} else if (binding.kind == BindingKind::ShaderData) {
							view = &descriptors.shader_data;
						}
						EXIT_IF(view->buffer == nullptr);
						m_descriptor_buffers.emplace_back(view->buffer, view->offset, view->range);
						break;
					}
					case BindingKind::Samplers:
						for (const auto resource: binding.resources) {
							const auto sampler = descriptors.samplers.at(resource);
							EXIT_IF(sampler == nullptr);
							m_descriptor_images.emplace_back(sampler, nullptr,
							                                 vk::ImageLayout::eUndefined);
						}
						break;
					case BindingKind::Count: EXIT("invalid descriptor binding kind");
				}
			}
			if (m_descriptor_buffers.size() != buffer_start) {
				write.pBufferInfo = m_descriptor_buffers.data() + buffer_start;
			}
			if (m_descriptor_images.size() != image_start) {
				write.pImageInfo = m_descriptor_images.data() + image_start;
			}
			m_descriptor_writes.push_back(write);
		}
		for (uint32_t i = 0; i < descriptors.images.size(); i++) {
			const auto expected =
			    descriptors.images[i].mip_views.empty()
			        ? 1u
			        : static_cast<uint32_t>(descriptors.images[i].mip_views.size());
			EXIT_IF(m_image_occurrences[i] != expected);
		}

		const auto shader_data_dwords = program.bindings.ShaderDataDwords();
		EXIT_IF(prepared->shader_data.size() != shader_data_dwords);
		if (program.bindings.UsesPushData()) {
			std::ranges::copy(prepared->shader_data,
			                  push_data.dwords.begin() + program.bindings.push_data_start_dword);
			has_push_data = true;
		}
	}

	if (has_push_data) {
		const auto stages = pipeline_bind_point == vk::PipelineBindPoint::eGraphics
		                        ? vk::ShaderStageFlags {GraphicsStages}
		                        : vk::ShaderStageFlags {vk::ShaderStageFlagBits::eCompute};
		vk_buffer.pushConstants(pipeline.pipeline_layout, stages, 0, sizeof(push_data),
		                        push_data.dwords.data());
	}

	if (!m_descriptor_writes.empty()) {
		EXIT_IF(pipeline.descriptor_set_layout == nullptr);
		if (pipeline.uses_push_descriptors) {
			vk_buffer.pushDescriptorSetKHR(pipeline_bind_point, pipeline.pipeline_layout, 0,
			                               static_cast<uint32_t>(m_descriptor_writes.size()),
			                               m_descriptor_writes.data());
		} else {
			const auto set = m_context.GetDescriptorHeap().Commit(pipeline.descriptor_set_layout);
			for (auto& write: m_descriptor_writes) {
				write.dstSet = set;
			}
			m_context.GetGraphics().device.updateDescriptorSets(
			    static_cast<uint32_t>(m_descriptor_writes.size()), m_descriptor_writes.data(), 0,
			    nullptr);
			vk_buffer.bindDescriptorSets(pipeline_bind_point, pipeline.pipeline_layout, 0, 1, &set,
			                             0, nullptr);
		}
	}
}

} // namespace Libs::Graphics
