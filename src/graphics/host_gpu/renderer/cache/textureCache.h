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
#include <array>
#include <atomic>
#include <bitset>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
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

	[[nodiscard]] ImageId       FindImage(ImageDesc& desc, bool exact_format = false);
	// Lookup for bindings the shader may never read (bindless table candidates). It resolves
	// like FindImage but returns an empty id instead of converting a depth image to color or
	// back: those conversions replace the image other passes use, which then convert it again.
	// When the description reads a color image of the same layout as depth, depth_source
	// receives that image (see FindDepthCopy()).
	[[nodiscard]] ImageId FindImageSpeculative(ImageDesc& desc, bool exact_format = false,
	                                           ImageId* depth_source = nullptr);
	// PS5 samples any memory as depth with a depth T#: a T# that compares against a color
	// image's memory reads its bytes as depth values. Vulkan compares only depth formats, so the
	// binding samples a depth image with the color image's layout, in `requested`'s depth
	// format, that is not registered over the guest memory and is copied from the color image
	// when it is acquired (FindTexture()) after the color image was written. The copy lives as
	// long as the color image.
	[[nodiscard]] ImageId FindDepthCopy(ImageId color_id, const ImageInfo& requested);
	// Whether the depth copy `image` no longer holds its color image's contents (false for
	// other images).
	[[nodiscard]] bool DepthCopyStale(const Image& image) const;
	// Whether acquiring the depth copy `image` (FindTexture()) would change nothing: it holds its
	// color image's contents, and the color image is clean and tracked (false for other images).
	[[nodiscard]] bool DepthCopyCurrent(const Image& image) const;
	void                        UpdateImage(ImageId id);
	[[nodiscard]] ImageId       FindImageFromRange(uint64_t address, uint64_t size,
	                                               bool ensure_valid = true);
	[[nodiscard]] vk::ImageView FindTexture(ImageId id, const ImageDesc& desc);
	// Hosts without attachment feedback loops cannot sample a depth/stencil target that the draw
	// writes. Copies `range` (the `aspects` given) of depth image `depth_id` into an image of its
	// own, outside any render pass, for the draw to sample instead: the draw reads the values from
	// before it ran. The copy lives until the depth image is deleted.
	[[nodiscard]] ImageId CopyDepthForFeedback(ImageId depth_id, const ImageSubresourceRange& range,
	                                           vk::ImageAspectFlags aspects);
	[[nodiscard]] vk::ImageView FindRenderTarget(ImageId id, const ImageDesc& desc);
	// stencil (optional) receives the stencil association the target refreshed, if any.
	[[nodiscard]] vk::ImageView FindDepthTarget(ImageId id, const ImageDesc& desc,
	                                            ImageId* stencil = nullptr);
	[[nodiscard]] Image&        GetImage(ImageId id) {
		auto& image = m_slot_images[id];
		TouchImage(image);
		return image;
	}
	void MarkGpuWritten(ImageId id);

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	// Before the guest CPU reads or writes [address, address + size): writes the images read
	// back lazily on those pages to guest memory (see ArmLazyReadback()) and watches them no
	// longer. May wait for the GPU, so the caller holds none of the cache's locks.
	void               FlushLazyReadback(uint64_t address, uint64_t size);
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size);
	// Writes a copy into whole levels of one layer of a GPU-written image straight into it, and
	// returns false for any other copy.
	[[nodiscard]] bool CopyToImage(uint64_t address, uint64_t source, uint64_t size);
	[[nodiscard]] bool IsRegionRegistered(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t address, uint64_t size);

	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	// Whether storage readback is on (constant while the GPU runs; the command processor reads it).
	[[nodiscard]] bool StorageReadbackEnabled() const { return m_readback_storage_images; }
	// With storage readback, a GL2 writeback makes GPU-written images visible to the CPU; returns
	// whether a download was recorded.
	[[nodiscard]] bool WriteBackImages();
	// The tick of the latest write-back pending over the range's pages, or 0 when none is.
	[[nodiscard]] uint64_t WriteBackTick(uint64_t address, uint64_t size);
	// Advances whenever an image is registered or unregistered.
	[[nodiscard]] uint64_t ImageSetEpoch() const { return m_image_set_epoch; }
	// The ImageSetEpoch of the last registration or unregistration of an image overlapping the
	// range's pages: while it is unchanged, a lookup in the range finds the same images.
	[[nodiscard]] uint64_t ImageEpochInRegion(uint64_t address, uint64_t size);
	// The granularity of ImageEpochInRegion(): log2 of its page size.
	static constexpr uint32_t ImagePageBits = 20;
	// Calls `function(address, size)` for the image range of every registration and
	// unregistration after ImageSetEpoch() was `epoch`, oldest first. False when the history no
	// longer reaches back that far.
	template <typename Function>
	[[nodiscard]] bool ForEachImageSetChangeSince(uint64_t epoch, Function&& function) {
		std::scoped_lock lock {m_lock};
		if (m_image_set_epoch - epoch > ImageSetChangeHistory) {
			return false;
		}
		for (auto change = epoch + 1; change <= m_image_set_epoch; ++change) {
			const auto& range = m_image_set_changes[change % ImageSetChangeHistory];
			function(range.first, range.second);
		}
		return true;
	}
	// A counter per image slot that advances whenever the image in the slot changes what a
	// binding of one of its views, as it was bound, depends on (see Image::StateChanged()), and
	// when the slot holds another image. While it is unchanged, such a binding still holds
	// without reading the image.
	[[nodiscard]] uint32_t ImageVersion(ImageId id) const noexcept {
		return m_image_versions.Load(id.index);
	}
	// A pinned image is used without its bindings looking it up or touching it: the garbage
	// collector keeps it and overlapping lookups do not delete it as unused. Pins nest.
	void PinImage(ImageId id);
	void UnpinImage(ImageId id);
	void RunGarbageCollector();

private:
	struct MetaDataInfo {
		// PS5 DB_DEPTH_VIEW encodes 13-bit base and last array-slice indices.
		std::bitset<8192> clear_mask;
		// The depth image that last acquired this HTile range.
		ImageId owner;
	};

	struct OverlapResult {
		ImageId image;
		int32_t mip   = -1;
		int32_t layer = -1;
		// Speculative lookups only: resolving the overlap would replace an owned image.
		bool rejected = false;
		// Rejected lookups only: the color image the description reads as depth, when a depth
		// copy of it can stand for the description (see FindDepthCopy()).
		ImageId depth_source;
	};

	// A recorded image download whose guest pages stay inaccessible until it lands.
	struct WriteBack {
		GuestRange range;
		uint64_t   tick;
		bool       operator==(const WriteBack&) const = default;
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
	static_assert(ImagePageTable::kPageBits == ImagePageBits);

	// Registered images per page of the image page table. Written under m_lock, read without it.
	class RegisteredPageCounts final {
	public:
		RegisteredPageCounts() = default;
		~RegisteredPageCounts();
		KYTY_CLASS_NO_COPY(RegisteredPageCounts);

		void               Add(size_t page, int32_t delta);
		[[nodiscard]] bool Any(size_t first, size_t last_exclusive) const noexcept;

	private:
		static constexpr size_t kLeafBits = 10;
		static constexpr size_t kLeafSize = size_t {1} << kLeafBits;
		using Leaf                        = std::array<std::atomic<uint32_t>, kLeafSize>;

		std::array<std::atomic<Leaf*>, ImagePageTable::kPageCount / kLeafSize> m_leaves {};
	};

	// The ImageVersion() counters, in chunks that never move: images keep a pointer to theirs.
	class ImageVersions final {
	public:
		ImageVersions() = default;
		~ImageVersions();
		KYTY_CLASS_NO_COPY(ImageVersions);

		[[nodiscard]] std::atomic<uint32_t>& Counter(uint32_t index);
		[[nodiscard]] uint32_t               Load(uint32_t index) const noexcept {
			if ((index >> kChunkBits) >= kMaxChunks) {
				return 0;
			}
			const auto* chunk = m_chunks[index >> kChunkBits].load(std::memory_order_acquire);
			return chunk == nullptr
			           ? 0
			           : (*chunk)[index & (kChunkSize - 1)].load(std::memory_order_acquire);
		}

	private:
		static constexpr size_t kChunkBits = 10;
		static constexpr size_t kChunkSize = size_t {1} << kChunkBits;
		static constexpr size_t kMaxChunks = 4096;
		using Chunk                        = std::array<std::atomic<uint32_t>, kChunkSize>;

		std::array<std::atomic<Chunk*>, kMaxChunks> m_chunks {};
	};
	// Gives a new image its slot's version counter and advances it.
	void AttachImageVersion(ImageId id);

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
	// Releases the depth feedback copy of a depth image, if it has one.
	void                      ReleaseDepthFeedbackCopy(ImageId depth_id);
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

	[[nodiscard]] OverlapResult ResolveOverlap(const ImageInfo& requested, BindingType binding,
	                                           ImageId cached, ImageId merged,
	                                           bool speculative = false);
	[[nodiscard]] ImageId       FindImageImpl(ImageDesc& desc, bool exact_format, bool speculative,
	                                          ImageId* depth_source = nullptr);
	[[nodiscard]] static bool   DepthOverlapNeedsRecreate(const ImageInfo& requested,
	                                                      BindingType binding, const Image& cached);
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
	                               const CommandRecorder& command);
	void RefreshCopySource(ImageId id);
	// Caller holds m_lock.
	void RefreshDepthCopy(ImageId copy_id);
	void ReleaseDepthCopies(ImageId color_id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	// A copy between the depth aspect of a depth image and a color image of the same texel size with
	// vkCmdCopyImage (VK_KHR_maintenance8); false when the device or the formats do not allow it.
	[[nodiscard]] bool CopyDepthColor(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	// The depth image's stencil plane record covering exactly range, or none.
	[[nodiscard]] ImageId FindStencilPlane(const Image& depth, GuestRange range) const;
	[[nodiscard]] ImageId AssociateStencil(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	// Validates the view and binding of a description; ImageOps::Validate() checks its image
	// information.
	void ValidateImageViewDesc(const ImageDesc& desc) const;

	[[nodiscard]] bool DownloadImageMemory(ImageId id);
	[[nodiscard]] bool DownloadQueuedImages();
	// Readback of a large linear image is lazy until the guest CPU touches its memory: the pages
	// are access-watched while the GPU writes the image, and the first CPU access writes the
	// image to guest memory and from then on reads it back after every submission, as for
	// smaller images. Games that never read such an image (full-screen post-processing
	// targets) then cost no readback at all.
	void               ArmLazyReadback(Image& image);
	void               DisarmLazyReadback(Image& image);
	void               FlushLazyImages(std::span<const std::pair<ImageId, uint64_t>> images);
	[[nodiscard]] static uint64_t ReadbackConsumerKey(const ImageInfo& info) noexcept;
	// A render or depth target the GPU wrote is freed without its pixels ever reaching guest
	// memory (the game changed its resolution and the replacement has another extent): its range
	// is noted, so that a target created over that memory within a few frames is not initialized
	// from the stale bytes the guest memory still holds, which only the target's first write
	// would overwrite.
	void                      NoteDiscardedTarget(ImageId id, const Image& image);
	[[nodiscard]] static bool IsTargetImage(const Image& image) noexcept;
	[[nodiscard]] bool        WasDiscardedTarget(const Image& image) const;

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	// Before the images, which point into it.
	ImageVersions                                     m_image_versions;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	MultiLevelPageTable<uint64_t, ImagePageTable::kPageBits, ImagePageTable::kAddressSpaceBits,
	                    ImagePageTable::kFirstLevelBits>
	    m_image_page_epochs;
	RegisteredPageCounts                              m_registered_pages;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::mutex                                        m_write_back_mutex;
	std::vector<WriteBack>                            m_write_backs;
	// Linear images the guest CPU read while they were read back lazily, by address and size:
	// they are read back after every submission.
	std::unordered_set<uint64_t>                      m_readback_consumers;
	// The number of images read back lazily right now.
	std::atomic<uint32_t>                             m_lazy_images {0};
	struct DiscardedTarget {
		uint64_t begin = 0;
		uint64_t end   = 0;
		uint64_t tick  = 0;
	};
	static constexpr size_t DiscardedTargetCount = 32;
	std::array<DiscardedTarget, DiscardedTargetCount> m_discarded_targets {};
	size_t                                            m_discarded_next = 0;
	// Color image -> its depth copies, one per depth format (see FindDepthCopy).
	std::unordered_map<ImageId, std::vector<ImageId>> m_depth_copies;
	// Depth image -> its depth feedback copy (see CopyDepthForFeedback).
	std::unordered_map<ImageId, ImageId>              m_depth_feedback_copies;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	uint64_t         m_image_set_epoch        = 0;
	// The image range of the last registrations and unregistrations, by ImageSetEpoch.
	static constexpr uint64_t ImageSetChangeHistory = 1024;
	std::array<std::pair<uint64_t, uint64_t>, ImageSetChangeHistory> m_image_set_changes {};
	bool             m_readback_linear_images = false;
	bool             m_readback_tiled_images  = false;
	bool             m_readback_storage_images = false;
	// Lookups that found an existing image with the same backing, by description: while no image
	// on the pages of the range was registered or unregistered since, the same lookup finds the
	// same image.
	struct ExactLookup {
		ImageInfo info;
		ImageId   id;
		uint64_t  epoch        = 0;
		bool      exact_format = false;
		bool      valid        = false;
	};
	static constexpr size_t                                   ExactLookupWays = 1024;
	std::unique_ptr<std::array<ExactLookup, ExactLookupWays>> m_exact_lookups =
	    std::make_unique<std::array<ExactLookup, ExactLookupWays>>();
	[[nodiscard]] static size_t ExactLookupSlot(const ImageInfo& info) noexcept;
	[[nodiscard]] uint64_t      ImageEpochInRegionUnlocked(uint64_t address, uint64_t size) const;

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
