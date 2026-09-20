#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memoryHeadroom.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/tiler.h"

#include <atomic>
#include <bitset>
#include <map>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class Buffer;
class BufferCache;
class CommandBuffer;
class CommandScheduler;
class RenderExecutor;
class TextureCache;
struct TextureCacheTestAccess;
struct TextureDownloadChunk;

namespace BcAlias {
[[nodiscard]] bool Matches(const ImageInfo& requested, const ImageInfo& cached);
[[nodiscard]] bool Publish(TextureCache& cache, Image& image);
void Resolve(TextureCache& cache, ImageId cached, ImageId merged);
} // namespace BcAlias

class TextureCache {
public:
	enum class BindingType : uint8_t { Texture, Storage, RenderTarget, DepthTarget, VideoOut };

	struct ImageDesc {
		ImageInfo     info;
		ImageViewInfo view_info;
		BindingType   type = BindingType::Texture;
	};

	TextureCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	             BufferCache& buffer_cache);
	~TextureCache();
	KYTY_CLASS_NO_COPY(TextureCache);

	[[nodiscard]] ImageId       FindImage(ImageDesc& desc, bool exact_format = false);
	void                        UpdateImage(ImageId id);
	[[nodiscard]] ImageId       FindImageFromRange(uint64_t address, uint64_t size,
	                                               bool ensure_valid = true);
	[[nodiscard]] vk::ImageView FindTexture(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindRenderTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindDepthTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] Image&        GetImage(ImageId id) {
		auto& image = m_slot_images[id];
		TouchImage(image);
		return image;
	}
	// A write that left the stencil plane alone must not claim it; another surface may share it.
	void MarkGpuWritten(ImageId id, bool stencil_written = true);

	// Called once per presented frame; image staleness is judged against it, not submissions.
	void AdvanceFrame() noexcept { m_frame_index.fetch_add(1, std::memory_order_relaxed); }

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionRegistered(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t address, uint64_t size);

	// Inside a registered metadata surface, not only at its base: layers are cleared one by one.
	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsWithinMeta(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	// Marks only the whole, slice-aligned slices a CP-DMA fill or compute clear covers.
	[[nodiscard]] bool ClearMetaSlices(uint64_t address, uint64_t size);
	// A shader store inside a metadata surface supersedes the pending clears it can reach.
	void               DiscardMetaClears(uint64_t address, uint64_t size);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void RunGarbageCollector(bool per_frame = false);

private:
	void FreePublishedEvictions();

	struct MetaDataInfo {
		// Footprint of the registered surface; the registry is keyed by base address alone.
		uint64_t range_size = 0;
		uint32_t slices     = 0;
		// PS5 DB_DEPTH_VIEW encodes 13-bit base and last array-slice indices.
		std::bitset<8192> clear_mask;
	};

	// The registered surface whose footprint holds the address. Caller holds m_lock.
	std::map<uint64_t, MetaDataInfo>::iterator FindMetaContaining(uint64_t address);

	struct OverlapResult {
		ImageId image;
		int32_t mip   = -1;
		int32_t layer = -1;
	};

	using ImageIds       = InlinePageOwnerList<ImageId, 16>;
	using ImagePageTable = MultiLevelPageTable<ImageIds, 20, 44, 14>;

	// Callers have validated the nonempty 44-bit range with TryGetPageRange.
	template <typename Func>
	static void ForEachPage(uint64_t address, size_t size, Func&& func) {
		using FuncReturn = typename std::invoke_result<Func, uint64_t>::type;
		static constexpr bool RETURNS_BOOL = std::is_same_v<FuncReturn, bool>;
		const uint64_t page_end = (address + size - 1) >> ImagePageTable::kPageBits;
		for (uint64_t page = address >> ImagePageTable::kPageBits; page <= page_end; ++page) {
			if constexpr (RETURNS_BOOL) {
				if (func(page)) {
					break;
				}
			} else {
				func(page);
			}
		}
	}

	[[nodiscard]] ImageId     InsertImage(const ImageInfo& info);
	void                      RegisterImage(ImageId id);
	void                      UnregisterImage(ImageId id);
	void                      DeleteImage(ImageId id);
	void                      FreeImage(ImageId id);
	void                      TouchImage(Image& image);
	void                      TrackImage(ImageId id);
	void                      TrackImageHead(ImageId id);
	void                      TrackImageTail(ImageId id);
	void                      UntrackImage(ImageId id);
	void                      UntrackImageHead(ImageId id);
	void                      UntrackImageTail(ImageId id);
	void                      MarkAsMaybeDirty(ImageId id, Image& image);
	void                      TrackImageDownload(ImageId id, Image& image);
	[[nodiscard]] static bool SameGuestLayout(const ImageInfo& cached, const ImageInfo& requested);
	[[nodiscard]] static bool SameBacking(const ImageInfo& cached, const ImageInfo& requested,
	                                      bool exact_format);
	[[nodiscard]] static BindingType UploadBinding(const Image& image);
	// `with_download_plan` asks whether a write-back could be built; only the collector needs it.
	[[nodiscard]] Headroom::CollectorImageFacts CollectorFacts(const Image& image,
	                                                           bool         with_download_plan);
	// Frees what can be freed with no GPU work. Caller holds m_lock; returns accounted bytes.
	[[nodiscard]] uint64_t ReclaimForAllocation(uint64_t needed);

	// Caller holds m_lock; it also serializes the per-image query epoch.
	[[nodiscard]] ImageIds      FindImagesInRegion(uint64_t address, uint64_t size,
	                                               bool page_overlap) const;
	[[nodiscard]] ImageId       StencilPlaneOwner(const ImageDesc& desc) const;
	[[nodiscard]] OverlapResult ResolveOverlap(const ImageInfo& requested, BindingType binding,
	                                           ImageId cached, ImageId merged);
	[[nodiscard]] ImageId       ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
	                                                ImageId cached);
	[[nodiscard]] ImageId       ExpandImage(const ImageInfo& info, ImageId source);
	void                        RefreshImage(ImageId id);
	void                        MaterializeColorClear(ImageId id, const ImageDesc& desc,
	                                                uint32_t metadata_base_layer);
	void                        InitializeImage(ImageId id);
	[[nodiscard]] bool          CanDownload(const Image& image) const;
	void UploadImage(Image& image, Buffer& source, uint64_t source_offset);
	void DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
	                   uint64_t destination_size, uint32_t levels);
	// Displaces an unresolvable overlap victim: parks it when guest memory cannot reproduce it.
	void                  RetireOverlap(ImageId id, bool abandoned);
	[[nodiscard]] bool    ParkImage(ImageId id);
	bool                  UnparkImage(ImageId id);
	void                  WakeImage(ImageId id, const ImageIds& candidates);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	void               DownloadColorRegions(Image& image, std::vector<vk::BufferImageCopy>& regions,
	void               DownloadDepthRegions(Image& image, std::vector<vk::BufferImageCopy>& regions,
	                                        Buffer& destination, uint64_t destination_offset,
	                                        uint64_t destination_size);
	void CommitGpuWrite(Image& image);
	// Caller holds m_lock. Volume layer ranges select depth slices.
	void ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
	                const vk::ImageSubresourceRange& range, const vk::ClearValue& clear);
	// Caller holds m_lock and ends rendering. Begins rendering `layers` layers of one color mip
	// through a `format` view, clearing them or, when `clear` is null, loading them; returns the
	// render area.
	vk::Rect2D BeginColorRendering(Image& image, vk::Format format, uint32_t level,
	                               uint32_t layer, uint32_t layers, const vk::ClearValue* clear,
	                               vk::CommandBuffer command);
	void PrepareImageCopy(Image& image);
	void RefreshCopySource(ImageId id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	[[nodiscard]] ImageId AssociateStencil(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	void ValidateImageDesc(const ImageDesc& desc) const;

	void               InvalidateCpuAliases(uint64_t address, uint64_t size);
	[[nodiscard]] bool DownloadImageMemory(ImageId id);
	// An empty chunk region list downloads the whole transfer in one batch.
	[[nodiscard]] bool DownloadImageBatch(Image& image, const TextureDownloadChunk& chunk,
	                                      uint64_t alignment);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	std::atomic<uint64_t>                             m_frame_index {0};
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	// Evicted images stay registered until their download lands; m_evict_published has a leaf lock.
	std::unordered_map<ImageId, uint64_t>  m_evict_pending;
	std::unordered_map<ImageId, uint64_t>  m_evict_backoff;
	std::unordered_map<uint64_t, uint64_t> m_evicted_addresses;
	std::mutex                             m_evict_published_lock;
	std::vector<ImageId>                   m_evict_published;
	std::unordered_map<uint64_t, ImageId>  m_stencil_planes;
	uint64_t                               m_gc_frame           = UINT64_MAX;
	mutable uint32_t m_image_query_epoch      = 0;
	bool             m_readback_linear_images = false;
	bool             m_readback_tiled_images  = false;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
	friend class ImageHeap;
	friend bool BcAlias::Matches(const ImageInfo&, const ImageInfo&);
	friend bool BcAlias::Publish(TextureCache&, Image&);
	friend void BcAlias::Resolve(TextureCache&, ImageId, ImageId);
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
