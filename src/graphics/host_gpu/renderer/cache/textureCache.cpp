#include "graphics/host_gpu/renderer/cache/textureCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/refusalReport.h"
#include "graphics/host_gpu/renderer/render.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

constexpr uint64_t NumFramesBeforeRemoval = 32;
// Largest GPU-written tiled image (in texels) that is written back to guest memory on submit.
constexpr uint64_t SmallTiledReadbackTexels = 64 * 64;

// Pool surfaces are never unmapped, so age is the only sign an image stopped owning its bytes.
[[nodiscard]] bool ImageAbandoned(const Image& image, uint64_t current_frame) {
	return current_frame - std::min(current_frame, image.frame_accessed_last) >
	       NumFramesBeforeRemoval;
}

// A live tiled image has never written its pixels to guest memory.
[[nodiscard]] bool ImageOwnsGuestBytes(const Image& image) {
	return !image.info.IsTiled();
}

// An emergency reclaim runs mid-draw, so it is capped.
constexpr size_t MaxEmergencyReclaimImages = 256;

// Unlike SafeToDownload, a merely suspected CPU write does not make a tiled image reproducible.
[[nodiscard]] bool OverlapVictimPixelsAreUnrecoverable(const Image& image) {
	return image.IsGpuModified() && !image.IsBufferModified() && !image.IsDefinitelyCpuDirty() &&
	       !ImageOwnsGuestBytes(image);
}

[[nodiscard]] bool DecodeColorClear(const TextureCache::ImageDesc& desc, uint8_t code,
                                  vk::ClearColorValue& clear) {
	const auto& metadata = desc.info.metadata;
	const auto  format   = desc.info.IsDepth()
	                           ? TextureGetSurfaceFormatInfo(desc.info.guest_format).vk_format
	                           : desc.view_info.format;
	const bool  cmask    = metadata.kind == ImageMetadataKind::Cmask;
	if (cmask ? code == 0 : code == 0x20) {
		// Register clears belong to the color buffer; the texture pipe cannot decode them.
		return desc.type == TextureCache::BindingType::RenderTarget &&
		       metadata.clear_register_valid &&
		       DecodePackedColorClear(format, metadata.clear_word, clear);
	}
	if (cmask) {
		return false;
	}
	switch (code) {
		case 0x00:
		case 0x40:
		case 0x80:
		case 0xc0: break;
		default: return false;
	}
	clear = {};
	if (code == 0x00) {
		return true;
	}
	switch (format) {
		case vk::Format::eR8Unorm:
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR4G4B4A4UnormPack16:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eB10G11R11UfloatPack32: break;
		default: return false;
	}
	const float          rgb   = (code & 0x80u) != 0 ? 1.0f : 0.0f;
	const float          alpha = (code & 0x40u) != 0 ? 1.0f : 0.0f;
	std::array<float, 4> channels {rgb, rgb, rgb, alpha};
	if (!metadata.dcc_alpha_msb) {
		std::swap(channels[0], channels[3]);
	}
	// DCC clear decoding clamps missing lanes before applying the format swizzle.
	const auto components = vk::componentCount(format);
	if (components == 1) {
		channels[0] = channels[3];
	} else if (components == 2) {
		channels[1] = channels[3];
	}
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR5G6B5UnormPack16: std::swap(channels[0], channels[2]); break;
		case vk::Format::eR4G4B4A4UnormPack16:
			std::reverse(channels.begin(), channels.end());
			break;
		default: break;
	}
	clear.float32 = channels;
	return true;
}

// Per-layer depth-plane copies, the row-splittable form the chunked download plans over.
[[nodiscard]] std::vector<vk::BufferImageCopy>
BuildDepthCopies(const ImageInfo& info, uint64_t slice_stride, vk::ImageAspectFlags aspect) {
	std::vector<vk::BufferImageCopy> copies(info.resources.layers);
	for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
		auto& copy             = copies[layer];
		copy.bufferOffset      = slice_stride * layer;
		copy.bufferRowLength   = info.pitch;
		copy.bufferImageHeight = info.extent.height;
		copy.imageSubresource  = {aspect, 0, layer, 1};
		copy.imageExtent       = {info.extent.width, info.extent.height, 1};
	}
	return copies;
}

// Only an 8-bit colour sample or storage image can alias a stencil plane; a target stays its own.
[[nodiscard]] bool IsStencilPlaneView(const Image& image) {
	return !image.info.IsDepth() && image.info.bytes_per_block == 1 && !image.usage.render_target;
}

RefusalReporter g_upload_source_reports;

void ReportUploadSourceRefused(const Image& image) {
	const auto& info        = image.info;
	const auto  occurrences = g_upload_source_reports.Observe(
	    info.data.address, info.data.size, static_cast<uint64_t>(info.guest_format));
	if (occurrences == 0) {
		return;
	}
	char repeat[32] = "";
	LOGF("TextureCache: image upload skipped addr=0x%016" PRIx64 " size=0x%016" PRIx64 "%s\n",
	     info.data.address, info.data.size, RefusalReporter::RepeatSuffix(repeat, occurrences));
}

// A replacement may hold more layers than requested only where its data range covers them.
bool GrowLayers(ImageInfo& info, const ImageInfo& requested, const ImageInfo& cached) {
	if (info.resources.levels != 1 || requested.resources.levels != 1 ||
	    requested.resources.layers == 0 || requested.IsVolume() ||
	    requested.data.size % requested.resources.layers != 0 ||
	    requested.mip_layout[0].offset != 0 ||
	    requested.mip_layout[0].size != requested.data.size ||
	    cached.data.address != requested.data.address) {
		return false;
	}
	const auto size = requested.data.size / requested.resources.layers * info.resources.layers;
	if (cached.data.size < size) {
		return false;
	}
	info.data.size          = size;
	info.mip_layout[0].size = size;
	return true;
}

// VK_KHR_maintenance8 copies between a depth aspect and a size-matched colour format.
bool DepthColorCopyEnabled(const GraphicContext& graphics) {
	return graphics.maintenance8_enabled;
}

// The ranges a buffer wrote after the alias's last GPU write travel with the copy.
[[nodiscard]] bool SupersededAliasCanSync(const Image& alias) {
	if (!alias.IsSuperseded()) {
		return true;
	}
	if (!alias.SupersededAfterLastGpuWrite()) {
		// Older buffer writes were reconciled into the alias before it was written again.
		return true;
	}
	return !alias.SupersededSinceGpuWriteRanges().empty() &&
	       !alias.SupersededSinceGpuWrite(alias.info.data.address, alias.info.data.size);
}

[[nodiscard]] bool LayoutAliasCandidate(const Image& image) {
	return image.registered && !image.dormant && !image.depth_id && !image.info.data.Empty() &&
	       image.backing.image != nullptr && image.backing.samples == 1 &&
	       image.info.samples == 1 && !image.info.IsDepth() && !image.info.HasStencil() &&
	       image.info.metadata.compression == VideoOutCompression::Uncompressed;
}

// Compares only guest placement; the host linear fields follow the extent and format.
[[nodiscard]] bool SameGuestMipLayout(const ImageInfo& a, const ImageInfo& b) {
	return std::equal(a.mip_layout.begin(), a.mip_layout.end(), b.mip_layout.begin(),
	                  [](const ImageMipInfo& left, const ImageMipInfo& right) {
		                  return left.offset == right.offset && left.size == right.size &&
		                         left.pitch == right.pitch && left.height == right.height &&
		                         left.tail_x == right.tail_x && left.tail_y == right.tail_y &&
		                         left.surface_z == right.surface_z;
	                  });
}

// A tiled chain stores small mips first, so the level count moves mip 0.
[[nodiscard]] bool SharedMipsAtSameOffsets(const ImageInfo& a, const ImageInfo& b) {
	const auto levels = std::min(
	    {a.resources.levels, b.resources.levels, static_cast<uint32_t>(a.mip_layout.size())});
	for (uint32_t level = 0; level < levels; level++) {
		if (a.mip_layout[level].offset != b.mip_layout[level].offset) {
			return false;
		}
	}
	return true;
}

// A one-row 2D image places its texels exactly where a 1D image of that width does.
[[nodiscard]] Prospero::ImageType RowType(const ImageInfo& info) {
	return info.type == Prospero::ImageType::kColor2D && info.extent.height == 1
	           ? Prospero::ImageType::kColor1D
	           : info.type;
}

[[nodiscard]] bool SameAliasLayout(const ImageInfo& a, const ImageInfo& b) {
	// Equal pitch and tile mode put a texel at the same address in both, so extents may differ.
	return a.data == b.data && a.resources == b.resources && a.pitch == b.pitch &&
	       a.bytes_per_block == b.bytes_per_block && a.tile_mode == b.tile_mode &&
	       RowType(a) == RowType(b) && a.IsBlock() == b.IsBlock() && SameGuestMipLayout(a, b) &&
	       a.extent.depth == b.extent.depth;
}

} // namespace

TextureCache::TextureCache(GraphicContext& graphics, CommandScheduler& scheduler,
                           PageManager& page_manager, BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_page_manager(page_manager),
      m_blit_helper(graphics, scheduler),
      m_tiler(graphics, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)),
      m_buffer_cache(buffer_cache),
      m_readback_linear_images(Config::ReadbackLinearImagesEnabled()),
      m_readback_tiled_images(Config::ReadbackTiledImagesEnabled()) {
	if (m_graphics.CanReportMemoryUsage()) {
		const auto budget     = m_graphics.GetTotalMemoryBudget();
		const auto thresholds = Headroom::ResolveCollectorThresholds(budget);
		if (budget != 0) {
			m_trigger_gc_memory  = thresholds.trigger;
			m_pressure_gc_memory = thresholds.pressure;
			m_critical_gc_memory = thresholds.critical;
		}
	}
}

TextureCache::~TextureCache() {
	m_slot_images.ForEach([&](ImageId id, const Image& image) {
		if (image.registered) {
			UnregisterImage(id);
		}
	});
}

bool TextureCache::SameGuestLayout(const ImageInfo& cached, const ImageInfo& requested) {
	if (cached.data.address != requested.data.address) {
		return false;
	}
	if (cached.data.size != requested.data.size) {
		return false;
	}
	if (cached.extent != requested.extent) {
		return false;
	}
	if (cached.resources.levels < requested.resources.levels ||
	    cached.resources.layers < requested.resources.layers) {
		return false;
	}
	if (cached.samples != requested.samples) {
		return false;
	}
	if (cached.bytes_per_block != requested.bytes_per_block) {
		return false;
	}
	if (cached.tile_mode != requested.tile_mode) {
		return false;
	}
	if (cached.type != requested.type) {
		return false;
	}
	return true;
}

bool TextureCache::SameBacking(const ImageInfo& cached, const ImageInfo& requested,
                               bool exact_format) {
	if (!SameGuestLayout(cached, requested)) {
		return false;
	}
	if (cached.GetColorTransform() != requested.GetColorTransform()) {
		return false;
	}
	if (!ImageViewOps::FormatsCompatible(cached.pixel_format, requested.pixel_format)) {
		return false;
	}
	if (exact_format && cached.pixel_format != requested.pixel_format) {
		return false;
	}
	return true;
}

TextureCache::BindingType TextureCache::UploadBinding(const Image& image) {
	if (image.info.IsDepth()) {
		if (image.info.tile_mode == Prospero::TileMode::kDepth ||
		    image.info.tile_mode == Prospero::TileMode::kLinear) {
			return BindingType::DepthTarget;
		}
		return BindingType::Texture;
	}
	if (image.usage.render_target) {
		return BindingType::RenderTarget;
	}
	if (image.usage.video_out) {
		return BindingType::VideoOut;
	}
	return image.usage.storage ? BindingType::Storage : BindingType::Texture;
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
	GraphicContext::ImageAllocationReport allocation {};
	auto id = m_slot_images.insert(m_graphics, m_scheduler, info, &allocation);
	if (m_slot_images[id].BackingFailed()) {
		// Let the collector hand memory back and ask the driver once more.
		m_slot_images.erase(id);
		const auto                            requested = allocation.size;
		const auto                            reclaimed = ReclaimForAllocation(requested);
		GraphicContext::ImageAllocationReport retry {};
		id = m_slot_images.insert(m_graphics, m_scheduler, info, &retry);
		if (m_slot_images[id].BackingFailed()) {
			EXIT("out of device memory for an image: extent=%ux%ux%u format=%u levels=%u layers=%u "
			     "requested=%" PRIu64 " bytes, result=%s; reclaimed=%" PRIu64
			     " bytes from %zu live images (%" PRIu64 " bytes accounted); device usage=%" PRIu64
			     " budget=%" PRIu64 " heap=%" PRIu64 " (budget %s, host fallback %s)\n",
			     info.extent.width, info.extent.height, info.extent.depth,
			     static_cast<uint32_t>(info.pixel_format), info.resources.levels,
			     info.resources.layers, requested != 0 ? requested : retry.size,
			     vk::to_string(retry.result).c_str(), reclaimed, m_slot_images.size(),
			     m_total_used_memory, retry.budget.usage, retry.budget.budget,
			     retry.budget.heap_size, retry.budget.reported ? "reported" : "unknown",
			     retry.host_fallback_allowed ? "tried" : "not permitted for this usage");
		}
	}
	RegisterImage(id);
	return id;
}

uint64_t TextureCache::ReclaimForAllocation(uint64_t needed) {
	const auto           frame = m_frame_index.load(std::memory_order_relaxed);
	std::vector<ImageId> candidates;
	m_lru_cache.ForEachItemBelow(m_gc_tick, [&](ImageId id) {
		const auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || !Headroom::ReclaimableWithoutGpuWork(
		                            CollectorFacts(*owner, false), ImageAbandoned(*owner, frame))) {
			return false;
		}
		candidates.push_back(id);
		return candidates.size() >= MaxEmergencyReclaimImages;
	});

	uint64_t freed = 0;
	for (const auto id: candidates) {
		const auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		freed += owner->AccountedSize();
		FreeImage(id);
		if (needed != 0 && freed >= needed) {
			break;
		}
	}
	if (freed == 0) {
		return 0;
	}

	// FreeImage defers the VkImage destruction; run it so the retry sees the memory.
	if (m_scheduler.Active()) {
		m_scheduler.Wait(m_scheduler.CurrentTick());
		m_scheduler.PopPendingOperations();
	}
	return freed;
}

void TextureCache::RegisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.registered || image.info.data.Empty()) {
		EXIT("TextureCache: invalid image registration\n");
	}
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: image registration is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size,
	            [this, id](uint64_t page) { m_image_page_table[page].push_back(id); });
	m_region_generation++;
	image.registered         = true;
	image.lru_id             = m_lru_cache.Insert(id, m_gc_tick);
	image.frame_touched_last = m_frame_index.load(std::memory_order_relaxed);
	m_total_used_memory += image.AccountedSize();
}

void TextureCache::UnregisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	image.HeapChanged();
	UntrackImage(id);
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: registered image is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr || !owners->Erase(id)) {
			EXIT("TextureCache: image missing from page owner index\n");
		}
	});
	m_region_generation++;
	m_lru_cache.Free(image.lru_id);
	const auto accounted = image.AccountedSize();
	if (accounted > m_total_used_memory) {
		EXIT("TextureCache: image accounting underflow\n");
	}
	m_total_used_memory -= accounted;
	image.registered = false;
}

void TextureCache::DeleteImage(ImageId id) {
	m_evict_pending.erase(id);
	std::erase_if(m_stencil_planes, [id](const auto& entry) { return entry.second == id; });
	auto* image = m_slot_images.try_get(id);
	if (image != nullptr && image->depth_twin) {
		if (auto* twin = m_slot_images.try_get(image->depth_twin);
		    twin != nullptr && twin->depth_twin == id && !twin->dormant) {
			twin->depth_twin = {};
			twin->layer_content.clear();
		}
		// A parked twin keeps the dead id, so FindImage never wakes it and the collector frees it.
		image->depth_twin = {};
		image->layer_content.clear();
	}
	if (image == nullptr || !image->registered) {
		return;
	}
	if (image->depth_id) {
		std::erase(m_slot_images[image->depth_id].stencil_ids, id);
	} else {
		// Detach first: deleting each association also unlinks it from its depth owner.
		for (const auto association: std::exchange(image->stencil_ids, {})) {
			FreeImage(association);
		}
	}
	if (image->IsGpuModified()) {
		EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\n");
	}
	m_download_images.erase(id);
	if (image->info.metadata.kind == ImageMetadataKind::Htile) {
		m_surface_metas.erase(image->info.metadata.range.address);
	}
	UnregisterImage(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_images.erase(id); });
	} else {
		m_slot_images.erase(id);
	}
}

void TextureCache::FreeImage(ImageId id) {
	m_evict_backoff.erase(id);
	auto& image = m_slot_images[id];
	if (image.IsGpuModified()) {
		image.ClearGpuModified();
	}
	DeleteImage(id);
}

void TextureCache::TouchImage(Image& image) {
	image.frame_touched_last = m_frame_index.load(std::memory_order_relaxed);
	if (image.registered) {
		m_lru_cache.Touch(image.lru_id, m_gc_tick);
	}
}

void TextureCache::MarkAsMaybeDirty(ImageId id, Image& image) {
	image.MarkMaybeCpuDirty();
	UntrackImage(id);
}

void TextureCache::TrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	const auto image_end   = image.info.data.End();
	if (image_begin == image.track_addr && image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked()) {
		image.track_addr     = image_begin;
		image.track_addr_end = image_end;
		m_page_manager.UpdatePageWatchers<true>(image_begin, image.info.data.size);
		return;
	}
	if (image_begin < image.track_addr) {
		TrackImageHead(id);
	}
	if (image.track_addr_end < image_end) {
		TrackImageTail(id);
	}
}

void TextureCache::TrackImageHead(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	if (image_begin == image.track_addr) {
		return;
	}
	if (!image.IsTracked() || image_begin > image.track_addr) {
		EXIT("TextureCache: invalid image head tracking range\n");
	}
	const auto size  = image.track_addr - image_begin;
	image.track_addr = image_begin;
	m_page_manager.UpdatePageWatchers<true>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_end = image.info.data.End();
	if (image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked() || image.track_addr_end > image_end) {
		EXIT("TextureCache: invalid image tail tracking range\n");
	}
	const auto address   = image.track_addr_end;
	const auto size      = image_end - address;
	image.track_addr_end = image_end;
	m_page_manager.UpdatePageWatchers<true>(address, size);
}

void TextureCache::UntrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.IsTracked()) {
		return;
	}
	const auto address   = image.track_addr;
	const auto size      = image.track_addr_end - image.track_addr;
	image.track_addr     = 0;
	image.track_addr_end = 0;
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::UntrackImageHead(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto begin = image.info.data.address;
	if (!image.IsTracked() || begin < image.track_addr) {
		return;
	}
	const auto address = Common::AlignDown(begin + TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	const auto size    = address - begin;
	image.track_addr   = address;
	image.HeapChanged();
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	} else if (size != 0) {
		image.MarkMaybeCpuDirty();
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(begin, size);
	}
}

void TextureCache::UntrackImageTail(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto end   = image.info.data.End();
	if (!image.IsTracked() || image.track_addr_end < end) {
		return;
	}
	const auto address   = Common::AlignDown(end, TRACKER_PAGE_SIZE);
	const auto size      = end - address;
	image.track_addr_end = address;
	image.HeapChanged();
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	} else if (size != 0) {
		image.MarkMaybeCpuDirty();
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::TrackImageDownload(ImageId id, Image& image) {
	// Small tiled targets are written back too: games read GPU results such as reductions and
	// simulation state from them on the CPU (PPSA04263 keeps the origin of its ocean wave grid
	// in a 1x1 R32G32 target, and its water physics read it to sample the wave heights).
	const auto& info   = image.info;
	const auto  texels = static_cast<uint64_t>(info.extent.width) * info.extent.height *
	                     info.extent.depth * info.resources.layers;
	if (info.IsTiled() ? m_readback_tiled_images && texels <= SmallTiledReadbackTexels
	                   : m_readback_linear_images) {
		if (!image.IsGpuModified()) {
			EXIT("TextureCache: cannot enroll a non-GPU-owned image for download\n");
		}
		m_download_images.insert(id);
	}
}

TextureCache::ImageIds TextureCache::FindImagesInRegion(uint64_t address, uint64_t size,
                                                        bool page_overlap) const {
	const auto key   = (address >> 12u) * 0x9e3779b97f4a7c15ull ^ size ^ (page_overlap ? 1u : 0u);
	auto&      entry = m_region_queries[(key ^ (key >> 29u)) % m_region_queries.size()];
	if (entry.generation == m_region_generation && entry.address == address && entry.size == size &&
	    entry.page_overlap == page_overlap) {
		ImageIds result;
		for (const auto id: entry.ids) {
			result.push_back(id);
		}
		return result;
	}
	auto result        = FindImagesInRegionUncached(address, size, page_overlap);
	entry.generation   = m_region_generation;
	entry.address      = address;
	entry.size         = size;
	entry.page_overlap = page_overlap;
	entry.ids.clear();
	result.ForEach([&](ImageId id) { entry.ids.push_back(id); });
	return result;
}

TextureCache::ImageIds TextureCache::FindImagesInRegionUncached(uint64_t address, uint64_t size,
                                                                bool page_overlap) const {
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(address, size, pages)) {
		return {};
	}

	uint32_t query_epoch = ++m_image_query_epoch;
	if (query_epoch == 0) {
		m_slot_images.ForEach([](ImageId, const Image& image) { image.query_epoch = 0; });
		query_epoch = ++m_image_query_epoch;
	}

	ImageIds result;
	ForEachPage(address, size, [&](uint64_t page) {
		const auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr) {
			return;
		}
		owners->ForEach([&](ImageId id) {
			auto* image = m_slot_images.try_get(id);
			if (image == nullptr) {
				return;
			}
			if (image->query_epoch == query_epoch) {
				return;
			}
			image->query_epoch = query_epoch;
			if (image->Overlaps(address, size, page_overlap)) {
				result.push_back(id);
			}
		});
	});
	return result;
}

void TextureCache::ValidateImageDesc(const ImageDesc& desc) const {
	ImageOps::Validate(desc.info);
	if (desc.view_info.format == vk::Format::eUndefined || desc.view_info.level_count == 0 ||
	    desc.view_info.layer_count == 0 ||
	    desc.view_info.base_level >= desc.info.resources.levels ||
	    desc.view_info.level_count > desc.info.resources.levels - desc.view_info.base_level ||
	    (!desc.info.IsVolume() &&
	     (desc.view_info.base_layer >= desc.info.resources.layers ||
	      desc.view_info.layer_count > desc.info.resources.layers - desc.view_info.base_layer))) {
		EXIT("TextureCache: invalid image view description\n");
	}
	if (desc.type == BindingType::DepthTarget && !IsSupportedDepthTargetFormat(desc.info)) {
		EXIT("TextureCache: unsupported depth image description\n");
	}
	if (desc.type == BindingType::VideoOut && !IsSupportedVideoOutFormat(desc.info)) {
		EXIT("TextureCache: unsupported video-out image description\n");
	}
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression == VideoOutCompression::Unsupported) {
		EXIT("TextureCache: unsupported compressed video-out description\n");
	}
}

void TextureCache::PrepareImageCopy(Image& image) {
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::RefreshCopySource(ImageId id) {
	auto& image = m_slot_images[id];
	RefreshImage(id);
	if (image.IsDefinitelyCpuDirty()) {
		EXIT("TextureCache: image copy source remained CPU-dirty after refresh\n");
	}
}

bool TextureCache::CopyD16(Image& destination, Image& source) {
	const bool source_depth      = source.info.IsDepth();
	const bool destination_depth = destination.info.IsDepth();
	if (source_depth == destination_depth) {
		return false;
	}
	auto&      depth          = source_depth ? source : destination;
	auto&      color          = source_depth ? destination : source;
	const auto transfer_bytes = DepthAspectTransferBytes(depth.backing.format);
	if (depth.info.bytes_per_block != sizeof(uint16_t) ||
	    color.info.bytes_per_block != sizeof(uint16_t) || transfer_bytes != sizeof(uint32_t)) {
		return false;
	}
	EXIT_IF(source.backing.samples != 1 || destination.backing.samples != 1 ||
	        source.info.resources.levels != 1 || destination.info.resources.levels != 1 ||
	        source.info.extent != destination.info.extent ||
	        source.info.resources.layers != destination.info.resources.layers);

	const auto     layers = depth.info.resources.layers;
	const uint64_t depth_slice =
	    static_cast<uint64_t>(depth.info.pitch) * depth.info.extent.height * transfer_bytes;
	const uint64_t color_slice =
	    static_cast<uint64_t>(color.info.pitch) * color.info.extent.height * sizeof(uint16_t);
	EXIT_IF(layers == 0 || depth_slice > UINT64_MAX / layers || color_slice > UINT64_MAX / layers);
	const auto                       depth_size = depth_slice * layers;
	const auto                       color_size = color_slice * layers;
	const std::array                 depth_copies {
	    vk::BufferImageCopy {0,
	                         depth.info.pitch,
	                         depth.info.extent.height,
	                         {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
	                         {},
	                         depth.info.extent}};
	const std::array color_copies {
	    vk::BufferImageCopy {0,
	                         color.info.pitch,
	                         color.info.extent.height,
	                         {vk::ImageAspectFlagBits::eColor, 0, 0, layers},
	                         {},
	                         color.info.extent}};

	auto                         depth_buffer = m_tiler.GetScratchBuffer(depth_size);
	auto                         color_buffer = m_tiler.GetScratchBuffer(color_size, depth_buffer.buffer);
	const TileManager::D16Layout promote_layout {
	    .width               = depth.info.extent.width,
	    .height              = depth.info.extent.height,
	    .layers              = layers,
	    .source_row_stride   = static_cast<uint64_t>(color.info.pitch) * sizeof(uint16_t),
	    .target_row_stride   = static_cast<uint64_t>(depth.info.pitch) * transfer_bytes,
	    .source_slice_stride = color_slice,
	    .target_slice_stride = depth_slice,
	};
	const bool d32 = DepthAspectTransferFormat(depth.backing.format) == vk::Format::eD32Sfloat;
	if (source_depth) {
		source.Download(depth_copies, depth_buffer.buffer, depth_buffer.offset, depth_buffer.size);
		m_tiler.ConvertD16(depth_buffer, color_buffer, TileManager::D16Direction::Demote, d32,
		                   {.width               = promote_layout.width,
		                    .height              = promote_layout.height,
		                    .layers              = promote_layout.layers,
		                    .source_row_stride   = promote_layout.target_row_stride,
		                    .target_row_stride   = promote_layout.source_row_stride,
		                    .source_slice_stride = promote_layout.target_slice_stride,
		                    .target_slice_stride = promote_layout.source_slice_stride});
		destination.Upload(color_copies, color_buffer.buffer, color_buffer.offset,
		                   color_buffer.size);
	} else {
		source.Download(color_copies, color_buffer.buffer, color_buffer.offset, color_buffer.size);
		m_tiler.ConvertD16(color_buffer, depth_buffer, TileManager::D16Direction::Promote, d32,
		                   promote_layout);
		destination.Upload(depth_copies, depth_buffer.buffer, depth_buffer.offset,
		                   depth_buffer.size);
	}
	return true;
}

void TextureCache::CopyImage(ImageId destination_id, ImageId source_id) {
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: cannot issue an unequal-sample image copy\n");
	}
	PrepareImageCopy(destination);
	if (source.IsBufferModified()) {
		if (source.info.data == destination.info.data) {
			destination.MarkBufferModified();
		}
		return;
	}
	const bool source_depth = source.info.IsDepth();
	const bool dest_depth   = destination.info.IsDepth();
	const bool direct_copy =
	    source.info.GetColorTransform() == destination.info.GetColorTransform() &&
	    (source.backing.image_type == destination.backing.image_type ||
	     (source.backing.image_type != vk::ImageType::e1D &&
	      destination.backing.image_type != vk::ImageType::e1D)) &&
	    (source.backing.format == destination.backing.format ||
	     (!source_depth && !dest_depth &&
	      vk::blockSize(source.backing.format) == vk::blockSize(destination.backing.format)));
	const bool depth_color_copy =
	    !direct_copy && source_depth != dest_depth && DepthColorCopyEnabled(m_graphics) &&
	    source.backing.samples == 1 && destination.backing.samples == 1 &&
	    source.info.GetColorTransform() == destination.info.GetColorTransform() &&
	    source.backing.image_type == destination.backing.image_type &&
	    Image::DepthColorCopyCompatible(source.backing.format, destination.backing.format);
	if (direct_copy || depth_color_copy) {
		destination.CopyImage(source);
	} else if (!CopyD16(destination, source)) {
		if (source.backing.samples != 1 || destination.backing.samples != 1) {
			EXIT("TextureCache: cross-format multisample image copy is unsupported\n");
		}
		auto& copy_buffer = m_buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
		destination.CopyImageWithBuffer(source, copy_buffer, m_tiler);
	}
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
}

void TextureCache::CopyImageMip(ImageId destination_id, ImageId source_id, uint32_t mip,
                                uint32_t layer) {
	RefreshCopySource(source_id);
	if (m_slot_images[destination_id].IsBufferModified()) {
		RefreshImage(destination_id);
	}
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	TrackImage(destination_id);
	if (source.IsBufferModified() || source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: invalid mip-copy ownership or sample count\n");
	}
	destination.CopyMip(source, mip, layer);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cached_id) {
	auto& cached = m_slot_images[cached_id];
	if ((!cached.info.IsDepth() && !requested.IsDepth()) ||
	    cached.info.tile_mode != requested.tile_mode) {
		return {};
	}
	const bool stencil_match = requested.HasStencil() == cached.info.HasStencil();
	const bool bpp_match     = requested.bytes_per_block == cached.info.bytes_per_block;
	// PPSA04264
	const bool raw_d16_texture =
	    binding == BindingType::Texture && cached.info.IsDepth() &&
	    cached.info.guest_format == Prospero::BufferFormat::k16UNorm &&
	    requested.guest_format == Prospero::BufferFormat::k16UInt &&
	    requested.pixel_format == vk::Format::eR16Uint && cached.backing.samples == 1 &&
	    requested.samples == 1 && requested.data == cached.info.data &&
	    requested.extent == cached.info.extent && requested.resources == cached.info.resources &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    !cached.info.HasMetadata();
	// PPSA30803: a pooled depth address reused for a color texture is a different surface.
	if (binding == BindingType::Texture && cached.info.IsDepth() && !requested.IsDepth() &&
	    !raw_d16_texture &&
	    !IsSupportedSampledDepthFormat(cached.info.pixel_format, requested.pixel_format)) {
		return {};
	}
	// PPSA04264, PPSA04288
	// A partial view retains the entire matching array layout. HTile belongs to
	// the depth source, independently of the data slices copied into color storage.
	const bool retain_cached_layout =
	    requested.samples == 1 && cached.info.samples == 1 && cached.backing.samples == 1 &&
	    requested.bytes_per_block == cached.info.bytes_per_block &&
	    requested.data.address == cached.info.data.address &&
	    requested.data.size < cached.info.data.size && requested.extent == cached.info.extent &&
	    requested.resources.levels == 1 && cached.info.resources.levels == 1 &&
	    requested.resources.layers != 0 && cached.info.resources.layers != 0 &&
	    requested.resources.layers < cached.info.resources.layers &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    requested.mip_layout[0].offset == 0 &&
	    cached.info.mip_layout[0].offset == 0 &&
	    requested.mip_layout[0].size == requested.data.size &&
	    cached.info.mip_layout[0].size == cached.info.data.size &&
	    requested.data.size % requested.resources.layers == 0 &&
	    cached.info.data.size % cached.info.resources.layers == 0 &&
	    requested.data.size / requested.resources.layers ==
	        cached.info.data.size / cached.info.resources.layers &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    (cached.info.metadata.kind == ImageMetadataKind::None ||
	     cached.info.metadata.kind == ImageMetadataKind::Htile);
	bool recreate = cached.info.resources < requested.resources ||
	                requested.IsVolume() != cached.info.IsVolume();
	switch (binding) {
		case BindingType::Texture:
			recreate |= requested.IsDepth() && !cached.info.IsDepth();
			recreate |= raw_d16_texture;
			break;
		case BindingType::Storage: recreate |= cached.info.IsDepth(); break;
		case BindingType::RenderTarget: recreate |= cached.info.IsDepth(); break;
		case BindingType::DepthTarget:
			recreate |= !cached.info.IsDepth();
			recreate |= cached.info.IsDepth() && !(stencil_match && bpp_match);
			break;
		case BindingType::VideoOut: recreate |= cached.info.IsDepth(); break;
	}
	if (!recreate) {
		return cached_id;
	}
	RefreshImage(cached_id);
	auto info = requested;
	if (retain_cached_layout) {
		info.data       = cached.info.data;
		info.resources  = cached.info.resources;
		info.mip_layout          = cached.info.mip_layout;
		info.tiling              = cached.info.tiling;
		info.tiled_slice_stride  = cached.info.tiled_slice_stride;
		info.linear_slice_stride = cached.info.linear_slice_stride;
		info.first_tail_level    = cached.info.first_tail_level;
	} else if (const auto resources = std::max(requested.resources, cached.info.resources);
	           resources != requested.resources &&
	           cached.info.resources.layers > requested.resources.layers) {
		// Keep extra cached layers only where the cached data range holds them.
		auto grown      = info;
		grown.resources = resources;
		if (GrowLayers(grown, requested, cached.info)) {
			info = grown;
		}
	}
	info.htile_initial_clear = false;
	if (const auto twin_id = ReuseDepthTwin(cached_id, info)) {
		return twin_id;
	}
	const auto replacement_id = InsertImage(info);
	auto&      replacement    = m_slot_images[replacement_id];
	replacement.usage         = cached.usage;
	if (cached.binding.is_bound || cached.binding.is_target) {
		cached.binding.needs_rebind = true;
	}
	if (cached.backing.samples == replacement.backing.samples) {
		const bool copy_supported =
		    cached.backing.samples == 1 || cached.backing.format == replacement.backing.format ||
		    (!cached.info.IsDepth() && !replacement.info.IsDepth() &&
		     ImageViewOps::FormatsCompatible(cached.backing.format, replacement.backing.format));
		if (copy_supported) {
			CopyImage(replacement_id, cached_id);
		} else {
			LOGF_COLOR(Log::Color::BrightYellow,
			           "TextureCache: unsupported cross-format multisample depth copy\n");
		}
	} else if (cached.backing.samples == 1 && replacement.backing.samples > 1 &&
	           replacement.info.IsDepth()) {
		RefreshCopySource(cached_id);
		if (cached.IsBufferModified() || cached.IsDefinitelyCpuDirty()) {
			EXIT("TextureCache: multisample depth conversion source is not native-current\n");
		}
		PrepareImageCopy(replacement);
		m_blit_helper.ReinterpretColorAsMsDepth(cached, replacement);
		CommitGpuWrite(replacement);
	} else {
		LOGF_COLOR(Log::Color::BrightYellow,
		           "TextureCache: unsupported unequal-sample depth overlap copy (%u -> %u)\n",
		           cached.backing.samples, replacement.backing.samples);
	}
	if (binding != BindingType::Texture && LinkDepthTwins(replacement_id, cached_id)) {
		return replacement_id;
	}
	const auto carried_meta =
	    cached.info.HasMetadata() && cached.info.metadata.kind == ImageMetadataKind::Htile
	        ? m_surface_metas.find(cached.info.metadata.range.address)
	        : m_surface_metas.end();
	const bool carry_meta      = carried_meta != m_surface_metas.end();
	const auto carried_address = carry_meta ? carried_meta->first : 0;
	const auto carried_info    = carry_meta ? carried_meta->second : MetaDataInfo {};
	FreeImage(cached_id);
	if (carry_meta) {
		m_surface_metas.insert_or_assign(carried_address, carried_info);
	}
	return replacement_id;
}

ImageId TextureCache::ReuseDepthTwin(ImageId cached_id, const ImageInfo& info) {
	auto& cached = m_slot_images[cached_id];
	if (!cached.depth_twin) {
		return {};
	}
	const auto twin_id = cached.depth_twin;
	const auto twin    = m_slot_images.try_get(twin_id);
	if (twin == nullptr || twin->depth_twin != cached_id || !twin->dormant || !twin->registered ||
	    twin->info.pixel_format != info.pixel_format || twin->info.data != info.data ||
	    twin->info.resources != info.resources || twin->info.extent != info.extent ||
	    twin->info.type != info.type || twin->info.tile_mode != info.tile_mode ||
	    twin->info.samples != info.samples) {
		return {};
	}
	(void)UnparkImage(twin_id);
	twin->binding = {};
	SyncDepthTwin(twin_id, cached_id);
	auto& revived = m_slot_images[twin_id];
	revived.usage = cached.usage;
	TouchImage(revived);
	cached.ClearGpuModified();
	if (!ParkImage(cached_id)) {
		FreeImage(cached_id);
	}
	return twin_id;
}

bool TextureCache::LinkDepthTwins(ImageId live_id, ImageId parked_id) {
	auto&      live   = m_slot_images[live_id];
	auto&      parked = m_slot_images[parked_id];
	const bool shape = live.info.IsDepth() != parked.info.IsDepth() &&
	                   live.info.data == parked.info.data && !live.info.HasStencil() &&
	                   !parked.info.HasStencil() && !parked.depth_id && live.backing.samples == 1 &&
	                   parked.backing.samples == 1 &&
	                   live.backing.image_type == vk::ImageType::e2D &&
	                   parked.backing.image_type == vk::ImageType::e2D &&
	                   live.backing.extent == parked.backing.extent &&
	                   live.backing.mip_levels == parked.backing.mip_levels &&
	                   live.backing.layers == parked.backing.layers &&
	                   live.info.GetColorTransform() == parked.info.GetColorTransform() &&
	                   DepthColorCopyEnabled(m_graphics) &&
	                   Image::DepthColorCopyCompatible(live.backing.format, parked.backing.format);
	if (!shape) {
		return false;
	}
	if (parked.depth_twin && parked.depth_twin != live_id) {
		const auto stale = parked.depth_twin;
		if (auto* old = m_slot_images.try_get(stale); old != nullptr && old->dormant) {
			old->ClearGpuModified();
			FreeImage(stale);
		}
	}
	parked.ClearGpuModified();
	if (!ParkImage(parked_id)) {
		return false;
	}
	const auto epoch  = live.IsGpuModified() ? live.GpuWriteEpoch() : NextImageWriteEpoch();
	live.depth_twin   = parked_id;
	parked.depth_twin = live_id;
	live.layer_content.assign(live.backing.layers, epoch);
	parked.layer_content.assign(parked.backing.layers, epoch);
	return true;
}

void TextureCache::SyncDepthTwin(ImageId destination_id, ImageId source_id) {
	RefreshCopySource(source_id);
	auto&      destination = m_slot_images[destination_id];
	auto&      source      = m_slot_images[source_id];
	const auto layers      = destination.backing.layers;
	const bool layered     = !source.IsBufferModified() && !destination.IsBufferModified() &&
	                         !destination.IsCpuDirty() && source.layer_content.size() == layers &&
	                         destination.layer_content.size() == layers;
	if (!layered) {
		auto content = source.layer_content;
		CopyImage(destination_id, source_id);
		auto& copied = m_slot_images[destination_id];
		if (content.size() == layers) {
			copied.layer_content = std::move(content);
		} else {
			copied.layer_content.assign(layers, NextImageWriteEpoch());
		}
		return;
	}
	PrepareImageCopy(destination);
	auto     merged = destination.layer_content;
	uint32_t first  = 0;
	while (first < layers) {
		if (source.layer_content[first] <= destination.layer_content[first]) {
			first++;
			continue;
		}
		uint32_t end = first + 1;
		while (end < layers && source.layer_content[end] > destination.layer_content[end]) {
			end++;
		}
		destination.CopyLayers(source, first, end - first);
		for (uint32_t layer = first; layer < end; layer++) {
			merged[layer] = source.layer_content[layer];
		}
		first = end;
	}
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
	destination.layer_content = std::move(merged);
}

void TextureCache::NarrowTwinWrite(Image& image, std::vector<uint64_t>&& before, bool was_dirty,
                                   const ImageViewInfo& view) {
	if (was_dirty || before.empty() || before.size() != image.layer_content.size() ||
	    image.info.IsVolume()) {
		return;
	}
	const auto epoch = image.GpuWriteEpoch();
	const auto end =
	    std::min<uint64_t>(uint64_t {view.base_layer} + view.layer_count, before.size());
	for (uint64_t layer = view.base_layer; layer < end; layer++) {
		before[layer] = epoch;
	}
	image.layer_content = std::move(before);
}

bool TextureCache::ParkImage(ImageId id) {
	auto owner = m_slot_images.try_get(id);
	if (owner == nullptr || !owner->registered || owner->dormant) {
		return false;
	}
	if (owner->depth_id) {
		// A stencil proxy is reached through its depth image, so parking it would strand it.
		return false;
	}
	// Parking only removes the image as an answer to overlapping lookups.
	owner->dormant = true;
	if (owner->binding.is_bound || owner->binding.is_target) {
		owner->binding.needs_rebind = true;
	}
	// Resident heaps rediscover their elements onto whatever now answers the lookup.
	owner->HeapChanged();
	return true;
}

// Readmits a parked image to the overlap walk, deciding nothing about what displaced it.
bool TextureCache::UnparkImage(ImageId id) {
	auto owner = m_slot_images.try_get(id);
	if (owner == nullptr || !owner->dormant) {
		return false;
	}
	owner->dormant = false;
	return true;
}

void TextureCache::WakeImage(ImageId id, const ImageIds& candidates) {
	if (!UnparkImage(id)) {
		return;
	}
	// The alias parked below may hold pixels written while this image slept.
	SyncLayoutAlias(id, candidates);
	auto       owner   = m_slot_images.try_get(id);
	const auto address = owner->info.data.address;
	const auto size    = owner->info.data.size;
	candidates.ForEach([&](ImageId other) {
		if (other == id) {
			return;
		}
		auto conflict = m_slot_images.try_get(other);
		if (conflict == nullptr || conflict->dormant || !conflict->registered) {
			return;
		}
		// Bindings are draw-scoped and a draw may hold two overlapping surfaces (SILENT HILL f).
		if (conflict->binding.is_target || conflict->binding.is_bound ||
		    !conflict->Overlaps(address, size)) {
			return;
		}
		// Parking never destroys, so the exchange can repeat every frame.
		(void)ParkImage(other);
	});
}

void TextureCache::RetireOverlap(ImageId id, bool abandoned) {
	auto owner = m_slot_images.try_get(id);
	if (owner == nullptr || owner->dormant) {
		return;
	}
	// Park pixels guest memory cannot reproduce; freeing them blacks out the surface.
	if ((owner->SafeToDownload() || OverlapVictimPixelsAreUnrecoverable(*owner)) && ParkImage(id)) {
		return;
	}
	if (abandoned) {
		FreeImage(id);
	}
}

namespace BcAlias {

bool Matches(const ImageInfo& requested, const ImageInfo& cached) {
	return requested.guest_format != cached.guest_format &&
	       requested.IsBlock() != cached.IsBlock() &&
	       requested.tile_mode == cached.tile_mode &&
	       requested.bytes_per_block == cached.bytes_per_block &&
	       requested.samples == 1 && cached.samples == 1 &&
	       (requested.data != cached.data || requested.resources != cached.resources ||
	        requested.BlockExtent() != cached.BlockExtent()) &&
	       !TextureCache::SameBacking(cached, requested, false);
}

bool Publish(TextureCache& cache, Image& image) {
	if (!cache.CanDownload(image)) {
		return false;
	}
	// ObtainBuffer preserves padding and records ownership before the GPU-only transfer.
	auto [buffer, offset] =
	    cache.m_buffer_cache.ObtainBuffer(image.info.data.address, image.info.data.size, true);
	cache.DownloadImage(image, *buffer, offset, image.info.data.size, image.info.resources.levels);
	image.ClearGpuModified();
	image.MarkBufferModified();
	return true;
}

void Resolve(TextureCache& cache, ImageId cached_id, ImageId merged_id) {
	auto& cached = cache.m_slot_images[cached_id];
	auto* merged = merged_id ? &cache.m_slot_images[merged_id] : nullptr;
	// PS5 raw block images can span several packed BC mips. Transfer their complete
	// guest bytes, including tail padding, before retiring the incompatible backing.
	if (cached.SafeToDownload()) {
		if (merged && merged->SafeToDownload() && !Publish(cache, *merged)) {
			return;
		}
		if (!Publish(cache, cached)) {
			return;
		}
		if (merged) {
			merged->MarkBufferModified();
		}
	}
	cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
	cache.FreeImage(cached_id);
}

} // namespace BcAlias

TextureCache::OverlapResult TextureCache::ResolveOverlap(const ImageInfo& requested,
                                                         BindingType binding, ImageId cached_id,
                                                         ImageId merged_id) {
	auto owner = m_slot_images.try_get(cached_id);
	if (owner == nullptr || !owner->registered) {
		return {merged_id};
	}
	if (BcAlias::Matches(requested, owner->info) && !owner->IsCpuDirty()) {
		BcAlias::Resolve(*this, cached_id, merged_id);
		return {merged_id};
	}
	auto&      cached = *owner;
	const bool safe_to_delete =
	    ImageAbandoned(cached, m_frame_index.load(std::memory_order_relaxed)) &&
	    cached.heap_refs.load(std::memory_order_relaxed) == 0u;

	const uint32_t requested_block = requested.bytes_per_block * requested.samples;
	const uint32_t cached_block    = cached.info.bytes_per_block * cached.info.samples;
	if (requested.data.address == cached.info.data.address &&
	    requested.BlockExtent() == cached.info.BlockExtent() && requested_block == cached_block) {
		if (const auto depth_id = ResolveDepthOverlap(requested, binding, cached_id)) {
			return {depth_id};
		}
		// Equal pitch does not imply equal mip placement: a changed extent can move
		// a level into or out of the mip tail. These are separate guest layouts.
		if (requested.tile_mode != cached.info.tile_mode ||
		    (requested.resources == cached.info.resources &&
		     !SameGuestMipLayout(requested, cached.info)) ||
		    !SharedMipsAtSameOffsets(requested, cached.info)) {
			RetireOverlap(cached_id, safe_to_delete);
			return {merged_id};
		}
		if (requested.GetColorTransform() != cached.info.GetColorTransform()) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.IsBlock() && !cached.info.IsBlock()) {
			return {ExpandImage(requested, cached_id)};
		}
		// Volume depth is not an array-layer count. A larger depth can retain the
		// same block-slice layout while requiring a larger native image.
		if ((requested.IsVolume() || cached.info.IsVolume()) &&
		    (requested.data.size == cached.info.data.size ||
		     (requested.type == cached.info.type && requested.resources == cached.info.resources &&
		      requested.extent.width == cached.info.extent.width &&
		      requested.extent.height == cached.info.extent.height &&
		      requested.extent.depth > cached.info.extent.depth))) {
			return {ExpandImage(requested, cached_id)};
		}
		// PPSA08394
		// A view cannot change the native image type or grow its extent.
		if (requested.data.size == cached.info.data.size &&
		    requested.resources == cached.info.resources &&
		    ImageViewOps::FormatsCompatible(cached.info.pixel_format, requested.pixel_format) &&
		    (requested.type != cached.info.type
		         ? requested.extent == cached.info.extent
		         : requested.extent.width > cached.info.extent.width &&
		               requested.extent.height >= cached.info.extent.height &&
		               requested.extent.depth >= cached.info.extent.depth)) {
			return {ExpandImage(requested, cached_id)};
		}
		// PS5 mip tails can expose more levels without increasing the guest allocation.
		if (requested.pixel_format == cached.info.pixel_format &&
		    requested.type == cached.info.type && requested.resources > cached.info.resources &&
		    (requested.data.size > cached.info.data.size ||
		     (requested.data.size == cached.info.data.size &&
		      requested.extent == cached.info.extent &&
		      cached.info.resources.levels > 1 &&
		      requested.resources.layers == cached.info.resources.layers))) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.type != cached.info.type) {
			return {merged_id};
		}
		// A view of the wider image would stretch normalized coordinates over its extent.
		const auto blocks = [](const ImageInfo& info, uint32_t texels) {
			return info.IsBlock() ? (texels + 3u) / 4u : texels;
		};
		if ((binding == BindingType::Texture || binding == BindingType::Storage) &&
		    (blocks(requested, requested.extent.width) <
		         blocks(cached.info, cached.info.extent.width) ||
		     blocks(requested, requested.extent.height) <
		         blocks(cached.info, cached.info.extent.height))) {
			return {merged_id};
		}
		if (requested.pixel_format != cached.info.pixel_format ||
		    requested.data.size <= cached.info.data.size) {
			const auto result_id = merged_id ? merged_id : cached_id;
			const auto result    = m_slot_images.try_get(result_id);
			return {result != nullptr && ImageViewOps::FormatsCompatible(result->info.pixel_format,
			                                                             requested.pixel_format)
			            ? result_id
			            : ImageId {}};
		}
		// The same address reallocated as a bigger surface of the same shape: expand it.
		if (requested.type == cached.info.type &&
		    requested.extent.width >= cached.info.extent.width &&
		    requested.extent.height >= cached.info.extent.height &&
		    requested.extent.depth >= cached.info.extent.depth) {
			return {ExpandImage(requested, cached_id)};
		}
		EXIT("TextureCache: unresolvable equal-address image overlap, address=0x%016" PRIx64
		     " requested=%ux%u "
		     "cached=%ux%u requested_size=0x%016" PRIx64 " cached_size=0x%016" PRIx64
		     " type=%u/%u tile=%u/%u requested_extent=%ux%ux%u cached_extent=%ux%ux%u\n",
		     requested.data.address, requested.resources.levels, requested.resources.layers,
		     cached.info.resources.levels, cached.info.resources.layers, requested.data.size,
		     cached.info.data.size, static_cast<uint32_t>(requested.type),
		     static_cast<uint32_t>(cached.info.type), static_cast<uint32_t>(requested.tile_mode),
		     static_cast<uint32_t>(cached.info.tile_mode), requested.extent.width,
		     requested.extent.height, requested.extent.depth, cached.info.extent.width,
		     cached.info.extent.height, cached.info.extent.depth);
	}

	const int32_t requested_mip = requested.MipOf(cached.info);
	if (requested_mip >= 0) {
		const int32_t layer = requested.SliceOf(cached.info, requested_mip);
		return {cached_id, requested_mip, layer};
	}

	const int32_t mip = cached.info.MipOf(requested);
	if (mip >= 0) {
		const int32_t layer = cached.info.SliceOf(requested, mip);
		if (!merged_id) {
			return {ExpandImage(requested, cached_id)};
		}
		cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
		m_slot_images[merged_id].binding.is_target |= cached.binding.is_target;
		CopyImageMip(merged_id, cached_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
		FreeImage(cached_id);
		return {merged_id};
	}
	if (requested.data.address >= cached.info.data.address) {
		RetireOverlap(cached_id, safe_to_delete);
	}
	return {merged_id};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId source_id) {
	RefreshCopySource(source_id);
	const auto expanded_id = InsertImage(info);
	auto&      source      = m_slot_images[source_id];
	if (source.binding.is_bound || source.binding.is_target) {
		source.binding.needs_rebind = true;
	}
	InitializeImage(expanded_id);
	const int32_t mip = source.info.MipOf(info);
	const int32_t layer = source.info.SliceOf(info, mip);
	if (layer >= 0) {
		CopyImageMip(expanded_id, source_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
	} else {
		CopyImage(expanded_id, source_id);
	}
	FreeImage(source_id);
	return expanded_id;
}

void TextureCache::SyncLayoutAlias(ImageId id, const ImageIds& candidates) {
	const auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !LayoutAliasCandidate(*image)) {
		return;
	}
	auto newest_epoch = image->ContentEpoch();
	if (image->IsBufferModified()) {
		newest_epoch = std::max(newest_epoch, image->BufferWriteEpoch());
	}
	ImageId newest {};
	for (const auto candidate: candidates) {
		const auto* alias = m_slot_images.try_get(candidate);
		// A linear alias reaches its siblings through readback and guest memory instead.
		if (candidate == id || alias == nullptr || !LayoutAliasCandidate(*alias) ||
		    ImageOwnsGuestBytes(*alias) || !alias->IsGpuModified() || alias->IsBufferModified() ||
		    !SupersededAliasCanSync(*alias) || !SameAliasLayout(alias->info, image->info) ||
		    alias->ContentEpoch() <= newest_epoch) {
			continue;
		}
		newest       = candidate;
		newest_epoch = alias->ContentEpoch();
	}
	if (!newest) {
		return;
	}
	std::vector<std::pair<uint64_t, uint64_t>> newer_in_memory;
	if (const auto& source = m_slot_images[newest]; source.SupersededAfterLastGpuWrite()) {
		const auto ranges = source.SupersededSinceGpuWriteRanges();
		newer_in_memory.assign(ranges.begin(), ranges.end());
	}
	CopyImage(id, newest);
	auto& synced = m_slot_images[id];
	if (synced.IsGpuModified()) {
		synced.alias_stamp_epoch   = synced.GpuWriteEpoch();
		synced.alias_content_epoch = newest_epoch;
	}
	// The copy carried the source's stale bytes there too; FindImage's reconcile replaces them.
	for (const auto& [begin, end]: newer_in_memory) {
		(void)synced.Supersede(begin, end - begin);
	}
}

bool TextureCache::CanDownload(const Image& image) const {
	const auto& info = image.info;
	if (info.samples != 1 || image.backing.samples != 1 || info.resources.layers == 0 ||
	    info.data.size % info.resources.layers != 0) {
		return false;
	}
	if (UploadBinding(image) == BindingType::DepthTarget) {
		return IsSupportedDepthPlaneReadback(info) &&
		       Prospero::NumBytesPerElement(info.guest_format) == info.bytes_per_block;
	}
	return info.metadata.compression == VideoOutCompression::Uncompressed &&
	       (!info.IsTiled() || !Prospero::IsFmaskTextureFormat(info.guest_format)) &&
	       info.LinearSize() != 0;
}

Headroom::CollectorImageFacts TextureCache::CollectorFacts(const Image& image,
                                                           bool         with_download_plan) {
	Headroom::CollectorImageFacts facts {};
	facts.registered       = image.registered;
	facts.depth_associated = static_cast<bool>(image.depth_id);
	facts.video_out        = image.usage.video_out;
	facts.gpu_modified     = image.IsGpuModified() && !image.IsSuperseded();
	facts.stencil_plane    = image.info.HasStencil() && image.GpuWriteSerial() != 0;
	if (!facts.gpu_modified) {
		return facts;
	}
	facts.safe_to_download = image.SafeToDownload();
	// Planning a download costs a layout walk, so only the collector asks for it.
	facts.downloadable =
	    with_download_plan && facts.safe_to_download && !image.depth_id && CanDownload(image);
	return facts;
}

void TextureCache::UploadImage(Image& image, Buffer& source, uint64_t source_offset) {
	auto&       destination  = image.depth_id ? m_slot_images[image.depth_id] : image;
	const auto& info         = image.info;
	const auto  binding      = image.depth_id ? BindingType::DepthTarget : UploadBinding(image);
	const bool  device_local = source.IsDeviceLocal();
	if (binding != BindingType::DepthTarget) {
		if ((binding == BindingType::RenderTarget || binding == BindingType::VideoOut) &&
		    (info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
		     info.samples != 1 || image.backing.samples != 1)) {
			EXIT("TextureCache: invalid color-attachment upload\n");
		}
		if (binding == BindingType::VideoOut &&
		    info.metadata.compression != VideoOutCompression::Uncompressed) {
			EXIT("TextureCache: invalid video-out upload\n");
		}
		const auto linear = m_tiler.Detile(source.Handle(), source_offset, info, device_local);
		const auto copies = info.BufferCopies(linear.offset);
		destination.Upload(copies, linear.buffer, linear.offset, linear.size);
		Common::DebugCounters::Add(Common::DebugCounters::Counter::TextureUploadBytes, linear.size);
		return;
	}

	if (info.samples != 1 || destination.backing.samples != 1 ||
	    info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block) {
		EXIT("TextureCache: invalid depth upload\n");
	}
	const auto layers          = info.resources.layers;
	const auto full_slice_size = info.linear_slice_stride;
	auto       linear          = m_tiler.Detile(source.Handle(), source_offset, info, device_local);
	auto       copies          = info.BufferCopies(linear.offset);
	if (image.depth_id) {
		for (auto& copy: copies) {
			copy.imageSubresource.baseArrayLayer += image.stencil_subresources.base_layer;
		}
	}
	const auto transfer_bytes = image.depth_id ? 1u : DepthAspectTransferBytes(info.pixel_format);
	if (transfer_bytes != info.bytes_per_block) {
		const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
		EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
		                     transfer_bytes != sizeof(uint32_t) || texels_per_slice > UINT32_MAX ||
		                     texels_per_slice > UINT64_MAX / transfer_bytes);
		const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
		EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
		auto promoted = m_tiler.GetScratchBuffer(transfer_slice * layers, linear.buffer);
		m_tiler.ConvertD16(
		    linear, promoted, TileManager::D16Direction::Promote,
		    info.pixel_format == vk::Format::eD32SfloatS8Uint,
		    {.width               = info.extent.width,
		     .height              = info.extent.height,
		     .layers              = layers,
		     .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
		     .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
		     .source_slice_stride = full_slice_size,
		     .target_slice_stride = transfer_slice});
		linear = promoted;
		copies.resize(1);
		copies[0].bufferOffset      = linear.offset;
		copies[0].bufferRowLength   = info.pitch;
		copies[0].bufferImageHeight = info.extent.height;
		copies[0].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, 0, layers};
		copies[0].imageExtent       = info.extent;
	}
	destination.Upload(copies, linear.buffer, linear.offset, linear.size);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::TextureUploadBytes, linear.size);
}

void TextureCache::InitializeImage(ImageId id) {
	auto& image = m_slot_images[id];
	TrackImage(id);
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (image.IsCpuDirty()) {
			image.RefreshComplete();
		}
		return;
	}
	if (image.info.samples > 1) {
		return;
	}
	const bool upload = image.IsBufferModified() || image.IsCpuDirty();
	if (upload) {
		StreamHold hold(m_buffer_cache.GetUtilityBuffer(MemoryUsage::Upload),
		                &m_buffer_cache.GetUtilityBuffer(MemoryUsage::Stream));
		const auto [source, source_offset] =
		    m_buffer_cache.ObtainBufferForImage(image.info.data.address, image.info.data.size);
		if (source == nullptr) {
			// Clear the guest-modified flag so the refusal costs one attempt, not one per frame.
			ReportUploadSourceRefused(image);
			image.ClearBufferModified();
			if (image.IsCpuDirty()) {
				image.RefreshComplete();
			}
			return;
		}
		UploadImage(image, *source, source_offset);
		image.ClearBufferModified();
	}
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::MaterializeColorClear(ImageId id, const ImageDesc& desc,
                                       uint32_t metadata_base_layer) {
	if (desc.info.metadata.kind != ImageMetadataKind::Dcc &&
	    desc.info.metadata.kind != ImageMetadataKind::Cmask) {
		return;
	}
	const auto range = desc.info.metadata.range;
	{
		std::scoped_lock lock {m_lock};
		auto& image         = m_slot_images[id];
		image.info.metadata = desc.info.metadata;
		// Native color metadata must not retain a reused HTile clear flag.
		m_surface_metas.erase(range.address);
		if (range.size == 0 || desc.info.resources.levels != 1) {
			return;
		}
	}
	const auto layers = desc.info.TransferLayers();
	// These one-mip surfaces use complete 4 KiB color metadata blocks.
	constexpr uint64_t MetadataBlockSize = 0x1000;
	if (!range.Valid() || range.address % MetadataBlockSize != 0 || layers == 0 ||
	    range.size % layers != 0 || (range.size / layers) % MetadataBlockSize != 0) {
		EXIT("TextureCache: color metadata slices must contain aligned 4 KiB blocks\n");
	}
	const auto& view           = desc.view_info;
	const bool  volume_texture = desc.info.IsVolume() && view.type == vk::ImageViewType::e3D;
	const auto  first          = volume_texture ? 0u : metadata_base_layer;
	const auto  image_first    = volume_texture ? 0u : view.base_layer;
	const auto  count          = volume_texture ? desc.info.extent.depth : view.layer_count;
	if (first >= layers || count > layers - first) {
		EXIT("TextureCache: color view exceeds its native metadata slices\n");
	}
	const auto       slice_size = range.size / layers;
	const GuestRange viewed {range.address + slice_size * first, slice_size * count};
	if (m_buffer_cache.IsRegionGpuModified(viewed.address, viewed.size)) {
		std::array<vk::ClearAttachment, TileManager::ColorClearSlots> clears {};
		uint32_t keys = 0;
		for (uint32_t key = 0; key < clears.size(); key++) {
			clears[key].aspectMask = vk::ImageAspectFlagBits::eColor;
			if (DecodeColorClear(desc, static_cast<uint8_t>(key << 5u),
			                     clears[key].clearValue.color)) {
				keys |= 1u << key;
			}
		}
		// No selected key to clear, or none left since the last check.
		if (m_buffer_cache.IsColorMetadataChecked(viewed.address, viewed.size, keys)) {
			return;
		}
		{
			std::scoped_lock lock {m_lock};
			auto&            image = m_slot_images[id];
			// Compressed images cannot upload, so they must already hold native contents.
			const bool native_current = (image.usage.render_target || image.IsGpuModified()) &&
			                            !image.IsBufferModified() && !image.IsCpuDirty();
			// 2D-array attachment views select 2D layers or, of a volume, depth slices.
			if (m_graphics.conditional_rendering_enabled &&
			    image.backing.image_type != vk::ImageType::e1D &&
			    count <= m_graphics.GetPhysicalDeviceProperties().limits.maxFramebufferLayers &&
			    (image.backing.usage & vk::ImageUsageFlagBits::eColorAttachment) &&
			    (m_graphics.GetFormatProperties(view.format).optimalTilingFeatures &
			     vk::FormatFeatureFlagBits::eColorAttachment) &&
			    (desc.info.metadata.compression == VideoOutCompression::Uncompressed ||
			     native_current)) {
				// The GPU selects each slice's clear, so the CPU never waits for the metadata.
				Common::DebugCounters::Add(Common::DebugCounters::Counter::ColorMetadataChecks);
				InitializeImage(id);
				const bool expand = desc.type != BindingType::VideoOut;
				const auto [buffer, offset] =
				    m_buffer_cache.ObtainBuffer(viewed.address, viewed.size, expand);
				const auto predicates = m_tiler.CheckColorClear(
				    {buffer->Handle(), offset, viewed.size}, count, keys, expand);
				if (expand) {
					m_buffer_cache.MarkColorMetadataChecked(viewed.address, viewed.size, keys);
				}
				const auto command = m_scheduler.Current().Handle();
				const auto area    = BeginColorRendering(image, view.format, view.base_level,
				                                         image_first, count, nullptr, command);
				vk::ClearRect rect {.rect = area, .layerCount = 1};
				for (uint32_t slice = 0; slice < count; slice++) {
					rect.baseArrayLayer = slice;
					for (uint32_t key = 0; key < clears.size(); key++) {
						if ((keys & (1u << key)) != 0) {
							const vk::ConditionalRenderingBeginInfoEXT predicate {
							    .buffer = predicates,
							    .offset = (slice * clears.size() + key) * sizeof(uint32_t)};
							command.beginConditionalRenderingEXT(&predicate);
							command.clearAttachments(1, &clears[key], 1, &rect);
							command.endConditionalRenderingEXT();
						}
					}
				}
				command.endRendering();
				CommitGpuWrite(image);
				return;
			}
		}
		// Finish native metadata writes before reading backing bytes. This can submit the
		// scheduler, so discovery runs before final draw uploads and never holds the texture
		// lock across it.
		Common::DebugCounters::ScopedTimer timer(
		    Common::DebugCounters::Counter::ColorMetadataReadbacks,
		    Common::DebugCounters::Counter::ColorMetadataReadbackNs);
		m_buffer_cache.ReadMemory(viewed.address, viewed.size, false);
	}
	// The guest may release the metadata under a cached image; that leaves no clear to decode.
	const auto unreadable = [&](uint64_t address) {
		static RefusalReporter reports;
		const auto             occurrences = reports.Observe(address, slice_size, 0);
		if (occurrences != 0) {
			char repeat[32] = "";
			LOGF("TextureCache: color metadata backing unreadable addr=0x%016" PRIx64
			     ", fast-clear skipped%s\n",
			     address, RefusalReporter::RepeatSuffix(repeat, occurrences));
		}
	};
	for (uint32_t slice = 0; slice < count; slice++) {
		const auto address = range.address + slice_size * (first + slice);
		uint8_t code = 0;
		if (!LibKernel::Memory::TryReadBacking(address, &code, sizeof(code))) {
			unreadable(address);
			continue;
		}
		vk::ClearValue clear {};
		if (!DecodeColorClear(desc, code, clear.color)) {
			continue;
		}
		std::vector<uint8_t> bytes(slice_size);
		if (!LibKernel::Memory::TryReadBacking(address, bytes.data(), bytes.size())) {
			unreadable(address);
			continue;
		}
		if (!std::all_of(bytes.begin(), bytes.end(), [code](uint8_t byte) { return byte == code; })) {
			continue;
		}
		{
			std::scoped_lock lock {m_lock};
			const auto& image = m_slot_images[id];
			const bool depth = image.info.IsDepth();
			if (depth) {
				const auto value = clear.color.float32[0];
				clear.depthStencil = {value, 0u};
			}
			ClearImage(m_scheduler.Current(), id, depth ? image.backing.format : view.format,
			           {depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
			            view.base_level, view.level_count,
			            image_first + slice, 1}, clear);
		}
		// Publish the conversion's expanded keys without treating them as guest writes
		// to overlapping image data. Invalidate the buffer before updating its backing.
		if (desc.type != BindingType::VideoOut) {
			std::fill(bytes.begin(), bytes.end(), uint8_t {0xff});
			m_buffer_cache.InvalidateMemory(address, slice_size);
			LibKernel::Memory::WriteBacking(address, bytes.data(), bytes.size());
		}
	}
}

void TextureCache::RefreshImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.depth_id &&
	    (m_slot_images[image.depth_id].info.metadata.stencil_compressed ||
	     m_slot_images[image.depth_id].info.samples != 1)) {
		return;
	}
	TrackImage(id);
	if (image.IsMaybeCpuDirty()) {
		(void)image.ResolveMaybeCpuHash(image.HashGuestEdges());
	}
	bool cpu_dirty = image.IsBufferModified() || image.IsDefinitelyCpuDirty();
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (cpu_dirty) {
			EXIT("TextureCache: compressed guest image refresh is unsupported\n");
		}
		return;
	}
	if (!cpu_dirty) {
		return;
	}
	std::fill(image.layer_content.begin(), image.layer_content.end(), NextImageWriteEpoch());
	InitializeImage(id);
}

ImageId TextureCache::AssociateStencil(ImageId depth_id, GuestRange stencil) {
	if (!stencil.Valid()) {
		EXIT("TextureCache: invalid stencil association range\n");
	}
	auto& depth = m_slot_images[depth_id];
	if (!depth.info.IsDepth() || !depth.info.HasStencil()) {
		EXIT("TextureCache: stencil association requires a depth/stencil image\n");
	}
	depth.info.stencil = stencil;

	ImageId association {};
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->info.data == stencil &&
		    owner->info.extent == depth.info.extent &&
		    (owner->depth_id || IsStencilPlaneView(*owner))) {

			association = id;
		}
	}
	if (!association) {
		ImageInfo info {};
		info.data   = stencil;
		info.extent = depth.info.extent;
		association = InsertImage(info);
	}
	auto& record = m_slot_images[association];
	TouchImage(record);
	// InsertImage may have grown the slot vector, so re-fetch the depth owner.
	const auto& owner = m_slot_images[depth_id];
	if (record.depth_id != depth_id || record.stencil_subresources != owner.stencil_subresources) {
		// The record becomes another image; resident heaps must not keep sampling the old one.
		record.HeapChanged();
		ImageInfo plane {};
		plane.data         = stencil;
		plane.pixel_format = vk::Format::eS8Uint;
		plane.guest_format = Prospero::BufferFormat::k8UInt;
		plane.type         = owner.info.type;
		plane.extent       = owner.info.extent;
		plane.resources    = {owner.stencil_subresources.level_count,
		                      owner.stencil_subresources.layer_count};
		plane.tile_mode    = owner.info.tile_mode;
		plane.pitch =
		    plane.IsTiled() ? TileGetDepthPitch(plane.extent.width, 1, 0) : owner.info.pitch;
		plane.bytes_per_block = 1;
		plane.UpdateSize();
		record.info = plane;
		if (record.depth_id != depth_id) {
			if (record.depth_id) {
				std::erase(m_slot_images[record.depth_id].stencil_ids, association);
			}
			m_slot_images[depth_id].stencil_ids.push_back(association);
			record.depth_id = depth_id;
		}
		record.stencil_subresources = owner.stencil_subresources;
	}
	return association;
}

ImageId TextureCache::StencilPlaneOwner(const ImageDesc& desc) const {
	const auto found = m_stencil_planes.find(desc.info.data.address);
	if (found == m_stencil_planes.end()) {
		return {};
	}
	const auto* depth = m_slot_images.try_get(found->second);
	if (depth == nullptr || !depth->registered || depth->dormant || depth->depth_id ||
	    !depth->info.IsDepth() || !depth->info.HasStencil() ||
	    depth->info.stencil.address != desc.info.data.address ||
	    depth->info.extent.width != desc.info.extent.width ||
	    depth->info.extent.height != desc.info.extent.height || !depth->IsGpuModified() ||
	    depth->backing.image == nullptr) {
		return {};
	}
	return found->second;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_format) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		EXIT("TextureCache: image lookup requires a valid command buffer\n");
	}
	ValidateImageDesc(desc);
	EXIT_IF(desc.info.data.Empty());
	const auto metadata_base_layer = desc.view_info.base_layer;

	ImageId result {};
	{
		std::scoped_lock lock {m_lock};
		const auto       candidates =
		    FindImagesInRegion(desc.info.data.address, desc.info.data.size, false);
		bool block_alias = false;

		ImageId    parked {};
		ImageId    parked_layout {};
		const bool stencil_plane_read = desc.type == BindingType::Texture && !exact_format &&
		                                !desc.info.IsDepth() && desc.info.bytes_per_block == 1 &&
		                                desc.info.samples == 1 && desc.info.resources.levels == 1 &&
		                                desc.info.resources.layers == 1;
		// A sampled stencil plane reads its depth owner unless it is a target or overwritten.
		const auto sampled_plane_redirect = [&](ImageId owner_id) {
			if (m_slot_images[owner_id].binding.is_target) {
				return false;
			}
			const auto owner_epoch = m_slot_images[owner_id].GpuWriteEpoch();
			for (const auto id: candidates) {
				const auto image = m_slot_images.try_get(id);
				if (image == nullptr || image->depth_id ||
				    image->info.data.address != desc.info.data.address) {
					continue;
				}
				if ((image->IsGpuModified() && image->GpuWriteEpoch() > owner_epoch) ||
				    (image->IsBufferModified() && image->BufferWriteEpoch() > owner_epoch)) {
					return false;
				}
			}
			return true;
		};
		for (const auto id: candidates) {
			const auto image = m_slot_images.try_get(id);
			if (image == nullptr) {
				continue;
			}
			if (stencil_plane_read && image->depth_id &&
			    image->info.data.address == desc.info.data.address &&
			    image->info.data.size != desc.info.data.size &&
			    image->info.extent == desc.info.extent) {
				result = id;
				break;
			}
			// Stencil descriptors name the associated plane; its depth owner supplies
			// the native image and the canonical guest tiling.
			if ((desc.type == BindingType::Texture || desc.type == BindingType::Storage) &&
			    image->depth_id && image->info.data == desc.info.data &&
			    m_slot_images[image->depth_id].info.samples == desc.info.samples &&
			    image->info.extent == desc.info.extent && image->info.type == desc.info.type &&
			    image->info.resources.levels >= desc.info.resources.levels &&
			    image->info.resources.layers >= desc.info.resources.layers &&
			    desc.info.bytes_per_block == 1 &&
			    desc.info.guest_format == Prospero::BufferFormat::k8UInt &&
			    (desc.type != BindingType::Texture || sampled_plane_redirect(image->depth_id))) {
				result = id;
				break;
			}
			// The exact-backing match itself runs below, where a parked owner is told apart.
			if (!image->dormant && !SameBacking(image->info, desc.info, exact_format) &&
			    image->SafeToDownload() && BcAlias::Matches(desc.info, image->info)) {
				block_alias = true;
			}
		}
		ImageId stencil_owner {};
		if (stencil_plane_read && !result) {
			stencil_owner = StencilPlaneOwner(desc);
			if (stencil_owner && sampled_plane_redirect(stencil_owner)) {
				result = AssociateStencil(stencil_owner, m_slot_images[stencil_owner].info.stencil);
			}
		}
		const bool stencil_plane_found = static_cast<bool>(result);
		// An image another surface has since GPU-written over holds stale pixels there.
		const auto request_begin = desc.info.data.address;
		const auto request_end   = desc.info.data.End();
		const auto content_epoch = [](const Image& image) {
			uint64_t epoch = image.IsGpuModified() ? image.ContentEpoch() : 0;
			return image.IsBufferModified() ? std::max(epoch, image.BufferWriteEpoch()) : epoch;
		};
		const auto written_epoch = [&](const Image& writer) {
			const auto& info   = writer.info;
			const auto  layers = info.resources.layers;
			if (layers == 0 || info.resources.levels != 1 || info.data.size % layers != 0 ||
			    writer.layer_content.size() != layers) {
				return writer.ContentEpoch();
			}
			const auto slice_size = info.data.size / layers;
			const auto first      = (request_begin - info.data.address) / slice_size;
			const auto last       = (request_end - 1 - info.data.address) / slice_size;
			uint64_t   epoch      = 0;
			for (auto layer = first; layer <= last; layer++) {
				epoch = std::max(epoch, writer.layer_content[layer]);
			}
			return epoch;
		};
		// Only a writer the overlap walk hands back as a view of itself may take the request.
		const auto serves_request = [&](const ImageInfo& info) {
			if (info.IsVolume() || desc.info.IsVolume() || info.IsBlock() != desc.info.IsBlock()) {
				return false;
			}
			if (info.data.address != request_begin) {
				return desc.info.MipOf(info) >= 0;
			}
			return info.type == desc.info.type && info.tile_mode == desc.info.tile_mode &&
			       info.BlockExtent() == desc.info.BlockExtent() &&
			       info.bytes_per_block * info.samples ==
			           desc.info.bytes_per_block * desc.info.samples &&
			       info.GetColorTransform() == desc.info.GetColorTransform() &&
			       ImageViewOps::FormatsCompatible(info.pixel_format, desc.info.pixel_format);
		};
		const auto overwritten = [&](ImageId id) {
			const auto& image = m_slot_images[id];
			if ((desc.type != BindingType::Texture && desc.type != BindingType::Storage) ||
			    image.depth_id || image.info.IsDepth() || image.info.data != desc.info.data) {
				return false;
			}
			const auto epoch = content_epoch(image);
			for (const auto other_id: candidates) {
				const auto* writer = m_slot_images.try_get(other_id);
				if (other_id == id || writer == nullptr || !writer->registered || writer->dormant ||
				    writer->depth_id || writer->info.IsDepth() ||
				    writer->backing.image == nullptr || !writer->IsGpuModified() ||
				    writer->info.data.address > request_begin ||
				    writer->info.data.End() < request_end || !serves_request(writer->info) ||
				    SameAliasLayout(writer->info, image.info)) {
					// SyncLayoutAlias brings a same-layout alias up to date instead.
					continue;
				}
				if (written_epoch(*writer) > epoch) {
					return true;
				}
			}
			return false;
		};
		std::vector<ImageId> stale;
		for (const auto id: candidates) {
			const auto image = m_slot_images.try_get(id);
			// A parked twin is only revived through ResolveDepthOverlap, which syncs it first.
			if (image == nullptr || stencil_plane_found || (image->dormant && image->depth_twin)) {
				continue;
			}
			if (overwritten(id)) {
				stale.push_back(id);
				continue;
			}
			if (image->depth_id && desc.type != BindingType::Texture &&
			    desc.type != BindingType::Storage && image->backing.image != nullptr &&
			    SameGuestLayout(image->info, desc.info)) {
				// A stencil proxy has the S8 format; a target over its backing still reclaims it.
				result = id;
			} else if (SameBacking(image->info, desc.info, exact_format)) {
				// Only an exact backing match hands a parked image back; prefer a live owner.
				(image->dormant ? parked : result) = id;
			} else if (image->dormant && SameGuestLayout(image->info, desc.info)) {
				parked_layout = id;
			}
		}
		if (!result && parked) {
			WakeImage(parked, candidates);
			result = parked;
		}
		// PPSA03541: readmit a same-layout parked image for the overlap walk.
		if (!result && parked_layout) {
			// Readmit it only: displacing the live surfaces here parks a binding the caller holds.
			(void)UnparkImage(parked_layout);
		}

		int32_t view_mip   = -1;
		int32_t view_layer = -1;
		if (!result) {
			for (const auto candidate: candidates) {
				// A parked image must not enter the walk: it would retire the live image.
				const auto owner = m_slot_images.try_get(candidate);
				if (owner == nullptr || owner->dormant ||
				    std::ranges::find(stale, candidate) != stale.end()) {
					continue;
				}
				view_mip                = -1;
				view_layer              = -1;
				const auto& merged_info = result ? m_slot_images[result].info : desc.info;
				const auto  overlap     = ResolveOverlap(merged_info, desc.type, candidate, result);
				if (overlap.image) {
					result     = overlap.image;
					view_mip   = overlap.mip;
					view_layer = overlap.layer;
				}
			}
		} else if (block_alias) {
			for (const auto candidate: candidates) {
				const auto* owner = m_slot_images.try_get(candidate);
				if (candidate != result && owner != nullptr && owner->registered &&
				    !owner->dormant && !owner->IsCpuDirty() &&
				    BcAlias::Matches(m_slot_images[result].info, owner->info)) {
					BcAlias::Resolve(*this, candidate, result);
				}
			}
		}

		if (result) {
			auto& resolved = m_slot_images[result];
			if (resolved.depth_id) {
				// A stencil proxy serves reads; a target binding reclaims the address.
				if (desc.type != BindingType::Texture && desc.type != BindingType::Storage) {
					FreeImage(result);
					result = {};
				}
			} else if (exact_format && resolved.info.pixel_format != desc.info.pixel_format) {
				result = {};
			} else if (resolved.info.resources.levels < desc.info.resources.levels ||
			           resolved.info.resources.layers < desc.info.resources.layers) {
				FreeImage(result);
				result = {};
			}
		}
		if (!result) {
			result         = InsertImage(desc.info);
			auto& inserted = m_slot_images[result];
			if (m_buffer_cache.HasGpuDirtyBytes(inserted.info.data.address,
			                                    inserted.info.data.size)) {
				inserted.MarkBufferModified(
				    BufferWriteEpochOf(inserted.info.data.address, inserted.info.data.size));
			}
		}
		SyncLayoutAlias(result, candidates);
		auto& image = m_slot_images[result];
		if (view_mip >= 0) {
			desc.view_info.base_level = static_cast<uint32_t>(view_mip);
		}
		if (view_layer >= 0) {
			desc.view_info.base_layer = static_cast<uint32_t>(view_layer);
		}
		image.tick_accessed_last  = m_scheduler.CurrentTick();
		image.frame_accessed_last = m_frame_index.load(std::memory_order_relaxed);
		TouchImage(image);
	}
	bool superseded = false;
	{
		std::scoped_lock lock {m_lock};
		const auto&      image = m_slot_images[result];
		superseded             = image.IsSuperseded() && !image.depth_id;
	}
	if (superseded) {
		m_buffer_cache.ReconcileSupersededImage(result);
	}
	MaterializeColorClear(result, desc, metadata_base_layer);
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression != VideoOutCompression::Uncompressed) {
		std::scoped_lock lock {m_lock};
		const auto& image = m_slot_images[result];
		const bool guest_dirty = image.IsBufferModified() || image.IsCpuDirty();
		const bool native_current =
		    (image.usage.render_target || image.IsGpuModified()) && !guest_dirty;
		if (!native_current) {
			EXIT("TextureCache: compressed video-out read requires clean native GPU "
			     "contents\n");
		}
	}
	return result;
}

void TextureCache::UpdateImage(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	RefreshImage(id);
}

ImageId TextureCache::FindImageFromRange(uint64_t address, uint64_t size, bool ensure_valid) {
	if (!GuestRange {address, size}.Valid()) {
		return {};
	}
	std::scoped_lock lock {m_lock};
	ImageIds         matches;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || owner->dormant || owner->info.data.address != address) {
			continue;
		}
		if (ensure_valid && owner->depth_id) {
			owner = m_slot_images.try_get(owner->depth_id);
		}
		if (owner == nullptr || (ensure_valid && !owner->SafeToDownload())) {
			continue;
		}
		matches.push_back(id);
	}
	ImageId selected {};
	if (matches.size() == 1) {
		selected = matches.front();
	} else {
		for (const auto id: matches) {
			const auto& image = m_slot_images[id];
			if (image.info.data.size == size) {
				selected = id;
				break;
			}
		}
	}
	if (selected && ensure_valid) {
		const auto owner = m_slot_images.try_get(selected);
		if (owner != nullptr && owner->depth_id) {
			selected = owner->depth_id;
		}
	}
	return selected;
}

vk::ImageView TextureCache::FindTexture(ImageId id, const ImageDesc& desc) {
	std::scoped_lock lock {m_lock};
	auto&            image       = m_slot_images[id];
	auto             twin_before = image.layer_content;
	const bool       twin_dirty =
	    image.IsBufferModified() || image.IsCpuDirty() || image.IsMaybeCpuDirty();
	TouchImage(image);
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: texture requires rediscovery before final acquisition\n");
	}
	if (desc.type == BindingType::Storage) {
		image.MarkGpuModified();
	}
	RefreshImage(id);
	if (image.info.HasStencil() &&
	    desc.info.data.address >= image.info.stencil.address &&
	    desc.info.data.End() <= image.info.stencil.End()) {
		for (const auto stencil_id:
		     FindImagesInRegion(image.info.stencil.address, image.info.stencil.size, false)) {
			const auto* stencil = m_slot_images.try_get(stencil_id);
			if (stencil != nullptr && stencil->depth_id == id &&
			    stencil->info.data == image.info.stencil) {
				RefreshImage(stencil_id);
				break;
			}
		}
	}
	switch (desc.type) {
		case BindingType::Texture: break;
		case BindingType::Storage:
			CommitGpuWrite(image);
			NarrowTwinWrite(image, std::move(twin_before), twin_dirty, desc.view_info);
			TrackImageDownload(id, image);
			break;
		default: EXIT("TextureCache: invalid texture binding\n");
	}
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindRenderTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::RenderTarget) {
		EXIT("TextureCache: invalid color-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: color target requires rediscovery before final acquisition\n");
	}
	auto       twin_before = image.layer_content;
	const bool twin_dirty =
	    image.IsBufferModified() || image.IsCpuDirty() || image.IsMaybeCpuDirty();
	TouchImage(image);
	image.MarkGpuModified();
	image.usage.render_target = true;
	RefreshImage(id);
	CommitGpuWrite(image);
	NarrowTwinWrite(image, std::move(twin_before), twin_dirty, desc.view_info);
	TrackImageDownload(id, image);
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindDepthTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::DepthTarget) {
		EXIT("TextureCache: invalid depth-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: depth target requires rediscovery before final acquisition\n");
	}
	auto       twin_before = image.layer_content;
	const bool twin_dirty =
	    image.IsBufferModified() || image.IsCpuDirty() || image.IsMaybeCpuDirty();
	TouchImage(image);
	image.MarkGpuModified();
	if (desc.info.HasStencil()) {
		const auto layers = image.info.resources.layers;
		EXIT_IF(layers == 0 || image.info.data.size % layers != 0 ||
		        desc.info.data.address < image.info.data.address);
		const auto slice_size = image.info.data.size / layers;
		const auto offset     = desc.info.data.address - image.info.data.address;
		EXIT_IF(slice_size == 0 || offset % slice_size != 0 || offset / slice_size >= layers ||
		        desc.info.resources.layers > layers - offset / slice_size);
		image.stencil_subresources = {0, desc.info.resources.levels,
		                             static_cast<uint32_t>(offset / slice_size),
		                             desc.info.resources.layers};
	}
	image.info.stencil = desc.info.stencil;
	if (desc.info.HasStencil()) {
		m_stencil_planes[desc.info.stencil.address] = id;
	}
	image.info.metadata = desc.info.metadata;
	if (desc.info.HasMetadata()) {
		const auto [entry, inserted] =
		    m_surface_metas.try_emplace(desc.info.metadata.range.address);
		auto&      meta       = entry->second;
		const auto range_size = desc.info.metadata.range.size;
		const auto slices     = desc.info.resources.layers;
		if (inserted) {
			meta.range_size = range_size;
			meta.slices     = slices;
			if (image.info.htile_initial_clear) {
				meta.clear_mask.set();
			}
		} else {
			// Keep the accumulated clear state but describe the live binding's footprint.
			const bool same_slices =
			    meta.slices != 0 && slices != 0 && meta.range_size % meta.slices == 0 &&
			    range_size % slices == 0 && meta.range_size / meta.slices == range_size / slices;
			if (!same_slices || slices > meta.slices) {
				meta.range_size = range_size;
				meta.slices     = slices;
			}
		}
	}
	RefreshImage(id);
	CommitGpuWrite(image);
	NarrowTwinWrite(image, std::move(twin_before), twin_dirty, desc.view_info);
	if (desc.info.HasStencil()) {
		RefreshImage(AssociateStencil(id, desc.info.stencil));
	}
	return image.FindView(desc.view_info);
}

void TextureCache::MarkGpuWritten(ImageId id, bool stencil_written) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id) {
		EXIT("TextureCache: cannot mark an unavailable image GPU-written\n");
	}
	TrackImage(id);
	CommitGpuWrite(image);
	if (image.info.HasStencil() && stencil_written) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
}

void TextureCache::CommitGpuWrite(Image& image) {
	if (!image.depth_id && image.backing.image == nullptr) {
		EXIT("TextureCache: GPU writes require a native image or stencil association\n");
	}
	image.ClearBufferModified();
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	image.MarkGpuModified();
}

namespace {

struct BlockPiece {
	uint32_t level   = 0;
	uint32_t block_x = 0;
	uint32_t block_y = 0;
	uint32_t texel_x = 0;
	uint32_t texel_y = 0;
	uint32_t width   = 0;
	uint32_t height  = 0;
};

bool RenderTargetBlockPieces(const ImageInfo& info, uint64_t offset,
                             std::vector<BlockPiece>& pieces) {
	const auto& block  = info.tiling.block;
	const auto  levels = info.resources.levels;
	if (!info.IsTiled() || info.IsVolume() || info.resources.layers != 1 ||
	    block.family != TileBlockFamily::RenderTarget64KB || block.block_depth != 1 ||
	    block.block_width == 0 || block.block_height == 0 || block.block_size == 0 ||
	    info.tiling.texel_width != 1 || info.tiling.texel_height != 1 || levels > 16) {
		return false;
	}
	pieces.clear();
	for (uint32_t level = 0; level < levels; level++) {
		const auto& mip = info.mip_layout[level];
		if (mip.size == 0 || offset + block.block_size <= mip.offset ||
		    offset >= mip.offset + mip.size) {
			continue;
		}
		// A mip tail packs several levels into one block; leave those to guest memory.
		if (level >= info.first_tail_level) {
			return false;
		}
		if ((offset - mip.offset) % block.block_size != 0 || mip.pitch == 0) {
			return false;
		}
		const auto     extent  = info.MipExtent(level);
		const uint64_t index   = (offset - mip.offset) / block.block_size;
		const uint64_t per_row = (mip.pitch + block.block_width - 1) / block.block_width;
		const uint64_t texel_x = index % per_row * block.block_width;
		const uint64_t texel_y = index / per_row * block.block_height;
		if (texel_x >= extent.width || texel_y >= extent.height) {
			continue; // padding only
		}
		pieces.push_back(
		    {level, 0, 0, static_cast<uint32_t>(texel_x), static_cast<uint32_t>(texel_y),
		     static_cast<uint32_t>(std::min<uint64_t>(block.block_width, extent.width - texel_x)),
		     static_cast<uint32_t>(
		         std::min<uint64_t>(block.block_height, extent.height - texel_y))});
	}
	return true;
}

} // namespace

bool BufferCache::CopyImageForBufferCopy(uint64_t source, uint64_t destination, uint64_t size) {
	if (!GuestRange {source, size}.Valid() || !GuestRange {destination, size}.Valid()) {
		return false;
	}
	auto& textures = m_texture_cache;
	struct Plan {
		ImageId from;
		ImageId to;
		// No live destination: wake this parked one, or create one with the source's layout.
		ImageId              parked;
		bool                 create = false;
		std::vector<ImageId> aliases;

		bool operator==(const Plan&) const = default;
	};
	const auto plan_copy = [&]() -> std::optional<Plan> {
		Plan plan;
		for (const auto id: textures.FindImagesInRegion(source, size, false)) {
			const auto* image = textures.m_slot_images.try_get(id);
			if (image != nullptr && !image->dormant && !image->depth_id &&
			    image->info.data == GuestRange {source, size}) {
				if (plan.from) {
					return std::nullopt;
				}
				plan.from = id;
			}
		}
		if (!plan.from) {
			return std::nullopt;
		}
		const auto& from     = textures.m_slot_images[plan.from];
		const auto  copyable = [](const Image& image) {
			const auto kind = image.info.metadata.kind;
			return image.registered && image.backing.image != nullptr && image.info.samples == 1 &&
			       image.backing.samples == 1 &&
			       image.info.metadata.compression == VideoOutCompression::Uncompressed &&
			       (kind == ImageMetadataKind::None ||
			        (image.info.IsDepth() && kind == ImageMetadataKind::Htile) ||
			        (!image.info.IsDepth() &&
			         (kind == ImageMetadataKind::Dcc || kind == ImageMetadataKind::Cmask)));
		};
		const auto same_layout = [&](const Image& image) {
			const auto& a = from.info;
			const auto& b = image.info;
			const auto  formats =
			    from.backing.format == image.backing.format ||
			    (vk::blockSize(from.backing.format) == vk::blockSize(image.backing.format) &&
			     (a.IsDepth() || b.IsDepth()) &&
			     Image::DepthColorCopyCompatible(from.backing.format, image.backing.format));
			return formats && a.guest_format == b.guest_format && a.tile_mode == b.tile_mode &&
			       a.type == b.type && a.extent == b.extent && a.resources == b.resources &&
			       a.bytes_per_block == b.bytes_per_block && a.pitch == b.pitch &&
			       a.GetColorTransform() == b.GetColorTransform() &&
			       a.resources.levels <= a.mip_layout.size() &&
			       std::equal(
			           a.mip_layout.begin(), a.mip_layout.begin() + a.resources.levels,
			           b.mip_layout.begin(), [](const ImageMipInfo& x, const ImageMipInfo& y) {
				           return x.offset == y.offset && x.size == y.size && x.pitch == y.pitch;
			           });
		};
		for (const auto id: textures.FindImagesInRegion(destination, size, false)) {
			const auto* image = textures.m_slot_images.try_get(id);
			if (image == nullptr) {
				continue;
			}
			const bool candidate = !image->depth_id &&
			                       image->info.data == GuestRange {destination, size} &&
			                       copyable(*image) && same_layout(*image);
			if (candidate && !image->dormant) {
				if (plan.to) {
					return std::nullopt;
				}
				plan.to = id;
			} else if (candidate && !plan.parked) {
				plan.parked = id;
			} else {
				plan.aliases.push_back(id);
			}
		}
		if (plan.to && plan.parked) {
			plan.aliases.push_back(plan.parked);
			plan.parked = {};
		}
		plan.create = !plan.to && !plan.parked &&
		              from.info.metadata.kind == ImageMetadataKind::None && !from.info.HasStencil();
		if (!plan.to && !plan.parked && !plan.create) {
			return std::nullopt;
		}
		// The source image must hold the bytes: nothing newer may sit in guest memory or a buffer.
		if (!copyable(from) || from.IsBufferModified() || from.IsCpuDirty() ||
		    from.IsMaybeCpuDirty() || from.IsSuperseded()) {
			return std::nullopt;
		}
		// Nor may another surface over the source have written its share of the bytes since.
		for (const auto id: textures.FindImagesInRegion(source, size, false)) {
			const auto* other = textures.m_slot_images.try_get(id);
			if (id != plan.from && other != nullptr &&
			    ((other->IsGpuModified() && other->GpuWriteEpoch() > from.GpuWriteEpoch()) ||
			     other->IsBufferModified())) {
				return std::nullopt;
			}
		}
		std::ranges::sort(plan.aliases);
		return plan;
	};

	const auto copy_image = [&](ImageId to_id, ImageId from_id) {
		auto& to   = textures.m_slot_images[to_id];
		auto& from = textures.m_slot_images[from_id];
		if (!Image::CopyCarriesStencil(from.backing.format, to.backing.format)) {
			textures.CopyImage(to_id, from_id);
			return;
		}
		textures.RefreshCopySource(from_id);
		textures.PrepareImageCopy(to);
		std::vector<vk::ImageCopy> regions;
		const auto levels = std::min(from.backing.mip_levels, to.backing.mip_levels);
		const auto layers = std::min(from.backing.layers, to.backing.layers);
		for (uint32_t level = 0; level < levels; level++) {
			vk::ImageCopy copy {};
			copy.srcSubresource = {vk::ImageAspectFlagBits::eDepth, level, 0, layers};
			copy.dstSubresource = {vk::ImageAspectFlagBits::eDepth, level, 0, layers};
			copy.extent         = vk::Extent3D {
			    std::max(std::min(from.backing.extent.width, to.backing.extent.width) >> level, 1u),
			    std::max(std::min(from.backing.extent.height, to.backing.extent.height) >> level,
			             1u),
			    1};
			regions.push_back(copy);
		}
		to.CopyRegions(from, regions);
		if (from.IsGpuModified()) {
			to.MarkGpuModified();
		}
		to.ClearBufferModified();
	};

	std::optional<Plan> plan;
	{
		std::scoped_lock lock {textures.m_lock};
		plan = plan_copy();
		if (!plan) {
			return false;
		}
		if (plan->aliases.empty() && plan->to) {
			// No other surface sees these bytes: the destination image is all of them.
			auto& to = textures.m_slot_images[plan->to];
			copy_image(plan->to, plan->from);
			textures.TrackImage(plan->to);
			textures.CommitGpuWrite(to);
			textures.TrackImageDownload(plan->to, to);
			return true;
		}
	}

	const auto source_id            = FindBuffer(source, size);
	const auto destination_id       = FindBuffer(destination, size);
	auto [from_buffer, from_offset] = ObtainBuffer(source, size, false, false, source_id, true);
	auto [buffer, offset]           = ObtainBuffer(destination, size, true, false, destination_id);
	std::scoped_lock lock {textures.m_lock};
	const auto       again = plan_copy();
	if (buffer == nullptr || from_buffer == nullptr || !again || *again != *plan) {
		return false;
	}
	if (plan->parked) {
		(void)textures.UnparkImage(plan->parked);
		plan->to = plan->parked;
	} else if (plan->create) {
		auto info                   = textures.m_slot_images[plan->from].info;
		info.data.address           = destination;
		plan->to                    = textures.InsertImage(info);
		auto& created               = textures.m_slot_images[plan->to];
		created.tick_accessed_last  = m_scheduler.CurrentTick();
		created.frame_accessed_last = textures.m_frame_index.load(std::memory_order_relaxed);
		textures.TouchImage(created);
	}
	auto& to = textures.m_slot_images[plan->to];
	buffer->CopyFrom(m_scheduler.Current(), *from_buffer, from_offset, offset, size);
	copy_image(plan->to, plan->from);
	textures.TrackImage(plan->to);
	textures.CommitGpuWrite(to);
	{
		constexpr uint64_t PageBits = 16;
		const auto         epoch    = NextImageWriteEpoch();
		for (auto page = destination >> PageBits; page <= (destination + size - 1) >> PageBits;
		     page++) {
			textures.m_buffer_write_epochs[page] = epoch;
		}
	}
	to.MarkGpuModified();
	textures.TrackImageDownload(plan->to, to);

	std::vector<BlockPiece>    to_pieces;
	std::vector<BlockPiece>    alias_pieces;
	std::vector<vk::ImageCopy> regions;
	for (const auto id: plan->aliases) {
		auto&      alias  = textures.m_slot_images[id];
		bool       synced = false;
		const auto begin  = std::max(alias.info.data.address, destination);
		const auto end    = std::min(alias.info.data.End(), destination + size);
		if (alias.backing.image != nullptr && alias.registered && !alias.depth_id &&
		    alias.info.samples == 1 && alias.backing.samples == 1 && !alias.info.IsDepth() &&
		    !alias.info.HasStencil() && !alias.info.IsBlock() &&
		    alias.info.metadata.compression == VideoOutCompression::Uncompressed &&
		    alias.info.metadata.kind == ImageMetadataKind::None &&
		    alias.info.resources.layers == 1 && alias.info.type == to.info.type &&
		    to.info.resources.layers == 1 && !to.info.IsBlock() &&
		    alias.info.bytes_per_block == to.info.bytes_per_block && !alias.IsBufferModified() &&
		    !alias.IsCpuDirty() && !alias.IsMaybeCpuDirty() && !alias.IsSuperseded() &&
		    to.info.IsTiled() && alias.info.IsTiled() && begin < end) {
			const auto& to_block    = to.info.tiling.block;
			const auto& alias_block = alias.info.tiling.block;
			const auto  block_size  = uint64_t {to_block.block_size};
			synced = block_size != 0 && alias_block.block_size == block_size &&
			         alias_block.block_width == to_block.block_width &&
			         alias_block.block_height == to_block.block_height &&
			         (begin - destination) % block_size == 0 &&
			         (begin - alias.info.data.address) % block_size == 0 &&
			         (end - begin) % block_size == 0 &&
			         vk::blockSize(alias.backing.format) == vk::blockSize(to.backing.format);
			regions.clear();
			for (auto block = begin; synced && block < end; block += block_size) {
				synced = RenderTargetBlockPieces(to.info, block - destination, to_pieces) &&
				         RenderTargetBlockPieces(alias.info, block - alias.info.data.address,
				                                 alias_pieces);
				for (const auto& from_piece: to_pieces) {
					for (const auto& into: alias_pieces) {
						const auto x0 = std::max(from_piece.block_x, into.block_x);
						const auto y0 = std::max(from_piece.block_y, into.block_y);
						const auto x1 = std::min(from_piece.block_x + from_piece.width,
						                         into.block_x + into.width);
						const auto y1 = std::min(from_piece.block_y + from_piece.height,
						                         into.block_y + into.height);
						if (x0 >= x1 || y0 >= y1) {
							continue;
						}
						vk::ImageCopy copy {};
						copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, from_piece.level, 0,
						                       1};
						copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, into.level, 0, 1};
						copy.srcOffset      = vk::Offset3D {
						    static_cast<int32_t>(from_piece.texel_x + x0 - from_piece.block_x),
						    static_cast<int32_t>(from_piece.texel_y + y0 - from_piece.block_y), 0};
						copy.dstOffset = vk::Offset3D {
						    static_cast<int32_t>(into.texel_x + x0 - into.block_x),
						    static_cast<int32_t>(into.texel_y + y0 - into.block_y), 0};
						copy.extent = vk::Extent3D {x1 - x0, y1 - y0, 1};
						regions.push_back(copy);
					}
				}
			}
		}
		if (synced) {
			alias.CopyRegions(to, regions);
			alias.MarkGpuModified();
			continue;
		}
		if (alias.info.data.address != destination && alias.IsGpuModified() &&
		    !alias.Supersede(destination, size)) {
			continue;
		}
		if (alias.IsGpuModified()) {
			alias.ClearGpuModified();
		}
		alias.MarkBufferModified();
	}
	return true;
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
                                        uint32_t packed_clear) {
	if (command.IsInvalid() || !GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid image clear\n");
	}
	std::scoped_lock      lock {m_lock};
	ImageId               selected {};
	ImageId               stencil_id {};
	vk::ImageAspectFlags  aspect {};
	ImageSubresourceRange subresources;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || owner->dormant) {
			continue;
		}
		vk::ImageAspectFlags  candidate {};
		ImageId               candidate_id = id;
		ImageSubresourceRange candidate_subresources {0, owner->info.resources.levels, 0,
		                                              owner->info.TransferLayers()};
		if (owner->depth_id && owner->info.data.address == address &&
		    owner->info.data.size == size) {
			candidate              = vk::ImageAspectFlagBits::eStencil;
			candidate_id           = owner->depth_id;
			candidate_subresources = owner->stencil_subresources;
			owner        = m_slot_images.try_get(candidate_id);
			if (owner == nullptr || owner->backing.image == nullptr || !owner->info.HasStencil()) {
				continue;
			}
			// A stale proxy fill erases the current plane; a partial view's own slice is current.
			const auto& plane         = owner->info.stencil;
			const auto& bound         = owner->stencil_subresources;
			bool        current_plane = plane == GuestRange {address, size};
			if (!current_plane && bound.layer_count != 0 && plane.size % bound.layer_count == 0) {
				const auto slice = plane.size / bound.layer_count;
				const auto shift = uint64_t {bound.base_layer} * slice;
				current_plane =
				    slice != 0 && plane.address >= shift &&
				    address == plane.address - shift +
				                   uint64_t {candidate_subresources.base_layer} * slice &&
				    size == uint64_t {candidate_subresources.layer_count} * slice;
			}
			if (!current_plane) {
				continue;
			}
		} else if (!owner->depth_id && owner->info.data.address == address &&
		           owner->info.data.size == size) {
			candidate = owner->info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
			                                  : vk::ImageAspectFlagBits::eColor;
		}
		if (!candidate) {
			continue;
		}
		if (selected && (selected != candidate_id || subresources != candidate_subresources)) {
			return false;
		}
		selected     = candidate_id;
		stencil_id   = candidate == vk::ImageAspectFlagBits::eStencil ? id : ImageId {};
		aspect       = candidate;
		subresources = candidate_subresources;
	}
	if (!selected) {
		return false;
	}
	auto&          image = m_slot_images[selected];
	vk::ClearValue clear {};
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		if (!DecodeColorDwordFill(image.info.pixel_format, packed_clear, clear.color)) {
			return false;
		}
	} else {
		uint8_t stencil_clear = 0;
		if ((aspect == vk::ImageAspectFlagBits::eDepth &&
		     !DecodePackedDepthClear(image.info.pixel_format, packed_clear, clear.depthStencil.depth)) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil &&
		     !DecodePackedStencilClear(packed_clear, stencil_clear))) {
			return false;
		}
		clear.depthStencil.stencil = stencil_clear;
	}
	ClearImage(command, selected, image.backing.format,
	           {aspect, subresources.base_level, subresources.level_count,
	            subresources.base_layer, subresources.layer_count}, clear);
	if (stencil_id) {
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
	return true;
}

void TextureCache::ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
                              const vk::ImageSubresourceRange& range, const vk::ClearValue& clear) {
	auto& image = m_slot_images[id];
	const auto aspects = image.info.IsDepth() ? ImageViewOps::DepthAspectMask(image.backing.format)
	                                          : vk::ImageAspectFlagBits::eColor;
	EXIT_IF(range.baseMipLevel >= image.info.resources.levels);
	const auto layers = image.info.IsVolume()
	                        ? std::max(image.info.extent.depth >> range.baseMipLevel, 1u)
	                        : image.backing.layers;
	EXIT_IF(command.IsInvalid() || image.depth_id || !range.aspectMask || range.levelCount == 0 ||
	        range.levelCount > image.info.resources.levels - range.baseMipLevel ||
	        range.layerCount == 0 || range.baseArrayLayer >= layers ||
	        range.layerCount > layers - range.baseArrayLayer ||
	        (range.aspectMask & aspects) != range.aspectMask);
	const bool full_subresources = range.baseMipLevel == 0 &&
	                               range.levelCount == image.info.resources.levels &&
	                               range.baseArrayLayer == 0 && range.layerCount == layers;
	const bool full_image = range.aspectMask == aspects && full_subresources;
	TrackImage(id);
	if (!full_image && (image.IsBufferModified() || image.IsCpuDirty())) {
		InitializeImage(id);
		if (image.info.samples == 1 && (image.IsBufferModified() || image.IsCpuDirty())) {
			EXIT("TextureCache: image clear retained guest ownership\n");
		}
	}
	if (image.info.HasStencil() && (range.aspectMask & vk::ImageAspectFlagBits::eStencil)) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		if (!full_subresources) {
			RefreshImage(stencil_id);
		} else {
			TrackImage(stencil_id);
			CommitGpuWrite(m_slot_images[stencil_id]);
		}
	}
	command.EndRendering();
	// Transfer clears use the backing format; aliased clears must encode through their view.
	if (format != image.backing.format || (image.info.IsVolume() && !full_image)) {
		EXIT_NOT_IMPLEMENTED(range.aspectMask != vk::ImageAspectFlagBits::eColor ||
		                     range.levelCount != 1);
		BeginColorRendering(image, format, range.baseMipLevel, range.baseArrayLayer,
		                    range.layerCount, &clear, command.Handle());
		command.Handle().endRendering();
		CommitGpuWrite(image);
		return;
	}
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	auto native_range = range;
	if (image.info.IsVolume()) {
		native_range.baseArrayLayer = 0;
		native_range.layerCount     = 1;
	}
	if (range.aspectMask == vk::ImageAspectFlagBits::eColor) {
		command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                                 &clear.color, 1, &native_range);
	} else {
		command.Handle().clearDepthStencilImage(image.backing.image,
		                                        vk::ImageLayout::eTransferDstOptimal,
		                                        &clear.depthStencil, 1, &native_range);
	}
	CommitGpuWrite(image);
}

vk::Rect2D TextureCache::BeginColorRendering(Image& image, vk::Format format, uint32_t level,
                                             uint32_t layer, uint32_t layers,
                                             const vk::ClearValue* clear,
                                             vk::CommandBuffer     command) {
	ImageViewInfo view {};
	view.format      = format;
	view.type        = layers == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
	view.base_level  = level;
	view.base_layer  = layer;
	view.layer_count = layers;
	view.usage       = vk::ImageUsageFlagBits::eColorAttachment;
	image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
	              vk::AccessFlagBits2::eColorAttachmentRead |
	                  vk::AccessFlagBits2::eColorAttachmentWrite,
	              {}, command);
	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = image.FindView(view);
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	if (clear != nullptr) {
		attachment.loadOp     = vk::AttachmentLoadOp::eClear;
		attachment.clearValue = *clear;
	}
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = {std::max(image.info.extent.width >> level, 1u),
	                                  std::max(image.info.extent.height >> level, 1u)};
	rendering.layerCount           = layers;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	command.beginRendering(&rendering);
	return rendering.renderArea;
}

void TextureCache::InvalidateMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid memory-invalidation range\n");
	}
	std::scoped_lock lock {m_lock};
	InvalidateCpuAliases(address, size);
}

void TextureCache::DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset) {
	const auto&    info             = image.info;
	const auto     layers           = info.resources.layers;
	const auto     full_slice_size  = info.linear_slice_stride;
	const auto     transfer_bytes   = DepthAspectTransferBytes(info.pixel_format);
	const bool     device_local     = destination.IsDeviceLocal();
	const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
	EXIT_NOT_IMPLEMENTED(transfer_bytes == 0 || texels_per_slice > UINT32_MAX ||
	                     texels_per_slice > UINT64_MAX / transfer_bytes ||
	                     texels_per_slice > UINT64_MAX / info.bytes_per_block);
	const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
	const uint64_t guest_slice    = texels_per_slice * info.bytes_per_block;
	EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
	const uint64_t transfer_size = transfer_slice * layers;
	EXIT_NOT_IMPLEMENTED(guest_slice > full_slice_size);
	if (transfer_bytes == info.bytes_per_block) {
		if (!info.IsTiled()) {
			const auto copies = info.BufferCopies(destination_offset);
			image.Download(copies, destination.Handle(), destination_offset, info.data.size);
		} else {
			const auto copies = info.BufferCopies();
			m_tiler.TileImage(image, copies, destination.Handle(), destination_offset,
			                  info.data.size, 0, device_local);
		}
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	const std::array copies {vk::BufferImageCopy {0,
	                                              info.pitch,
	                                              info.extent.height,
	                                              {vk::ImageAspectFlagBits::eDepth, 0, 0, layers},
	                                              {},
	                                              info.extent}};
	auto host_linear = m_tiler.GetScratchBuffer(transfer_size);
	image.Download(copies, host_linear.buffer, 0, host_linear.size);
	const bool tiled        = info.IsTiled();
	auto guest_linear = tiled ? m_tiler.GetScratchBuffer(info.LinearSize(), host_linear.buffer)
	                          : TileManager::Result {destination.Handle(), destination_offset,
	                                                 destination.Size() - destination_offset};
	m_tiler.ConvertD16(host_linear, guest_linear, TileManager::D16Direction::Demote,
	                   DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat,
	                   {.width               = info.extent.width,
	                    .height              = info.extent.height,
	                    .layers              = layers,
	                    .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
	                    .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
	                    .source_slice_stride = transfer_slice,
	                    .target_slice_stride = full_slice_size});
	if (tiled) {
		m_tiler.Tile(guest_linear.buffer, guest_linear.offset, guest_linear.size,
		             destination.Handle(), destination_offset, info.data.size, info, 0,
		             device_local);
	}
}

void TextureCache::DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
                                 uint64_t destination_size, uint32_t levels) {
	if (!CanDownload(image)) {
		EXIT("TextureCache: invalid image download\n");
	}
	if (UploadBinding(image) == BindingType::DepthTarget) {
		EXIT_IF(destination_size != image.info.data.size || levels != image.info.resources.levels);
		DownloadDepth(image, destination, destination_offset);
		return;
	}

	const auto& info   = image.info;
	auto        copies = info.BufferCopies(0, levels);
	if (info.IsTiled()) {
		m_tiler.TileImage(image, copies, destination.Handle(), destination_offset, destination_size,
		                  levels, destination.IsDeviceLocal());
		return;
	}
	DownloadColorRegions(image, copies, info.GetColorTransform(), destination, destination_offset,
	                     destination_size);
}

void TextureCache::DownloadColorRegions(Image& image, std::vector<vk::BufferImageCopy>& regions,
                                        ColorTransform transform, Buffer& destination,
                                        uint64_t destination_offset, uint64_t destination_size) {
	if (transform != ColorTransform::None) {
		auto linear = m_tiler.GetScratchBuffer(destination_size);
		image.Download(regions, linear.buffer, 0, linear.size);
		m_tiler.TransformColor(linear, {destination.Handle(), destination_offset, destination_size},
		                       transform, false);
		return;
	}
	for (auto& copy: regions) {
		copy.bufferOffset += destination_offset;
	}
	image.Download(regions, destination.Handle(), destination_offset, destination_size);
}

void TextureCache::DownloadDepthRegions(Image& image, std::vector<vk::BufferImageCopy>& regions,
                                        Buffer& destination, uint64_t destination_offset,
                                        uint64_t destination_size) {
	const auto& info           = image.info;
	const auto  transfer_bytes = DepthAspectTransferBytes(info.pixel_format);
	if (transfer_bytes == info.bytes_per_block) {
		for (auto& copy: regions) {
			copy.bufferOffset += destination_offset;
		}
		image.Download(regions, destination.Handle(), destination_offset, destination_size);
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	const uint64_t source_row = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t);
	const uint64_t target_row = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t);
	auto           host       = regions;
	uint64_t       host_size  = 0;
	for (auto& copy: host) {
		copy.bufferOffset = host_size;
		host_size += source_row * copy.imageExtent.height;
	}
	auto host_linear = m_tiler.GetScratchBuffer(host_size);
	image.Download(host, host_linear.buffer, 0, host_linear.size);
	const bool d32 = DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat;
	for (size_t index = 0; index < regions.size(); index++) {
		const uint32_t rows          = regions[index].imageExtent.height;
		const uint64_t source_offset = host[index].bufferOffset;
		const uint64_t target_offset = destination_offset + regions[index].bufferOffset;
		m_tiler.ConvertD16(
		    {host_linear.buffer, source_offset, host_linear.size - source_offset},
		    {destination.Handle(), target_offset, destination.Size() - target_offset},
		    TileManager::D16Direction::Demote, d32,
		    {.width               = info.extent.width,
		     .height              = rows,
		     .layers              = 1,
		     .source_row_stride   = source_row,
		     .target_row_stride   = target_row,
		     .source_slice_stride = source_row * rows,
		     .target_slice_stride = target_row * rows});
	}
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size,
                                             bool whole_image) {
	std::scoped_lock lock {m_texture_cache.m_lock};
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::vector<ImageId> owners;
	for (const auto id: m_texture_cache.FindImagesInRegion(vaddr, size, false)) {
		const auto owner = m_texture_cache.m_slot_images.try_get(id);
		if (owner == nullptr || owner->depth_id || owner->info.data.address != vaddr ||
		    !owner->SafeToDownload()) {
			continue;
		}
		owners.push_back(id);
	}
	std::ranges::sort(owners, [&](ImageId a, ImageId b) {
		return m_texture_cache.m_slot_images[a].GpuWriteEpoch() <
		       m_texture_cache.m_slot_images[b].GpuWriteEpoch();
	});
	bool written = false;
	for (size_t index = 0; index < owners.size(); index++) {
		const auto& image   = m_texture_cache.m_slot_images[owners[index]];
		const bool  covered = std::any_of(
		    owners.begin() + static_cast<ptrdiff_t>(index) + 1, owners.end(), [&](ImageId newer) {
			    return m_texture_cache.m_slot_images[newer].info.data.size >= image.info.data.size;
		    });
		if (!covered) {
			written |= WriteBackImage(buffer, owners[index], size, whole_image);
		}
	}
	return written;
}

bool BufferCache::WriteBackImage(Buffer& buffer, Common::SlotId id, uint64_t size,
                                 bool whole_image) {
	auto& image = m_texture_cache.m_slot_images[id];
	// The GPU thread owns image retirement; CPU invalidation can dirty this image after lookup.
	if (!image.SafeToDownload()) {
		return false;
	}
	if (whole_image && size < image.info.data.size) {
		return false;
	}
	if (!buffer.IsInBounds(image.info.data.address, 1)) {
		return false;
	}
	const auto buf_offset = buffer.Offset(image.info.data.address);
	const auto available  = buffer.Size() - buf_offset;
	uint32_t   levels     = 0;
	uint64_t   copy_size  = 0;
	if (buffer.IsInBounds(image.info.data.address, image.info.data.size)) {
		levels    = image.info.resources.levels;
		copy_size = image.info.data.size;
	} else if (image.info.IsVolume() || image.info.resources.layers > 1) {
		// Array and volume slices are strided across the full mip chain.
		return false;
	} else {
		for (; levels < image.info.resources.levels; ++levels) {
			const auto& mip = image.info.mip_layout[levels];
			if (mip.size == 0 || mip.offset > available || mip.size > available - mip.offset) {
				break;
			}
			copy_size = std::max(copy_size, mip.offset + mip.size);
		}
	}
	if (copy_size == 0) {
		return false;
	}
	if (!m_texture_cache.CanDownload(image) ||
	    (TextureCache::UploadBinding(image) == TextureCache::BindingType::DepthTarget &&
	     copy_size != image.info.data.size)) {
		return false;
	}
	MarkGpuWrite(image.info.data.address, copy_size);
	ForgetColorMetadataChecks(image.info.data.address, copy_size);
	if (!image.SupersededAfterLastGpuWrite()) {
		image.ClearSuperseded();
		m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, levels);
		return true;
	}
	struct Span {
		uint64_t offset = 0;
		uint64_t size   = 0;
	};
	std::vector<Span> spans;
	uint64_t          saved = 0;
	for (const auto& [begin, end]: image.SupersededRanges()) {
		const auto first = std::max(begin, image.info.data.address);
		const auto last  = std::min(end, image.info.data.address + copy_size);
		if (first < last) {
			spans.push_back({first - image.info.data.address, last - first});
			saved += last - first;
		}
	}
	if (saved == 0) {
		m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, levels);
		return true;
	}
	auto stash = std::make_unique<Buffer>(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, saved);
	auto&    command = m_scheduler.Current();
	uint64_t at      = 0;
	for (const auto& span: spans) {
		stash->CopyFrom(command, buffer, buf_offset + span.offset, at, span.size);
		at += span.size;
	}
	m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, levels);
	at = 0;
	for (const auto& span: spans) {
		buffer.CopyFrom(m_scheduler.Current(), *stash, at, buf_offset + span.offset, span.size);
		at += span.size;
	}
	m_scheduler.DeferOperation([owner = std::move(stash)]() mutable { owner.reset(); });
	return true;
}

void BufferCache::ReconcileSupersededImage(Common::SlotId id) {
	uint64_t address = 0;
	uint64_t size    = 0;
	{
		std::scoped_lock lock {m_texture_cache.m_lock};
		auto*            image = m_texture_cache.m_slot_images.try_get(id);
		if (image == nullptr || !image->registered || image->depth_id || !image->IsSuperseded()) {
			return;
		}
		if (!image->SupersededAfterLastGpuWrite()) {
			image->ClearSuperseded();
			return;
		}
		if (!image->SafeToDownload() || image->backing.image == nullptr) {
			return;
		}
		address = image->info.data.address;
		size    = image->info.data.size;
	}
	auto [buffer, offset] = ObtainBuffer(address, size, true, false);
	(void)offset;
	std::scoped_lock lock {m_texture_cache.m_lock};
	auto*            image = m_texture_cache.m_slot_images.try_get(id);
	if (image == nullptr || buffer == nullptr || !image->SupersededAfterLastGpuWrite()) {
		return;
	}
	const bool written = WriteBackImage(*buffer, id, size, true);
	if (written) {
		image->ClearGpuModified();
		image->MarkBufferModified();
	}
}

bool TextureCache::DownloadImageMemory(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.depth_id) {
		return false;
	}
	if (!image.SafeToDownload() || !CanDownload(image)) {
		return false;
	}
	const auto& info      = image.info;
	const auto  alignment = std::max<uint64_t>(info.bytes_per_block, 4);
	const auto  capacity =
	    Common::AlignDown(m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download).Size(), 64);
	if (info.data.size <= capacity) {
		return DownloadImageBatch(image, {0, info.data.size, {}}, alignment);
	}

	// Mirror the buffer path: split into batches that each fit the download stream.
	std::vector<vk::BufferImageCopy> regions;
	TextureDownloadBlock             block {};
	uint64_t                         chunk_alignment = 1;
	if (UploadBinding(image) == BindingType::DepthTarget) {
		regions     = BuildDepthCopies(info, info.data.size / info.resources.layers,
		                               vk::ImageAspectFlagBits::eDepth);
		block.bytes = info.bytes_per_block;
	} else if (!info.IsTiled()) {
		const auto extent = vk::blockExtent(image.backing.format);
		block             = {extent[0], extent[1], vk::blockSize(image.backing.format)};
		// Each array layer or volume slice becomes its own copy for the row planner.
		for (const auto& copy: info.BufferCopies()) {
			const uint64_t row_texels =
			    copy.bufferRowLength != 0 ? copy.bufferRowLength : copy.imageExtent.width;
			const uint64_t height_texels =
			    copy.bufferImageHeight != 0 ? copy.bufferImageHeight : copy.imageExtent.height;
			const uint64_t slice_bytes = (row_texels + block.width - 1) / block.width *
			                             block.bytes *
			                             ((height_texels + block.height - 1) / block.height);
			const uint32_t slices      = copy.imageSubresource.layerCount * copy.imageExtent.depth;
			for (uint32_t index = 0; index < slices; index++) {
				auto slice = copy;
				slice.bufferOffset += slice_bytes * index;
				if (copy.imageExtent.depth > 1) {
					slice.imageOffset.z += static_cast<int32_t>(index);
					slice.imageExtent.depth = 1;
				} else {
					slice.imageSubresource.baseArrayLayer += index;
					slice.imageSubresource.layerCount = 1;
				}
				regions.push_back(slice);
			}
		}
		const auto transform = info.GetColorTransform();
		chunk_alignment      = transform == ColorTransform::SwapBgra16        ? 8
		                       : transform == ColorTransform::Reverse10_11_11 ? 4
		                                                                      : 1;
	}
	if (regions.empty()) {
		// A tiled colour transfer has no partial form; callers treat this as unservable.
		return false;
	}
	std::vector<TextureDownloadChunk> chunks;
	if (!TexturePlanDownloadChunks(regions, info.data.size, block, capacity, chunk_alignment,
	                               chunks)) {
		EXIT("TextureCache: cannot split an image download past the download buffer: "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 " format=%u extent=%ux%u\n",
		     info.data.address, info.data.size, static_cast<uint32_t>(info.pixel_format),
		     info.extent.width, info.extent.height);
	}
	for (const auto& chunk: chunks) {
		if (!DownloadImageBatch(image, chunk, alignment)) {
			return false;
		}
	}
	return true;
}

bool TextureCache::DownloadImageBatch(Image& image, const TextureDownloadChunk& chunk,
                                      uint64_t alignment) {
	const GuestRange range {image.info.data.address + chunk.offset, chunk.size};
	auto&            download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	StreamHold       hold(download);
	// Mapping past the stream's end waits for the previous batch to be written back.
	auto [mapped, offset] = download.Map(range.size, alignment);
	if (mapped == nullptr) {
		EXIT("TextureCache: failed to map reusable download buffer\n");
	}
	download.Commit();
	if (!LibKernel::Memory::TryReadBacking(range.address, mapped, range.size)) {
		return false;
	}
	download.Flush(offset, range.size);

	if (chunk.regions.empty()) {
		DownloadImage(image, download, offset, range.size, image.info.resources.levels);
	} else if (UploadBinding(image) == BindingType::DepthTarget) {
		auto regions = chunk.regions;
		DownloadDepthRegions(image, regions, download, offset, range.size);
	} else {
		auto regions = chunk.regions;
		DownloadColorRegions(image, regions, image.info.GetColorTransform(), download, offset,
		                     range.size);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::TextureDownloadBytes, range.size);
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download.Handle();
	barrier.offset              = offset;
	barrier.size                = range.size;
	m_scheduler.EndRendering();
	m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
	                                               1, &barrier, 0, nullptr);
	std::vector<std::pair<uint64_t, uint64_t>> kept;
	uint64_t                                   cursor = range.address;
	const bool honour_superseded                      = image.SupersededAfterLastGpuWrite();
	for (const auto& [begin, end]: honour_superseded
	                                   ? image.SupersededRanges()
	                                   : std::span<const std::pair<uint64_t, uint64_t>> {}) {
		const auto first = std::max(begin, range.address);
		const auto last  = std::min(end, range.address + range.size);
		if (first >= last) {
			continue;
		}
		if (first > cursor) {
			kept.emplace_back(cursor, first);
		}
		cursor = std::max(cursor, last);
	}
	if (cursor < range.address + range.size) {
		kept.emplace_back(cursor, range.address + range.size);
	}
	m_scheduler.DeferPriorityOperation(
	    [&download, range, mapped, offset, kept = std::move(kept)] {
		    download.Invalidate(offset, range.size);
		    for (const auto& [begin, end]: kept) {
			    LibKernel::Memory::WriteBacking(
			        begin, static_cast<const uint8_t*>(mapped) + (begin - range.address),
			        end - begin);
		    }
	    },
	    range.address, range.size);
	return true;
}

uint64_t TextureCache::BufferWriteEpochOf(uint64_t address, uint64_t size) const {
	constexpr uint64_t PageBits = 16;
	uint64_t           epoch    = 0;
	for (auto page = address >> PageBits; page <= (address + size - 1) >> PageBits; page++) {
		if (const auto found = m_buffer_write_epochs.find(page);
		    found != m_buffer_write_epochs.end()) {
			epoch = std::max(epoch, found->second);
		}
	}
	return epoch != 0 ? epoch : NextImageWriteEpoch();
}

void TextureCache::InvalidateMemoryFromGPU(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return;
	}
	std::scoped_lock lock {m_lock};
	{
		constexpr uint64_t PageBits = 16;
		const auto         epoch    = NextImageWriteEpoch();
		for (auto page = address >> PageBits; page <= (address + size - 1) >> PageBits; page++) {
			m_buffer_write_epochs[page] = epoch;
		}
	}
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto& image = m_slot_images[id];
		if (!image.Overlaps(address, size)) {
			continue;
		}
		if (image.info.data.address != address && image.IsGpuModified() &&
		    !image.Supersede(address, size)) {
			continue;
		}
		if (image.IsGpuModified()) {
			image.ClearGpuModified();
		}
		image.MarkBufferModified();
	}
}

bool TextureCache::IsRegionRegistered(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_lock};
	return !FindImagesInRegion(address, size, false).empty();
}

bool TextureCache::IsRegionGpuModified(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return false;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, false)) {
		const auto& image = m_slot_images[id];
		// PPSA17168: shader data read over a reused render target's memory.
		if (!image.depth_id && image.IsGpuModified() && !image.IsDefinitelyCpuDirty() &&
		    ImageOwnsGuestBytes(image)) {
			return true;
		}
	}
	return false;
}

void TextureCache::InvalidateCpuAliases(uint64_t address, uint64_t size) {
	const auto page_begin = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(address + size, TRACKER_PAGE_SIZE);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		if (owner->Overlaps(address, size)) {
			owner->MarkCpuDirty();
			UntrackImage(id);
			continue;
		}
		const auto image_begin = owner->info.data.address;
		const auto image_end   = owner->info.data.End();
		if (page_end < image_end) {
			UntrackImageHead(id);
		} else if (image_begin < page_begin) {
			UntrackImageTail(id);
		} else {
			MarkAsMaybeDirty(id, *owner);
		}
	}
}

std::map<uint64_t, TextureCache::MetaDataInfo>::iterator
TextureCache::FindMetaContaining(uint64_t address) {
	auto found = m_surface_metas.upper_bound(address);
	if (found == m_surface_metas.begin()) {
		return m_surface_metas.end();
	}
	--found;
	const auto size = std::max<uint64_t>(found->second.range_size, 1);
	return address - found->first < size ? found : m_surface_metas.end();
}

bool TextureCache::IsWithinMeta(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_lock};
	const auto       found = FindMetaContaining(address);
	return found != m_surface_metas.end() && size <= found->second.range_size &&
	       address - found->first <= found->second.range_size - size;
}

bool TextureCache::IsMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	return FindMetaContaining(address) != m_surface_metas.end();
}

bool TextureCache::IsMetaCleared(uint64_t address, uint32_t slice) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= found->second.clear_mask.size()) {
		return false;
	}
	return found->second.clear_mask.test(slice);
}

bool TextureCache::ClearMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	found->second.clear_mask.set();
	return true;
}

bool TextureCache::ClearMetaSlices(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_lock};
	const auto       found = FindMetaContaining(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	auto& info = found->second;
	if (info.range_size == 0 || info.slices == 0 || info.range_size % info.slices != 0) {
		// Nothing to measure the write against, so it is not evidence of a clear.
		return false;
	}
	const auto offset = address - found->first;
	if (offset == 0 && size >= info.range_size) {
		info.clear_mask.set();
		return true;
	}
	const auto slice_size = info.range_size / info.slices;
	if (offset % slice_size != 0) {
		// Starts inside a slice: not a clear this registry can represent.
		return false;
	}
	const auto first   = offset / slice_size;
	const auto slices  = std::min<uint64_t>(info.slices, info.clear_mask.size());
	const auto covered = first < slices ? std::min<uint64_t>(size / slice_size, slices - first) : 0;
	if (covered == 0) {
		// Shorter than a slice: not a clear this registry can represent.
		return false;
	}
	for (uint64_t slice = first; slice < first + covered; slice++) {
		info.clear_mask.set(slice);
	}
	return true;
}

void TextureCache::DiscardMetaClears(uint64_t address, uint64_t size) {
	if (size == 0) {
		return;
	}
	std::scoped_lock lock {m_lock};
	const auto       found = FindMetaContaining(address);
	if (found == m_surface_metas.end()) {
		return;
	}
	auto&      info   = found->second;
	const auto offset = address - found->first;
	if (info.range_size == 0 || size > info.range_size - offset) {
		// A binding past the surface is a pool spanning it, not a store aimed at the metadata.
		return;
	}
	if (info.slices == 0 || info.range_size % info.slices != 0) {
		info.clear_mask.reset();
		return;
	}
	const auto slice_size = info.range_size / info.slices;
	const auto last =
	    std::min<uint64_t>((offset + size - 1) / slice_size, info.clear_mask.size() - 1);
	for (auto slice = offset / slice_size; slice <= last; slice++) {
		info.clear_mask.reset(slice);
	}
}

bool TextureCache::TouchMeta(uint64_t address, uint32_t slice, bool is_clear) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= found->second.clear_mask.size()) {
		return false;
	}
	found->second.clear_mask.set(slice, is_clear);
	return true;
}

bool TextureCache::UnmapMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid unmap range\n");
	}
	std::scoped_lock lock {m_lock};
	for (auto page = address >> 16; page <= (address + size - 1) >> 16; page++) {
		m_buffer_write_epochs.erase(page);
	}
	for (auto metadata = m_surface_metas.begin(); metadata != m_surface_metas.end();) {
		const auto base = metadata->first;
		if (base >= address && base < address + size) {
			metadata = m_surface_metas.erase(metadata);
		} else {
			++metadata;
		}
	}
	auto images = FindImagesInRegion(address, size, false);
	bool freed  = false;
	for (const auto id: images) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		FreeImage(id);
		freed = true;
	}
	return freed;
}

void TextureCache::RunGarbageCollector(bool per_frame) {
	std::scoped_lock lock {m_lock};
	const uint64_t   tick     = m_gc_tick++;
	const uint64_t   gc_frame = m_frame_index.load(std::memory_order_relaxed);
	if (per_frame && gc_frame == m_gc_frame && m_total_used_memory < m_pressure_gc_memory) {
		FreePublishedEvictions();
		return;
	}
	m_gc_frame = gc_frame;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
		// Idle pooled images go before any live one does.
		if (m_total_used_memory >= m_pressure_gc_memory && m_graphics.TrimImagePool() != 0) {
			m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
		}
	}
	FreePublishedEvictions();
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}
	if (m_total_used_memory >= m_pressure_gc_memory) {
		m_tiler.ReleaseScratch();
	}
	const uint64_t frame       = m_frame_index.load(std::memory_order_relaxed);
	uint32_t       evictions   = 0;
	const auto     budget      = m_graphics.GetMemoryBudget();
	const bool     over_budget = budget.reported && budget.usage > budget.budget;
	if ((tick & 1023u) == 0u) {
		std::erase_if(m_evicted_addresses, [frame](const auto& entry) {
			return frame - std::min(frame, entry.second) >= Headroom::EvictBackoffFrames;
		});
	}
	const auto collect = [&](bool allow_aggressive) {
		bool           pressured  = m_total_used_memory >= m_pressure_gc_memory;
		bool           aggressive = allow_aggressive && m_total_used_memory >= m_critical_gc_memory;
		const uint64_t age       = std::min<uint64_t>(aggressive ? 160 : pressured ? 80 : 16, tick);
		size_t               deletions = over_budget ? 128 : aggressive ? 40 : pressured ? 20 : 10;
		std::vector<ImageId> candidates;
		candidates.reserve(deletions);
		// Deleting depth recursively deletes its stencil association, so finish LRU traversal
		// first.
		m_lru_cache.ForEachItemBelow(tick - age, [&](ImageId id) {
			candidates.push_back(id);
			return candidates.size() == deletions;
		});
		for (const auto id: candidates) {
			if (deletions == 0) {
				break;
			}
			--deletions;
			auto owner = m_slot_images.try_get(id);
			if (owner == nullptr) {
				continue;
			}
			if (owner->heap_refs.load(std::memory_order_relaxed) != 0u) {
				// Resident heaps sample it without touching it.
				TouchImage(*owner);
				continue;
			}
			if (m_evict_pending.contains(id)) {
				continue;
			}
			// Evict: restorable pixels not yet written back.
			const auto requeue = [&] {
				if (owner->registered) {
					m_lru_cache.Touch(owner->lru_id, tick);
				}
			};
			const auto verdict =
			    Headroom::ClassifyForCollection(CollectorFacts(*owner, true), pressured);
			if (verdict == Headroom::CollectorVerdict::Skip ||
			    verdict == Headroom::CollectorVerdict::Keep) {
				requeue();
				continue;
			}
			uint64_t backoff_until = 0;
			if (const auto backoff = m_evict_backoff.find(id); backoff != m_evict_backoff.end()) {
				backoff_until = backoff->second;
			}
			if (const auto evicted = m_evicted_addresses.find(owner->info.data.address);
			    evicted != m_evicted_addresses.end()) {
				backoff_until =
				    std::max(backoff_until, evicted->second + Headroom::EvictBackoffFrames);
			}
			if (!Headroom::CollectionAllowed(verdict, frame, owner->frame_touched_last,
			                                 backoff_until, aggressive, over_budget) ||
			    (verdict == Headroom::CollectorVerdict::Evict &&
			     evictions >= Headroom::MaxEvictionsPerCollection)) {
				requeue();
				continue;
			}
			if (verdict == Headroom::CollectorVerdict::Evict) {
				++evictions;
				m_evicted_addresses.insert_or_assign(owner->info.data.address, frame);
				if (!DownloadImageMemory(id)) {
					continue;
				}
				// A submit runs deferred destructions, so re-read the slot pointer.
				owner = m_slot_images.try_get(id);
				if (owner == nullptr) {
					continue;
				}
				m_evict_pending.insert_or_assign(id, owner->GpuWriteSerial());
				m_scheduler.DeferPriorityOperation([this, id] {
					const std::scoped_lock published_lock {m_evict_published_lock};
					m_evict_published.push_back(id);
				});
				continue;
			}
			// DeleteImage defers the VkImage destruction; nothing here may submit or wait.
			FreeImage(id);
			Common::DebugCounters::Add(Common::DebugCounters::Counter::TextureEvictions);
			if (m_total_used_memory < m_critical_gc_memory && aggressive) {
				deletions >>= 2;
				aggressive = false;
			}
			if (m_total_used_memory < m_pressure_gc_memory && pressured) {
				deletions >>= 1;
				pressured = false;
			}
		}
	};
	collect(false);
	if (m_total_used_memory >= m_critical_gc_memory) {
		collect(true);
	}
}

void TextureCache::FreePublishedEvictions() {
	std::vector<ImageId> ready;
	{
		const std::scoped_lock published_lock {m_evict_published_lock};
		ready.swap(m_evict_published);
	}
	for (const auto id: ready) {
		const auto pending = m_evict_pending.find(id);
		if (pending == m_evict_pending.end()) {
			continue;
		}
		const auto serial_at_eviction = pending->second;
		m_evict_pending.erase(pending);
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || !owner->registered) {
			continue;
		}
		if (owner->GpuWriteSerial() != serial_at_eviction) {
			m_evict_backoff.insert_or_assign(id, m_frame_index.load(std::memory_order_relaxed) +
			                                         Headroom::EvictBackoffFrames);
			continue;
		}
		FreeImage(id);
	}
}

void TextureCache::ProcessDownloadImages() {
	if (!m_readback_linear_images && !m_readback_tiled_images) {
		return;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: m_download_images) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->registered && owner->IsGpuModified()) {
			(void)DownloadImageMemory(id);
		}
	}
	m_download_images.clear();
}

} // namespace Libs::Graphics
