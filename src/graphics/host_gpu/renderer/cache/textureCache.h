#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/writeWatchSet.h"

#include <array>
#include <bitset>
#include <map>
#include <type_traits>
#include <unordered_set>
#include <unordered_map>
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
void Resolve(TextureCache& cache, ImageId cached, ImageId merged);
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

	// `plain`: set when the answer came from a plain backing match, which holds while the
	// image over its range changes (RangeUnchangedSince); ReuseImage gives it again.
	[[nodiscard]] ImageId       FindImage(ImageDesc& desc, bool exact_format = false,
	                                      bool* plain = nullptr);
	// The image FindImage found for `desc` by a plain match, used again: its access is noted.
	void                        ReuseImage(ImageId id, const ImageDesc& desc);
	void                        UpdateImage(ImageId id);
	[[nodiscard]] ImageId       FindImageFromRange(uint64_t address, uint64_t size,
	                                               bool ensure_valid = true);
	[[nodiscard]] vk::ImageView FindTexture(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindTexture(ImageId id, const ImageInfo& info,
	                                        const ImageViewInfo& view_info, BindingType type);
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
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size);
	// A guest thread is about to write CPU memory in the range. An image the GPU wrote there is
	// copied to guest memory first, and the caller waits for the copy: after the write the image
	// is uploaded again from that memory, which must hold what the GPU wrote.
	void               SaveGpuWrites(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionRegistered(uint64_t address, uint64_t size);

	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);
	// A guest uniform-fill dispatch wrote `value` over [address, address + size). If the value is a
	// metadata code, the next colour clear resolution for that range uses it instead of reading the
	// bytes back from the GPU. Call after the dispatch is recorded, so its own write is counted.
	void RecordColorMetadataFill(uint64_t address, uint64_t size, uint32_t value);
	// Voids every record, and every check of slices without a clear: a GPU write is coming that
	// no write watch will see.
	void DropColorMetadataFills();

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void RunGarbageCollector();

private:
	struct MetaDataInfo {
		// PS5 DB_DEPTH_VIEW encodes 13-bit base and last array-slice indices.
		std::bitset<8192> clear_mask;
	};

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
	// A compressed video-out image is read from its native contents only.
	void                      CheckVideoOutRead(ImageId id, const ImageDesc& desc);
	void                      RegisterImage(ImageId id);
	void                      UnregisterImage(ImageId id);
	void                      DeleteImage(ImageId id);
	void                      FreeImage(ImageId id);
	// While an expanded image is first filled: the levels and layers the copy from the smaller
	// image overwrites right after, which the upload leaves out.
	struct UploadSkip {
		uint32_t layers = 0;
		uint32_t levels = 0;
	};
	UploadSkip m_upload_skip;
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

	// Caller holds m_lock; it also serializes the per-image query epoch.
	[[nodiscard]] ImageIds      FindImagesInRegion(uint64_t address, uint64_t size,
	                                               bool page_overlap) const;
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
	// The image's last download to guest memory has not landed yet.
	[[nodiscard]] bool DownloadPending(const Image& image);
	void DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
	                   uint64_t destination_size, uint32_t levels);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
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

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	RecursiveTrackingLock                             m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	// Colour metadata codes the guest wrote with a fill dispatch, by metadata block address. Valid
	// while the write generation of the range is unchanged.
	struct ColorMetadataFill {
		uint64_t          size       = 0;
		uint8_t           code       = 0;
		WriteWatchSet::Id watch      = 0;
		uint64_t          generation = 0;
		// Sub-ranges (offset, size) the renderer has since made CPU memory holding 0xff, the code
		// for "no clear", when it consumed a slice's clear. The record does not speak for them:
		// a guest write to CPU memory is not seen by the watch.
		std::vector<std::pair<uint64_t, uint64_t>> consumed;

		// True when any byte of the sub-range was consumed.
		[[nodiscard]] bool IsConsumed(uint64_t offset, uint64_t bytes) const {
			for (const auto& [begin, length]: consumed) {
				if (offset < begin + length && begin < offset + bytes) {
					return true;
				}
			}
			return false;
		}
	};
	std::map<uint64_t, ColorMetadataFill> m_color_metadata_fills;
	// A record is a cache: dropping one costs a read-back, so the map is simply emptied when a
	// guest fills more distinct ranges than any set of render targets needs.
	static constexpr size_t MaxColorMetadataFills = 1024;
	using ColorMetadataFillMap                    = std::map<uint64_t, ColorMetadataFill>;
	ColorMetadataFillMap::iterator DropColorMetadataFill(ColorMetadataFillMap::iterator fill);
	void                           DropColorMetadataFillsLocked();
	// Colour metadata slices whose bytes held no clear for an image, by metadata block range.
	// While nothing writes the range (the write generation is unchanged and every CPU write would
	// fault), binding the image again finds the same, and MaterializeColorClear returns early.
	struct ColorMetadataChecked {
		ImageId           image;
		uint64_t          size         = 0;
		uint32_t          first        = 0;
		uint32_t          count        = 0;
		// What DecodeColorClear reads of the description.
		vk::Format        format               = vk::Format::eUndefined;
		uint64_t          clear_word           = 0;
		ImageMetadataKind kind                 = ImageMetadataKind::None;
		BindingType       type                 = BindingType::Texture;
		bool              clear_register_valid = false;
		bool              dcc_alpha_msb        = false;
		WriteWatchSet::Id watch                = 0;
		uint64_t          generation           = 0;
		// A check from an earlier epoch is void (DropColorMetadataFills); its watch is kept.
		uint64_t          epoch                = 0;

		[[nodiscard]] bool SameSlices(const ColorMetadataChecked& other) const noexcept {
			return image == other.image && size == other.size && first == other.first &&
			       count == other.count && format == other.format &&
			       clear_word == other.clear_word && kind == other.kind && type == other.type &&
			       clear_register_valid == other.clear_register_valid &&
			       dcc_alpha_msb == other.dcc_alpha_msb;
		}
	};
	// Images can share a metadata block address with other sizes or descriptions: one record each.
	struct ColorMetadataCheckedKey {
		uint64_t address = 0;
		uint64_t size    = 0;
		ImageId  image;
		bool     operator==(const ColorMetadataCheckedKey&) const = default;
	};
	struct ColorMetadataCheckedHash {
		size_t operator()(const ColorMetadataCheckedKey& key) const noexcept {
			return std::hash<uint64_t> {}(key.address ^ (key.size << 20u)) ^
			       (std::hash<ImageId> {}(key.image) * 0x9e3779b97f4a7c15ull);
		}
	};
	std::unordered_map<ColorMetadataCheckedKey, ColorMetadataChecked, ColorMetadataCheckedHash>
	    m_color_metadata_checked;
	uint64_t                                           m_color_metadata_checked_epoch = 1;
	static constexpr size_t MaxColorMetadataChecked = 4096;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	// Recent FindImagesInRegion answers. Image ranges do not change while an image is
	// registered, so an answer holds until an image over its range is registered or
	// unregistered (RangeUnchangedSince).
	struct RegionQuery {
		uint64_t             address      = 0;
		uint64_t             size         = 0;
		uint64_t             since        = UINT64_MAX;
		bool                 page_overlap = false;
		std::vector<ImageId> ids;
	};
	mutable std::array<RegionQuery, 64> m_region_queries {};
	// The ranges of the last ImageChangeLogSize registrations, unregistrations and changes of
	// images, in order. An answer about a range, made when m_image_change_count was `since`,
	// holds while none of the changes after it overlaps the range: images elsewhere, even next
	// to it, do not end it.
	struct ImageChange {
		uint64_t address = 0;
		uint64_t size    = 0;
	};
	static constexpr size_t                     ImageChangeLogSize = 256;
	std::array<ImageChange, ImageChangeLogSize> m_image_changes {};
	uint64_t                                    m_image_change_count = 0;
	void                                        NoteImageChange(uint64_t address, uint64_t size);
	// Moves `since` to the current count when the range is unchanged. False also when the log
	// no longer reaches back to `since`.
	[[nodiscard]] bool RangeUnchangedSince(uint64_t address, uint64_t size, uint64_t& since) const;
	// FindImage answers that came from a plain backing match. What decides such an answer is
	// the requested info and the images registered over its range, so it holds until an image
	// over the range changes.
	struct FindImageKey {
		uint64_t address         = 0;
		uint64_t size            = 0;
		uint32_t width           = 0;
		uint32_t height          = 0;
		uint32_t depth           = 0;
		uint32_t levels          = 0;
		uint32_t layers          = 0;
		uint32_t samples         = 0;
		uint32_t bytes_per_block = 0;
		uint32_t pixel_format    = 0;
		uint32_t guest_format    = 0;
		uint32_t tile_mode       = 0;
		uint8_t  type            = 0;
		uint8_t  binding         = 0;
		uint8_t  exact_format    = 0;
		uint8_t  bgra16          = 0;
		bool     operator==(const FindImageKey&) const = default;
	};
	struct FindImageKeyHash {
		size_t operator()(const FindImageKey& key) const noexcept;
	};
	struct FoundImage {
		uint64_t since = 0;
		ImageId  id;
	};
	[[nodiscard]] static FindImageKey MakeFindImageKey(const ImageDesc& desc, bool exact_format);
	static constexpr size_t MaxFoundImages = 1u << 16u;
	std::unordered_map<FindImageKey, FoundImage, FindImageKeyHash> m_found_images;
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
