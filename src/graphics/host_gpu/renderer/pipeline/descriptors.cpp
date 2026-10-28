#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hackNotice.h"
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
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/execHelpers.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/occlusionQueries.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <vector>

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
		case BindingKind::ShaderData:
		case BindingKind::SharedMemory: return vk::DescriptorType::eStorageBuffer;
		case BindingKind::UniformBuffers: return vk::DescriptorType::eUniformBuffer;
		case BindingKind::Count: EXIT("invalid native descriptor binding kind");
	}
	EXIT("invalid native descriptor binding kind");
}

// Visits each bindless image table range once, with the first root image that samples it.
template <typename Fn>
void ForEachImageTable(const std::vector<ShaderRecompiler::IR::ImageResource>& images, Fn&& fn) {
	for (uint32_t i = 0; i < images.size(); i++) {
		const auto& image = images[i];
		if (image.table_capacity == 0u) {
			continue;
		}
		const auto kind  = *ShaderRecompiler::IR::DescriptorBindingForImage(image);
		bool       first = true;
		for (uint32_t j = 0; j < i && first; j++) {
			first = images[j].table_capacity == 0u ||
			        images[j].table_descriptor_base != image.table_descriptor_base ||
			        ShaderRecompiler::IR::DescriptorBindingForImage(images[j]) != kind;
		}
		if (first) {
			fn(i, kind);
		}
	}
}

vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture, uint32_t element) {
	if (!texture.image_id) {
		EXIT_IF(texture.image_view != nullptr || !texture.mip_views.empty());
		return {nullptr, nullptr, vk::ImageLayout::eGeneral};
	}
	vk::ImageView view = nullptr;
	if (texture.mip_views.empty()) {
		if (element == 0u) {
			view = texture.image_view;
		}
	} else if (element < texture.mip_views.size()) {
		view = texture.mip_views[element];
	}
	EXIT_IF(view == nullptr || texture.layout == vk::ImageLayout::eUndefined);
	return {nullptr, view, texture.layout};
}

// A depth image a table names may be the depth target of a later draw, which samples it in the
// attachment layout: such elements of the tables of draws are visited by every binding, so their
// descriptors follow the layout and AcquireRenderTargets() sees them. Those of compute programs
// are checked as the others (GTA V's ray tracing tables have ~800 depth copies).
static bool DepthTableElement(TextureCache& texture_cache, const TextureBinding& texture) {
	if (!texture.image_id) return false;
	return texture_cache.GetImage(texture.image_id).info.IsDepth();
}

static Prospero::ImageType TextureType(const ShaderTextureResource& descriptor) {
	const auto type = descriptor.Type();
	return type == Prospero::ImageType::kCube ? Prospero::ImageType::kColor2DArray : type;
}

// 1D textures are 2D images one texel high, so they share 2D surfaces at their address.
static Prospero::ImageType TextureBaseType(Prospero::ImageType type) {
	switch (type) {
		case Prospero::ImageType::kColor1D:
		case Prospero::ImageType::kColor1DArray:
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

static vk::DescriptorBufferInfo
NativeStorageBuffer(RenderContext& context, const PreparedBindings::BufferSource& source,
                    const ShaderRecompiler::IR::BufferResource& resource, uint32_t& buffer_offset) {
	buffer_offset = 0;

	const auto& [address, size, id] = source;
	if (address == 0 || size == 0) {
		// Null descriptors read zero and drop writes, with or without shader bounds checks.
		return {nullptr, 0, vk::WholeSize};
	}
	const auto& graphics  = context.GetGraphics();
	// A buffer with a uniform view aligns both views alike: they share the base adjustment.
	const auto  alignment =
	    resource.uniform_index == UINT32_MAX
	        ? graphics.StorageMinAlignment()
	        : std::max<vk::DeviceSize>(
	              graphics.StorageMinAlignment(),
	              graphics.GetPhysicalDeviceProperties().limits.minUniformBufferOffsetAlignment);
	if (size > graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange) {
		EXIT("storage buffer range is unsupported\n");
	}
	// Occlusion counters the GPU has not finished dumping under the range reach the copy before
	// the operation runs (see OcclusionQueries::PrepareRead()).
	auto&      queries  = context.GetOcclusionQueries();
	const bool counters = queries.PrepareRead(address, size);
	auto [buffer, offset] = context.GetBufferCache().ObtainBuffer(
	    address, size, resource.written,
	    resource.formatted || (resource.read && Config::SyncRawImageBuffersEnabled()), id);
	if (counters) {
		queries.NoteReadTarget(address, size, *buffer, offset);
	}
	const auto aligned_offset = Common::AlignDown(offset, alignment);
	const auto adjustment     = offset - aligned_offset;
	const auto max_range      = graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange;
	if (adjustment >= 256 || size > max_range - adjustment) {
		EXIT("storage buffer offset adjustment is unsupported\n");
	}
	buffer_offset = static_cast<uint32_t>(adjustment);
	const vk::DescriptorBufferInfo result {buffer->Handle(), aligned_offset, size + adjustment};
	if (resource.written) {
		context.GetTextureCache().InvalidateMemoryFromGPU(address, size);
	}
	return result;
}

// Reserved and unsupported T# fields must be zero; perf_mod only scales sampler state and the
// mip-stats counter only counts sampling.
static bool IsCanonicalTextureEncoding(const ShaderTextureResource& descriptor, bool r128) {
	constexpr uint32_t field1_reserved_mask = 1u << 29u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	const uint32_t     field3_expected = descriptor.DstSelXYZW() |
	                                     (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	                                     (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	                                     (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t     field4_expected = descriptor.Depth() | (descriptor.BaseArray5() << 16u);
	const uint32_t     field5_expected = (static_cast<uint32_t>(descriptor.PerfMod5()) << 20u) |
	                                     (uint32_t {descriptor.MipStatsCntEn()} << 25u) |
	                                     (static_cast<uint32_t>(descriptor.MaxMip()) << 4u);
	if ((descriptor.fields[1] & field1_reserved_mask) != 0 ||
	    (descriptor.fields[2] & field2_reserved_mask) != 0 || descriptor.fields[3] != field3_expected) {
		return false;
	}
	if (r128) {
		return descriptor.fields[4] == 0 && descriptor.fields[5] == 0 && descriptor.fields[6] == 0 &&
		       descriptor.fields[7] == 0;
	}
	return descriptor.fields[4] == field4_expected && descriptor.fields[5] == field5_expected;
}

bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor, bool r128) {
	if (!IsCanonicalTextureEncoding(descriptor, r128)) {
		return false;
	}
	if (r128) {
		return true;
	}
	if (descriptor.MsaaDepth() && !IsMultisampledTexture(descriptor.Type())) {
		return false;
	}
	// Bits 0-7 name the mip-stats counter. A clear COMPRESSION_EN disables the metadata and its
	// settings.
	const auto         metadata_control = descriptor.fields[6] & 0x00ffff00u;
	constexpr uint32_t pipe_aligned     = 1u << 19u;
	if (!descriptor.MetaCompress()) {
		return true;
	}
	constexpr uint32_t htile_control = pipe_aligned | 1u << 21u;
	const uint32_t expected_control  = htile_control | (descriptor.MsaaDepth() ? (1u << 10u) : 0u);
	const auto     metadata_addr     = descriptor.MetaAddr() << 8u;
	return metadata_control == expected_control && GuestRange {metadata_addr, 1}.Valid() &&
	       (metadata_addr & 0x7fffu) == 0 &&
	       descriptor.TileMode() == Prospero::TileMode::kDepth;
}

struct SampledDepthBindingSupport {
	bool resource_ok = false;
	bool encoding_ok = false;
	bool view_ok     = false;

	[[nodiscard]] bool Supported() const { return resource_ok && encoding_ok && view_ok; }
};

static SampledDepthBindingSupport
CheckSampledDepthBinding(const ShaderRecompiler::IR::ImageResource& resource,
                         const ShaderTextureResource& descriptor, const Image& image,
                         vk::Format view_format) {
	return {
	    .resource_ok = IsSupportedSampledDepthResource(resource),
	    // Comparison sampling changes the host format, not the guest metadata encoding.
	    .encoding_ok = descriptor.TileMode() != Prospero::TileMode::kDepth ||
	                   IsSupportedDepthTextureEncoding(descriptor, resource.r128),
	    .view_ok =
	        IsSupportedSampledDepthView(image.info.pixel_format, view_format, descriptor.DstSelXYZW()),
	};
}

static void ValidateSampledDepthBinding(const ShaderRecompiler::IR::ImageResource& resource,
                                        const ShaderTextureResource& descriptor, const Image& image,
                                        vk::Format view_format, uint64_t size) {
	const auto [resource_ok, encoding_ok, view_ok] =
	    CheckSampledDepthBinding(resource, descriptor, image, view_format);
	if (resource_ok && encoding_ok && view_ok) {
		return;
	}
	const auto descriptor_pitch =
	    TileGetTexturePitch(descriptor.Format(), static_cast<uint32_t>(descriptor.Width5()) + 1u,
	                        descriptor.TileMode());
	EXIT("unsupported sampled depth image: resource=%d encoding=%d view=%d "
	     "class=%u numeric=%u dimension=%u mip_mode=%u read=%d written=%d atomic=%d compare=%d "
	     "guest_format=%u swizzle=0x%03x image_format=%d view_format=%d image_layers=%u "
	     "descriptor_type=%u base_array=%u depth=%u descriptor_pitch=%u target_pitch=%u "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, encoding_ok, view_ok,
	     static_cast<uint32_t>(resource.resource_class),
	     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(resource.dimension),
	     static_cast<uint32_t>(resource.mip_mode), resource.read, resource.written, resource.atomic,
	     resource.depth_compare, static_cast<uint32_t>(descriptor.Format()),
	     descriptor.DstSelXYZW(), static_cast<int>(image.info.pixel_format),
	     static_cast<int>(view_format), image.info.resources.layers,
	     static_cast<uint32_t>(descriptor.Type()), descriptor.BaseArray5(), descriptor.Depth(),
	     descriptor_pitch, image.info.pitch, descriptor.Base40(), size, descriptor.fields[0],
	     descriptor.fields[1], descriptor.fields[2], descriptor.fields[3], descriptor.fields[4],
	     descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
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
	// Storage cube coordinates address individual faces, including partial cube views.
	const bool is_cube = resource.cube && descriptor.Type() == Prospero::ImageType::kCube &&
	                     descriptor.Width5() == descriptor.Height5() &&
	                     descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_2d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray &&
	    ((!resource.cube && is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth()) ||
	     is_cube);
	const bool is_3d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D &&
	                   descriptor.Type() == Prospero::ImageType::kColor3D &&
	                   descriptor.BaseArray5() == 0;
	const bool is_2d_msaa =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa &&
	    descriptor.Type() == Prospero::ImageType::kColor2DMsaa && descriptor.Depth() == 0 &&
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
	// A 4.8 fixed-point MIN_LOD at or below BASE_LEVEL clamps no level of the view.
	return (is_1d || is_1d_array || is_2d || is_2d_array || is_3d || is_2d_msaa) &&
	       supported_tile && descriptor.BaseLevel() <= descriptor.LastLevel() &&
	       descriptor.MinLod() <= descriptor.BaseLevel() * 256u && supported_swizzle &&
	       descriptor.BCSwizzle() == 0 && !descriptor.MsaaDepth();
}

void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size) {
	const auto format        = descriptor.Format();
	const bool resource_ok   = IsSupportedStorageImageResource(resource);
	const bool descriptor_ok = IsSupportedStorageTextureDescriptor(resource, descriptor);
	const bool encoding_ok   = IsCanonicalTextureEncoding(descriptor, resource.r128);
	const bool uint_resource    = resource.numeric_class == Prospero::TextureNumericClass::Uint;
	const bool raw_sint_storage = uint_resource &&
	    ShaderRecompiler::IR::RawSignedStorageFormat(resource, format) != Prospero::BufferFormat::kInvalid;
	const auto numeric_class =
	    Prospero::SampledTextureNumericClass(Prospero::RemapTextureFormat(format));
	const bool raw_float_atomic = format == Prospero::BufferFormat::k32Float && uint_resource &&
	                              resource.atomic && !resource.atomic64;
	const bool format_ok =
	    raw_sint_storage || raw_float_atomic ||
	    (numeric_class != Prospero::TextureNumericClass::Unsupported &&
	     numeric_class != Prospero::TextureNumericClass::Sint &&
	     uint_resource == (numeric_class == Prospero::TextureNumericClass::Uint) &&
	     (!resource.atomic || format == (resource.atomic64 ? Prospero::BufferFormat::k32_32UInt
	                                                       : Prospero::BufferFormat::k32UInt)));
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	EXIT("unsupported storage texture: shader=0x%016" PRIx64 " resource=%d descriptor=%d encoding=%d "
	     "format=%d class=%u numeric=%u dimension=%u mip_mode=%u atomic=%d compare=%d "
	     "base_level=%u last_level=%u max_mip=%u min_lod=%u base_array=%u bc=%u msaa=%d "
	     "depth_tile_bpe=%u swizzle_ok=%d "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " extent=%ux%ux%u type=%u format=%u tile=%u swizzle=0x%03x read=%d written=%d "
	     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     Common::HackNotice::CurrentShaderSlot(), resource_ok, descriptor_ok,
	     encoding_ok, format_ok, static_cast<uint32_t>(resource.resource_class),
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

// sRGB views decode before filtering; a sampler that skips degamma reads through a UNorm view.
static vk::Format SampledViewFormat(const GraphicContext& graphics, Prospero::BufferFormat format,
                                    vk::Format pixel_format, bool skip_degamma) {
	if (skip_degamma) return SrgbUnormViewFormat(pixel_format);
	switch (format) {
		case Prospero::BufferFormat::k8Srgb:
			return graphics.r8_srgb_views ? vk::Format::eR8Srgb : pixel_format;
		case Prospero::BufferFormat::k8_8Srgb:
			return graphics.r8g8_srgb_views ? vk::Format::eR8G8Srgb : pixel_format;
		default: return pixel_format;
	}
}

// Atomics and raw signed stores use unsigned integer views; sRGB images store through UNorm.
static vk::Format StorageViewFormat(const ShaderRecompiler::IR::ImageResource& resource,
                                    Prospero::BufferFormat format, vk::Format pixel_format) {
	if (resource.atomic64) return vk::Format::eR64Uint;
	if (resource.atomic) return vk::Format::eR32Uint;
	const auto raw_sint_format = ShaderRecompiler::IR::RawSignedStorageFormat(resource, format);
	if (raw_sint_format != Prospero::BufferFormat::kInvalid) return VulkanFormat(raw_sint_format);
	return SrgbUnormViewFormat(pixel_format);
}

static ImageViewInfo TextureViewInfo(const ShaderRecompiler::IR::ImageResource& resource,
                                     const ShaderTextureResource& descriptor, vk::Format format,
                                     const SurfaceFormatInfo& surface_format, bool storage,
                                     uint32_t view_levels, uint32_t image_layers) {
	ImageViewInfo view {};
	view.format      = format;
	view.aspect      = vk::ImageAspectFlagBits::eColor;
	view.base_level  = descriptor.BaseLevel();
	view.level_count = view_levels;
	if (descriptor.MinLod() > descriptor.LastLevel() * 256u) {
		EXIT("texture minimum LOD exceeds last mip level: min_lod=%u last_level=%u\n",
		     descriptor.MinLod(), descriptor.LastLevel());
	}
	const auto base_lod = view.base_level * 256u;
	if (descriptor.MinLod() > base_lod) {
		view.min_lod = descriptor.MinLod() - base_lod;
	}
	view.usage = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
	view.mapping =
	    storage || resource.conversion_format != Prospero::BufferFormat::kInvalid
	        ? vk::ComponentMapping {}
	        : TextureGetComponentMapping(descriptor.DstSelXYZW(), surface_format.host_to_storage);
	switch (resource.dimension) {
		case ShaderRecompiler::Decoder::ImageDimension::Dim3D:
			view.type        = vk::ImageViewType::e3D;
			view.base_layer  = 0;
			view.layer_count = 1;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim1DArray:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DArray:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaaArray:
			view.type       = vk::ImageViewType::e2DArray;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture array base layer is out of bounds\n");
			}
			view.layer_count = image_layers - view.base_layer;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim1D:
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

static bool MatchTextureMipView(ImageInfo& info, ImageInfo physical, uint32_t physical_levels,
                                bool metadata, uint32_t view_levels, uint32_t& base_level);

static bool ResolveTextureMipView(ImageInfo& info, uint32_t physical_levels, bool metadata,
                                  uint32_t view_levels, uint32_t& base_level) {
	if (!info.IsTiled()) {
		return false;
	}
	// A view entirely past MAX_MIP addresses its own chain starting at its base mip (PPSA02110
	// compresses mip 1 through a T# that declares one level).
	if (base_level >= physical_levels) {
		ImageInfo  physical = info;
		const auto mip      = info.MipExtent(base_level);
		physical.extent     = {mip.width, mip.height, info.IsVolume() ? mip.depth : 1u};
		if (MatchTextureMipView(info, std::move(physical), view_levels, metadata, view_levels,
		                        base_level)) {
			return true;
		}
	}
	// Otherwise the view reads the declared chain, past MAX_MIP in its mip tail too (ASTRO BOT
	// samples mip 6 of a 1920x1080 target that declares mips 0-5).
	return MatchTextureMipView(info, info, physical_levels, metadata, view_levels, base_level);
}

// Whether the view of `info` from base_level fits in `physical` with physical_levels mips; if so,
// `info` becomes `physical` where they differ and base_level the view's mip in it.
static bool MatchTextureMipView(ImageInfo& info, ImageInfo physical, uint32_t physical_levels,
                                bool metadata, uint32_t view_levels, uint32_t& base_level) {
	physical.resources.levels = physical_levels;
	physical.UpdateSize();
	info.UpdateSize();
	const auto same_mip = [&](uint32_t target, uint32_t source) {
		return physical.mip_layout[target] == info.mip_layout[source] &&
		       physical.MipExtent(target) == info.MipExtent(source);
	};
	if (physical.first_tail_level == info.first_tail_level &&
	    physical.tiled_slice_stride == info.tiled_slice_stride &&
	    physical.data.size == info.data.size) {
		bool matches = true;
		for (uint32_t level = 0; level < physical_levels; ++level) {
			matches &= same_mip(level, level);
		}
		if (matches) {
			return true;
		}
	}
	if (metadata ||
	    (info.TransferLayers() > 1 && physical.tiled_slice_stride != info.tiled_slice_stride)) {
		return false;
	}
	// T# addresses the last mip. A view can select the same stored subresources
	// with different mip indices; inaccessible mips need no host representation.
	for (uint32_t base = 0; base + view_levels <= physical_levels; ++base) {
		bool matches = true;
		for (uint32_t i = 0; i < view_levels; ++i) {
			const auto source = base_level + i;
			const auto target = base + i;
			if (!same_mip(target, source) ||
			    (target >= physical.first_tail_level) != (source >= info.first_tail_level)) {
				matches = false;
				break;
			}
		}
		if (matches) {
			info       = std::move(physical);
			base_level = base;
			return true;
		}
	}
	return false;
}

// What ResolveTexture derives from a T# and the shader's view of it before any texture-cache
// lookup: a pure function of both (EXITs on unsupported descriptors).
static TextureDescDerivation DeriveTextureDesc(const ShaderRecompiler::IR::ImageResource& resource,
                                               const ShaderTextureResource& descriptor) {
	const bool storage = resource.written;
	const auto address         = descriptor.Base40();
	const auto width           = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height          = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto base_level      = descriptor.BaseLevel();
	const auto last_level      = descriptor.LastLevel();
	const auto type            = TextureType(descriptor);
	const bool multisampled    = IsMultisampledTexture(type);
	const auto max_mip         = resource.r128 ? last_level : descriptor.MaxMip();
	const auto physical_levels = multisampled ? 1u : static_cast<uint32_t>(max_mip) + 1u;
	// IMAGE_STORE addresses BASE_LEVEL; only IMAGE_STORE_MIP selects other view mips.
	const bool single_storage_mip =
	    storage && resource.mip_mode != ShaderRecompiler::IR::ImageMipMode::Dynamic;
	auto view_levels = multisampled || single_storage_mip
	                       ? 1u
	                       : static_cast<uint32_t>(last_level - base_level) + 1u;
	auto levels =
	    multisampled ? 1u : std::max(physical_levels, base_level + view_levels);
	const auto tile       = descriptor.TileMode();
	const bool depth_tile = tile == Prospero::TileMode::kDepth;
	const bool msaa_tile  = depth_tile || tile == Prospero::TileMode::kRenderTarget;
	const bool msaa_array = type == Prospero::ImageType::kColor2DMsaaArray;
	if ((!multisampled && base_level > last_level) ||
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
	const auto depth          = static_cast<uint32_t>(descriptor.Depth()) + 1u;
	const auto format         = ShaderRecompiler::IR::ReadsDepthBits(resource)
	                                ? Prospero::BufferFormat::k16UNorm
	                                : descriptor.Format();
	const auto surface_format = TextureGetSurfaceFormatInfo(format);
	const bool shader_conversion =
	    surface_format.conversion_format != Prospero::BufferFormat::kInvalid;
	const bool sampled_numeric_class =
	    storage || resource.numeric_class ==
	                   Prospero::SampledTextureNumericClass(Prospero::RemapTextureFormat(format));
	if (!storage && resource.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled &&
	    !sampled_numeric_class) {
		EXIT("sampled image numeric class mismatch: numeric=%u format=%u addr=0x%016" PRIx64 "\n",
		     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(format), address);
	}

	const bool    volume       = type == Prospero::ImageType::kColor3D;
	const bool    layered      = type == Prospero::ImageType::kColor1DArray ||
	                             type == Prospero::ImageType::kColor2DArray ||
	                             type == Prospero::ImageType::kColor2DMsaaArray;
	auto          image_layers = layered ? depth : 1u;
	// A cube holds six faces even when DEPTH, its last face slice, ends before them (Teardown's
	// sky cube has DEPTH 0).
	if (descriptor.Type() == Prospero::ImageType::kCube) {
		image_layers = std::max(image_layers, 6u);
	}
	auto          view_base    = static_cast<uint32_t>(base_level);
	auto pixel_format = surface_format.vk_format;
	if (resource.depth_compare) {
		if (const auto* depth_format = FindGuestDepthFormatPolicy(format)) {
			pixel_format = depth_format->depth_attachment_format;
		}
	}
	const auto view_format =
	    storage ? StorageViewFormat(resource, format, pixel_format) : pixel_format;
	const auto              block_bytes = Prospero::BlockCompressedBytesPerBlock(format);
	TextureCache::ImageDesc desc {};
	desc.info.data         = {address, 0};
	desc.info.pixel_format = pixel_format;
	desc.info.guest_format = format;
	desc.info.type         = TextureBaseType(type);
	desc.info.extent       = {width, height, volume ? depth : 1u};
	desc.info.resources    = {levels, image_layers};
	desc.info.bytes_per_block =
	    block_bytes != 0 ? block_bytes : Prospero::NumBytesPerElement(format);
	desc.info.samples   = samples;
	desc.info.tile_mode = tile;
	if (!resource.r128 && descriptor.MetaCompress() && tile != Prospero::TileMode::kDepth) {
		desc.info.metadata.kind = ImageMetadataKind::Dcc;
	}
	if (levels > physical_levels) {
		if (!ResolveTextureMipView(desc.info, physical_levels,
		                           !resource.r128 && descriptor.MetaCompress(), view_levels,
		                           view_base)) {
			// The layout stores no mip past MAX_MIP here, so a view starting in the chain ends at
			// MAX_MIP; dynamic-mip shaders still count mips up to LAST_LEVEL.
			if (base_level >= physical_levels ||
			    resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::Dynamic) {
				EXIT("unsupported texture mip view changes physical layout: base=%u last=%u "
				     "max=%u extent=%ux%ux%u tile=%u\n",
				     base_level, last_level, max_mip, width, height, depth,
				     static_cast<uint32_t>(tile));
			}
			view_levels                = physical_levels - base_level;
			desc.info.resources.levels = physical_levels;
			desc.info.data.size        = 0;
			desc.info.UpdateSize();
		}
	} else {
		desc.info.UpdateSize();
	}
	// Image heaps keep the descriptors of unmapped textures, which PS5 never samples, whatever
	// their layout: ResolveTexture checks the layout of a heap slot once it finds it mapped.
	const auto alignment = desc.info.DataAlignment();
	EXIT_NOT_IMPLEMENTED(!resource.heap &&
	                     (desc.info.data.size == 0 || alignment == 0 ||
	                      (address & (static_cast<uint64_t>(alignment) - 1u)) != 0));
	if (storage) {
		ValidateStorageTexture(resource, descriptor, desc.info.data.size);
	}
	if (!resource.r128 && descriptor.MetaCompress() && tile != Prospero::TileMode::kDepth) {
		TileSizeAlign metadata_size {};
		(void)TileGetDccSize(width, height, volume ? depth : image_layers,
		                     desc.info.bytes_per_block, physical_levels, tile, metadata_size,
		                     std::countr_zero(samples));
		desc.info.metadata.range         = {descriptor.MetaAddr() << 8u, metadata_size.size};
		desc.info.metadata.dcc_alpha_msb = descriptor.DccAlphaPos();
	}
	desc.view_info = TextureViewInfo(resource, descriptor, view_format, surface_format, storage,
	                                 view_levels, desc.info.resources.layers);
	desc.view_info.base_level = view_base;
	desc.type = storage ? TextureCache::BindingType::Storage : TextureCache::BindingType::Texture;
	desc.heap = resource.heap;
	const auto data_size = desc.info.data.size;
	return {std::move(desc), shader_conversion, pixel_format, view_format, data_size};
}

size_t RenderExecutor::TextureDescKeyHash::operator()(const TextureDescKey& key) const noexcept {
	// Every draw hashes the key of each texture: independent multiplies of 64-bit lanes instead
	// of a dependent chain per dword.
	std::array<uint64_t, 7> lanes {};
	static_assert(sizeof(key.dwords) + sizeof(key.view) <= sizeof(lanes));
	std::memcpy(lanes.data(), key.dwords.data(), sizeof(key.dwords));
	std::memcpy(reinterpret_cast<uint8_t*>(lanes.data()) + sizeof(key.dwords), key.view.data(),
	            sizeof(key.view));
	constexpr std::array<uint64_t, 7> Multipliers {
	    0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0xd6e8feb86659fd93ull,
	    0xff51afd7ed558ccdull, 0xc4ceb9fe1a85ec53ull, 0x94d049bb133111ebull};
	uint64_t hash = 0;
	for (size_t i = 0; i < lanes.size(); i++) {
		const auto mixed = (lanes[i] ^ (lanes[i] >> 31u)) * Multipliers[i];
		hash ^= (mixed << (i * 9u % 64u)) | (mixed >> ((64u - i * 9u % 64u) % 64u));
	}
	hash ^= hash >> 32u;
	hash *= 0x9e3779b97f4a7c15ull;
	return static_cast<size_t>(hash ^ (hash >> 29u));
}

RenderExecutor::TextureDescKey
RenderExecutor::MakeTextureDescKey(const ShaderRecompiler::IR::ImageResource&   resource,
                                   const ShaderRecompiler::IR::DescriptorValue& value) {
	TextureDescKey key {};
	key.dwords  = value.dwords;
	key.view[0] = static_cast<uint32_t>(resource.resource_class) |
	              (static_cast<uint32_t>(resource.numeric_class) << 8u) |
	              (static_cast<uint32_t>(resource.dimension) << 16u) |
	              (static_cast<uint32_t>(resource.mip_mode) << 24u);
	key.view[1] = resource.mip_count;
	key.view[2] = static_cast<uint32_t>(resource.conversion_format);
	key.view[3] = resource.shader_swizzle;
	key.view[4] =
	    static_cast<uint32_t>(resource.read) | (static_cast<uint32_t>(resource.written) << 1u) |
	    (static_cast<uint32_t>(resource.atomic) << 2u) |
	    (static_cast<uint32_t>(resource.depth_compare) << 3u) |
	    (static_cast<uint32_t>(resource.cube) << 4u) | (static_cast<uint32_t>(resource.r128) << 5u) |
	    (static_cast<uint32_t>(resource.heap) << 6u);
	return key;
}

bool RenderExecutor::ReuseTextureLookup(TextureDescDerivation& derived, TextureBinding& binding) {
	auto&       texture_cache = m_context.GetTextureCache();
	auto&       found         = derived.found;
	auto*       image         = texture_cache.m_slot_images.try_get(found.id);
	const auto& range         = found.desc.info.data;
	if (image == nullptr || !image->registered || image->binding.needs_rebind ||
	    ImageLookupState::Of(found.id, *image) != found.state) {
		found.valid = false;
		return false;
	}
	if (const auto set_epoch = texture_cache.ImageSetEpoch(); found.image_set_epoch != set_epoch) {
		if (texture_cache.ImageEpochInRegion(range.address, range.size) > found.image_set_epoch) {
			found.valid = false;
			return false;
		}
		// No image of the range changed until now: later draws compare with now instead of
		// scanning the pages of the range again.
		found.image_set_epoch = set_epoch;
	}
	// As the lookup and the acquisition would: the image stays in use.
	image->tick_accessed_last = texture_cache.m_gc_tick;
	texture_cache.TouchImage(*image);
	binding.image_id   = found.id;
	binding.image_view = found.view;
	binding.desc       = found.desc;
	binding.layout     = vk::ImageLayout::eUndefined;
	binding.mip_views.clear();
	binding.lookup = &derived;
	binding.reused = true;
	return true;
}

// The image at a bindless table element's address cannot be viewed in the format its T# reads it
// as: the element binds a null image instead of the memory PS5 would reinterpret.
static TextureBinding DeclineTableView(TextureCache::ImageDesc& desc, vk::Format image_format,
                                       vk::Format view_format) {
	Common::HackNotice::Report(
	    "bindless table texture bound as null image",
	    "RenderExecutor::ResolveTexture (incompatible image view)", 0, 0,
	    fmt::format("texture 0x{:016x} image format {} view format {}", desc.info.data.address,
	                static_cast<int>(image_format), static_cast<int>(view_format)));
	return {ImageId {}, nullptr, std::move(desc)};
}

TextureBinding RenderExecutor::ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
                                              const ShaderRecompiler::IR::DescriptorValue& value,
                                              bool table_candidate, GuestRange* examined) {
	if (resource.atomic64 && !m_context.GetGraphics().shader_image_int64_atomics_enabled) {
		EXIT("64-bit image atomics require shaderImageInt64Atomics\n");
	}
	auto       descriptor = DecodeNativeDescriptor<ShaderTextureResource>(value);
	const bool storage    = resource.written;
	if (storage) {
		ValidateStorageImageResource(resource);
	}

	auto& texture_cache = m_context.GetTextureCache();
	if (descriptor.IsNull()) {
		return {.layout = vk::ImageLayout::eGeneral};
	}
	// Image heap resolves do not follow the draws that sample them and storage access is not
	// sampling: neither reports samples.
	if (m_lod_stats && descriptor.MipStatsCntEn() && !resource.heap && !storage) {
		auto&         counter = m_mip_stats[descriptor.MipStatsCntId()];
		const uint8_t base    = descriptor.BaseLevel();
		counter.finest_level  = std::min(counter.finest_level, base);
		if (descriptor.MinLod() > base << 8u) {
			counter.clamped = true;
		} else {
			counter.unclamped = true;
		}
	}

	// The description is computed once per T# and view of it; images are looked up every time.
	const auto key    = MakeTextureDescKey(resource, value);
	auto       cached = m_texture_descs.find(key);
	if (cached == m_texture_descs.end()) {
		auto derived = DeriveTextureDesc(resource, descriptor);
		if (m_texture_descs.size() >= MaxTextureDescs) {
			m_clear_texture_descs = true;
		}
		cached = m_texture_descs.emplace(key, std::move(derived)).first;
	}
	auto& derived = cached->second;
	// Image heaps keep the descriptors of unmapped textures, which PS5 never samples, whatever
	// their layout; a mapped one is checked like any other T# (see DeriveTextureDesc).
	if (resource.heap) {
		const auto& data = derived.desc.info.data;
		if (!m_context.IsMapped(data.address, data.size)) {
			return {.layout = vk::ImageLayout::eGeneral};
		}
		const auto alignment = derived.desc.info.DataAlignment();
		EXIT_NOT_IMPLEMENTED(data.size == 0 || alignment == 0 ||
		                     (data.address & (static_cast<uint64_t>(alignment) - 1u)) != 0);
	}
	// Sampled textures without metadata or stencil: the image and view the last lookup of this
	// description found, when nothing they depend on changed (see TextureDescDerivation::Found).
	// Lookups with metadata materialize fast clears and run every time.
	const bool reusable = !table_candidate && !storage && examined == nullptr &&
	                      resource.mip_mode != ShaderRecompiler::IR::ImageMipMode::Dynamic &&
	                      derived.desc.info.metadata.kind == ImageMetadataKind::None &&
	                      !derived.desc.info.HasStencil();
	if (reusable && derived.found.valid) {
		TextureBinding binding;
		if (ReuseTextureLookup(derived, binding)) {
			return binding;
		}
	}
	auto        desc              = derived.desc;
	const bool  shader_conversion = derived.shader_conversion;
	const auto  pixel_format      = derived.pixel_format;
	const auto  view_format       = derived.view_format;

	if (examined != nullptr) {
		*examined = desc.info.data;
	}
	// A bindless table keeps the T#s of the textures the game has unmapped since, as image heaps
	// do (above), and PS5 never samples them: the GPU would fault on the unmapped pages. A
	// GPU-written range needs no backing; unbacked PRT pages read as zero and count as readable.
	if (table_candidate &&
	    ((!texture_cache.IsRegionGpuModified(desc.info.data.address, desc.info.data.size) &&
	      !Libs::LibKernel::Memory::IsBackingReadable(desc.info.data.address,
	                                                  desc.info.data.size)) ||
	     (desc.info.metadata.kind != ImageMetadataKind::None &&
	      !Libs::LibKernel::Memory::IsBackingReadable(desc.info.metadata.range.address,
	                                                  desc.info.metadata.range.size)))) {
		return {.layout = vk::ImageLayout::eGeneral};
	}
	ImageId depth_source;
	auto    id = table_candidate
	                 ? texture_cache.FindImageSpeculative(desc, shader_conversion, &depth_source)
	                 : texture_cache.FindImage(desc, shader_conversion);
	if (!id && depth_source) {
		// GTA V's ray hit shading (CS 0xc6c91cf1882cdc3b) compares against shadow maps the game
		// writes as R16 or R32 float colors: the table element samples a depth copy of the color
		// image instead of converting the image other passes write.
		id = texture_cache.FindDepthCopy(depth_source, desc.info);
	}
	if (!id) {
		if (table_candidate) {
			// The speculative lookup declined to convert the image at the address between depth
			// and color, and no depth copy can stand for it: the element binds a null image
			// instead of the memory PS5 would read through this T#.
			Common::HackNotice::Report("bindless table texture bound as null image",
			                           "RenderExecutor::ResolveTexture (conversion declined)", 0, 0);
			return {id, nullptr, std::move(desc)};
		}
		// Another extent at the address holds it; the heap retries once an image is created.
		return {.desc = std::move(desc), .layout = vk::ImageLayout::eGeneral};
	}
	auto*      image               = &texture_cache.GetImage(id);
	const bool stencil_association = static_cast<bool>(image->depth_id);
	if (stencil_association) {
		id    = image->depth_id;
		image = &texture_cache.GetImage(id);
	} else if (image->info.IsDepth()) {
		if (storage) {
			EXIT("depth target cannot be bound as a storage image\n");
		}
		if (table_candidate &&
		    !CheckSampledDepthBinding(resource, descriptor, *image, pixel_format).Supported()) {
			return DeclineTableView(desc, image->info.pixel_format, pixel_format);
		}
		ValidateSampledDepthBinding(resource, descriptor, *image, pixel_format,
		                            desc.info.data.size);
	} else if (storage) {
		ValidateStorageColorView(image->info.pixel_format, view_format, descriptor.DstSelXYZW());
	} else {
		// Set after FindImage, whose metadata clears need the attachment-capable format.
		const auto format = ShaderRecompiler::IR::ReadsDepthBits(resource)
		                        ? Prospero::BufferFormat::k16UNorm
		                        : descriptor.Format();
		desc.view_info.format = SampledViewFormat(m_context.GetGraphics(), format, pixel_format,
		                                          resource.skip_degamma);
		if (table_candidate && !IsSupportedSampledColorView(image->info.pixel_format,
		                                                    desc.view_info.format,
		                                                    descriptor.DstSelXYZW())) {
			return DeclineTableView(desc, image->info.pixel_format, desc.view_info.format);
		}
		(void)SelectSampledColorView(image->info.pixel_format, desc.view_info.format,
		                             descriptor.DstSelXYZW());
	}
	TextureBinding binding {id, nullptr, std::move(desc)};
	// Recorded once the view is acquired (AcquireImageViews()). Stencil associations and images
	// with stencil refresh another image as well: they are looked up every time.
	if (reusable && !stencil_association && !image->info.HasStencil()) {
		binding.lookup = &derived;
	}
	return binding;
}

// A bindless table holds every texture the game has loaded; bind a T# the shader's view cannot
// read (other class, dimension or layout) as a null image, which the shader never selects for it.
// ResolveTexture() binds a null image as well for an element whose guest memory is gone, and for
// one the texture cache declines or cannot view as the T# reads it (announced there). An element
// that compares against a color image samples a depth copy of it (TextureCache::FindDepthCopy).
TextureBinding RenderExecutor::ResolveTableTexture(
    const ShaderRecompiler::IR::ImageResource&   root,
    const ShaderRecompiler::IR::DescriptorValue& value, GuestRange* examined) {
	if (ShaderRecompiler::IR::ImageTableSlotCompatible(root, value)) {
		auto texture = ResolveTexture(root, value, true, examined);
		if (texture.image_id) {
			return texture;
		}
	}
	return ResolveTexture(root, ShaderRecompiler::IR::DescriptorValue {.dword_count = 8u});
}

size_t RenderExecutor::TableDescriptorHash::operator()(
    const std::array<uint32_t, 8>& dwords) const noexcept {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const auto dword: dwords) {
		hash = (hash ^ dword) * 0x100000001b3ull;
		hash ^= hash >> 29u;
	}
	return static_cast<size_t>(hash);
}

RenderExecutor::TableResolution&
RenderExecutor::FindTableResolution(const ShaderRecompiler::IR::ImageResource& root) {
	constexpr size_t MaxTables   = 8;
	constexpr size_t MaxElements = size_t {1} << 16u;
	const auto       tick        = ++m_table_resolution_tick;
	// A shader samples several roots from the same tables. Where a root reads its table and where
	// the shader uses it do not affect how a T# resolves, so such roots share their elements.
	auto view                 = root;
	view.source               = 0;
	view.first_use_pc         = 0;
	view.table                = ShaderRecompiler::IR::ImageResource::NoImageTable;
	view.table_capacity       = 0;
	view.table_mapping_offset = 0;
	view.table_entry_mask     = 0;
	auto found = std::ranges::find_if(m_table_resolutions,
	                                  [&](const TableResolution& entry) { return entry.root == view; });
	if (found == m_table_resolutions.end()) {
		if (m_table_resolutions.size() < MaxTables) {
			found = m_table_resolutions.emplace(m_table_resolutions.end());
		} else {
			found = std::ranges::min_element(m_table_resolutions, {}, &TableResolution::last_use);
		}
		found->root = std::move(view);
		m_retired_table_elements.push_back(std::move(found->elements));
		found->elements.clear();
		found->slots.clear();
		m_table_generation++;
	} else if (found->elements.size() > MaxElements) {
		m_retired_table_elements.push_back(std::move(found->elements));
		found->elements.clear();
		found->slots.clear();
		m_table_generation++;
	}
	found->last_use = tick;
	return *found;
}

// What ResolveTexture checks of the backing for a table candidate (a GPU-modified range needs no
// backing).
bool RenderExecutor::TableElementBackingReadable(const TableElementResolution& element) {
	const auto& data = element.range;
	const bool  data_readable =
	    m_context.GetTextureCache().IsRegionGpuModified(data.address, data.size) ||
	    Libs::LibKernel::Memory::IsBackingReadable(data.address, data.size);
	return data_readable &&
	       (element.metadata_range.Empty() ||
	        Libs::LibKernel::Memory::IsBackingReadable(element.metadata_range.address,
	                                                   element.metadata_range.size));
}

RenderExecutor::TableElementResolution&
RenderExecutor::ResolveTableElement(TableResolution&                             table,
                                    const ShaderRecompiler::IR::DescriptorValue& value,
                                    uint64_t stamp, TableElementResolution* known) {
	auto&      texture_cache = m_context.GetTextureCache();
	const auto backing_epoch = Libs::LibKernel::Memory::BackingEpoch();
	bool       inserted      = false;
	if (known == nullptr) {
		const auto [entry, emplaced] = table.elements.try_emplace(value.dwords);
		known                        = &entry->second;
		inserted                     = emplaced;
	}
	auto& element = *known;
	if (inserted) {
		if (m_free_table_element_versions.empty()) {
			element.version_slot = static_cast<uint32_t>(m_table_element_versions.size());
			m_table_element_versions.push_back(0);
		} else {
			element.version_slot = m_free_table_element_versions.back();
			m_free_table_element_versions.pop_back();
		}
	}
	if (!inserted && element.prepared_stamp == stamp) {
		// Resolved for an earlier slot of the same bindings.
		return element;
	}
	const auto set_epoch = texture_cache.ImageSetEpoch();
	if (!inserted && !element.volatile_dcc) {
		if (element.permanent ||
		    (element.checked_epoch == set_epoch && element.backing_epoch == backing_epoch)) {
			return element;
		}
		if (element.backing_epoch != backing_epoch && element.backing_checkable &&
		    TableElementBackingReadable(element) == element.backing_readable) {
			element.backing_epoch = backing_epoch;
		}
		const auto image_epoch =
		    element.range.Valid()
		        ? texture_cache.ImageEpochInRegion(element.range.address, element.range.size)
		        : 0;
		if (image_epoch <= element.image_epoch && element.backing_epoch == backing_epoch) {
			element.checked_epoch = set_epoch;
			return element;
		}
	}
	// Record the epoch before resolving: an image the lookup itself inserts makes the next use
	// resolve again, after which the element is stable.
	const auto prepared_stamp = element.prepared_stamp;
	const auto prepared_index = element.prepared_index;
	const auto version_slot   = element.version_slot;
	element                   = {};
	element.prepared_stamp    = prepared_stamp;
	element.prepared_index    = prepared_index;
	element.version_slot      = version_slot;
	TableElementChanged(element);
	element.image_epoch       = set_epoch;
	element.checked_epoch = set_epoch;
	element.backing_epoch = backing_epoch;
	GuestRange examined {};
	element.texture  = ResolveTableTexture(table.root, value, &examined);
	const auto& data = element.texture.desc.info.data;
	if (!data.Empty()) {
		element.range        = data;
		element.volatile_dcc = element.texture.desc.info.metadata.kind == ImageMetadataKind::Dcc;
		if (element.texture.desc.info.metadata.kind != ImageMetadataKind::None) {
			element.metadata_range = element.texture.desc.info.metadata.range;
		}
		element.backing_checkable = data.Valid();
		element.backing_readable  = element.backing_checkable && TableElementBackingReadable(element);
	} else if (!ShaderRecompiler::IR::ImageTableSlotCompatible(table.root, value)) {
		element.permanent = true;
	} else {
		// Declined by the texture cache lookup, or rejected before it from the T# and the
		// backing alone (no range examined: only a backing change can alter the outcome).
		element.range = examined;
	}
	return element;
}

vk::Sampler RenderExecutor::NativeSampler(const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                          uint32_t                                        index,
                                          const ShaderRecompiler::IR::DescriptorValue&    value) {
	auto        descriptor = DecodeNativeDescriptor<ShaderSamplerResource>(value);
	const auto& sampler    = program.info.samplers[index];
	if (!sampler.depth_compare) {
		descriptor.fields[0] &= ~(0x7u << 12u);
	}
	if (sampler.force_point_filtering) {
		descriptor.SetPointFiltering();
	}
	// Samplers live as long as the sampler cache: remember recent ones without its lock.
	uint64_t hash = sampler.integer_border ? 0x9e3779b97f4a7c15ull : 0;
	for (const auto field: descriptor.fields) {
		hash = (hash ^ field) * 0x100000001b3ull;
	}
	auto& entry = m_sampler_memo[(hash ^ (hash >> 32u)) % m_sampler_memo.size()];
	if (entry.sampler != nullptr && entry.integer_border == sampler.integer_border &&
	    std::memcmp(entry.fields.data(), descriptor.fields, sizeof(entry.fields)) == 0) {
		return entry.sampler;
	}
	const auto native = m_context.GetSamplerCache().GetSampler(descriptor, sampler.integer_border);
	std::memcpy(entry.fields.data(), descriptor.fields, sizeof(entry.fields));
	entry.integer_border = sampler.integer_border;
	entry.sampler        = native;
	return native;
}

static vk::DescriptorBufferInfo NativeUpload(RenderContext&            context,
                                             std::span<const uint32_t> data) {
	EXIT_IF(data.empty());
	auto&      cache  = context.GetBufferCache();
	const auto offset = cache.StreamUpload(data.data(), data.size_bytes(), 256);
	auto&      buffer = cache.GetUtilityBuffer(MemoryUsage::Stream);
	return {buffer.Handle(), offset, data.size_bytes()};
}

// Whether binding `texture` (a table element nothing else in the operation binds) leaves its
// image as it is: CommitBindings() would record no barrier for it.
// Whether a table element's view, just acquired in `state`, can be kept while the image keeps that
// state (see TableViewCurrent()). A depth copy has no guest memory to be dirty (see
// TextureCache::FindDepthCopy()).
static bool TableViewKeepable(const ImageLookupState& state, const Image& image) {
	return !state.cpu_dirty && !state.buffer_modified &&
	       (!image.info.data.Empty() || image.is_depth_copy);
}

// Whether the view a table element acquired in `view` still holds: acquiring it again would only
// refresh the image's use. GTA V's ray tracing tables visit ~700 depth copies per frame.
static bool TableViewCurrent(const TextureCache& texture_cache, const Image& image,
                             const TextureBinding& texture, const TableViewMemo& view) {
	return view.valid && texture.image_view &&
	       ImageLookupState::Of(texture.image_id, image) == view.state &&
	       (!image.is_depth_copy || texture_cache.DepthCopyCurrent(image));
}

static bool TableImageReady(const Image& image, const TextureBinding& texture) {
	if (image.binding.is_bound || image.binding.is_target || image.binding.force_general ||
	    texture.desc.type == TextureCache::BindingType::Storage) {
		return false;
	}
	// As the transit of a sampled element (see CommitBindings()) and Image::GetBarriers().
	const auto layout = image.info.data.Empty() ? vk::ImageLayout::eGeneral
	                    : image.info.IsDepth()  ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
	                                            : vk::ImageLayout::eShaderReadOnlyOptimal;
	if (texture.layout != layout) {
		return false;
	}
	const auto ready = [layout](const VulkanImageState& state) {
		return state.layout == layout && state.access_mask == vk::AccessFlagBits2::eShaderRead;
	};
	const auto& subresources = image.backing.subresource_states;
	const auto& view         = texture.desc.view_info;
	ImageSubresourceRange range {view.base_level, view.level_count, view.base_layer,
	                             view.layer_count};
	if (image.info.IsVolume()) {
		range.base_layer  = 0;
		range.layer_count = 1;
	}
	const auto& resources = image.info.resources;
	const bool  partial   = range.base_level != 0 || range.level_count != resources.levels ||
	                     range.base_layer != 0 || range.layer_count != resources.layers;
	if (!partial && subresources.empty()) {
		return ready(image.backing.state);
	}
	if (subresources.empty()) {
		// Every subresource has the image's state.
		return ready(image.backing.state);
	}
	if (subresources.size() != size_t {resources.levels} * resources.layers) {
		return false;
	}
	for (uint32_t level = range.base_level; level < range.base_level + range.level_count; level++) {
		for (uint32_t layer = range.base_layer; layer < range.base_layer + range.layer_count;
		     layer++) {
			const auto index = size_t {level} * resources.layers + layer;
			if (index >= subresources.size() || !ready(subresources[index])) {
				return false;
			}
		}
	}
	return true;
}

void RenderExecutor::MarkTableElement(TableBindingState& state, uint32_t element) {
	if (state.marks[element] != state.mark_stamp) {
		state.marks[element] = state.mark_stamp;
		m_table_marked.push_back(element);
	}
}

void RenderExecutor::MarkTablePages(TableBindingState& state, uint64_t address, uint64_t size) {
	if (size == 0) {
		return;
	}
	constexpr uint32_t PageBits = TextureCache::ImagePageBits;
	const uint64_t     first    = address >> PageBits;
	const uint64_t     last =
	    (size > UINT64_MAX - address ? UINT64_MAX : address + size - 1) >> PageBits;
	const uint64_t from = first > state.max_span ? first - state.max_span : 0;
	auto span = std::ranges::lower_bound(state.spans, from, {}, &TableBindingState::PageSpan::first);
	for (; span != state.spans.end() && span->first <= last; ++span) {
		if (span->last >= first) {
			MarkTableElement(state, span->element);
		}
	}
}

void RenderExecutor::MarkTableChanges(TableBindingState& state) {
	const auto backing_epoch = Libs::LibKernel::Memory::BackingEpoch();
	if (backing_epoch != state.backing_epoch) {
		for (const auto element: state.unchecked_backing) {
			MarkTableElement(state, element);
		}
		m_table_changes.clear();
		if (Libs::LibKernel::Memory::BackingChangesSince(state.backing_epoch, m_table_changes)) {
			for (const auto& [address, size]: m_table_changes) {
				MarkTablePages(state, address, size);
			}
		} else {
			for (uint32_t element = 0; element < state.elements.size(); element++) {
				MarkTableElement(state, element);
			}
		}
		state.backing_epoch = backing_epoch;
	}
	auto&      texture_cache = m_context.GetTextureCache();
	const auto set_epoch     = texture_cache.ImageSetEpoch();
	if (set_epoch != state.set_epoch) {
		m_table_changes.clear();
		const bool known = texture_cache.ForEachImageSetChangeSince(
		    state.set_epoch,
		    [&](uint64_t address, uint64_t size) { m_table_changes.emplace_back(address, size); });
		if (known) {
			for (const auto& [address, size]: m_table_changes) {
				MarkTablePages(state, address, size);
			}
		} else {
			for (uint32_t element = 0; element < state.elements.size(); element++) {
				MarkTableElement(state, element);
			}
		}
		state.set_epoch = set_epoch;
	}
}

void RenderExecutor::RebuildTableSpans(TableBindingState& state) {
	constexpr uint32_t PageBits = TextureCache::ImagePageBits;
	state.spans.clear();
	state.max_span = 0;
	for (uint32_t element = 0; element < state.elements.size(); element++) {
		auto&       entry      = state.elements[element];
		const auto& resolution = *entry.resolution;
		entry.span_range       = resolution.range;
		entry.span_metadata    = resolution.metadata_range;
		for (const auto& range: {resolution.range, resolution.metadata_range}) {
			if (range.size == 0) {
				continue;
			}
			const uint64_t first = range.address >> PageBits;
			const uint64_t last  = (range.size > UINT64_MAX - range.address
			                            ? UINT64_MAX
			                            : range.address + range.size - 1) >>
			                      PageBits;
			state.spans.push_back({first, last, element});
			state.max_span = std::max(state.max_span, last - first);
		}
	}
	std::ranges::sort(state.spans, {}, &TableBindingState::PageSpan::first);
}

void RenderExecutor::AddTableSpans(TableBindingState& state, uint32_t element) {
	// As RebuildTableSpans() does; MergeTableSpans() puts them in order.
	constexpr uint32_t PageBits   = TextureCache::ImagePageBits;
	auto&              entry      = state.elements[element];
	const auto&        resolution = *entry.resolution;
	entry.span_range              = resolution.range;
	entry.span_metadata           = resolution.metadata_range;
	for (const auto& range: {resolution.range, resolution.metadata_range}) {
		if (range.size == 0) {
			continue;
		}
		const uint64_t first = range.address >> PageBits;
		const uint64_t last  = (range.size > UINT64_MAX - range.address
		                            ? UINT64_MAX
		                            : range.address + range.size - 1) >>
		                      PageBits;
		state.spans.push_back({first, last, element});
		state.max_span = std::max(state.max_span, last - first);
	}
}

void RenderExecutor::MergeTableSpans(TableBindingState& state, size_t sorted) {
	// Merges the spans AddTableSpans() appended after the first `sorted` into them, in the order
	// inserting each one after the spans with its first page would give. Inserting them one by
	// one moved the spans after each: GTA V's ray tracing tables have ~26 000 elements and take
	// in hundreds while it streams textures in (up to 9 ms in one binding).
	auto& spans = state.spans;
	if (sorted == spans.size()) {
		return;
	}
	const auto middle = spans.begin() + static_cast<std::ptrdiff_t>(sorted);
	std::stable_sort(middle, spans.end(), [](const auto& a, const auto& b) {
		return a.first < b.first;
	});
	m_table_spans.clear();
	m_table_spans.reserve(spans.size());
	std::ranges::merge(std::ranges::subrange(spans.begin(), middle),
	                   std::ranges::subrange(middle, spans.end()),
	                   std::back_inserter(m_table_spans), {}, &TableBindingState::PageSpan::first,
	                   &TableBindingState::PageSpan::first);
	spans.swap(m_table_spans);
}

void RenderExecutor::UnpinTableBindings(TableBindingState& state) {
	auto& texture_cache = m_context.GetTextureCache();
	for (auto& element: state.elements) {
		if (element.pinned) {
			texture_cache.UnpinImage(element.pinned);
			element.pinned = {};
		}
	}
	state.valid = false;
}

void RenderExecutor::IndexTableSlots(TableBindingState& state) {
	// A counting sort of the slots by element.
	state.element_slot_begin.assign(state.elements.size() + 1, 0);
	for (const auto element: state.slots) {
		state.element_slot_begin[element + 1]++;
	}
	for (size_t element = 0; element < state.elements.size(); element++) {
		state.element_slot_begin[element + 1] += state.element_slot_begin[element];
	}
	state.element_slots.resize(state.slots.size());
	std::vector<uint32_t> next(state.element_slot_begin.begin(), state.element_slot_begin.end() - 1);
	for (size_t slot = 0; slot < state.slots.size(); slot++) {
		state.element_slots[next[state.slots[slot]]++] = static_cast<uint32_t>(slot);
	}
}

void RenderExecutor::PatchTableSlots(const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                     const ShaderRecompiler::IR::ResourceSnapshot&   snapshot,
                                     TableBindingState& state, uint64_t stamp) {
	auto& texture_cache = m_context.GetTextureCache();
	const ShaderRecompiler::IR::DescriptorValue null_value {.dword_count = 8u};
	// The root image of each range, in the order ReuseTableBindings() compared them.
	std::vector<uint32_t> roots;
	ForEachImageTable(program.info.images,
	                  [&](uint32_t root_index, auto /*kind*/) { roots.push_back(root_index); });
	auto&        element_of   = state.element_of_version;
	const size_t sorted_spans = state.spans.size();
	for (const auto& changed: m_table_changed_slots) {
		auto&       range = state.ranges[changed.range];
		const auto& table = snapshot.image_tables.at(range.table);
		const auto& value = changed.index != 0u && changed.index < table.slots.size()
		                        ? table.slots[changed.index]
		                        : null_value;
		state.values[changed.slot] = value;
		// The element of the T#, as the slot by slot resolution in PrepareBindings() finds it.
		auto&                   resolution = *range.resolution;
		auto&                   memo       = resolution.slots[changed.index];
		TableElementResolution* element    = nullptr;
		bool                    resolved   = false;
		if (memo.element != nullptr && memo.dwords == value.dwords) {
			element = memo.element;
		} else {
			if (const auto found = resolution.elements.find(value.dwords);
			    found != resolution.elements.end()) {
				element = &found->second;
			} else {
				element  = &ResolveTableElement(resolution, value, stamp);
				resolved = true;
			}
			memo = {value.dwords, element, element->version_slot};
		}
		const auto version_slot = memo.version_slot;
		if (version_slot < element_of.size() && element_of[version_slot] != UINT32_MAX) {
			// GTA V moves thousands of T#s between the slots of its ray tracing tables in a
			// binding. The element's descriptor holds for this slot as for its others: whatever
			// may change it marks the element for a visit, which writes all its slots (see
			// RebindReusedTables()). CommitBindings() writes this slot.
			state.slots[changed.slot] = element_of[version_slot];
			m_table_moved_slots.push_back(changed.slot);
			continue;
		}
		if (!resolved) {
			(void)ResolveTableElement(resolution, value, stamp, element);
		}
		// As FinishTableBindings() records the elements of a slot by slot resolution; the visit
		// binds it and writes its descriptor.
		const auto index = static_cast<uint32_t>(state.elements.size());
		state.elements.push_back({.resolution = element, .table = &resolution, .source = value,
		                          .root = roots.at(changed.range)});
		auto& entry = state.elements.back();
		if (element->volatile_dcc || (!state.compute && DepthTableElement(texture_cache, element->texture))) {
			state.always.push_back(index);
		} else if (!element->permanent && !element->backing_checkable) {
			state.unchecked_backing.push_back(index);
		}
		entry.pinned = element->texture.image_id;
		texture_cache.PinImage(entry.pinned);
		// Written with the visit (the element has no view yet).
		state.infos.emplace_back();
		state.marks.push_back(0);
		state.checked.emplace_back();
		AddTableSpans(state, index);
		EXIT_IF(version_slot == UINT32_MAX);
		if (version_slot >= element_of.size()) {
			element_of.resize(size_t {version_slot} + 1, UINT32_MAX);
		}
		element_of[version_slot]  = index;
		state.slots[changed.slot] = index;
		MarkTableElement(state, index);
	}
	MergeTableSpans(state, sorted_spans);
	IndexTableSlots(state);
	// Elements no slot names any more keep their images pinned: once there are many, they are
	// dropped.
	size_t unused = 0;
	for (size_t element = 0; element < state.elements.size(); element++) {
		unused += state.element_slot_begin[element + 1] == state.element_slot_begin[element] ? 1 : 0;
	}
	if (unused > std::max<size_t>(64, state.elements.size() / 16)) {
		DropUnusedTableElements(state);
	}
}

void RenderExecutor::DropUnusedTableElements(TableBindingState& state) {
	// Keeps the order of the elements that slots still name; IndexTableSlots() is current.
	auto&      texture_cache = m_context.GetTextureCache();
	const auto count         = static_cast<uint32_t>(state.elements.size());
	auto&      remap         = m_table_remap;
	remap.assign(count, UINT32_MAX);
	uint32_t kept = 0;
	for (uint32_t element = 0; element < count; element++) {
		auto& entry = state.elements[element];
		if (state.element_slot_begin[element + 1] == state.element_slot_begin[element]) {
			if (entry.pinned) {
				texture_cache.UnpinImage(entry.pinned);
			}
			state.element_of_version[entry.resolution->version_slot] = UINT32_MAX;
			continue;
		}
		remap[element] = kept;
		if (kept != element) {
			state.element_of_version[entry.resolution->version_slot] = kept;
			state.elements[kept]                                     = std::move(entry);
			state.infos[kept]                                        = state.infos[element];
			state.marks[kept]                                        = state.marks[element];
			state.checked[kept]                                      = state.checked[element];
		}
		kept++;
	}
	state.elements.resize(kept);
	state.infos.resize(kept);
	state.marks.resize(kept);
	state.checked.resize(kept);
	for (auto& slot: state.slots) {
		slot = remap[slot];
	}
	const auto renumber = [&](std::vector<uint32_t>& elements) {
		std::erase_if(elements, [&](uint32_t element) { return remap[element] == UINT32_MAX; });
		for (auto& element: elements) {
			element = remap[element];
		}
	};
	renumber(state.always);
	renumber(state.unchecked_backing);
	// Marked by this binding: a dropped element needs no visit.
	renumber(m_table_marked);
	std::erase_if(state.spans, [&](const auto& span) { return remap[span.element] == UINT32_MAX; });
	for (auto& span: state.spans) {
		span.element = remap[span.element];
	}
	IndexTableSlots(state);
}

void RenderExecutor::FinishTableBindings(TableBindingState& state,
                                         const PreparedBindings& prepared) {
	if (state.generation != m_table_generation ||
	    state.elements.size() != m_table_image_infos.size() ||
	    state.values.size() != prepared.table_slots.size()) {
		UnpinTableBindings(state);
		return;
	}
	auto& texture_cache = m_context.GetTextureCache();
	state.infos         = m_table_image_infos;
	state.slots         = prepared.table_slots;
	state.slot_infos.resize(state.slots.size());
	for (size_t slot = 0; slot < state.slots.size(); slot++) {
		state.slot_infos[slot] = state.infos[state.slots[slot]];
	}
	IndexTableSlots(state);
	auto& element_of = state.element_of_version;
	std::ranges::fill(element_of, UINT32_MAX);
	for (uint32_t index = 0; index < state.elements.size(); index++) {
		const auto version_slot = state.elements[index].resolution->version_slot;
		EXIT_IF(version_slot == UINT32_MAX);
		if (version_slot >= element_of.size()) {
			element_of.resize(size_t {version_slot} + 1, UINT32_MAX);
		}
		element_of[version_slot] = index;
	}
	state.always.clear();
	state.unchecked_backing.clear();
	for (uint32_t index = 0; index < state.elements.size(); index++) {
		auto&       element    = state.elements[index];
		const auto& resolution = *element.resolution;
		if (resolution.volatile_dcc || (!state.compute && DepthTableElement(texture_cache, resolution.texture))) {
			state.always.push_back(index);
		} else if (!resolution.permanent && !resolution.backing_checkable) {
			state.unchecked_backing.push_back(index);
		}
		element.pinned = resolution.texture.image_id;
		texture_cache.PinImage(element.pinned);
	}
	RebuildTableSpans(state);
	state.marks.assign(state.elements.size(), 0);
	state.checked.assign(state.elements.size(), {});
	state.mark_stamp = 0;
	state.valid      = true;
}

bool RenderExecutor::ReuseTableBindings(const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                        const ShaderRecompiler::IR::ResourceSnapshot&   snapshot,
                                        TableBindingState& state, uint64_t stamp,
                                        PreparedBindings& prepared) {
	if (!state.valid || state.generation != m_table_generation) {
		return false;
	}
	// The same tables with the same T# in every slot.
	const ShaderRecompiler::IR::DescriptorValue null_value {.dword_count = 8u};
	size_t                                      range_index = 0;
	size_t                                      slot_index  = 0;
	bool                                        same        = true;
	// Slots may name other T#s now (GTA V adds the textures it streams in to its bindless tables,
	// and in places moves thousands of T#s between the ~15 000 slots it fills, at times most of
	// them): those slots are bound again, the rest is reused. Binding a changed slot costs less
	// than resolving a slot again, however many change.
	m_table_changed_slots.clear();
	ForEachImageTable(program.info.images, [&](uint32_t root_index, auto /*kind*/) {
		if (!same) {
			return;
		}
		const auto& root = program.info.images.at(root_index);
		if (range_index == state.ranges.size()) {
			same = false;
			return;
		}
		auto& cached = state.ranges[range_index++];
		if (cached.table != root.table || cached.capacity != root.table_capacity ||
		    !(cached.root == root) || &FindTableResolution(root) != cached.resolution ||
		    state.generation != m_table_generation) {
			same = false;
			return;
		}
		const auto& table = snapshot.image_tables.at(root.table);
		// The same slots as when the values were recorded (the table's words did not change):
		// nothing to compare. GTA V's tables hold ~16 000 T#s, compared on every binding.
		if (table.slots_id != 0 && table.slots_id == cached.slots_id) {
			slot_index += root.table_capacity;
			return;
		}
		cached.slots_id = table.slots_id;
		for (uint32_t slot = 0; slot < root.table_capacity; slot++) {
			const auto& value =
			    slot != 0u && slot < table.slots.size() ? table.slots[slot] : null_value;
			if (!(state.values[slot_index] == value)) {
				m_table_changed_slots.push_back(
				    {static_cast<uint32_t>(slot_index), static_cast<uint32_t>(range_index - 1), slot});
			}
			slot_index++;
		}
	});
	if (!same) {
		return false;
	}
	if (range_index != state.ranges.size()) {
		return false;
	}
	state.mark_stamp++;
	m_table_marked.clear();
	m_table_moved_slots.clear();
	if (!m_table_changed_slots.empty()) {
		PatchTableSlots(program, snapshot, state, stamp);
	}
	for (const auto element: state.always) {
		MarkTableElement(state, element);
	}
	MarkTableChanges(state);
	for (const auto element: m_table_marked) {
		auto& entry = state.elements[element];
		(void)ResolveTableElement(*entry.table, entry.source, stamp, entry.resolution);
	}
	prepared.table_visits      = m_table_marked;
	prepared.table_moved_slots = m_table_moved_slots;
	return true;
}

void RenderExecutor::RebindReusedTables(PreparedBindings& prepared) {
	auto&       state         = *static_cast<TableBindingState*>(prepared.table_state);
	const auto& program       = prepared.runtime->module->info;
	auto&       texture_cache = m_context.GetTextureCache();
	const auto  tick          = texture_cache.m_gc_tick;
	const auto  stale         = [&](const TextureBinding& binding) {
        if (!binding.image_id) {
            return false;
        }
        const auto* image = texture_cache.m_slot_images.try_get(binding.image_id);
        return image == nullptr || (!image->registered && !image->info.data.Empty()) ||
               image->binding.needs_rebind;
	};
	// The elements PrepareBindings() marked, and those whose image is no longer bound as the
	// state records: removed, replaced, dirty, with another view or in another layout.
	const auto unchanged = [&](uint32_t index) {
		const auto& element = state.elements[index];
		const auto& texture = element.resolution->texture;
		const auto& view    = element.resolution->view;
		if (!texture.image_id) {
			// A null element has no image to change: only its own resolution (marked above) can.
			return element.pinned == texture.image_id && texture.image_view == nullptr &&
			       state.infos[index].imageView == nullptr &&
			       texture.layout == state.infos[index].imageLayout;
		}
		const auto* image = texture_cache.m_slot_images.try_get(texture.image_id);
		return image != nullptr && !image->binding.needs_rebind &&
		       texture.image_id == element.pinned && texture.image_view != nullptr &&
		       texture.image_view == state.infos[index].imageView &&
		       texture.layout == state.infos[index].imageLayout &&
		       (image->info.data.Empty() ||
		        (image->registered && view.valid &&
		         ImageLookupState::Of(texture.image_id, *image) == view.state)) &&
		       (!image->is_depth_copy || texture_cache.DepthCopyCurrent(*image)) && TableImageReady(*image, texture);
	};
	// The images the operation binds so far: TableImageReady() reads the binding state they have
	// in this operation, which their versions do not count.
	const auto bound_stamp = ++m_bound_image_stamp;
	for (const auto id: m_bound_images) {
		if (!id) {
			continue;
		}
		if (id.index >= m_bound_image_marks.size()) {
			m_bound_image_marks.resize(size_t {id.index} + 1, 0);
		}
		m_bound_image_marks[id.index] = bound_stamp;
	}
	// An element found unchanged with versions that still hold is still unchanged: the check
	// compares a few dense counters instead of reading the element's resolution and image.
	using Checked              = TableBindingState::Checked;
	const auto still_unchanged = [&](const Checked& checked) {
		if (checked.image == Checked::None ||
		    m_table_element_versions[checked.element] != checked.element_version) {
			return false;
		}
		return checked.image == Checked::NullImage ||
		       ((checked.image >= m_bound_image_marks.size() ||
		         m_bound_image_marks[checked.image] != bound_stamp) &&
		        texture_cache.ImageVersion(ImageId {checked.image}) == checked.image_version);
	};
	const auto element_count = static_cast<uint32_t>(state.elements.size());
	EXIT_IF(state.checked.size() != element_count);
	m_table_checks.clear();
	for (uint32_t index = 0; index < element_count; index++) {
		if (state.marks[index] != state.mark_stamp && !still_unchanged(state.checked[index])) {
			m_table_checks.push_back(index);
		}
	}
	// The others are checked by reading them and their images (scattered in memory): after a
	// large change, helper threads take part, each flagging the elements of its chunks.
	const auto check_count = static_cast<uint32_t>(m_table_checks.size());
	m_table_flags.assign(check_count, 0);
	GetExecHelpers().ParallelFor(check_count, 256, [&](uint32_t begin, uint32_t end) {
		for (uint32_t check = begin; check < end; check++) {
			const auto index   = m_table_checks[check];
			auto&      checked = state.checked[index];
			// The versions before the check: a change during or after it moves them.
			const auto& resolution = *state.elements[index].resolution;
			const auto  image_id   = resolution.texture.image_id;
			const auto  slot       = resolution.version_slot;
			const Checked seen {
			    .image           = image_id ? image_id.index : Checked::NullImage,
			    .image_version   = image_id ? texture_cache.ImageVersion(image_id) : 0u,
			    .element         = slot,
			    .element_version = slot < m_table_element_versions.size()
			                           ? m_table_element_versions[slot]
			                           : 0u};
			if (!unchanged(index)) {
				m_table_flags[check] = 1;
				checked              = {};
				continue;
			}
			// A depth copy also depends on its color image (DepthCopyCurrent()): depth elements
			// are checked by every binding (and those of draws visited, see DepthTableElement()).
			const auto* image = image_id ? texture_cache.m_slot_images.try_get(image_id) : nullptr;
			if (slot >= m_table_element_versions.size() ||
			    (image != nullptr && (image->info.IsDepth() || image->is_depth_copy))) {
				checked = {};
				continue;
			}
			checked = seen;
		}
	});
	for (uint32_t check = 0; check < check_count; check++) {
		if (m_table_flags[check] != 0) {
			MarkTableElement(state, m_table_checks[check]);
		}
	}
	size_t visited = 0;
	for (;;) {
		for (; visited < m_table_marked.size(); visited++) {
			auto& element = state.elements[m_table_marked[visited]];
			auto& texture = element.resolution->texture;
			auto& view    = element.resolution->view;
			// As RebindImages() does for every element of resolved tables.
			if (stale(texture)) {
				if (auto* old_image = texture_cache.m_slot_images.try_get(texture.image_id)) {
					old_image->binding = {};
				}
				texture = ResolveTableTexture(program.info.images.at(element.root), element.source);
				view.valid = false;
			}
			BindImage(texture.image_id, false);
			if (!texture.image_id) {
				// Bound as a null descriptor: nothing to acquire.
				view.valid = false;
			} else if (auto* image = texture_cache.m_slot_images.try_get(texture.image_id);
			    image != nullptr && TableViewCurrent(texture_cache, *image, texture, view)) {
				image->tick_accessed_last = tick;
				texture_cache.TouchImage(*image);
			} else {
				texture.image_view  = texture_cache.FindTexture(texture.image_id, texture.desc);
				auto& acquired         = texture_cache.GetImage(texture.image_id);
				view.state             = ImageLookupState::Of(texture.image_id, acquired);
				view.valid             = TableViewKeepable(view.state, acquired);
			}
			if (texture.image_id != element.pinned) {
				texture_cache.UnpinImage(element.pinned);
				element.pinned = texture.image_id;
				texture_cache.PinImage(element.pinned);
			}
			// Other programs' states share the element; this one writes its descriptor below.
			TableElementChanged(*element.resolution);
			state.checked[m_table_marked[visited]] = {};
		}
		// The lookups above may have registered images over other elements' ranges.
		MarkTableChanges(state);
		if (visited == m_table_marked.size()) {
			break;
		}
		for (auto index = visited; index < m_table_marked.size(); index++) {
			auto& element = state.elements[m_table_marked[index]];
			(void)ResolveTableElement(*element.table, element.source, m_table_prepare_stamp,
			                          element.resolution);
		}
	}
	// Elements whose range changed are found by their new range from now on.
	for (const auto index: m_table_marked) {
		const auto& element    = state.elements[index];
		const auto& resolution = *element.resolution;
		if (resolution.range.address != element.span_range.address ||
		    resolution.range.size != element.span_range.size ||
		    resolution.metadata_range.address != element.span_metadata.address ||
		    resolution.metadata_range.size != element.span_metadata.size) {
			RebuildTableSpans(state);
			break;
		}
	}
	prepared.table_visits = m_table_marked;
}

void RenderExecutor::FlushDeferredDispatchBarrier() {
	m_dispatch_chained = false;
	if (!m_deferred_barrier) {
		return;
	}
	m_deferred_barrier = false;
	ShaderAccessBarrier(m_context.GetCommandScheduler().Current().Handle(),
	                    vk::PipelineStageFlagBits::eComputeShader);
}

void RenderExecutor::BeginGuestOperation(uint32_t queue, bool dispatch_direct, bool guest_sync) {
	m_guest_queue = queue;
	if (m_deferred_barrier && dispatch_direct && !guest_sync && queue == m_deferred_barrier_queue) {
		m_dispatch_chained = true;
		return;
	}
	FlushDeferredDispatchBarrier();
}

void RenderExecutor::BindImage(ImageId id, bool storage) {
	if (!id) {
		return;
	}
	auto& image = m_context.GetTextureCache().GetImage(id);
	if (image.binding.is_bound) {
		image.binding.force_general |= image.binding.shader_write != storage;
	}
	image.binding.is_bound = true;
	image.binding.shader_write |= storage;
	m_bound_images.push_back(id);
}

void RenderExecutor::BindRenderTarget(ImageId id) {
	auto& image             = m_context.GetTextureCache().GetImage(id);
	image.binding.is_target = true;
	m_bound_images.push_back(id);
}

void RenderExecutor::ResetBindings() {
	for (auto& elements: m_retired_table_elements) {
		for (auto& [dwords, element]: elements) {
			if (element.version_slot < m_table_element_versions.size()) {
				// Holders of the counter (table binding states of an older generation) see it move.
				m_table_element_versions[element.version_slot]++;
				m_free_table_element_versions.push_back(element.version_slot);
			}
		}
	}
	m_retired_table_elements.clear();
	for (const auto id: m_bound_images) {
		m_context.GetTextureCache().m_slot_images[id].binding = {};
	}
	m_bound_images.clear();
	if (m_clear_texture_descs) {
		m_clear_texture_descs = false;
		m_texture_descs.clear();
		m_texture_descs_generation++;
	}
}

void RenderExecutor::PrepareBindings(const ShaderStageRuntime& runtime,
                                     PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(!runtime);
	const auto& program  = runtime.module->info;
	const auto& snapshot = *runtime.resources;
	// Names the program in notices and errors of the resolution below.
	const Common::HackNotice::ScopedShader shader_scope(program.shader_hash);
	prepared.runtime = &runtime;
	prepared.gds = {nullptr, 0, VK_WHOLE_SIZE};
	prepared.flattened_srt = {};
	prepared.shader_data_buffer = {};
	prepared.shared_memory = {};
	prepared.images.resize(program.info.images.size());
	prepared.shader_data.clear();
	const bool same_program = prepared.last_program == &program &&
	                          prepared.last_descs_generation == m_texture_descs_generation;
	const bool same_images  = same_program && prepared.last_images == snapshot.images;
	// Otherwise the slots whose T# did not change keep their descriptions: one changed texture
	// does not look up every other one again.
	const bool same_slots = same_program && prepared.last_images.size() == snapshot.images.size() &&
	                        prepared.image_descs.size() == program.info.images.size();
	if (!same_slots) {
		prepared.image_descs.assign(program.info.images.size(), nullptr);
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		if (program.info.images[i].heap) {
			prepared.images[i] = {};
			continue;
		}
		// Sampled textures whose last lookup still holds are written in place.
		const auto& resource = program.info.images[i];
		if (!resource.written &&
		    resource.mip_mode != ShaderRecompiler::IR::ImageMipMode::Dynamic) {
			auto* derived = prepared.image_descs[i];
			if (!same_images &&
			    (derived == nullptr || !same_slots || prepared.last_images[i] != snapshot.images[i])) {
				const auto cached =
				    m_texture_descs.find(MakeTextureDescKey(resource, snapshot.images[i]));
				derived = cached != m_texture_descs.end() ? &cached->second : nullptr;
			}
			if (derived != nullptr && derived->found.valid &&
			    ReuseTextureLookup(*derived, prepared.images[i])) {
				prepared.image_descs[i] = derived;
				BindImage(prepared.images[i].image_id, false);
				continue;
			}
		}
		auto binding = ResolveTexture(program.info.images[i], snapshot.images[i]);
		// The description whose lookups later draws reuse (see ResolveTexture()).
		prepared.image_descs[i] = binding.lookup;
		BindImage(binding.image_id, binding.desc.type == TextureCache::BindingType::Storage);
		binding.mip_views.swap(prepared.images[i].mip_views);
		binding.mip_views.clear();
		prepared.images[i] = std::move(binding);
	}
	if (!same_images) {
		prepared.last_images = snapshot.images;
	}
	prepared.table_images.clear();
	prepared.table_views.clear();
	prepared.table_sources.clear();
	prepared.table_roots.clear();
	prepared.table_versions.clear();
	prepared.table_slots.clear();
	prepared.table_state   = nullptr;
	prepared.tables_reused = false;
	prepared.table_visits.clear();
	prepared.table_moved_slots.clear();
	const auto stamp = ++m_table_prepare_stamp;
	TableBindingState* table_state = nullptr;
	if (std::ranges::any_of(program.info.images, [](const ShaderRecompiler::IR::ImageResource& image) {
		    return image.table_capacity != 0u;
	    })) {
		table_state           = &m_table_states[&program];
		table_state->compute  = program.stage == ShaderType::Compute;
		prepared.table_state  = table_state;
		prepared.tables_reused =
		    ReuseTableBindings(program, snapshot, *table_state, stamp, prepared);
		if (!prepared.tables_reused) {
			// Resolved slot by slot below; the state records what this binding finds.
			UnpinTableBindings(*table_state);
			table_state->valid         = false;
			table_state->generation    = m_table_generation;
			table_state->set_epoch     = m_context.GetTextureCache().ImageSetEpoch();
			table_state->backing_epoch = Libs::LibKernel::Memory::BackingEpoch();
			table_state->ranges.clear();
			table_state->values.clear();
			table_state->elements.clear();
		}
	}
	ForEachImageTable(program.info.images, [&](uint32_t root_index, auto /*kind*/) {
		if (prepared.tables_reused) {
			return;
		}
		const auto& root  = program.info.images.at(root_index);
		const auto& table = snapshot.image_tables.at(root.table);
		const ShaderRecompiler::IR::DescriptorValue null_value {.dword_count = 8u};
		auto& resolution = FindTableResolution(root);
		table_state->ranges.push_back(
		    {root, &resolution, root.table, root.table_capacity, table.slots_id});
		// Bind each distinct element once, however many slots name it.
		const auto add = [&](TableElementResolution&                      element,
		                     const ShaderRecompiler::IR::DescriptorValue& value) {
			if (element.prepared_stamp != stamp) {
				element.prepared_stamp = stamp;
				element.prepared_index = static_cast<uint32_t>(prepared.table_images.size());
				BindImage(element.texture.image_id, false);
				prepared.table_images.push_back(&element.texture);
				prepared.table_views.push_back(&element.view);
				prepared.table_sources.push_back(value);
				prepared.table_roots.push_back(root_index);
				prepared.table_versions.push_back(element.version_slot);
				table_state->elements.push_back(
				    {.resolution = &element, .table = &resolution, .source = value,
				     .root = root_index});
			}
			prepared.table_slots.push_back(element.prepared_index);
		};
		if (resolution.slots.size() < root.table_capacity) {
			resolution.slots.resize(root.table_capacity);
		}
		for (uint32_t slot = 0; slot < root.table_capacity; slot++) {
			const auto& value =
			    slot != 0u && slot < table.slots.size() ? table.slots[slot] : null_value;
			table_state->values.push_back(value);
			auto& memo = resolution.slots[slot];
			if (memo.element != nullptr && memo.dwords == value.dwords) {
				add(ResolveTableElement(resolution, value, stamp, memo.element), value);
			} else {
				auto& element = ResolveTableElement(resolution, value, stamp);
				memo          = {value.dwords, &element, element.version_slot};
				add(element, value);
			}
		}
	});
	// Samplers live as long as the sampler cache: the same S#s of the same program give the same
	// samplers.
	if (!same_program || prepared.samplers.size() != program.info.samplers.size() ||
	    prepared.last_samplers != snapshot.samplers) {
		prepared.samplers.clear();
		prepared.samplers.reserve(program.info.samplers.size());
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			prepared.samplers.push_back(NativeSampler(
			    program, i, snapshot.samplers[program.info.samplers[i].snapshot_index]));
		}
		prepared.last_samplers = snapshot.samplers;
	}
	prepared.last_program          = &program;
	prepared.last_descs_generation = m_texture_descs_generation;
	prepared.shader_data.reserve(program.bindings.ShaderDataDwords());
	for (const auto reg: program.info.user_data_registers) {
		prepared.shader_data.push_back(snapshot.user_data[reg - program.user_data_base]);
	}
	prepared.shader_data.resize(program.bindings.ShaderDataDwords());
	if (program.bindings.descriptor_counts[static_cast<size_t>(BindingKind::Gds)] != 0) {
		prepared.gds.buffer = m_context.GetBufferCache().GetGdsBuffer()->Handle();
	}
}

void RenderExecutor::FindBuffers(std::span<PreparedBindings* const> stages, bool find_buffers) {
	KYTY_PROFILER_FUNCTION();
	auto& cache = m_context.GetBufferCache();
	for (auto* stage: stages) {
		auto&       prepared = *stage;
		const auto& program  = prepared.runtime->module->info;
		const auto& snapshot = *prepared.runtime->resources;
		const auto& layout   = program.bindings;
		prepared.buffer_sources.resize(layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)]);
		if (layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)] == 0) {
			continue;
		}
		for (uint32_t resource = 0; resource < program.info.buffers.size(); ++resource) {
			const auto& metadata = program.info.buffers[resource];
			if (metadata.descriptor_index == UINT32_MAX) continue;
			auto& source = prepared.buffer_sources[metadata.descriptor_index];
			const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[resource]);
			const auto address = descriptor.Base48();
			auto size = descriptor.GetSize();
			if (size != 0 && descriptor.Stride() == 0 && descriptor.OutOfBounds() == 2 &&
			    !metadata.scalar) {
				// OOB_SELECT=2 only tests NumRecords != 0; a zero-stride fetch
				// still needs its full payload when NumRecords is smaller than it.
				size = std::max(size, uint64_t {metadata.max_byte_extent});
			}
			if (address == 0 || size == 0) {
				source = {};
				continue;
			}
			if (descriptor.NumRecords() == UINT32_MAX) {
				if (program.info.buffers[resource].written) {
					// Sentinel write ranges end before tables captured by any bound stage.
					for (const auto* reader: stages) {
						for (const auto [read_address, read_size]: reader->runtime->resources->specialization_reads) {
							if (read_size == 0) continue;
							if (read_address > address) {
								size = std::min(size, read_address - address);
							} else if (address - read_address < read_size) {
								EXIT("scalar resource reads overlap a shader buffer write\n");
							}
						}
					}
				}
				//  PPSA15246
				const auto& graphics = m_context.GetGraphics();
				const auto  limit =
				    uint64_t {graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange} -
				    (graphics.StorageMinAlignment() - 1);
				size = std::min({size, uint64_t {256} * 1024 * 1024, limit});
			}
			// Finite GPU descriptors can span sparse PRT holes; clamp only oversized/sentinel ranges.
			if (size >= (uint64_t {1} << 30u) || descriptor.NumRecords() == UINT32_MAX) {
				size = Libs::LibKernel::Memory::ClampRangeSize(address, size);
			}
			// Without device-address reads, ObtainBuffer() finds the buffer when it needs one
			// (draws stream most small buffers from CPU-dirty pages without it).
			source = {address, size, find_buffers ? cache.FindBuffer(address, size) : NULL_BUFFER_ID};
		}
	}
}

void RenderExecutor::RebindBuffers(PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	const auto& program  = prepared.runtime->module->info;
	const auto& snapshot = *prepared.runtime->resources;
	const auto& layout   = program.bindings;

	prepared.buffers.resize(layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)]);
	prepared.uniform_buffers.resize(
	    layout.descriptor_counts[static_cast<size_t>(BindingKind::UniformBuffers)]);
	std::fill(prepared.shader_data.begin() + layout.memory_offset_dword,
	          prepared.shader_data.end(), 0);
	auto pack_memory_offset = [&](uint32_t index, uint32_t offset) {
		const auto dword = layout.memory_offset_dword + index / 4u;
		const auto shift = (index % 4u) * 8u;
		prepared.shader_data[dword] |= offset << shift;
	};
	for (const auto& resource: program.info.buffers) {
		const auto index = resource.descriptor_index;
		if (index == UINT32_MAX) continue;
		uint32_t buffer_offset = 0;
		prepared.buffers[index] = NativeStorageBuffer(m_context, prepared.buffer_sources[index],
		                                              resource, buffer_offset);
		pack_memory_offset(index, buffer_offset);
		// Null bindings allow 0 bytes, not the shared null buffer's 16, so no store reaches it.
		prepared.shader_data[layout.MemorySizeDword() + index] =
		    static_cast<uint32_t>(prepared.buffer_sources[index].size + buffer_offset);
		if (resource.uniform_index != UINT32_MAX) {
			auto view = prepared.buffers[index];
			// A null descriptor (empty binding) keeps offset 0 and the whole size, as Vulkan requires.
			if (view.buffer != nullptr) {
				view.range = std::min<vk::DeviceSize>(
				    {view.range, ShaderRecompiler::IR::UniformBufferBytes,
				     m_context.GetGraphics().GetPhysicalDeviceProperties().limits.maxUniformBufferRange});
			}
			prepared.uniform_buffers[resource.uniform_index] = view;
		}
	}
	if (layout.descriptor_counts[static_cast<size_t>(BindingKind::FlattenedSrt)] != 0) {
		prepared.flattened_srt = NativeUpload(m_context, snapshot.flattened_srt);
	}
	if (layout.descriptor_counts[static_cast<size_t>(BindingKind::ShaderData)] != 0) {
		prepared.shader_data_buffer = NativeUpload(m_context, prepared.shader_data);
	}
}

void RenderExecutor::RebindImages(PreparedBindings& prepared) {
	KYTY_PROFILER_FUNCTION();
	const auto& program       = prepared.runtime->module->info;
	const auto& snapshot      = *prepared.runtime->resources;
	const Common::HackNotice::ScopedShader shader_scope(program.shader_hash);
	auto&       images        = prepared.images;
	auto&       texture_cache = m_context.GetTextureCache();
	// A null element (a null T# or a declined lookup) has no image to be stale.
	const auto stale = [&](const TextureBinding& binding) {
		if (!binding.image_id) {
			return false;
		}
		const auto* image = texture_cache.m_slot_images.try_get(binding.image_id);
		return image == nullptr || (!image->registered && !image->info.data.Empty()) ||
		       image->binding.needs_rebind;
	};
	// Table elements may share images with each other and with the direct images: decide which
	// are stale before any rebind clears an image's binding state.
	std::vector<bool> stale_tables(prepared.table_images.size());
	for (size_t k = 0; k < prepared.table_images.size(); k++) {
		stale_tables[k] = stale(*prepared.table_images[k]);
	}
	// Acquire each view before a later overlapping descriptor can replace its image.
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		if (!images[i].image_id) continue;
		auto& old_image = texture_cache.m_slot_images[images[i].image_id];
		if (!old_image.registered || old_image.binding.needs_rebind) {
			old_image.binding = {};
			images[i] = ResolveTexture(program.info.images[i], snapshot.images[i]);
			BindImage(images[i].image_id,
			          images[i].desc.type == TextureCache::BindingType::Storage);
		}
		auto& binding = images[i];
		binding.mip_views.clear();
		const auto& resource = program.info.images[i];
		if (resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::Dynamic) {
			EXIT_IF(resource.mip_count == 0u ||
			        resource.mip_count != binding.desc.view_info.level_count);
			binding.mip_views.reserve(resource.mip_count);
			for (uint32_t mip = 0; mip < resource.mip_count; mip++) {
				auto desc = binding.desc;
				desc.view_info.base_level += mip;
				desc.view_info.level_count = 1;
				// The shader selects the mip after applying the guest minimum LOD.
				desc.view_info.min_lod = 0;
				binding.mip_views.push_back(texture_cache.FindTexture(binding.image_id, desc));
			}
			binding.image_view = binding.mip_views.front();
		} else if (!binding.reused) {
			binding.image_view = texture_cache.FindTexture(binding.image_id, binding.desc);
		}
		auto&      image   = texture_cache.GetImage(binding.image_id);
		if (binding.lookup != nullptr && !binding.reused) {
			auto& found           = binding.lookup->found;
			found.id              = binding.image_id;
			found.desc            = binding.desc;
			found.view            = binding.image_view;
			found.image_set_epoch = texture_cache.ImageSetEpoch();
			found.state           = ImageLookupState::Of(binding.image_id, image);
			found.valid           = true;
		}
		// The binding no longer refers to the description, which may be cleared.
		binding.lookup = nullptr;
		binding.reused = false;
	}
	const auto tick = m_context.GetTextureCache().m_gc_tick;
	EXIT_IF(prepared.table_versions.size() != prepared.table_images.size());
	const auto element_changed = [&](size_t k) {
		if (const auto slot = prepared.table_versions[k]; slot < m_table_element_versions.size()) {
			m_table_element_versions[slot]++;
		}
	};
	for (size_t k = 0; k < prepared.table_images.size(); k++) {
		auto& texture = *prepared.table_images[k];
		auto& view    = *prepared.table_views[k];
		// A rediscovery above may have replaced (expanded, merged or recreated) the image this
		// element resolved to.
		if (stale_tables[k] || stale(texture)) {
			if (auto* old_image = texture_cache.m_slot_images.try_get(texture.image_id)) {
				old_image->binding = {};
			}
			texture = ResolveTableTexture(program.info.images.at(prepared.table_roots[k]),
			                              prepared.table_sources[k]);
			BindImage(texture.image_id, false);
			view.valid = false;
			element_changed(k);
		}
		if (!texture.image_id) {
			// Bound as a null descriptor: nothing to acquire.
			view.valid = false;
			continue;
		}
		// Most elements are textures nothing touched since their last acquisition: keep the
		// view and refresh the image's use, as the acquisition would. Guest-dirty images are
		// refreshed by the acquisition.
		if (auto* image = texture_cache.m_slot_images.try_get(texture.image_id);
		    image != nullptr && TableViewCurrent(texture_cache, *image, texture, view)) {
			image->tick_accessed_last = tick;
			texture_cache.TouchImage(*image);
			continue;
		}
		texture.image_view = texture_cache.FindTexture(texture.image_id, texture.desc);
		auto& image        = texture_cache.GetImage(texture.image_id);
		view.state = ImageLookupState::Of(texture.image_id, image);
		view.valid = TableViewKeepable(view.state, image);
		element_changed(k);
	}
	if (prepared.tables_reused) {
		RebindReusedTables(prepared);
	}
}

void RenderExecutor::PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
                                             std::span<RenderColorInfo> colors) {
	bool                                     dma_write = false;
	std::array<const ShaderStageRuntime*, 4> dma_stages {};
	uint32_t                                 dma_stage_count = 0;
	bool                                     uses_image_heap = false;
	// Operations with device-address reads need their buffers before the BDA synchronization.
	FindBuffers(stages, std::ranges::any_of(stages, [](const auto* stage) {
		            return stage->runtime->module->info.info.uses_dma;
	            }));
	for (auto* stage: stages) {
		if (stage->runtime->module->info.info.uses_dma) {
			m_context.CacheDmaBases(*stage->runtime);
			dma_write |= stage->runtime->module->info.has_address_writes;
			EXIT_IF(dma_stage_count == dma_stages.size());
			dma_stages[dma_stage_count++] = stage->runtime;
		}
		uses_image_heap |= stage->runtime->module->info.info.uses_image_heap;
	}
	if (dma_stage_count != 0) {
		// Only operations whose device-address reach is unknown upload all cached memory.
		const bool reach_synchronized =
		    m_context.SynchronizeDmaFootprint(std::span {dma_stages.data(), dma_stage_count});
		m_context.PrepareBda(dma_write, !reach_synchronized);
	}
	if (uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	for (auto* stage: stages) {
		RebindImages(*stage);
	}
	auto& cache = m_context.GetTextureCache();
	for (auto& target: colors) {
		EXIT_IF(!target.image_id);
		const auto old_image = cache.m_slot_images.try_get(target.image_id);
		if (old_image == nullptr || !old_image->registered ||
		    old_image->binding.needs_rebind) {
			if (old_image != nullptr) {
				old_image->binding = {};
			}
			// A draw that reuses the targets of the draw before updates them in the render
			// target memo, which no longer describes them.
			InvalidateRenderTargetMemo();
			target.desc.view_info.base_level = target.guest_mip_level;
			target.desc.view_info.base_layer = target.guest_array_layer;
			target.image_id = cache.FindImage(target.desc);
			BindRenderTarget(target.image_id);
		}
	}
	// Discovery can read back PS5 metadata and submit the scheduler. Reserve draw buffers only
	// after image identities are final; attachment layout transitions follow buffer alias copies.
	for (auto* stage: stages) {
		RebindBuffers(*stage);
	}
}

void RenderExecutor::CommitBindings(CommandBuffer&                     buffer,
                                    vk::PipelineBindPoint              pipeline_bind_point,
                                    const PipelineCache::Pipeline&     pipeline,
                                    std::span<PreparedBindings* const> prepared_bindings) {
	KYTY_PROFILER_FUNCTION();
	auto                           vk_buffer              = buffer.Handle();
	size_t                         image_descriptor_count = 0;
	ShaderRecompiler::IR::PushData push_data;
	bool                           has_push_data = false;
	for (const auto* prepared: prepared_bindings) {
		const auto& program = prepared->runtime->module->info;
		for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
			const auto index = std::countr_zero(mask);
			const auto count = program.bindings.descriptor_counts[index];
			if (index >= ShaderRecompiler::IR::FirstImageBinding &&
			    index <= static_cast<size_t>(BindingKind::Samplers)) {
				image_descriptor_count += count;
			}
		}
		EXIT_IF(!(pipeline.push_stages & NativeShaderStage(program.stage)));
	}
	if (pipeline.uses_image_heap) {
		m_context.GetImageHeap().Commit(buffer, pipeline_bind_point, pipeline.pipeline_layout);
	}
	m_descriptor_images.resize(image_descriptor_count);
	m_descriptor_writes.clear();
	m_descriptor_copies.clear();
	m_slot_set_writes.clear();
	size_t image_cursor = 0;
	std::array<vk::DescriptorBufferInfo, 2> address_buffers;
	// The table ranges of the stages, in the order of their slots in the table states.
	struct TableRange {
		TableBindingState* state;
		BindingKind        kind;
		uint32_t           binding;
		uint32_t           base;
		uint32_t           capacity;
		uint32_t           first_slot;
		bool               copied;
	};
	std::vector<TableRange> table_ranges;

	for (auto* prepared: prepared_bindings) {
		const auto& program     = prepared->runtime->module->info;
		auto&       descriptors = *prepared;
		if (descriptors.gds.buffer != nullptr) {
			buffer.EndRendering();
			const auto barrier       = MakeGdsDependency(descriptors.gds.buffer);
			const auto shader_stages = ShaderPipelineStages(NativeShaderStage(program.stage));
			vk_buffer.pipelineBarrier(
			    vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eTransfer |
			        vk::PipelineStageFlagBits::eAllGraphics |
			        vk::PipelineStageFlagBits::eComputeShader,
			    shader_stages, vk::DependencyFlags {}, 0, nullptr, 1, &barrier, 0, nullptr);
		}

		const auto transit = [&](TextureBinding& binding) {
			if (!binding.image_id) return;
			auto& image = m_context.GetTextureCache().GetImage(binding.image_id);
			const auto&                 view = binding.desc.view_info;
			const ImageSubresourceRange range {view.base_level, view.level_count, view.base_layer,
			                                   view.layer_count};
			const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
			if (image.binding.is_target) {
				const auto layout = image.binding.attachment_layout;
				EXIT_IF(layout == vk::ImageLayout::eUndefined);
				if (image.info.IsDepth()) {
					const auto host_view =
					    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
					EXIT_IF(storage || host_view == image.views.end());
					const auto aspect = host_view->info.aspect;
					if (aspect & ~DepthReadableAspects(layout)) {
						EXIT("sampling a writable depth/stencil attachment aspect\n");
					}
				}
				image.Transit(layout,
				              image.binding.attachment_access | vk::AccessFlagBits2::eShaderRead |
				                  (image.binding.shader_write ? vk::AccessFlagBits2::eShaderWrite
				                                              : vk::AccessFlags2 {}),
				              {}, vk_buffer);
			} else if (image.binding.force_general && !image.info.IsDepth()) {
				const vk::AccessFlags2 storage_access = image.binding.shader_write
				                                            ? vk::AccessFlagBits2::eShaderWrite
				                                            : vk::AccessFlags2 {};
				image.Transit(vk::ImageLayout::eGeneral,
				              vk::AccessFlagBits2::eShaderRead | storage_access, {}, vk_buffer);
			} else if (storage) {
				image.Transit(vk::ImageLayout::eGeneral,
				              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
				              range, vk_buffer);
			} else {
				image.Transit(image.ReadLayout(), vk::AccessFlagBits2::eShaderRead, range, vk_buffer);
			}
			binding.layout = image.backing.state.layout;
		};
		for (auto& binding: descriptors.images) {
			transit(binding);
		}
		for (auto* binding: descriptors.table_images) {
			transit(*binding);
		}
		auto* table_state = static_cast<TableBindingState*>(descriptors.table_state);
		if (descriptors.tables_reused) {
			for (const auto index: descriptors.table_visits) {
				transit(table_state->elements[index].resolution->texture);
			}
		}

		// GTA V's ray tracing binds tables of 16 384 slots, about 100 000 descriptors per frame,
		// which barely change: writing them costs the driver ~5 ns each, copying them from the
		// state's slot set ~1 ns.
		const bool copy_tables = !pipeline.uses_push_descriptors && table_state != nullptr &&
		                         descriptors.tables_reused && table_state->slot_set_current;
		const auto stage_ranges = table_ranges.size();
		if (table_state != nullptr) {
			uint32_t first_slot = 0;
			ForEachImageTable(program.info.images, [&](uint32_t root_index, BindingKind kind) {
				const auto& root = program.info.images[root_index];
				const auto  binding = ShaderRecompiler::IR::NativeBinding(program.stage, kind);
				table_ranges.push_back({.state      = table_state,
				                        .kind       = kind,
				                        .binding    = binding,
				                        .base       = root.table_descriptor_base,
				                        .capacity   = root.table_capacity,
				                        .first_slot = first_slot,
				                        .copied     = copy_tables});
				first_slot += root.table_capacity;
			});
			if (!copy_tables) {
				// Until the slot set holds this binding's descriptors (see below).
				table_state->slot_set_current = false;
			}
		}
		const std::span<const TableRange> stage_tables =
		    std::span {table_ranges}.subspan(stage_ranges);

		std::array<size_t, ShaderRecompiler::IR::ImageBindingCount> image_starts {};
		for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
			const auto index = std::countr_zero(mask);
			const auto kind = static_cast<BindingKind>(index);
			vk::WriteDescriptorSet write {};
			write.dstBinding = ShaderRecompiler::IR::NativeBinding(program.stage, kind);
			write.descriptorType = NativeDescriptorType(kind);
			write.descriptorCount = program.bindings.descriptor_counts[index];
			const auto image_start = image_cursor;
			if (ShaderRecompiler::IR::ImageBindingResourceClass(kind) !=
			    ShaderRecompiler::IR::ImageResourceClass::None) {
				image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] = image_start;
				image_cursor += write.descriptorCount;
				std::array<const TableRange*, 16> tables {};
				size_t                            table_count = 0;
				for (const auto& table: stage_tables) {
					if (copy_tables && table.kind == kind) {
						EXIT_IF(table_count == tables.size());
						tables[table_count++] = &table;
					}
				}
				if (table_count != 0) {
					// The slots around the tables are written, the tables copied.
					const auto end = tables.begin() + static_cast<std::ptrdiff_t>(table_count);
					std::sort(tables.begin(), end,
					          [](const auto* a, const auto* b) { return a->base < b->base; });
					uint32_t next = 0;
					for (size_t t = 0; t < table_count; t++) {
						const auto& table = *tables[t];
						EXIT_IF(table.base < next ||
						        table.base + table.capacity > write.descriptorCount);
						if (table.base > next) {
							auto gap            = write;
							gap.dstArrayElement = next;
							gap.descriptorCount = table.base - next;
							gap.pImageInfo      = m_descriptor_images.data() + image_start + next;
							m_descriptor_writes.push_back(gap);
						}
						vk::CopyDescriptorSet copy {};
						copy.srcSet          = table_state->slot_set;
						copy.srcBinding      = table.binding;
						copy.srcArrayElement = table.base;
						copy.dstBinding      = table.binding;
						copy.dstArrayElement = table.base;
						copy.descriptorCount = table.capacity;
						m_descriptor_copies.push_back(copy);
						next = table.base + table.capacity;
					}
					if (next < write.descriptorCount) {
						write.dstArrayElement = next;
						write.descriptorCount -= next;
						write.pImageInfo      = m_descriptor_images.data() + image_start + next;
						m_descriptor_writes.push_back(write);
					}
					continue;
				}
			} else {
				switch (kind) {
					case BindingKind::Buffers:
						write.pBufferInfo = descriptors.buffers.data();
						break;
					case BindingKind::UniformBuffers:
						EXIT_IF(descriptors.uniform_buffers.size() != write.descriptorCount);
						for (const auto& view: descriptors.uniform_buffers) {
							EXIT_IF(view.range == 0);
						}
						write.pBufferInfo = descriptors.uniform_buffers.data();
						break;
					case BindingKind::BdaPagetable:
					case BindingKind::FaultBuffer: {
						auto& cache = m_context.GetBufferCache();
						const auto* bda_buffer = kind == BindingKind::BdaPagetable
						                             ? cache.GetBdaPageTableBuffer()
						                             : cache.GetFaultBuffer();
						auto& view = address_buffers[kind == BindingKind::FaultBuffer];
						view = {bda_buffer->Handle(), 0, bda_buffer->Size()};
						write.pBufferInfo = &view;
						break;
					}
					case BindingKind::FlattenedSrt:
					case BindingKind::ShaderData:
					case BindingKind::SharedMemory:
					case BindingKind::Gds: {
						const vk::DescriptorBufferInfo* view = &descriptors.gds;
						if (kind == BindingKind::FlattenedSrt) {
							view = &descriptors.flattened_srt;
						} else if (kind == BindingKind::ShaderData) {
							view = &descriptors.shader_data_buffer;
						} else if (kind == BindingKind::SharedMemory) {
							view = &descriptors.shared_memory;
						}
						EXIT_IF(view->buffer == nullptr);
						write.pBufferInfo = view;
						break;
					}
					case BindingKind::Samplers:
						for (const auto sampler: descriptors.samplers) {
							m_descriptor_images[image_cursor++] = {sampler, nullptr, vk::ImageLayout::eUndefined};
						}
						break;
					case BindingKind::Count: EXIT("invalid descriptor binding kind");
				}
			}
			if (image_cursor != image_start) {
				write.pImageInfo = m_descriptor_images.data() + image_start;
			}
			m_descriptor_writes.push_back(write);
		}
		for (uint32_t i = 0; i < program.info.images.size(); ++i) {
			const auto& resource = program.info.images[i];
			const auto& texture = descriptors.images[i];
			if (resource.heap) continue;
			const auto kind = *ShaderRecompiler::IR::DescriptorBindingForImage(resource);
			const auto start = image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] +
			                   resource.descriptor_index;
			for (uint32_t mip = 0; mip < resource.mip_count; ++mip) {
				m_descriptor_images[start + mip] = MakeImageInfo(texture, mip);
			}
		}
		size_t table_element = 0;
		m_table_image_infos.clear();
		if (descriptors.tables_reused) {
			for (const auto index: descriptors.table_visits) {
				table_state->infos[index] =
				    MakeImageInfo(table_state->elements[index].resolution->texture, 0u);
			}
			const auto write_slot = [&](uint32_t slot, const vk::DescriptorImageInfo& info) {
				auto&      slot_info = table_state->slot_infos[slot];
				const bool changed   = slot_info.sampler != info.sampler ||
				                     slot_info.imageView != info.imageView ||
				                     slot_info.imageLayout != info.imageLayout;
				if (copy_tables && changed) {
					const auto table =
					    std::ranges::find_if(stage_tables, [&](const TableRange& range) {
						    return slot >= range.first_slot && slot - range.first_slot < range.capacity;
					    });
					EXIT_IF(table == stage_tables.end());
					vk::WriteDescriptorSet write {};
					write.dstSet          = table_state->slot_set;
					write.dstBinding      = table->binding;
					write.dstArrayElement = table->base + (slot - table->first_slot);
					write.descriptorCount = 1;
					write.descriptorType  = NativeDescriptorType(table->kind);
					write.pImageInfo      = &slot_info;
					m_slot_set_writes.push_back(write);
				}
				slot_info = info;
			};
			// Only the slots of the visited elements change, and the slots that name another
			// element of the state now.
			for (const auto index: descriptors.table_visits) {
				const auto& info = table_state->infos[index];
				for (auto k = table_state->element_slot_begin[index];
				     k < table_state->element_slot_begin[index + 1]; k++) {
					write_slot(table_state->element_slots[k], info);
				}
			}
			for (const auto slot: descriptors.table_moved_slots) {
				write_slot(slot, table_state->infos[table_state->slots[slot]]);
			}
		}
		ForEachImageTable(program.info.images, [&](uint32_t root_index, BindingKind kind) {
			const auto& root  = program.info.images[root_index];
			const auto  start = image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] +
			                   root.table_descriptor_base;
			if (copy_tables) {
				table_element += root.table_capacity;
				return;
			}
			if (descriptors.tables_reused) {
				EXIT_IF(table_element + root.table_capacity > table_state->slot_infos.size());
				std::copy_n(table_state->slot_infos.begin() +
				                static_cast<std::ptrdiff_t>(table_element),
				            root.table_capacity,
				            m_descriptor_images.begin() + static_cast<std::ptrdiff_t>(start));
				table_element += root.table_capacity;
				return;
			}
			if (m_table_image_infos.empty()) {
				m_table_image_infos.reserve(descriptors.table_images.size());
				for (const auto* texture: descriptors.table_images) {
					m_table_image_infos.push_back(MakeImageInfo(*texture, 0u));
				}
			}
			EXIT_IF(table_element + root.table_capacity > descriptors.table_slots.size());
			for (uint32_t slot = 0; slot < root.table_capacity; slot++) {
				m_descriptor_images[start + slot] =
				    m_table_image_infos[descriptors.table_slots[table_element++]];
			}
		});
		if (table_state != nullptr && !descriptors.tables_reused) {
			FinishTableBindings(*table_state, descriptors);
		}

		if (program.bindings.UsesPushData()) {
			std::ranges::copy(prepared->shader_data,
			                  push_data.dwords.begin() + program.bindings.push_data_start_dword);
			has_push_data = true;
		}
	}

	if (has_push_data) {
		vk_buffer.pushConstants(pipeline.pipeline_layout, pipeline.push_stages, 0,
		                        sizeof(push_data), push_data.dwords.data());
	}

	if (!m_descriptor_writes.empty()) {
		if (pipeline.uses_push_descriptors) {
			vk_buffer.pushDescriptorSetKHR(pipeline_bind_point, pipeline.pipeline_layout, 0,
			                               static_cast<uint32_t>(m_descriptor_writes.size()),
			                               m_descriptor_writes.data());
		} else {
			auto&      heap   = m_context.GetDescriptorHeap();
			const auto device = m_context.GetGraphics().device;
			const auto set    = heap.Commit(pipeline.descriptor_set_layout);
			for (auto& write: m_descriptor_writes) {
				write.dstSet = set;
			}
			for (auto& copy: m_descriptor_copies) {
				copy.dstSet = set;
			}
			// The slots written into the slot sets go first: the copies read them.
			m_descriptor_writes.insert(m_descriptor_writes.end(), m_slot_set_writes.begin(),
			                           m_slot_set_writes.end());
			device.updateDescriptorSets(static_cast<uint32_t>(m_descriptor_writes.size()),
			                            m_descriptor_writes.data(),
			                            static_cast<uint32_t>(m_descriptor_copies.size()),
			                            m_descriptor_copies.data());
			// The tables that were written keep their descriptors in their states' slot sets for
			// the next bindings, while the states stay valid.
			m_descriptor_copies.clear();
			for (const auto& table: table_ranges) {
				auto& state = *table.state;
				if (table.copied || !state.valid) {
					continue;
				}
				if (!state.slot_set) {
					state.slot_set = heap.CommitPersistent(pipeline.descriptor_set_layout);
				}
				vk::CopyDescriptorSet copy {};
				copy.srcSet          = set;
				copy.srcBinding      = table.binding;
				copy.srcArrayElement = table.base;
				copy.dstSet          = state.slot_set;
				copy.dstBinding      = table.binding;
				copy.dstArrayElement = table.base;
				copy.descriptorCount = table.capacity;
				m_descriptor_copies.push_back(copy);
				state.slot_set_current = true;
			}
			if (!m_descriptor_copies.empty()) {
				device.updateDescriptorSets(0, nullptr,
				                            static_cast<uint32_t>(m_descriptor_copies.size()),
				                            m_descriptor_copies.data());
			}
			vk_buffer.bindDescriptorSets(pipeline_bind_point, pipeline.pipeline_layout, 0, 1, &set,
			                             0, nullptr);
		}
	}
}

} // namespace Libs::Graphics
