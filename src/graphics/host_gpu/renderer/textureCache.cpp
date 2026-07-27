#include "graphics/host_gpu/renderer/textureCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/resourceMutex.h"
#include "graphics/host_gpu/renderer/tiler.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <tuple>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

constexpr uint64_t NumFramesBeforeRemoval = 32;

thread_local const TextureCache* g_locked_cache = nullptr;

class CacheLock final {
public:
	CacheLock(const TextureCache& owner, TrackingSpinLock& lock): m_lock(lock) {
		if (g_locked_cache != nullptr) {
			EXIT("TextureCache: recursive cache lock\n");
		}
		g_locked_cache = &owner;
		m_lock.lock();
	}
	~CacheLock() {
		m_lock.unlock();
		g_locked_cache = nullptr;
	}

private:
	TrackingSpinLock& m_lock;
};

} // namespace

TextureCache::TextureCache(GraphicContext& graphics, CommandScheduler& scheduler,
                           PageManager& page_manager, BufferCache& buffer_cache,
                           ResourceMutex& resource_mutex)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_memory_tracker(page_manager, PageWatchMode::Write), m_blit_helper(graphics, scheduler),
      m_tiler(std::make_unique<TileManager>(graphics, scheduler,
                                            buffer_cache.GetUtilityBuffer(MemoryUsage::Stream))),
      m_buffer_cache(buffer_cache), m_resource_mutex(resource_mutex),
      m_readback_linear_images(Config::ReadbackLinearImagesEnabled()) {
	if (m_graphics.CanReportMemoryUsage()) {
		constexpr int64_t GiB = 1024ll * 1024 * 1024;
		const auto        budget =
		    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
		const auto threshold = std::min<int64_t>(budget, 8 * GiB);
		m_pressure_gc_memory = static_cast<uint64_t>(
		    std::max<int64_t>(std::min(budget - 6 * threshold / 10, budget - GiB), GiB + GiB / 2));
		m_critical_gc_memory = static_cast<uint64_t>(
		    std::max<int64_t>(std::min(budget - 2 * threshold / 10, budget - GiB / 2), 3 * GiB));
		m_trigger_gc_memory = static_cast<uint64_t>(std::max<int64_t>((budget - threshold) / 2, 0));
	}
}

TextureCache::~TextureCache() {
	for (uint32_t index = 0; index < m_slots.size(); index++) {
		if (m_slots[index].image != nullptr && m_slots[index].image->registered) {
			UnregisterImage({index, m_slots[index].generation}, false);
		}
		m_slots[index].image.reset();
	}
}

bool TextureCache::SameBacking(const ImageInfo& cached, const ImageInfo& requested,
                               bool exact_format) {
	const bool unit_extent =
	    requested.extent.width == 1 && requested.extent.height == 1 && requested.extent.depth == 1;
	return cached.data == requested.data && cached.extent == requested.extent &&
	       cached.samples == requested.samples &&
	       cached.bytes_per_block == requested.bytes_per_block &&
	       (cached.type == requested.type || unit_extent) &&
	       (exact_format
	            ? cached.pixel_format == requested.pixel_format
	            : ImageViewOps::FormatsCompatible(cached.pixel_format, requested.pixel_format));
}

TextureCache::BindingType TextureCache::UploadBinding(const Image& image) {
	if (image.info.IsDepth()) {
		return BindingType::DepthTarget;
	}
	if (image.usage.render_target) {
		return BindingType::RenderTarget;
	}
	if (image.usage.video_out) {
		return BindingType::VideoOut;
	}
	return image.usage.storage ? BindingType::Storage : BindingType::Texture;
}

bool TextureCache::SafeToDownload(const Image& image) {
	if (!image.SafeToDownload()) {
		return false;
	}
	const auto range = image.info.data;
	return !m_buffer_cache.HasGpuDirtyBytes(range.address, range.size) &&
	       !m_memory_tracker.IsRegionCpuModified(range.address, range.size);
}

Image& TextureCache::ResolveImage(ImageId id) {
	if (!id || id.index >= m_slots.size()) {
		EXIT("TextureCache: invalid image id\n");
	}
	auto& slot = m_slots[id.index];
	if (slot.generation != id.generation || slot.image == nullptr) {
		EXIT("TextureCache: stale image id\n");
	}
	return *slot.image;
}

const Image& TextureCache::ResolveImage(ImageId id) const {
	if (!id || id.index >= m_slots.size()) {
		EXIT("TextureCache: invalid image id\n");
	}
	const auto& slot = m_slots[id.index];
	if (slot.generation != id.generation || slot.image == nullptr) {
		EXIT("TextureCache: stale image id\n");
	}
	return *slot.image;
}

std::shared_ptr<Image> TextureCache::ResolveOwner(ImageId id) const {
	if (!id || id.index >= m_slots.size()) {
		return {};
	}
	const auto& slot = m_slots[id.index];
	return slot.generation == id.generation ? slot.image : nullptr;
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
	uint32_t index = 0;
	if (m_free_slots.empty()) {
		index = static_cast<uint32_t>(m_slots.size());
		m_slots.emplace_back();
	} else {
		index = m_free_slots.back();
		m_free_slots.pop_back();
	}
	auto& slot = m_slots[index];
	if (slot.image != nullptr) {
		EXIT("TextureCache: occupied free image slot\n");
	}
	slot.image = std::make_shared<Image>(m_graphics, m_scheduler, info);
	const ImageId id {index, slot.generation};
	if (!info.data.Empty()) {
		RegisterImage(id);
	}
	return id;
}

void TextureCache::RegisterImage(ImageId id) {
	auto& image = ResolveImage(id);
	if (image.registered || image.info.data.Empty()) {
		EXIT("TextureCache: invalid image registration\n");
	}
	std::vector<ImageOwnerIndex::ByteRange> ranges;
	ranges.push_back({image.info.data.address, image.info.data.size});
	if (!m_image_owner_index.Register(id, ranges)) {
		EXIT("TextureCache: duplicate or invalid image registration\n");
	}
	image.registered = true;
	image.lru_id     = m_lru_cache.Insert(id, m_gc_tick);
	m_total_used_memory += image.AccountedSize();
}

void TextureCache::UnregisterImage(ImageId id, bool release_tracking) {
	auto& image = ResolveImage(id);
	if (!image.registered) {
		return;
	}
	std::vector<ImageOwnerIndex::ByteRange> releases;
	if (!m_image_owner_index.Unregister(id, releases)) {
		EXIT("TextureCache: image missing from owner index\n");
	}
	m_lru_cache.Free(image.lru_id);
	if (release_tracking) {
		for (const auto& range: releases) {
			m_memory_tracker.UntrackMemory(range.address, range.size);
		}
	}
	const auto accounted = image.AccountedSize();
	if (accounted > m_total_used_memory) {
		EXIT("TextureCache: image accounting underflow\n");
	}
	m_total_used_memory -= accounted;
	image.registered = false;
}

void TextureCache::DeleteImage(ImageId id, bool release_tracking) {
	auto owner = ResolveOwner(id);
	if (owner == nullptr || !owner->registered) {
		return;
	}
	if (!owner->depth_id) {
		std::vector<ImageId> associations;
		for (uint32_t index = 0; index < m_slots.size(); index++) {
			const auto& slot = m_slots[index];
			if (slot.image != nullptr && slot.image->depth_id == id) {
				associations.push_back({index, slot.generation});
			}
		}
		for (const auto association: associations) {
			ReleaseGpuTracking(association);
			DeleteImage(association);
		}
	}
	if (owner->IsGpuModified()) {
		EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\n");
	}
	m_download_images.erase(id);
	if (owner->info.metadata.kind == ImageMetadataKind::Htile) {
		m_surface_metas.erase(owner->info.metadata.range.address);
	}
	UnregisterImage(id, release_tracking);
	const auto erase_slot = [this, id, retained = owner] {
		auto& slot = m_slots[id.index];
		if (slot.generation != id.generation || slot.image != retained) {
			EXIT("TextureCache: retired image slot changed before deferred erasure\n");
		}
		slot.image.reset();
		if (++slot.generation == 0) {
			slot.generation = 1;
		}
		m_free_slots.push_back(id.index);
	};
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation(erase_slot);
	} else {
		erase_slot();
	}
}

void TextureCache::DeleteImages(std::span<const ImageId> ids,
                                std::optional<ImageId>   native_source) {
	std::set<std::pair<uint32_t, uint32_t>> unique;
	for (const auto id: ids) {
		if (!id || !unique.emplace(id.index, id.generation).second) {
			continue;
		}
		auto owner = ResolveOwner(id);
		if (owner == nullptr) {
			continue;
		}
		if (native_source == id) {
			ReleaseGpuTracking(id);
		} else if (owner->IsGpuModified()) {
			DownloadImage(id);
			ReleaseGpuTracking(id);
		}
		DeleteImage(id);
	}
}

void TextureCache::RetainImage(CommandBuffer& command, ImageId id) {
	auto owner = ResolveOwner(id);
	if (owner == nullptr) {
		EXIT("TextureCache: retaining a stale image\n");
	}
	if (owner->depth_id) {
		auto depth = ResolveOwner(owner->depth_id);
		if (depth == nullptr) {
			EXIT("TextureCache: stencil association points to a stale depth image\n");
		}
		command.RetainResourceUntilFence(std::move(depth));
	}
	command.RetainResourceUntilFence(std::move(owner));
}

void TextureCache::TouchImage(Image& image) {
	if (image.registered) {
		m_lru_cache.Touch(image.lru_id, m_gc_tick);
	}
}

void TextureCache::TrackImageDownload(ImageId id) {
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	auto&           image = ResolveImage(id);
	TrackImageDownloadLocked(id, image);
}

void TextureCache::TrackImageDownloadLocked(ImageId id, Image& image) {
	if (m_readback_linear_images && !image.info.IsTiled() && !image.info.data.Empty()) {
		if (!image.IsGpuModified()) {
			EXIT("TextureCache: cannot enroll a non-GPU-owned image for download\n");
		}
		m_download_images.insert(id);
	}
}

Image& TextureCache::GetImage(ImageId id) {
	auto& image = ResolveImage(id);
	TouchImage(image);
	return image;
}

const Image& TextureCache::GetImage(ImageId id) const {
	return ResolveImage(id);
}

std::vector<ImageId> TextureCache::FindImagesInRegion(uint64_t address, uint64_t size,
                                                      bool page_overlap) const {
	return page_overlap ? m_image_owner_index.QueryCandidates(address, size)
	                    : m_image_owner_index.Query(address, size);
}

ImageId TextureCache::GetNullImage(const ImageDesc& desc) {
	auto&      command = m_scheduler.Current();
	const auto format  = desc.info.pixel_format;
	if (const auto found = m_null_images.find(format); found != m_null_images.end()) {
		RetainImage(command, found->second);
		return found->second;
	}
	ImageInfo info {};
	info.pixel_format    = desc.info.pixel_format;
	info.guest_format    = desc.info.guest_format;
	info.type            = Prospero::ImageType::kColor2D;
	info.extent          = {1, 1, 1};
	info.resources       = {1, 1};
	info.pitch           = 1;
	info.bytes_per_block = std::max(desc.info.bytes_per_block, 1u);
	info.samples         = 1;
	info.tile_mode       = Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
	info.mip_layout[0]   = {0, info.bytes_per_block, 1, 1};
	const auto id        = InsertImage(info);
	m_null_images.emplace(format, id);
	RetainImage(command, id);
	return id;
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
	const auto range = image.info.data;
	m_memory_tracker.ForEachUploadRange(
	    range.address, range.size, false, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::RefreshCopySource(ImageId id) {
	auto& image = ResolveImage(id);
	RefreshImage(id, ImageDesc {.info = image.info, .view_info = {}, .type = UploadBinding(image)});
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
	std::vector<vk::BufferImageCopy> depth_copies(layers);
	std::vector<vk::BufferImageCopy> color_copies(layers);
	for (uint32_t layer = 0; layer < layers; layer++) {
		depth_copies[layer].bufferOffset      = depth_slice * layer;
		depth_copies[layer].bufferRowLength   = depth.info.pitch;
		depth_copies[layer].bufferImageHeight = depth.info.extent.height;
		depth_copies[layer].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, layer, 1};
		depth_copies[layer].imageExtent       = depth.info.extent;
		color_copies[layer].bufferOffset      = color_slice * layer;
		color_copies[layer].bufferRowLength   = color.info.pitch;
		color_copies[layer].bufferImageHeight = color.info.extent.height;
		color_copies[layer].imageSubresource  = {vk::ImageAspectFlagBits::eColor, 0, layer, 1};
		color_copies[layer].imageExtent       = color.info.extent;
	}

	auto                         depth_buffer = m_tiler->GetScratchBuffer(depth_size);
	auto                         color_buffer = m_tiler->GetScratchBuffer(color_size);
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
		m_tiler->ConvertD16(depth_buffer, color_buffer, TileManager::D16Direction::Demote, d32,
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
		m_tiler->ConvertD16(color_buffer, depth_buffer, TileManager::D16Direction::Promote, d32,
		                    promote_layout);
		destination.Upload(depth_copies, depth_buffer.buffer, depth_buffer.offset,
		                   depth_buffer.size);
	}
	return true;
}

void TextureCache::CopyImage(ImageId destination_id, ImageId source_id) {
	RefreshCopySource(source_id);
	auto& destination = ResolveImage(destination_id);
	auto& source      = ResolveImage(source_id);
	if (source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: cannot issue an unequal-sample image copy\n");
	}
	PrepareImageCopy(destination);
	if (source.IsBufferModified()) {
		if (source.info.data == destination.info.data) {
			destination.MarkBufferModified();
		}
		RestoreGpuTracking(destination);
		return;
	}
	const bool source_depth = source.info.IsDepth();
	const bool dest_depth   = destination.info.IsDepth();
	const bool direct_copy =
	    source.backing.format == destination.backing.format ||
	    (!source_depth && !dest_depth &&
	     vk::blockSize(source.backing.format) == vk::blockSize(destination.backing.format));
	if (direct_copy) {
		destination.CopyImage(source);
	} else if (!CopyD16(destination, source)) {
		if (source.backing.samples != 1 || destination.backing.samples != 1) {
			EXIT("TextureCache: cross-format multisample image copy is unsupported\n");
		}
		auto& copy_buffer = m_buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
		destination.CopyImageWithBuffer(source, copy_buffer);
	}
	RetainImage(m_scheduler.Current(), source_id);
	RetainImage(m_scheduler.Current(), destination_id);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
	RestoreGpuTracking(destination);
}

void TextureCache::CopyImageMip(ImageId destination_id, ImageId source_id, uint32_t mip,
                                uint32_t layer) {
	RefreshCopySource(source_id);
	auto& destination = ResolveImage(destination_id);
	auto& source      = ResolveImage(source_id);
	if (source.IsBufferModified() || source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: invalid mip-copy ownership or sample count\n");
	}
	destination.CopyMip(source, mip, layer);
	RetainImage(m_scheduler.Current(), source_id);
	RetainImage(m_scheduler.Current(), destination_id);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	RestoreGpuTracking(destination);
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cached_id) {
	auto& cached = ResolveImage(cached_id);
	if (!cached.info.IsDepth() && !requested.IsDepth()) {
		return {};
	}
	const bool stencil_match = requested.HasStencil() == cached.info.HasStencil();
	const bool bpp_match     = requested.bytes_per_block == cached.info.bytes_per_block;
	bool       recreate      = cached.info.resources < requested.resources;
	switch (binding) {
		case BindingType::Texture: recreate |= requested.IsDepth() && !cached.info.IsDepth(); break;
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
	RefreshImage(cached_id,
	             ImageDesc {.info = cached.info, .view_info = {}, .type = UploadBinding(cached)});
	auto info                 = requested;
	info.resources            = std::max(requested.resources, cached.info.resources);
	info.htile_clear_mask     = 0;
	const auto replacement_id = InsertImage(info);
	auto&      replacement    = ResolveImage(replacement_id);
	replacement.usage         = cached.usage;
	if (cached.binding.is_bound || cached.binding.is_target) {
		cached.binding.needs_rebind = true;
	}
	bool copied = false;
	if (cached.backing.samples == replacement.backing.samples) {
		const bool copy_supported =
		    cached.backing.samples == 1 || cached.backing.format == replacement.backing.format ||
		    (!cached.info.IsDepth() && !replacement.info.IsDepth() &&
		     ImageViewOps::FormatsCompatible(cached.backing.format, replacement.backing.format));
		if (copy_supported) {
			CopyImage(replacement_id, cached_id);
			copied = true;
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
		auto& command = m_scheduler.Current();
		RetainImage(command, cached_id);
		RetainImage(command, replacement_id);
		CommitGpuWrite(replacement);
		copied = true;
	} else {
		LOGF_COLOR(Log::Color::BrightYellow,
		           "TextureCache: unsupported unequal-sample depth overlap copy (%u -> %u)\n",
		           cached.backing.samples, replacement.backing.samples);
	}
	if (copied) {
		DeleteImages(std::array {cached_id}, cached_id);
	} else {
		ReleaseGpuTracking(cached_id);
		DeleteImage(cached_id);
	}
	return replacement_id;
}

TextureCache::OverlapResult TextureCache::ResolveOverlap(const ImageInfo& requested,
                                                         BindingType binding, ImageId cached_id,
                                                         ImageId merged_id) {
	auto owner = ResolveOwner(cached_id);
	if (owner == nullptr) {
		return {merged_id};
	}
	auto&      cached       = *owner;
	const auto current_tick = m_scheduler.CurrentTick();
	const bool safe_to_delete =
	    current_tick - std::min(current_tick, cached.tick_accessed_last) > NumFramesBeforeRemoval;

	if (requested.data.address == cached.info.data.address) {
		const uint32_t requested_block = requested.bytes_per_block * requested.samples;
		const uint32_t cached_block    = cached.info.bytes_per_block * cached.info.samples;
		if (requested.BlockExtent() != cached.info.BlockExtent() ||
		    requested_block != cached_block) {
			if (safe_to_delete) {
				DeleteImages(std::array {cached_id}, cached_id);
			}
			return {merged_id};
		}

		if (const auto depth_id = ResolveDepthOverlap(requested, binding, cached_id)) {
			return {depth_id};
		}
		if (requested.IsBlock() && !cached.info.IsBlock()) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.data.size == cached.info.data.size &&
		    (requested.IsVolume() || cached.info.IsVolume())) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.pixel_format != cached.info.pixel_format ||
		    requested.data.size <= cached.info.data.size) {
			const auto result_id = merged_id ? merged_id : cached_id;
			const auto result    = ResolveOwner(result_id);
			return {result != nullptr && ImageViewOps::FormatsCompatible(result->info.pixel_format,
			                                                             requested.pixel_format)
			            ? result_id
			            : ImageId {}};
		}
		if (requested.type == cached.info.type && requested.resources > cached.info.resources) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.tile_mode != cached.info.tile_mode) {
			if (safe_to_delete) {
				DeleteImages(std::array {cached_id}, cached_id);
			}
			return {merged_id};
		}
		EXIT("TextureCache: unresolvable equal-address image overlap, address=0x%016" PRIx64
		     " requested=%ux%u "
		     "cached=%ux%u requested_size=0x%016" PRIx64 " cached_size=0x%016" PRIx64
		     " type=%u/%u tile=%u/%u\n",
		     requested.data.address, requested.resources.levels, requested.resources.layers,
		     cached.info.resources.levels, cached.info.resources.layers, requested.data.size,
		     cached.info.data.size, static_cast<uint32_t>(requested.type),
		     static_cast<uint32_t>(cached.info.type), requested.tile_mode, cached.info.tile_mode);
	}

	if (requested.data.address > cached.info.data.address) {
		const int32_t mip = requested.MipOf(cached.info);
		if (mip >= 0) {
			const int32_t layer = requested.SliceOf(cached.info, mip);
			if (layer >= 0) {
				return {cached_id, mip, layer};
			}
		}
		if (safe_to_delete) {
			DeleteImages(std::array {cached_id}, cached_id);
		}
		return {};
	}

	const int32_t mip = cached.info.MipOf(requested);
	if (mip >= 0) {
		const int32_t layer = cached.info.SliceOf(requested, mip);
		if (layer >= 0) {
			if (cached.binding.is_target) {
				cached.binding.needs_rebind = true;
				if (merged_id) {
					ResolveImage(merged_id).binding.is_target = true;
				}
				DeleteImages(std::array {cached_id}, cached_id);
				return {merged_id};
			}
			if (merged_id) {
				CopyImageMip(merged_id, cached_id, static_cast<uint32_t>(mip),
				             static_cast<uint32_t>(layer));
				DeleteImages(std::array {cached_id}, cached_id);
			}
		}
	}
	return {merged_id};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId source_id) {
	RefreshCopySource(source_id);
	const auto expanded_id = InsertImage(info);
	auto&      expanded    = ResolveImage(expanded_id);
	auto&      source      = ResolveImage(source_id);
	expanded.usage         = source.usage;
	if (source.binding.is_bound || source.binding.is_target) {
		source.binding.needs_rebind = true;
	}
	InitializeImage(expanded_id,
	                ImageDesc {.info = info, .view_info = {}, .type = UploadBinding(source)});
	CopyImage(expanded_id, source_id);
	DeleteImages(std::array {source_id}, source_id);
	return expanded_id;
}

struct TextureCache::ColorTransferPlan {
	TextureUploadLayout              layout;
	std::vector<vk::BufferImageCopy> regions;
	std::vector<GpuTileInfo>         tiles;
	bool                             tiled       = false;
	bool                             swap_bgra16 = false;
	bool                             valid       = false;
};

struct TextureCache::DownloadPlan {
	ColorTransferPlan color;
	bool              depth = false;
	bool              valid = false;
};

TextureCache::ColorTransferPlan
TextureCache::BuildColorTransfer(const Image& image, BindingType binding,
                                 TransferDirection direction) const {
	const auto& info             = image.info;
	uint32_t    format           = info.guest_format;
	uint32_t    layers           = info.TransferLayers();
	bool        volume           = info.IsVolume();
	bool        layered          = info.IsLayered();
	bool        allow_depth_tile = direction == TransferDirection::Upload;
	const char* owner =
	    direction == TransferDirection::Upload ? "TextureCache" : "TextureCache readback";

	ColorTransferPlan plan;
	if (direction == TransferDirection::Upload) {
		switch (binding) {
			case BindingType::Texture: break;
			case BindingType::Storage: owner = "StorageTextureCache"; break;
			case BindingType::RenderTarget:
			case BindingType::VideoOut:
				if (info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
				    info.samples != 1 || image.backing.samples != 1 ||
				    (binding == BindingType::VideoOut &&
				     info.metadata.compression != VideoOutCompression::Uncompressed)) {
					EXIT("TextureCache: invalid color-attachment upload\n");
				}
				format           = binding == BindingType::RenderTarget
				                       ? ImageOps::RenderTargetTransferFormat(info.bytes_per_block)
				                       : info.guest_format;
				layers           = info.resources.layers;
				volume           = false;
				layered          = layers > 1;
				allow_depth_tile = false;
				plan.swap_bgra16 = info.bgra16;
				owner = binding == BindingType::RenderTarget ? "RenderTarget" : "VideoOut";
				break;
			case BindingType::DepthTarget: return plan;
		}
	} else {
		if (binding == BindingType::DepthTarget) {
			return plan;
		}
		format           = binding == BindingType::RenderTarget
		                       ? ImageOps::RenderTargetTransferFormat(info.bytes_per_block)
		                       : info.guest_format;
		allow_depth_tile = binding == BindingType::Storage;
		plan.swap_bgra16 = info.bgra16;
	}

	plan.layout = TextureCalcUploadLayout(format, info.extent.width, info.extent.height,
	                                      info.resources.levels, layers, info.pitch, info.tile_mode,
	                                      info.data.size, allow_depth_tile, volume, owner);
	plan.regions = TextureBuildImageCopies(plan.layout, info.extent.width, info.extent.height,
	                                       layers, info.resources.levels, layered, volume);
	plan.tiled   = static_cast<Prospero::TileMode>(plan.layout.tile) != Prospero::TileMode::kLinear;
	if (plan.tiled) {
		// CMASK/FMASK decoding is intentionally deferred.
		if (plan.layout.tile_family == TileBlockFamily::Depth64KB &&
		    Prospero::IsFmaskTextureFormat(format)) {
			return plan;
		}
		if (!TextureBuildGpuTileInfos(info.data.size, plan.regions, plan.layout, format, layers,
		                              info.resources.levels, plan.tiles)) {
			return plan;
		}
	}
	plan.valid = true;
	return plan;
}

TextureCache::DownloadPlan TextureCache::BuildDownload(const Image& image) const {
	const auto&  info = image.info;
	DownloadPlan plan {.depth = info.IsDepth()};
	if (info.samples != 1 || image.backing.samples != 1) {
		return plan;
	}
	if (plan.depth) {
		plan.valid = IsSupportedDepthPlaneReadback(info) && info.resources.layers != 0 &&
		             info.data.size % info.resources.layers == 0 &&
		             Prospero::NumBytesPerElement(info.guest_format) == info.bytes_per_block;
		return plan;
	}
	if (info.metadata.compression != VideoOutCompression::Uncompressed) {
		return plan;
	}
	plan.color = BuildColorTransfer(image, UploadBinding(image), TransferDirection::Download);
	plan.valid = plan.color.valid;
	return plan;
}

void TextureCache::UploadImage(Image& image, const ImageDesc& desc, Buffer& source,
                               uint64_t source_offset) {
	const auto& info   = image.info;
	const auto  upload = [&](std::vector<vk::BufferImageCopy>& copies, TileManager::Result linear) {
		for (auto& copy: copies) {
			copy.bufferOffset += linear.offset;
		}
		image.Upload(copies, linear.buffer, linear.offset, linear.size);
	};

	if (desc.type != BindingType::DepthTarget) {
		auto plan = BuildColorTransfer(image, desc.type, TransferDirection::Upload);
		EXIT_NOT_IMPLEMENTED(!plan.valid);
		TileManager::Result linear {source.Handle(), source_offset, info.data.size};
		if (plan.tiled) {
			linear = m_tiler->Detile(source.Handle(), source_offset, info.data.size, info.data.size,
			                         plan.tiles);
		}
		if (plan.swap_bgra16) {
			linear = m_tiler->SwapBgra16(linear);
		}
		upload(plan.regions, linear);
		return;
	}

	if (desc.type != BindingType::DepthTarget || info.samples != 1 || image.backing.samples != 1 ||
	    info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block) {
		EXIT("TextureCache: invalid depth upload\n");
	}
	TileBlockLayout block {};
	EXIT_NOT_IMPLEMENTED(
	    !TileGetBlockLayout(TileBlockFamily::Depth64KB, info.bytes_per_block, block));
	const auto                       layers          = info.resources.layers;
	const auto                       full_slice_size = info.data.size / layers;
	std::vector<GpuTileInfo>         tiles;
	std::vector<vk::BufferImageCopy> copies(layers);
	tiles.reserve(layers);
	for (uint32_t layer = 0; layer < layers; layer++) {
		const uint64_t offset  = full_slice_size * layer;
		auto&          copy    = copies[layer];
		copy.bufferOffset      = offset;
		copy.bufferRowLength   = info.pitch;
		copy.bufferImageHeight = info.extent.height;
		copy.imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, layer, 1};
		copy.imageExtent       = {info.extent.width, info.extent.height, 1};
		if (static_cast<Prospero::TileMode>(info.tile_mode) != Prospero::TileMode::kLinear) {
			tiles.push_back({block.family, block.bytes_per_element, offset, full_slice_size, offset,
			                 full_slice_size, 0, info.extent.width, info.extent.height, 1,
			                 info.pitch});
			tiles.back().surface_z = layer;
		}
	}
	TileManager::Result linear {source.Handle(), source_offset, source.Size() - source_offset};
	if (!tiles.empty()) {
		linear =
		    m_tiler->Detile(source.Handle(), source_offset, info.data.size, info.data.size, tiles);
	}
	const auto transfer_bytes = DepthAspectTransferBytes(info.pixel_format);
	if (transfer_bytes != info.bytes_per_block) {
		const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
		EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
		                     transfer_bytes != sizeof(uint32_t) || texels_per_slice > UINT32_MAX ||
		                     texels_per_slice > UINT64_MAX / transfer_bytes);
		const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
		EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
		auto promoted = m_tiler->GetScratchBuffer(transfer_slice * layers);
		m_tiler->ConvertD16(
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
		for (uint32_t layer = 0; layer < layers; layer++) {
			copies[layer].bufferOffset = transfer_slice * layer;
		}
	}
	upload(copies, linear);
}

void TextureCache::InitializeImage(ImageId id, const ImageDesc& desc) {
	auto& image = ResolveImage(id);
	if (image.info.data.Empty()) {
		return;
	}
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		m_memory_tracker.ForEachUploadRange(
		    image.info.data.address, image.info.data.size, false,
		    [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
		if (image.IsCpuDirty()) {
			image.RefreshComplete();
		}
		return;
	}
	if (image.info.samples > 1) {
		RestoreGpuTracking(image);
		return;
	}
	bool data_gpu_owned = false;
	bool data_imported  = false;
	bool uploaded       = false;
	m_memory_tracker.ForEachUploadRange(
	    image.info.data.address, image.info.data.size, false,
	    [&](uint64_t, uint64_t) noexcept { uploaded = true; },
	    [&]() noexcept {
		    uploaded |= image.IsBufferModified() || image.IsDefinitelyCpuDirty();
		    if (!uploaded) {
			    return;
		    }
		    const auto source =
		        m_buffer_cache.ObtainBufferForImage(image.info.data.address, image.info.data.size);
		    if (source.buffer == nullptr) {
			    EXIT("TextureCache: failed to obtain image upload source\n");
		    }
		    data_gpu_owned |= source.gpu_owned;
		    data_imported = true;
		    UploadImage(image, desc, *source.buffer, source.offset);
	    });
	if (data_imported) {
		image.ClearBufferModified();
	}
	if (data_gpu_owned) {
		image.MarkGpuModified();
	}
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	RestoreGpuTracking(image);
}

void TextureCache::RefreshImage(ImageId id, const ImageDesc& desc) {
	auto& image           = ResolveImage(id);
	bool  unchanged_maybe = false;
	if (image.IsMaybeCpuDirty()) {
		const auto hash = image.HashGuestEdges();
		if (image.NeedsMaybeCpuHash()) {
			image.SetMaybeCpuHash(hash);
			return;
		}
		unchanged_maybe = !image.ResolveMaybeCpuHash(hash);
		if (unchanged_maybe) {
			m_memory_tracker.ForEachUploadRange(
			    image.info.data.address, image.info.data.size, false,
			    [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
		}
	}
	bool cpu_dirty = image.IsBufferModified() || image.IsDefinitelyCpuDirty();
	if (!unchanged_maybe) {
		cpu_dirty |=
		    m_memory_tracker.IsRegionCpuModified(image.info.data.address, image.info.data.size);
	}
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (cpu_dirty) {
			EXIT("TextureCache: compressed guest image refresh is unsupported\n");
		}
		RestoreGpuTracking(image);
		return;
	}
	if (!cpu_dirty) {
		RestoreGpuTracking(image);
		return;
	}
	InitializeImage(id, desc);
}

void TextureCache::AssociateStencil(ImageId depth_id, GuestRange stencil) {
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	AssociateStencilLocked(depth_id, stencil);
}

void TextureCache::AssociateStencilLocked(ImageId depth_id, GuestRange stencil) {
	if (!stencil.Valid()) {
		EXIT("TextureCache: invalid stencil association range\n");
	}
	auto& depth = ResolveImage(depth_id);
	if (!depth.info.IsDepth() || !depth.info.HasStencil()) {
		EXIT("TextureCache: stencil association requires a depth/stencil image\n");
	}

	ImageId association {};
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		const auto owner = ResolveOwner(id);
		if (owner != nullptr && owner->info.data.address == stencil.address) {
			association = id;
		}
	}
	if (!association) {
		ImageInfo info {};
		info.data   = stencil;
		info.extent = depth.info.extent;
		association = InsertImage(info);
	}
	auto& record = ResolveImage(association);
	TouchImage(record);
	record.AssociateDepth(depth_id);
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_format) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		EXIT("TextureCache: image lookup requires a valid command buffer\n");
	}
	ValidateImageDesc(desc);
	if (desc.info.data.Empty()) {
		CacheLock lock(*this, m_lock);
		return GetNullImage(desc);
	}

	ImageId result {};
	bool    replacement_buffer = false;
	bool    replacing_image    = false;
	{
		std::lock_guard            transaction(m_resource_mutex);
		CacheLock                  lock(*this, m_lock);
		const std::vector<ImageId> candidates =
		    FindImagesInRegion(desc.info.data.address, desc.info.data.size, false);

		for (const auto id: candidates) {
			const auto owner = ResolveOwner(id);
			if (owner == nullptr || owner->info.data != desc.info.data) {
				continue;
			}
			if (SameBacking(owner->info, desc.info, exact_format)) {
				result = id;
			}
		}

		int32_t view_mip   = -1;
		int32_t view_layer = -1;
		if (!result) {
			for (const auto candidate: candidates) {
				view_mip         = -1;
				view_layer       = -1;
				const auto owner = ResolveOwner(candidate);
				if (owner == nullptr) {
					continue;
				}
				const auto merged_info = result ? ResolveImage(result).info : desc.info;
				const auto overlap     = ResolveOverlap(merged_info, desc.type, candidate, result);
				if (overlap.image) {
					result     = overlap.image;
					view_mip   = overlap.mip;
					view_layer = overlap.layer;
				}
			}
		}

		if (result) {
			auto& resolved = ResolveImage(result);
			if (exact_format && resolved.info.pixel_format != desc.info.pixel_format) {
				result = {};
			} else if (resolved.info.resources < desc.info.resources) {
				ImageDesc refresh {
				    .info = resolved.info, .view_info = {}, .type = UploadBinding(resolved)};
				RefreshImage(result, refresh);
				if (resolved.IsGpuModified() && !SynchronizeImageToBuffer(result)) {
					EXIT("TextureCache: cannot preserve an unsupported replacement image\n");
				}
				replacement_buffer = resolved.IsBufferModified();
				DeleteImage(result);
				result          = {};
				replacing_image = true;
			}
		}
		if (!result) {
			result         = InsertImage(desc.info);
			auto& inserted = ResolveImage(result);
			if (replacement_buffer || m_buffer_cache.HasGpuDirtyBytes(inserted.info.data.address,
			                                                          inserted.info.data.size)) {
				inserted.MarkBufferModified();
			} else if (replacing_image) {
				m_memory_tracker.MarkRegionAsCpuModified(inserted.info.data.address,
				                                         inserted.info.data.size);
			}
			InitializeImage(result, desc);
		} else {
			RefreshImage(result, desc);
		}

		auto& image = ResolveImage(result);
		if (desc.type == BindingType::VideoOut &&
		    desc.info.metadata.compression != VideoOutCompression::Uncompressed) {
			const bool guest_dirty =
			    image.IsBufferModified() || image.IsCpuDirty() ||
			    m_memory_tracker.IsRegionCpuModified(image.info.data.address, image.info.data.size);
			const bool native_current =
			    (image.usage.render_target || image.IsGpuModified()) && !guest_dirty;
			if (!native_current) {
				EXIT("TextureCache: compressed video-out read requires clean native GPU "
				     "contents\n");
			}
		}
		if (view_mip >= 0) {
			desc.view_info.base_level = static_cast<uint32_t>(view_mip);
		}
		if (view_layer >= 0) {
			desc.view_info.base_layer = static_cast<uint32_t>(view_layer);
		}
		image.tick_accessed_last = m_scheduler.CurrentTick();
		TouchImage(image);
		RetainImage(command, result);
	}
	return result;
}

ImageId TextureCache::FindImageFromRange(uint64_t address, uint64_t size, bool ensure_valid) {
	if (!GuestRange {address, size}.Valid()) {
		return {};
	}
	CacheLock            lock(*this, m_lock);
	std::vector<ImageId> matches;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = ResolveOwner(id);
		if (owner == nullptr || owner->info.data.address != address) {
			continue;
		}
		if (ensure_valid && owner->depth_id) {
			owner = ResolveOwner(owner->depth_id);
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
			const auto& image = ResolveImage(id);
			if (image.info.data.size == size) {
				selected = id;
				break;
			}
		}
	}
	if (selected) {
		if (ensure_valid) {
			const auto owner = ResolveOwner(selected);
			if (owner != nullptr && owner->depth_id) {
				selected = owner->depth_id;
			}
		}
		RetainImage(m_scheduler.Current(), selected);
	}
	return selected;
}

vk::ImageView TextureCache::FindTexture(ImageId id, const ImageDesc& desc) {
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	auto&           image = ResolveImage(id);
	TouchImage(image);
	if (!image.info.data.Empty()) {
		if (!image.registered || image.depth_id || image.binding.needs_rebind) {
			EXIT("TextureCache: texture requires rediscovery before final acquisition\n");
		}
		RefreshImage(id, desc);
	}
	switch (desc.type) {
		case BindingType::Texture: break;
		case BindingType::Storage:
			if (image.info.data.Empty()) {
				image.MarkGpuModified();
			} else {
				if (!image.registered || image.depth_id) {
					EXIT("TextureCache: cannot acquire an unavailable storage image\n");
				}
				CommitGpuWrite(image);
			}
			TrackImageDownloadLocked(id, image);
			break;
		default: EXIT("TextureCache: invalid texture binding\n");
	}
	const auto view = image.FindView(desc.view_info);
	RetainImage(m_scheduler.Current(), id);
	return view;
}

vk::ImageView TextureCache::FindRenderTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::RenderTarget) {
		EXIT("TextureCache: invalid color-target binding\n");
	}
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	auto&           image = ResolveImage(id);
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: color target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	RefreshImage(id, desc);
	CommitGpuWrite(image);
	image.usage.render_target = true;
	TrackImageDownloadLocked(id, image);
	const auto view = image.FindView(desc.view_info);
	RetainImage(m_scheduler.Current(), id);
	return view;
}

vk::ImageView TextureCache::FindDepthTarget(ImageId id, const ImageDesc& desc) {
	if (desc.type != BindingType::DepthTarget) {
		EXIT("TextureCache: invalid depth-target binding\n");
	}
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	auto&           image = ResolveImage(id);
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: depth target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	RefreshImage(id, desc);
	if (desc.info.HasMetadata()) {
		image.info.metadata = desc.info.metadata;
		m_surface_metas.try_emplace(desc.info.metadata.range.address,
		                            MetaDataInfo {.clear_mask = image.info.htile_clear_mask});
	}
	CommitGpuWrite(image);
	image.usage.depth_target = true;
	if (desc.info.HasStencil()) {
		AssociateStencilLocked(id, desc.info.stencil);
	}
	const auto view = image.FindView(desc.view_info);
	RetainImage(m_scheduler.Current(), id);
	return view;
}

void TextureCache::MarkGpuWritten(ImageId id) {
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	auto&           image = ResolveImage(id);
	if (!image.registered || image.depth_id) {
		EXIT("TextureCache: cannot mark an unavailable image GPU-written\n");
	}
	CommitGpuWrite(image);
}

void TextureCache::CommitGpuWrite(Image& image) {
	if (image.depth_id || image.backing.image == nullptr) {
		EXIT("TextureCache: stencil association cannot own image contents\n");
	}
	const auto range = image.info.data;
	if (m_buffer_cache.HasGpuDirtyBytes(range.address, range.size)) {
		m_buffer_cache.DiscardGpuDirtyBytes(range.address, range.size);
	}
	m_buffer_cache.InvalidateImageAliases(range.address, range.size);
	image.ClearBufferModified();
	m_memory_tracker.ForEachUploadRange(
	    range.address, range.size, true, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	image.MarkGpuModified();
	RestoreGpuTracking(image);
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
                                        uint32_t packed_clear) {
	if (command.IsInvalid() || !GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid image clear\n");
	}
	m_buffer_cache.ValidateGpuAccess(address, size, false, true);
	std::lock_guard      transaction(m_resource_mutex);
	CacheLock            lock(*this, m_lock);
	ImageId              selected {};
	vk::ImageAspectFlags aspect {};
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = ResolveOwner(id);
		if (owner == nullptr) {
			continue;
		}
		vk::ImageAspectFlags candidate {};
		ImageId              candidate_id = id;
		if (owner->depth_id && owner->info.data.address == address &&
		    owner->info.data.size == size) {
			candidate    = vk::ImageAspectFlagBits::eStencil;
			candidate_id = owner->depth_id;
			owner        = ResolveOwner(candidate_id);
			if (owner == nullptr || owner->backing.image == nullptr || !owner->info.HasStencil()) {
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
		if (selected && selected != candidate_id) {
			return false;
		}
		selected = candidate_id;
		aspect   = candidate;
	}
	if (!selected) {
		return false;
	}
	auto&               image = ResolveImage(selected);
	vk::ClearColorValue color_clear {};
	float               depth_clear   = 0.0f;
	uint8_t             stencil_clear = 0;
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		if (!DecodePackedColorClear(image.info.pixel_format, packed_clear, color_clear)) {
			return false;
		}
	} else {
		if ((aspect == vk::ImageAspectFlagBits::eDepth &&
		     !DecodePackedDepthClear(image.info.pixel_format, packed_clear, depth_clear)) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil &&
		     !DecodePackedStencilClear(packed_clear, stencil_clear))) {
			return false;
		}
	}
	if (m_buffer_cache.HasGpuDirtyBytes(address, size)) {
		m_buffer_cache.DiscardGpuDirtyBytes(address, size);
	}
	if (image.IsBufferModified() || image.IsCpuDirty() ||
	    m_memory_tracker.IsRegionCpuModified(image.info.data.address, image.info.data.size)) {
		ImageDesc refresh {.info = image.info, .view_info = {}, .type = UploadBinding(image)};
		InitializeImage(selected, refresh);
		if (image.info.samples == 1 && (image.IsBufferModified() || image.IsCpuDirty())) {
			EXIT("TextureCache: image clear retained guest ownership\n");
		}
	}
	command.EndRendering();
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	const vk::ImageSubresourceRange range {aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
	                                       image.backing.layers};
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                                 &color_clear, 1, &range);
	} else {
		const vk::ClearDepthStencilValue clear {depth_clear, stencil_clear};
		command.Handle().clearDepthStencilImage(
		    image.backing.image, vk::ImageLayout::eTransferDstOptimal, &clear, 1, &range);
	}
	CommitGpuWrite(image);
	RetainImage(command, selected);
	return true;
}

void TextureCache::PrepareHostWrite(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid host-write range\n");
	}
	CacheLock lock(*this, m_lock);
	InvalidateCpuAliases(address, size);
	m_memory_tracker.ForEachDownloadRange<true>(address, size, [](uint64_t, uint64_t) noexcept {});
	m_memory_tracker.MarkRegionAsCpuModified(address, size);
}

void TextureCache::DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset) {
	const auto&    info             = image.info;
	const auto     layers           = info.resources.layers;
	const auto     full_slice_size  = info.data.size / layers;
	const auto     transfer_bytes   = DepthAspectTransferBytes(info.pixel_format);
	const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
	EXIT_NOT_IMPLEMENTED(transfer_bytes == 0 || texels_per_slice > UINT32_MAX ||
	                     texels_per_slice > UINT64_MAX / transfer_bytes ||
	                     texels_per_slice > UINT64_MAX / info.bytes_per_block);
	const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
	const uint64_t guest_slice    = texels_per_slice * info.bytes_per_block;
	EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
	const uint64_t transfer_size = transfer_slice * layers;
	EXIT_NOT_IMPLEMENTED(guest_slice > full_slice_size);
	std::vector<vk::BufferImageCopy> copies(layers);
	for (uint32_t layer = 0; layer < layers; layer++) {
		auto& copy             = copies[layer];
		copy.bufferOffset      = full_slice_size * layer;
		copy.bufferRowLength   = info.pitch;
		copy.bufferImageHeight = info.extent.height;
		copy.imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, layer, 1};
		copy.imageExtent       = {info.extent.width, info.extent.height, 1};
	}
	if (transfer_bytes == info.bytes_per_block) {
		if (!info.IsTiled()) {
			for (auto& copy: copies) {
				copy.bufferOffset += destination_offset;
			}
			image.Download(copies, destination.Handle(), destination_offset, info.data.size);
			return;
		}
		TileBlockLayout block {};
		EXIT_NOT_IMPLEMENTED(
		    !TileGetBlockLayout(TileBlockFamily::Depth64KB, info.bytes_per_block, block));
		std::vector<GpuTileInfo> tiles;
		tiles.reserve(layers);
		for (uint32_t layer = 0; layer < layers; layer++) {
			const uint64_t offset = full_slice_size * layer;
			tiles.push_back({block.family, block.bytes_per_element, offset, full_slice_size, offset,
			                 full_slice_size, 0, info.extent.width, info.extent.height, 1,
			                 info.pitch});
			tiles.back().surface_z = layer;
		}
		m_tiler->TileImage(image, copies, destination.Handle(), destination_offset, info.data.size,
		                   info.data.size, tiles);
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	for (uint32_t layer = 0; layer < layers; layer++) {
		copies[layer].bufferOffset = transfer_slice * layer;
	}
	auto host_linear = m_tiler->GetScratchBuffer(transfer_size);
	image.Download(copies, host_linear.buffer, 0, host_linear.size);
	const bool tiled        = info.IsTiled();
	auto       guest_linear = tiled ? m_tiler->GetScratchBuffer(info.data.size)
	                                : TileManager::Result {destination.Handle(), destination_offset,
	                                                       destination.Size() - destination_offset};
	m_tiler->ConvertD16(host_linear, guest_linear, TileManager::D16Direction::Demote,
	                    DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat,
	                    {.width             = info.extent.width,
	                     .height            = info.extent.height,
	                     .layers            = layers,
	                     .source_row_stride = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
	                     .target_row_stride = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
	                     .source_slice_stride = transfer_slice,
	                     .target_slice_stride = full_slice_size});
	if (!tiled) {
		return;
	}
	TileBlockLayout block {};
	EXIT_NOT_IMPLEMENTED(
	    !TileGetBlockLayout(TileBlockFamily::Depth64KB, info.bytes_per_block, block));
	std::vector<GpuTileInfo> tiles;
	tiles.reserve(layers);
	for (uint32_t layer = 0; layer < layers; layer++) {
		const uint64_t offset = full_slice_size * layer;
		tiles.push_back({block.family, block.bytes_per_element, offset, full_slice_size, offset,
		                 full_slice_size, 0, info.extent.width, info.extent.height, 1, info.pitch});
		tiles.back().surface_z = layer;
	}
	m_tiler->Tile(guest_linear.buffer, guest_linear.offset, info.data.size, destination.Handle(),
	              destination_offset, info.data.size, tiles);
}

void TextureCache::DownloadImageData(Image& image, Buffer& destination, uint64_t destination_offset,
                                     DownloadPlan plan) {
	if (!plan.valid) {
		EXIT("TextureCache: invalid image download plan\n");
	}
	if (plan.depth) {
		DownloadDepth(image, destination, destination_offset);
		return;
	}

	auto&      color     = plan.color;
	const auto transform = color.swap_bgra16 ? TileManager::ColorTransform::SwapBgra16
	                                         : TileManager::ColorTransform::None;
	if (!color.tiled) {
		if (transform == TileManager::ColorTransform::SwapBgra16) {
			auto linear = m_tiler->GetScratchBuffer(image.info.data.size);
			image.Download(color.regions, linear.buffer, 0, linear.size);
			m_tiler->SwapBgra16(linear,
			                    {destination.Handle(), destination_offset, image.info.data.size});
			return;
		}
		for (auto& copy: color.regions) {
			copy.bufferOffset += destination_offset;
		}
		image.Download(color.regions, destination.Handle(), destination_offset,
		               image.info.data.size);
		return;
	}

	m_tiler->TileImage(image, color.regions, destination.Handle(), destination_offset,
	                   image.info.data.size, image.info.data.size, color.tiles, transform);
}

std::pair<uint8_t*, uint64_t> TextureCache::MapDownload(uint64_t size, uint64_t alignment) {
	if (size == 0) {
		EXIT("TextureCache: cannot map an empty image download\n");
	}
	auto& download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto  mapping  = download.Map(size, std::max<uint64_t>(alignment, 4));
	if (mapping.first == nullptr) {
		EXIT("TextureCache: failed to map reusable download buffer\n");
	}
	download.Commit();
	return mapping;
}

void TextureCache::QueueDownload(GuestRange range, StreamBuffer& download, uint8_t* mapped,
                                 uint64_t offset) {
	vk::BufferMemoryBarrier barrier {};
	barrier.sType         = vk::StructureType::eBufferMemoryBarrier;
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
	const auto tick = m_scheduler.CurrentTick();
	m_buffer_cache.BeginBackingPublication(range.address, range.size, tick);
	m_scheduler.DeferPriorityOperation([this, &download, range, mapped, offset, tick] {
		download.Invalidate(offset, range.size);
		LibKernel::Memory::WriteBacking(range.address, mapped, range.size);
		m_buffer_cache.CompleteBackingPublication(range.address, range.size, tick);
	});
}

bool TextureCache::TryDownloadImage(ImageId id) {
	auto& image = ResolveImage(id);
	if (image.depth_id) {
		return false;
	}
	auto plan = BuildDownload(image);
	if (!plan.valid || !SafeToDownload(image)) {
		return false;
	}
	const auto range      = image.info.data;
	auto [mapped, offset] = MapDownload(range.size, image.info.bytes_per_block);
	auto& download        = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	if (!LibKernel::Memory::TryReadBacking(range.address, mapped, range.size)) {
		return false;
	}
	download.Flush(offset, range.size);

	DownloadImageData(image, download, offset, std::move(plan));

	QueueDownload(range, download, mapped, offset);
	return true;
}

void TextureCache::DownloadImage(ImageId id) {
	if (!TryDownloadImage(id)) {
		EXIT("TextureCache: unsupported image readback\n");
	}
	m_scheduler.FinishCurrent();
	m_scheduler.DrainPriorityOperations();
}

bool TextureCache::SynchronizeImageToBuffer(ImageId id) {
	auto& image = ResolveImage(id);
	if (image.depth_id) {
		return true;
	}
	auto plan = BuildDownload(image);
	if (!plan.valid) {
		return false;
	}
	const auto range   = image.info.data;
	const bool refresh = image.IsDefinitelyCpuDirty() ||
	                     m_memory_tracker.IsRegionCpuModified(range.address, range.size);
	if (refresh) {
		RefreshImage(id,
		             ImageDesc {.info = image.info, .view_info = {}, .type = UploadBinding(image)});
	}
	if (!image.IsGpuModified()) {
		return true;
	}
	if (image.IsDefinitelyCpuDirty() || image.IsBufferModified() ||
	    m_memory_tracker.IsRegionCpuModified(range.address, range.size)) {
		EXIT("TextureCache: image mirror source is not native-current\n");
	}
	auto [destination, offset] =
	    m_buffer_cache.ObtainBufferForImageWrite(range.address, range.size);
	if (destination == nullptr) {
		EXIT("TextureCache: failed to allocate image mirror\n");
	}
	DownloadImageData(image, *destination, offset, std::move(plan));
	m_scheduler.Current().RetainResourceUntilFence(destination);
	m_buffer_cache.PublishImageBuffer(range.address, range.size);
	image.MarkBufferModified();
	RetainImage(m_scheduler.Current(), id);
	ReleaseGpuTracking(id);
	return true;
}

bool TextureCache::SynchronizeImageToBuffer(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return false;
	}
	CacheLock lock(*this, m_lock);
	ImageId   selected {};
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = ResolveOwner(id);
		if (owner == nullptr || !owner->GpuOverlaps(address, size)) {
			continue;
		}
		if (selected) {
			EXIT("TextureCache: ambiguous image-to-buffer synchronization\n");
		}
		selected = id;
	}
	if (!selected) {
		return false;
	}
	return SynchronizeImageToBuffer(selected);
}

bool TextureCache::InvalidateMemoryFromGPU(uint64_t address, uint64_t size,
                                           bool formatted_buffer_write) {
	if (!GuestRange {address, size}.Valid()) {
		return false;
	}
	CacheLock lock(*this, m_lock);
	bool      found = false;
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = ResolveOwner(id);
		if (owner == nullptr || owner->depth_id || !owner->Overlaps(address, size)) {
			continue;
		}
		if (owner->IsGpuModified()) {
			if (!formatted_buffer_write) {
				EXIT("TextureCache: buffer write aliases GPU-modified image\n");
			}
			ReleaseGpuTracking(id);
		}
		owner->MarkBufferModified();
		found = true;
	}
	return found;
}

TextureCache::RegionInfo TextureCache::QueryRegion(uint64_t address, uint64_t size) {
	RegionInfo result {};
	if (!GuestRange {address, size}.Valid()) {
		return result;
	}
	CacheLock lock(*this, m_lock);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = ResolveOwner(id);
		if (owner == nullptr || owner->depth_id || !owner->Overlaps(address, size, true)) {
			continue;
		}
		result.image_pages = true;
		result.image_bytes |= owner->Overlaps(address, size);
		result.gpu_image_bytes |= owner->GpuOverlaps(address, size);
	}
	return result;
}

void TextureCache::InvalidateCpuAliases(uint64_t address, uint64_t size) {
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = ResolveOwner(id);
		if (owner == nullptr || owner->depth_id) {
			continue;
		}
		owner->InvalidateCpuWrite(address, size);
		if (owner->NeedsMaybeCpuHash()) {
			owner->SetMaybeCpuHash(owner->HashGuestEdges());
		}
	}
}

void TextureCache::RestoreGpuTracking(const Image& image) {
	if (!image.IsGpuModified()) {
		return;
	}
	constexpr uint64_t page_mask = TRACKER_PAGE_SIZE - 1;
	const auto         range     = image.info.data;
	const auto         begin     = range.address & ~page_mask;
	const auto         end       = (range.End() + page_mask) & ~page_mask;
	for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
		if (!m_memory_tracker.IsRegionGpuModified(page, TRACKER_PAGE_SIZE) &&
		    !m_memory_tracker.IsRegionCpuModified(page, TRACKER_PAGE_SIZE)) {
			m_memory_tracker.MarkRegionAsGpuModified(page, TRACKER_PAGE_SIZE);
		}
	}
}

void TextureCache::ReleaseGpuTracking(ImageId id) {
	auto owner = ResolveOwner(id);
	if (owner == nullptr || !owner->IsGpuModified()) {
		return;
	}
	const auto released = owner->info.data;
	owner->ClearGpuModified();
	m_memory_tracker.ForEachDownloadRange<true>(released.address, released.size,
	                                            [](uint64_t, uint64_t) noexcept {});
	for (const auto candidate: FindImagesInRegion(released.address, released.size, true)) {
		const auto survivor = ResolveOwner(candidate);
		if (survivor != nullptr && survivor.get() != owner.get() && !survivor->depth_id) {
			RestoreGpuTracking(*survivor);
		}
	}
	RestoreGpuTracking(*owner);
}

bool TextureCache::IsMeta(uint64_t address) {
	CacheLock lock(*this, m_lock);
	return m_surface_metas.contains(address);
}

bool TextureCache::IsMetaCleared(uint64_t address, uint32_t slice) {
	CacheLock  lock(*this, m_lock);
	const auto found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	return (found->second.clear_mask & (1u << slice)) != 0;
}

bool TextureCache::ClearMeta(uint64_t address) {
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	const auto      found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	found->second.clear_mask = UINT32_MAX;
	return true;
}

bool TextureCache::TouchMeta(uint64_t address, uint32_t slice, bool is_clear) {
	CacheLock  lock(*this, m_lock);
	const auto found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	if (is_clear) {
		found->second.clear_mask |= 1u << slice;
	} else {
		found->second.clear_mask &= ~(1u << slice);
	}
	return true;
}

bool TextureCache::InvalidateMemory(PageFaultAccess access, uint64_t address, uint64_t size,
                                    PageFaultPhase phase) noexcept {
	if ((access != PageFaultAccess::Read && access != PageFaultAccess::Write) ||
	    !GuestRange {address, size}.Valid()) {
		return false;
	}
	if (access == PageFaultAccess::Read) {
		return false;
	}
	if (phase == PageFaultPhase::Invalidate) {
		const bool gpu_image =
		    m_memory_tracker.InvalidateVirtualGpuWrite(access, address, size, phase);
		CpuFaultAction action = gpu_image ? CpuFaultAction::Download
		                                  : m_memory_tracker.BeginCpuFault(address, size, access);
		{
			CacheLock lock(*this, m_lock);
			InvalidateCpuAliases(address, size);
		}
		return action != CpuFaultAction::Untracked;
	}

	if (phase == PageFaultPhase::Complete) {
		const bool gpu_image = m_memory_tracker.IsRegionGpuModified(address, size);
		return gpu_image ? m_memory_tracker.InvalidateVirtualGpuWrite(access, address, size, phase)
		                 : m_memory_tracker.CompleteCpuFault(address, size, access, false);
	}
	if (phase != PageFaultPhase::Release) {
		return false;
	}
	(void)m_memory_tracker.InvalidateVirtualGpuWrite(access, address, size, phase);
	return true;
}

void TextureCache::UnmapMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid unmap range\n");
	}
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	auto            images = FindImagesInRegion(address, size, false);
	if (!images.empty()) {
		m_scheduler.Finish();
	}
	for (const auto id: images) {
		auto owner = ResolveOwner(id);
		if (owner == nullptr) {
			continue;
		}
		if (owner->IsGpuModified()) {
			ReleaseGpuTracking(id);
		}
		DeleteImage(id);
	}
	m_memory_tracker.UntrackMemory(address, size);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		if (const auto survivor = ResolveOwner(id); survivor != nullptr) {
			RestoreGpuTracking(*survivor);
		}
	}
}

void TextureCache::RunGarbageCollector() {
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	const uint64_t  tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}
	const auto collect = [&](bool allow_aggressive) {
		bool           pressured  = m_total_used_memory >= m_pressure_gc_memory;
		bool           aggressive = allow_aggressive && m_total_used_memory >= m_critical_gc_memory;
		const uint64_t age       = std::min<uint64_t>(aggressive ? 160 : pressured ? 80 : 16, tick);
		size_t         deletions = aggressive ? 40 : pressured ? 20 : 10;
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
			auto owner = ResolveOwner(id);
			if (owner == nullptr || !owner->registered || owner->depth_id) {
				continue;
			}
			if (owner->IsGpuModified()) {
				const bool safe = SafeToDownload(*owner);
				if (safe && owner->info.IsTiled()) {
					continue;
				}
				if (safe && !pressured) {
					continue;
				}
				if (safe && !TryDownloadImage(id)) {
					continue;
				}
				ReleaseGpuTracking(id);
			}
			DeleteImage(id);
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

void TextureCache::ProcessDownloadImages() {
	std::lock_guard transaction(m_resource_mutex);
	CacheLock       lock(*this, m_lock);
	for (const auto id: m_download_images) {
		const auto owner = ResolveOwner(id);
		if (owner != nullptr && owner->registered && owner->IsGpuModified()) {
			(void)TryDownloadImage(id);
		}
	}
	m_download_images.clear();
}

} // namespace Libs::Graphics
