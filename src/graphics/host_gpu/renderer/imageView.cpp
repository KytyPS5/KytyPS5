#include "graphics/host_gpu/renderer/imageView.h"

#include "common/assert.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/textureCache.h"

#include <mutex>

namespace Libs::Graphics {

namespace {

void CreateView(VulkanImage& image, int view_index,
                vk::ImageViewType view_type, vk::ImageAspectFlags aspect_mask,
                vk::ComponentMapping components, uint32_t base_array_layer, uint32_t base_mip_level,
                uint32_t layer_count, uint32_t level_count,
                vk::Format          view_format = vk::Format::eUndefined,
	             vk::ImageUsageFlags view_usage  = {}) {
	auto& graphics = GetRenderContext().GetGraphics();
	if (view_index < 0 || view_index >= VulkanImage::VIEW_MAX ||
	    image.image_view[view_index] != nullptr) {
		EXIT("invalid image-view creation target: image=%p index=%d current_view=%d\n",
		     static_cast<const void*>(&image), view_index,
		     view_index >= 0 && view_index < VulkanImage::VIEW_MAX &&
		         image.image_view[view_index] != nullptr);
	}

	vk::ImageViewUsageCreateInfo usage_info {};
	usage_info.sType = vk::StructureType::eImageViewUsageCreateInfo;
	usage_info.usage = view_usage;

	vk::ImageViewCreateInfo create_info {};
	create_info.sType      = vk::StructureType::eImageViewCreateInfo;
	create_info.pNext      = view_usage ? &usage_info : nullptr;
	create_info.image      = image.image;
	create_info.viewType   = view_type;
	create_info.format     = view_format != vk::Format::eUndefined ? view_format : image.format;
	create_info.components = components;
	create_info.subresourceRange.aspectMask     = aspect_mask;
	create_info.subresourceRange.baseArrayLayer = base_array_layer;
	create_info.subresourceRange.baseMipLevel   = base_mip_level;
	create_info.subresourceRange.layerCount     = layer_count;
	create_info.subresourceRange.levelCount     = level_count;

	const auto result =
	    graphics.device.createImageView(&create_info, nullptr, &image.image_view[view_index]);
	if (result != vk::Result::eSuccess || image.image_view[view_index] == nullptr) {
		EXIT("failed to create image view: result=%d image_format=%d view_format=%d index=%d\n",
		     static_cast<int>(result), static_cast<int>(image.format),
		     static_cast<int>(create_info.format), view_index);
	}
}

void CreateRenderTargetView(VulkanImage& image, int index,
                            vk::ComponentSwizzle r, vk::ComponentSwizzle g, vk::ComponentSwizzle b,
                            vk::ComponentSwizzle a, vk::ImageViewType type = vk::ImageViewType::e2D,
                            vk::Format          view_format = vk::Format::eUndefined,
                            vk::ImageUsageFlags view_usage = {}, uint32_t level_count = 0) {
	const auto layer_count = type == vk::ImageViewType::e2DArray ? image.layers : 1u;
	CreateView(image, index, type, vk::ImageAspectFlagBits::eColor, {r, g, b, a}, 0, 0,
	           layer_count, level_count == 0 ? image.mip_levels : level_count, view_format,
	           view_usage);
}

} // namespace

namespace ImageViewOps {

vk::ImageAspectFlags DepthAspectMask(vk::Format format) {
	switch (format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD32Sfloat: return vk::ImageAspectFlagBits::eDepth;
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32SfloatS8Uint:
			return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
		default: EXIT("unsupported depth/stencil image format: %d\n", static_cast<int>(format));
	}
}

bool FormatSupportsStorage(vk::Format format) {
	auto& graphics = GetRenderContext().GetGraphics();
	const auto properties = graphics.GetFormatProperties(format);
	return static_cast<bool>(properties.optimalTilingFeatures &
	                         vk::FormatFeatureFlagBits::eStorageImage);
}

void CreateRenderTargetViews(RenderTextureVulkanImage& image) {
	CreateRenderTargetView(image, VulkanImage::VIEW_DEFAULT,
	                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
	                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity);
	if (image.layers > 1) {
		CreateRenderTargetView(image, VulkanImage::VIEW_DEFAULT_ARRAY,
		                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
		                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
		                       vk::ImageViewType::e2DArray);
	}
	if (image.samples == 1 && FormatSupportsStorage(image.format)) {
		CreateRenderTargetView(image, VulkanImage::VIEW_STORAGE,
		                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
		                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
		                       vk::ImageViewType::e2D, vk::Format::eUndefined,
		                       vk::ImageUsageFlags {}, 1);
		if (image.layers > 1) {
			CreateRenderTargetView(image, VulkanImage::VIEW_STORAGE_ARRAY,
			                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
			                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
			                       vk::ImageViewType::e2DArray, vk::Format::eUndefined,
			                       vk::ImageUsageFlags {}, 1);
		}
	}
}

void CreateDepthViews(DepthStencilVulkanImage& image) {
	CreateView(image, VulkanImage::VIEW_DEFAULT, vk::ImageViewType::e2D,
	           DepthAspectMask(image.format),
	           {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
	            vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity},
	           0, 0, 1, 1);
}

void CreateVideoOutViews(VideoOutVulkanImage& image) {
	CreateRenderTargetView(image, VulkanImage::VIEW_DEFAULT,
	                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
	                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity);
	if ((image.format == vk::Format::eR8G8B8A8Srgb || image.format == vk::Format::eB8G8R8A8Srgb) &&
	    FormatSupportsStorage(vk::Format::eR8G8B8A8Uint)) {
		CreateRenderTargetView(image, VulkanImage::VIEW_STORAGE,
		                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
		                       vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
		                       vk::ImageViewType::e2D, vk::Format::eR8G8B8A8Uint,
		                       vk::ImageUsageFlagBits::eStorage, 1);
	}
}

void DestroyViews(VulkanImage& image) {
	auto& graphics = GetRenderContext().GetGraphics();
	for (auto& cached: image.view_cache.views) {
		if (cached.view != nullptr) {
			graphics.device.destroyImageView(cached.view, nullptr);
			cached.view = nullptr;
		}
	}
	image.view_cache.views.clear();
	for (auto& view: image.image_view) {
		if (view != nullptr) {
			graphics.device.destroyImageView(view, nullptr);
			view = nullptr;
		}
	}
}

} // namespace ImageViewOps

vk::ImageView TextureCache::GetRenderTargetAttachmentView(RenderTextureVulkanImage& image,
                                                          vk::Format format, uint32_t level,
                                                          uint32_t base_layer,
                                                          uint32_t layer_count) {
	if (image.format == vk::Format::eUndefined || level >= image.mip_levels || level >= 16 ||
	    layer_count == 0 || base_layer >= image.layers || layer_count > image.layers - base_layer) {
		EXIT("TextureCache: invalid render-target attachment view, image=%p format=%d"
		     " level=%u image_levels=%u base_layer=%u layer_count=%u image_layers=%u\n",
		     static_cast<const void*>(&image), static_cast<int>(format), level, image.mip_levels,
		     base_layer, layer_count, image.layers);
	}
	if (format != image.format && !IsRgba8SrgbReinterpretation(image.format, format)) {
		EXIT("TextureCache: incompatible render-target attachment view, image_format=%d"
		     " view_format=%d level=%u\n",
		     static_cast<int>(image.format), static_cast<int>(format), level);
	}

	return GetImageView(
	    image, {format, layer_count == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray,
	            vk::ImageAspectFlagBits::eColor, level, 1, base_layer, layer_count,
	            DstSel(4, 5, 6, 7), vk::ImageUsageFlagBits::eColorAttachment});
}

vk::ImageView TextureCache::GetDepthTargetAttachmentView(DepthStencilVulkanImage& image,
                                                         uint32_t                 base_layer,
                                                         uint32_t                 layer_count) {
	if (layer_count == 0 || base_layer >= image.layers || layer_count > image.layers - base_layer) {
		EXIT("TextureCache: invalid depth-target attachment view, image=%p base_layer=%u "
		     "layer_count=%u image_layers=%u\n",
		     static_cast<const void*>(&image), base_layer, layer_count, image.layers);
	}
	return GetImageView(image,
	                    {image.format,
	                     layer_count == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray,
	                     ImageViewOps::DepthAspectMask(image.format), 0, 1, base_layer, layer_count,
	                     DstSel(4, 5, 6, 7), vk::ImageUsageFlagBits::eDepthStencilAttachment});
}

vk::ImageView TextureCache::GetImageView(VulkanImage& image, const ImageViewInfo& info) {
	const bool supported_type  = info.type == vk::ImageViewType::e2D ||
	                             info.type == vk::ImageViewType::e2DArray ||
	                             info.type == vk::ImageViewType::e3D;
	const bool supported_usage = info.usage == vk::ImageUsageFlagBits::eSampled ||
	                             info.usage == vk::ImageUsageFlagBits::eStorage ||
	                             info.usage == vk::ImageUsageFlagBits::eColorAttachment ||
	                             info.usage == vk::ImageUsageFlagBits::eDepthStencilAttachment;
	const bool valid_shape =
	    (info.type == vk::ImageViewType::e2D && info.layer_count == 1) ||
	    info.type == vk::ImageViewType::e2DArray ||
	    (info.type == vk::ImageViewType::e3D && info.base_layer == 0 && info.layer_count == 1);
	if (info.format == vk::Format::eUndefined || !info.aspect || info.level_count == 0 ||
	    info.base_level >= (image.mip_levels) ||
	    info.level_count > image.mip_levels - info.base_level || info.layer_count == 0 ||
	    info.base_layer >= image.layers || info.layer_count > image.layers - info.base_layer ||
	    !supported_type || !valid_shape || !supported_usage) {
		EXIT("TextureCache: invalid dynamic image view, image=%p format=%d aspect=0x%x"
		     " swizzle=0x%03x mip=%u+%u layer=%u+%u type=%d usage=0x%x"
		     " image_levels=%u image_layers=%u\n",
		     static_cast<const void*>(&image), static_cast<int>(info.format),
		     static_cast<vk::ImageAspectFlags::MaskType>(info.aspect), info.swizzle,
		     info.base_level, info.level_count, info.base_layer, info.layer_count,
		     static_cast<int>(info.type), static_cast<vk::ImageUsageFlags::MaskType>(info.usage),
		     image.mip_levels, image.layers);
	}

	auto&           cache = image.view_cache;
	std::lock_guard lock(cache.mutex);
	for (const auto& cached: cache.views) {
		if (cached.info == info) {
			return cached.view;
		}
	}

	vk::ImageViewUsageCreateInfo usage {};
	usage.sType = vk::StructureType::eImageViewUsageCreateInfo;
	usage.usage = info.usage;
	vk::ImageViewCreateInfo create {};
	create.sType                           = vk::StructureType::eImageViewCreateInfo;
	create.pNext                           = &usage;
	create.image                           = image.image;
	create.viewType                        = info.type;
	create.format                          = info.format;
	create.components                      = info.usage == vk::ImageUsageFlagBits::eSampled
	                                             ? TextureGetComponentMapping(info.swizzle)
	                                             : vk::ComponentMapping {};
	create.subresourceRange.aspectMask     = info.aspect;
	create.subresourceRange.baseMipLevel   = info.base_level;
	create.subresourceRange.levelCount     = info.level_count;
	create.subresourceRange.baseArrayLayer = info.base_layer;
	create.subresourceRange.layerCount     = info.layer_count;
	vk::ImageView view                     = nullptr;
	const auto    result = m_graphics.device.createImageView(&create, nullptr, &view);
	if (result != vk::Result::eSuccess || view == nullptr) {
		EXIT("TextureCache: failed to create dynamic image view, result=%d format=%d"
		     " aspect=0x%x swizzle=0x%03x mip=%u+%u layer=%u+%u type=%d usage=0x%x\n",
		     static_cast<int>(result), static_cast<int>(info.format),
		     static_cast<vk::ImageAspectFlags::MaskType>(info.aspect), info.swizzle,
		     info.base_level, info.level_count, info.base_layer, info.layer_count,
		     static_cast<int>(info.type), static_cast<vk::ImageUsageFlags::MaskType>(info.usage));
	}
	cache.views.push_back({info, view});
	return view;
}

vk::ImageView TextureCache::GetDepthTargetSampledView(DepthStencilVulkanImage& image,
                                                      vk::Format view_format, uint32_t swizzle,
                                                      uint32_t base_level, uint32_t level_count,
                                                      vk::ImageViewType type, uint32_t base_layer,
                                                      uint32_t layer_count) {
	if (view_format == vk::Format::eUndefined ||
	    !IsSupportedSampledDepthView(image.format, view_format, swizzle)) {
		EXIT("TextureCache: invalid sampled depth-target view, image=%p image_format=%d"
		     " view_format=%d swizzle=0x%03x mip=%u+%u layer=%u+%u type=%d"
		     " image_levels=%u image_layers=%u\n",
		     static_cast<const void*>(&image), static_cast<int>(image.format),
		     static_cast<int>(view_format), swizzle, base_level, level_count, base_layer,
		     layer_count, static_cast<int>(type), image.mip_levels, image.layers);
	}
	const auto aspect = SampledDepthTargetAspect(image.format, view_format);
	if (!aspect) {
		EXIT("TextureCache: sampled depth-target view has no compatible aspect\n");
	}
	return GetImageView(image, {image.format, type, aspect, base_level, level_count, base_layer,
	                            layer_count, swizzle});
}

vk::ImageView TextureCache::GetSampledColorView(VulkanImage& image, vk::Format view_format,
                                                uint32_t swizzle, uint32_t base_level,
                                                uint32_t level_count, vk::ImageViewType type,
                                                uint32_t base_layer, uint32_t layer_count) {
	if (view_format == vk::Format::eUndefined || base_level >= 16 ||
	    (type != vk::ImageViewType::e2D && type != vk::ImageViewType::e2DArray) ||
	    !IsSupportedSampledColorView(image.format, view_format, swizzle)) {
		EXIT("TextureCache: invalid sampled color view, image=%p swizzle=0x%03x"
		     " view_format=%d mip=%u+%u layer=%u+%u type=%d image_levels=%u image_layers=%u\n",
		     static_cast<const void*>(&image), swizzle, static_cast<int>(view_format), base_level,
		     level_count, base_layer, layer_count, static_cast<int>(type), image.mip_levels,
		     image.layers);
	}
	const auto precreated_view = type == vk::ImageViewType::e2DArray
	                                 ? VulkanImage::VIEW_DEFAULT_ARRAY
	                                 : VulkanImage::VIEW_DEFAULT;
	const bool full_view = base_level == 0 && level_count == image.mip_levels && base_layer == 0 &&
	                       layer_count == (type == vk::ImageViewType::e2DArray ? image.layers : 1u);
	if (view_format == image.format && swizzle == DstSel(4, 5, 6, 7) && full_view &&
	    image.image_view[precreated_view] != nullptr) {
		return image.image_view[precreated_view];
	}
	return GetImageView(image, {view_format, type, vk::ImageAspectFlagBits::eColor, base_level,
	                            level_count, base_layer, layer_count, swizzle});
}

vk::ImageView TextureCache::GetRenderTargetStorageView(RenderTextureVulkanImage& image,
                                                       vk::Format view_format, uint32_t base_level,
                                                       uint32_t level_count, vk::ImageViewType type,
                                                       uint32_t base_layer, uint32_t layer_count) {
	if (view_format == vk::Format::eUndefined ||
	    (type != vk::ImageViewType::e2D && type != vk::ImageViewType::e2DArray)) {
		EXIT("TextureCache: invalid render-target storage view, image=%p view_format=%d"
		     " mip=%u+%u layer=%u+%u type=%d image_levels=%u image_layers=%u\n",
		     static_cast<const void*>(&image), static_cast<int>(view_format), base_level,
		     level_count, base_layer, layer_count, static_cast<int>(type), image.mip_levels,
		     image.layers);
	}
	const bool exact      = view_format == image.format;
	const bool compatible = view_format == BgraSrgbStorageViewFormat(image.format);
	if (!exact && !compatible) {
		EXIT("TextureCache: incompatible render-target storage view, image_format=%d"
		     " view_format=%d base=%u count=%u\n",
		     static_cast<int>(image.format), static_cast<int>(view_format), base_level,
		     level_count);
	}
	if (exact) {
		const auto index = type == vk::ImageViewType::e2DArray ? VulkanImage::VIEW_STORAGE_ARRAY
		                                                       : VulkanImage::VIEW_STORAGE;
		const bool full_view =
		    base_level == 0 && level_count == 1 && base_layer == 0 &&
		    layer_count == (type == vk::ImageViewType::e2DArray ? image.layers : 1u);
		if (full_view && image.image_view[index] != nullptr) {
			return image.image_view[index];
		}
	}

	if (compatible && !ImageViewOps::FormatSupportsStorage(view_format)) {
		EXIT("TextureCache: compatible render-target storage format lacks storage support,"
		     " image_format=%d view_format=%d base=%u count=%u\n",
		     static_cast<int>(image.format), static_cast<int>(view_format), base_level,
		     level_count);
	}
	return GetImageView(image, {view_format, type, vk::ImageAspectFlagBits::eColor, base_level,
	                            level_count, base_layer, layer_count, DstSel(4, 5, 6, 7),
	                            vk::ImageUsageFlagBits::eStorage});
}

vk::ImageView TextureCache::GetStorageTextureSampledView(StorageTextureVulkanImage& image,
                                                         const ImageInfo&           info) {
	const auto shape = SelectStorageSampledViewShape(info.type, info.depth, image.layers);
	if (image.image == nullptr || shape == StorageSampledViewShape::Unsupported ||
	    info.base_array != 0 || info.levels != image.mip_levels || info.base_level >= info.levels ||
	    info.view_levels == 0 || info.base_level + info.view_levels > info.levels) {
		EXIT("TextureCache: invalid sampled view of storage texture, image=%p type=%u depth=%u"
		     " base=%u levels=%u view_levels=%u image_levels=%u base_array=%u\n",
		     static_cast<const void*>(&image), info.type, info.depth, info.base_level, info.levels,
		     info.view_levels, image.mip_levels, info.base_array);
	}
	const auto view_format = TextureGetFormat(info.format);
	if (view_format != image.format && !IsRgba8SrgbReinterpretation(image.format, view_format) &&
	    !IsR32UintFloatReinterpretation(image.format, view_format)) {
		EXIT("TextureCache: incompatible sampled view of storage texture, image_format=%d"
		     " view_format=%d swizzle=0x%03x\n",
		     static_cast<int>(image.format), static_cast<int>(view_format), info.swizzle);
	}

	vk::ImageViewType type = static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM);
	switch (shape) {
		case StorageSampledViewShape::Image2D: type = vk::ImageViewType::e2D; break;
		case StorageSampledViewShape::Image2DArray: type = vk::ImageViewType::e2DArray; break;
		case StorageSampledViewShape::Image3D: type = vk::ImageViewType::e3D; break;
		case StorageSampledViewShape::Unsupported:
			EXIT("TextureCache: unsupported sampled storage-image view shape\n");
	}
	const auto layer_count = shape == StorageSampledViewShape::Image2DArray ? info.depth : 1u;
	return GetImageView(image, {view_format, type, vk::ImageAspectFlagBits::eColor, info.base_level,
	                            info.view_levels, 0, layer_count, info.swizzle});
}

vk::ImageView TextureCache::GetStorageTextureStorageView(StorageTextureVulkanImage& image,
                                                         uint32_t                   base_level) {
	if (image.image == nullptr || base_level >= (image.mip_levels)) {
		EXIT("TextureCache: invalid storage-texture mip view, image=%p level=%u levels=%u\n",
		     static_cast<const void*>(&image), base_level, image.mip_levels);
	}
	if (base_level == 0) {
		return image.image_view[VulkanImage::VIEW_DEFAULT];
	}
	return GetImageView(image, {image.format, vk::ImageViewType::e2D,
	                            vk::ImageAspectFlagBits::eColor, base_level, 1, 0, 1,
	                            DstSel(4, 5, 6, 7), vk::ImageUsageFlagBits::eStorage});
}

} // namespace Libs::Graphics
