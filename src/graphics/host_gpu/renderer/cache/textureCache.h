#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "common/smallVector.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/tiler.h"

#include <algorithm>
#include <bitset>
#include <map>
#include <mutex>
#include <optional>
#include <type_traits>
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

namespace BcAlias {
[[nodiscard]] bool Matches(const ImageInfo& requested, const ImageInfo& cached);
[[nodiscard]] bool Publish(TextureCache& cache, Image& image);
void               Resolve(TextureCache& cache, ImageId cached, ImageId merged);
} // namespace BcAlias

class TextureCache {
public:
	enum class BindingType : uint8_t { Texture, Storage, RenderTarget, DepthTarget, VideoOut };

	struct ImageDesc {
		ImageInfo     info;
		ImageViewInfo view_info;
		BindingType   type = BindingType::Texture;
		bool          heap = false; // PS5: resolved for a resident image heap.
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
	void MarkGpuWritten(ImageId id);

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	/// Invalidates images for a CPU write treated as running front to back over the range, such as
	/// a DMA fill or copy, as its page-by-page faults on a fully watched range would have.
	void InvalidateSequentialWrite(uint64_t address, uint64_t size);
	void InvalidateMemoryFromGPU(uint64_t address, uint64_t size);
	void InvalidateBufferWrite(uint64_t address, uint64_t size);
	// Writes a copy into whole levels of one layer of a GPU-written image straight into it, and
	// returns false for any other copy.
	[[nodiscard]] bool CopyToImage(uint64_t address, uint64_t source, uint64_t size);
	[[nodiscard]] bool IsRegionRegistered(uint64_t address, uint64_t size);

	[[nodiscard]] bool                 IsMeta(uint64_t address);
	[[nodiscard]] bool                 IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool                 ClearMeta(uint64_t address);
	[[nodiscard]] bool                 TouchMeta(uint64_t address, uint32_t slice, bool is_clear);
	[[nodiscard]] vk::ImageAspectFlags TakeMetaClear(uint64_t address, uint32_t slice,
	                                                 vk::ImageAspectFlags aspects);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	// With storage readback, a GL2 writeback makes GPU-written images visible to the CPU; returns
	// whether a download was recorded.
	[[nodiscard]] bool WriteBackImages();
	// The tick of the latest write-back pending over the range's pages, or 0 when none is.
	[[nodiscard]] uint64_t WriteBackTick(uint64_t address, uint64_t size);
	void                   RunGarbageCollector();

private:
	struct MetaDataInfo {
		// PS5 DB_DEPTH_VIEW encodes 13-bit base and last array-slice indices.
		std::bitset<8192> clear_mask;
		std::bitset<8192> stencil_clear_mask;
		// The depth image that last acquired this HTile range.
		ImageId owner;
		// Guest depth surface last bound with this HTile. One surface can use several HTile
		// allocations, such as depth-only and depth-stencil variants.
		uint64_t depth_address = 0;
	};

	struct OverlapResult {
		ImageId image;
		int32_t mip   = -1;
		int32_t layer = -1;
	};

	// A recorded image download whose guest pages stay inaccessible until it lands.
	struct WriteBack {
		GuestRange range;
		uint64_t   tick;
	};

	// PS5: guest addresses span 44 bits, so an entry keeps the whole image address.
	struct BucketEntry {
		uint64_t address;
		uint32_t size;
		ImageId  id;

		[[nodiscard]] bool Overlaps(uint64_t addr, size_t size) const noexcept {
			return address < addr + size && addr < address + this->size;
		}
	};

	struct alignas(64) Bucket {
		Common::SmallVector<BucketEntry, 3, uint32_t> entries;
	};
	static_assert(sizeof(Bucket) == 64);

	using ImagePageTable = MultiLevelPageTable<Bucket, 20, 44, 14>;

	// Callers pass a nonempty, valid guest range.
	template <typename Func>
	static void ForEachPage(uint64_t address, size_t size, Func&& func) {
		static constexpr bool RETURNS_BOOL =
		    std::is_same_v<std::invoke_result_t<Func, uint64_t>, bool>;
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

	// The walks below iterate the page entries in place: func must not insert, register,
	// unregister or free images. Caller holds m_lock.

	// Calls func once per image overlapping the region, from the first page both share; a func
	// returning true stops the walk.
	template <typename Func>
	void ForEachImageInRegion(uint64_t address, size_t size, Func&& func) {
		static constexpr bool BOOL_BREAK =
		    std::is_same_v<std::invoke_result_t<Func, ImageId, Image&>, bool>;
		ImagePageTable::PageRange pages {};
		if (!ImagePageTable::TryGetPageRange(address, size, pages)) {
			return;
		}
		const uint64_t first_page = address >> ImagePageTable::kPageBits;
		ForEachPage(address, size, [&](uint64_t page) {
			const auto* bucket = m_image_page_table.Find(page);
			if (bucket == nullptr) {
				if constexpr (BOOL_BREAK) {
					return false;
				} else {
					return;
				}
			}
			for (const auto& entry: bucket->entries) {
				const uint64_t base_page = entry.address >> ImagePageTable::kPageBits;
				if (page != std::max(first_page, base_page)) {
					continue;
				}
				if (!entry.Overlaps(address, size)) {
					continue;
				}
				Image& image = m_slot_images[entry.id];
				if constexpr (BOOL_BREAK) {
					if (func(entry.id, image)) {
						return true;
					}
				} else {
					func(entry.id, image);
				}
			}
			if constexpr (BOOL_BREAK) {
				return false;
			}
		});
	}

	// Calls func for each image registered at address.
	template <typename Func>
	void ForEachImageWithAddress(uint64_t address, Func&& func) {
		const auto* bucket = m_image_page_table.Find(address >> ImagePageTable::kPageBits);
		if (bucket == nullptr) {
			return;
		}
		for (const auto& entry: bucket->entries) {
			if (entry.address == address) {
				func(entry.id, m_slot_images[entry.id]);
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
	[[nodiscard]] static bool SameBacking(const ImageInfo& cached, const ImageInfo& requested,
	                                      bool exact_format);
	[[nodiscard]] static BindingType UploadBinding(const Image& image);
	// Colour attachment uploads need whole layers of single sample images. Other bindings pass.
	[[nodiscard]] static bool IsValidColorAttachmentUpload(const Image& image, BindingType binding);

	// Whether the image was last used too long ago to be replaced. It counts scheduler ticks, or
	// frames while the submit cadence makes the ticks too fast to mean frames.
	[[nodiscard]] bool          AccessedLongAgo(const Image& image) const;
	[[nodiscard]] OverlapResult ResolveOverlap(const ImageInfo& requested, BindingType binding,
	                                           ImageId cached, ImageId merged);
	void                        RetireReplacedLayout(ImageId cached, ImageId merged);
	[[nodiscard]] ImageId       ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
	                                                ImageId cached);
	[[nodiscard]] ImageId       ExpandImage(const ImageInfo& info, ImageId source);
	[[nodiscard]] bool          RetireThroughGuestMemory(ImageId replacement, ImageId retired);
	void                        RefreshImage(ImageId id);
	// Whether every viewed metadata slice is already checked for the keys. The slices are one
	// range lookup when they touch, and otherwise are looked up one by one.
	[[nodiscard]] bool IsColorClearChecked(uint64_t address, uint64_t slice_size, uint64_t mip_size,
	                                       uint32_t count, uint32_t keys) const;
	// Marks every viewed metadata slice checked for the keys, as one range when the slices touch.
	void MarkColorClearChecked(uint64_t address, uint64_t slice_size, uint64_t mip_size,
	                           uint32_t count, uint32_t keys);
	void MaterializeColorClear(ImageId id, const ImageDesc& desc, uint32_t metadata_base_layer);
	void InitializeImage(ImageId id);
	[[nodiscard]] bool CanDownload(const Image& image) const;
	void               UploadImage(Image& image, Buffer& source, uint64_t source_offset);
	void               DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
	                                 uint64_t destination_size, uint32_t levels);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	void CommitGpuWrite(Image& image);
	void ReleaseHtileOwner(ImageId id, const ImageMetadataInfo& released);
	// Caller holds m_lock. Volume layer ranges select depth slices.
	void ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
	                const vk::ImageSubresourceRange& range, const vk::ClearValue& clear);
	// Caller holds m_lock and ends rendering. Begins rendering `layers` layers of one color mip
	// through a `format` view, clearing them or, when `clear` is null, loading them; returns the
	// render area.
	vk::Rect2D BeginColorRendering(Image& image, vk::Format format, uint32_t level, uint32_t layer,
	                               uint32_t layers, const vk::ClearValue* clear,
	                               vk::CommandBuffer command);
	void       RefreshCopySource(ImageId id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	// The depth image's stencil plane record covering exactly range, or none.
	[[nodiscard]] ImageId FindStencilPlane(const Image& depth, GuestRange range) const;
	void                  CopyStencil(ImageId destination, ImageId source);
	[[nodiscard]] ImageId AssociateStencil(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	void ValidateImageDesc(const ImageDesc& desc) const;

	// Caller holds m_lock. A sequential write marks only the images its per-page faults reach.
	void InvalidateCpuAliases(uint64_t address, uint64_t size, bool sequential_write = false);
	// Caller holds m_lock. Refresh only the buffer-written range of an image, returning false
	// when the full upload has to run instead.
	[[nodiscard]] bool TryPartialRefresh(Image& image);
	void               RefreshLinearRange(Image& image, GuestRange range);
	// What a tiled partial refresh touches. The range is relative to the image data.
	struct TiledRefreshPlan {
		RefreshByteRange           range;
		std::vector<RefreshRegion> regions;
	};
	[[nodiscard]] static std::optional<TiledRefreshPlan> PlanTiledRefresh(const Image& image,
	                                                                      GuestRange   range);
	[[nodiscard]] bool RefreshTiledRange(Image& image, GuestRange range);
	void               MergeTiledRegions(Image& image, const TiledRefreshPlan& plan);
	void               DownloadForMerge(Image& image, const TileManager::Result& scratch,
	                                    std::span<const RefreshRegion> regions);
	void               UploadMerged(Image& image, const TileManager::Result& scratch,
	                                std::span<const RefreshRegion> regions);
	// Caller holds m_lock. With unlocked_copy, m_lock is released while the guest bytes are copied
	// and the download is dropped if the image changed meanwhile, so the caller must not keep
	// references or iterators into the cache across the call.
	[[nodiscard]] bool DownloadImageMemory(ImageId id, bool unlocked_copy = false);
	[[nodiscard]] bool DownloadQueuedImages();

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::mutex                                        m_write_back_mutex;
	std::vector<WriteBack>                            m_write_backs;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t m_critical_gc_memory = 3ull * 1024 * 1024 * 1024;
	// Written and read without the lock only on the GPU thread.
	uint64_t m_gc_tick                 = 0;
	bool     m_readback_linear_images  = false;
	bool     m_readback_tiled_images   = false;
	bool     m_readback_storage_images = false;
	bool     m_partial_buffer_refresh  = false;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
	friend class ImageHeap;
	friend bool BcAlias::Matches(const ImageInfo&, const ImageInfo&);
	friend bool BcAlias::Publish(TextureCache&, Image&);
	friend void BcAlias::Resolve(TextureCache&, ImageId, ImageId);
};

// Decodes a color metadata code into its clear color. Exposed so tests can compare it with the
// key mask.
[[nodiscard]] bool DecodeColorClear(const TextureCache::ImageDesc& desc, uint8_t code,
                                    vk::ClearColorValue& clear);
// Bit key is set when the clear code key << ColorClearKeyShift selects a clear for the image.
[[nodiscard]] uint32_t ColorClearKeys(const TextureCache::ImageDesc& desc);
// Whether the viewed metadata slices tile one range. A partial mip leaves gaps between slices
// that are never marked, and fewer than two slices gain nothing from a combined lookup.
[[nodiscard]] constexpr bool ColorClearSlicesContiguous(uint64_t mip_size, uint64_t slice_size,
                                                        uint32_t count) noexcept {
	return count >= 2 && mip_size == slice_size;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
