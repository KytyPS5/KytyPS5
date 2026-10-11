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
#include "graphics/host_gpu/renderer/image/colorClearHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/tiler.h"

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
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

// Per-slice clear state; a fill marks every slice, bindings consume them one at a time.
class MetaSliceMask {
public:
	[[nodiscard]] static MetaSliceMask All() {
		MetaSliceMask mask;
		mask.m_rest = true;
		return mask;
	}
	// UINT32_MAX marks every slice; any other value marks only the slices it names.
	[[nodiscard]] static MetaSliceMask FromBits32(uint32_t bits) {
		if (bits == UINT32_MAX) {
			return All();
		}
		MetaSliceMask mask;
		if (bits != 0) {
			mask.m_words.push_back(bits);
		}
		return mask;
	}

	[[nodiscard]] bool Any() const noexcept {
		if (m_rest) {
			return true;
		}
		for (const auto word: m_words) {
			if (word != 0) {
				return true;
			}
		}
		return false;
	}
	[[nodiscard]] bool Test(uint32_t slice) const noexcept {
		const auto word = slice / 64u;
		return word < m_words.size() ? ((m_words[word] >> (slice % 64u)) & 1u) != 0 : m_rest;
	}
	void Assign(uint32_t slice, bool cleared) {
		const auto word = slice / 64u;
		if (word >= m_words.size()) {
			if (cleared == m_rest) {
				return;
			}
			m_words.resize(word + 1u, m_rest ? UINT64_MAX : 0u);
		}
		const auto bit = uint64_t {1} << (slice % 64u);
		m_words[word]  = cleared ? (m_words[word] | bit) : (m_words[word] & ~bit);
	}

private:
	std::vector<uint64_t> m_words;
	// State of every slice past the explicit words.
	bool m_rest = false;
};

namespace BcAlias {
[[nodiscard]] bool Matches(const ImageInfo& requested, const ImageInfo& cached);
[[nodiscard]] bool Publish(TextureCache& cache, Image& image);
void               Resolve(TextureCache& cache, ImageId cached, ImageId merged);
} // namespace BcAlias

class TextureCache {
public:
	enum class BindingType : uint8_t { Texture, Storage, RenderTarget, DepthTarget, VideoOut };

	static void OverrideStencilPlaneRedirect(std::optional<bool> enabled);
	static void OverrideSupersedeReconcile(std::optional<bool> enabled,
	                                       std::optional<bool> at_base = std::nullopt);

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
	// A write that left the stencil plane alone must not claim it: the plane may be shared with
	// another depth surface that still holds its contents.
	void MarkGpuWritten(ImageId id, bool stencil_written = true);

	// Called once per presented frame. Image staleness is judged against this rather than against
	// the queue submission counter, which this title advances dozens of times inside one frame.
	void AdvanceFrame() noexcept { m_frame_index.fetch_add(1, std::memory_order_relaxed); }
	[[nodiscard]] uint64_t FrameIndex() const noexcept {
		return m_frame_index.load(std::memory_order_relaxed);
	}

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t address, uint64_t size);
	// Names the image holding a guest read back, so a refusal can say whether it is a live surface
	// or one left over a pool address the engine has since reused. Empty when nothing blocks.
	[[nodiscard]] std::string DescribeGpuModifiedRegion(uint64_t address, uint64_t size);

	// Inside a registered metadata surface, not only at its base: layers are cleared one by one.
	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsWithinMeta(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	// A CP-DMA fill or a compute clear landing on a metadata surface: marks only the whole,
	// slice-aligned slices the write covers. A marked slice discards the attachment contents at
	// load, so an address match on its own is not enough to spend it.
	[[nodiscard]] bool ClearMetaSlices(uint64_t address, uint64_t size);
	// A shader store inside a metadata surface supersedes the pending clears it can reach.
	void               DiscardMetaClears(uint64_t address, uint64_t size);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	bool UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void RunGarbageCollector(bool per_frame = false);

private:
	void FreePublishedEvictions();

	struct MetaDataInfo {
		enum class Type : uint8_t { CMask, FMask, HTile };

		Type type;
		// Footprint of the registered metadata surface, so a writer that touches only part
		// of it cannot be mistaken for one that cleared the whole thing. The registry is
		// keyed by base address alone and outlives the guest allocation, so a matching base
		// on its own proves nothing about what a writer covered.
		uint64_t      range_size = 0;
		uint32_t      slices     = 0;
		MetaSliceMask clear_mask = MetaSliceMask::All();
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
		using FuncReturn                   = typename std::invoke_result<Func, uint64_t>::type;
		static constexpr bool RETURNS_BOOL = std::is_same_v<FuncReturn, bool>;
		const uint64_t        page_end     = (address + size - 1) >> ImagePageTable::kPageBits;
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
	[[nodiscard]] ImageId     GetNullImage(const ImageDesc& desc);
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
	static void               MarkEdgeMaybeDirty(Image& image);
	[[nodiscard]] uint64_t    BufferWriteEpochOf(uint64_t address, uint64_t size) const;
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
	// An overlap the lookup cannot resolve into one surface has to displace the cached image, or
	// two owners answer for the same guest bytes and the merge loop walks both. Freeing is the
	// cheap way to displace it, and it is correct only while guest memory can still reproduce
	// the pixels. When it cannot, park the image instead of destroying it.
	void               RetireOverlap(ImageId id, bool abandoned);
	[[nodiscard]] bool ParkImage(ImageId id);
	// Re-admit a parked image and put down whatever claimed its bytes while it was away.
	bool                  UnparkImage(ImageId id);
	void                  WakeImage(ImageId id, const ImageIds& candidates);
	[[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
	                                          ImageId cached);
	[[nodiscard]] ImageId ReuseDepthTwin(ImageId cached, const ImageInfo& info);
	[[nodiscard]] bool    LinkDepthTwins(ImageId live, ImageId parked);
	void                  SyncDepthTwin(ImageId destination, ImageId source);
	static void NarrowTwinWrite(Image& image, std::vector<uint64_t>&& before, bool was_dirty,
	                            const ImageViewInfo& view);
	[[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId source);
	// Copies in the newest GPU write of a same-layout image whose format cannot share the backing.
	void SyncLayoutAlias(ImageId id, const ImageIds& candidates);
	void RefreshImage(ImageId id);
	void MaterializeColorClear(ImageId id, const ImageDesc& desc, uint32_t metadata_base_layer);
	// False when the target cannot take the GPU draw and the metadata must be read back.
	[[nodiscard]] bool
	     MaterializeColorClearOnGpu(ImageId id, const ImageDesc& desc, uint32_t first,
	                                uint32_t image_first, uint32_t count, uint64_t slice_size,
	                                std::span<const ColorClearHelper::Candidate> candidates);
	void InitializeImage(ImageId id);
	[[nodiscard]] bool CanDownload(const Image& image) const;
	void               UploadImage(Image& image, Buffer& source, uint64_t source_offset);
	void               DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
	                                 uint64_t destination_size, uint32_t levels);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	void DownloadColorRegions(Image& image, std::vector<vk::BufferImageCopy>& regions,
	                          ColorTransform transform, Buffer& destination,
	                          uint64_t destination_offset, uint64_t destination_size);
	void DownloadDepthRegions(Image& image, std::vector<vk::BufferImageCopy>& regions,
	                          Buffer& destination, uint64_t destination_offset,
	                          uint64_t destination_size);
	void CommitGpuWrite(Image& image);
	// Caller holds m_lock. Volume layer ranges select depth slices.
	void ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
	                const vk::ImageSubresourceRange& range, const vk::ClearValue& clear);
	void PrepareImageCopy(Image& image);
	void RefreshCopySource(ImageId id);
	[[nodiscard]] bool    CopyD16(Image& destination, Image& source);
	void                  CopyImage(ImageId destination, ImageId source);
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
	ColorClearHelper                                  m_color_clear_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	std::unordered_map<vk::Format, ImageId>           m_null_images;
	std::unordered_map<vk::Format, ImageId>           m_null_volume_images;
	std::unordered_map<vk::Format, ImageId>           m_null_1d_images;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	// Evicted images held registered until their download reaches guest memory. The priority runner
	// appends to m_evict_published under its own leaf mutex, never m_lock, which would deadlock a
	// caller already waiting on a priority operation.
	std::unordered_map<ImageId, uint64_t>  m_evict_pending;
	std::unordered_map<ImageId, uint64_t>  m_evict_backoff;
	std::unordered_map<uint64_t, uint64_t> m_evicted_addresses;
	// Image write epoch of the last GPU buffer write to each 64 KB page.
	std::unordered_map<uint64_t, uint64_t> m_buffer_write_epochs;
	std::mutex                             m_evict_published_lock;
	std::vector<ImageId>                   m_evict_published;
	std::map<uint64_t, MetaDataInfo>       m_surface_metas;
	std::unordered_map<uint64_t, ImageId>  m_stencil_planes;
	std::vector<ImageId>                   m_sampled_state_changes;
	uint64_t                               m_total_used_memory  = 0;
	uint64_t                               m_trigger_gc_memory  = 0;
	uint64_t                               m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t                               m_critical_gc_memory = 3ull * 1024 * 1024 * 1024;
	uint64_t                               m_gc_tick            = 0;
	uint64_t                               m_gc_frame           = UINT64_MAX;
	uint64_t                               m_gc_collections     = 0;
	// Advances whenever a registered image stops being servable: unregistered, parked, stencil
	// proxy.
	uint64_t         m_retire_epoch           = 0;
	mutable uint32_t m_image_query_epoch      = 0;
	bool             m_readback_linear_images = false;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
	friend class BindlessImageHeap;
	friend bool BcAlias::Matches(const ImageInfo&, const ImageInfo&);
	friend bool BcAlias::Publish(TextureCache&, Image&);
	friend void BcAlias::Resolve(TextureCache&, ImageId, ImageId);
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
