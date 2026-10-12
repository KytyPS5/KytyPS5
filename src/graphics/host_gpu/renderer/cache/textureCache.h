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

#include <array>
#include <atomic>
#include <map>
#include <memory>
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
	// Moves whenever a lookup could find a different image.
	[[nodiscard]] uint64_t LookupGeneration() const noexcept { return m_structure_generation; }
	// Repeats, in the same lookup generation, a lookup of an equal description that found `id`
	// and left `desc` with its view.
	[[nodiscard]] ImageId ReuseImage(ImageId id, const ImageDesc& desc,
	                                 uint32_t metadata_base_layer);
	// For a descriptor the GPU may never use: reuses an image with the same backing or inserts
	// one into an empty region, and returns an invalid id rather than reshape cached images.
	[[nodiscard]] ImageId       FindSpeculativeImage(ImageDesc& desc, bool exact_format = false);
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
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionRegistered(uint64_t address, uint64_t size);

	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void RunGarbageCollector();

private:
	struct MetaDataInfo {
		enum class Type : uint8_t { CMask, FMask, HTile };

		Type     type;
		uint32_t clear_mask = UINT32_MAX;
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
	void DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
	                   uint64_t destination_size, uint32_t levels);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	[[nodiscard]] bool CanDownloadStencil(const Image& association) const;
	void DownloadStencil(Image& association, Buffer& destination, uint64_t destination_offset);
	void CommitGpuWrite(Image& image);
	// Caller holds m_lock. Volume layer ranges select depth slices.
	void ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
	                const vk::ImageSubresourceRange& range, const vk::ClearValue& clear);
	void PrepareImageCopy(Image& image);
	void RefreshCopySource(ImageId id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	[[nodiscard]] ImageId AssociateStencil(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	void ValidateImageDesc(const ImageDesc& desc) const;

	// The result of an image lookup, valid while the cache's images and their matching state are
	// unchanged. A speculative overlap check sets view_mip to 1 when an image of another shape
	// overlaps. Caller holds m_lock.
	struct LookupMemo {
		ImageInfo     info;
		ImageViewInfo view;
		uint64_t      generation = 0;
		ImageId       result;
		int32_t       view_mip     = -1;
		int32_t       view_layer   = -1;
		BindingType   type         = BindingType::Texture;
		bool          exact_format = false;
		bool          speculative  = false;
	};
	[[nodiscard]] LookupMemo& LookupSlot(const ImageDesc& desc, bool exact_format, bool speculative);
	[[nodiscard]] bool        LookupMatches(const LookupMemo& memo, const ImageDesc& desc,
	                                        bool exact_format, bool speculative) const;
	void StoreLookup(LookupMemo& memo, const ImageDesc& desc, const ImageViewInfo& view,
	                 bool exact_format, bool speculative, ImageId result, int32_t view_mip,
	                 int32_t view_layer);
	void ChangeStructure() { ++m_structure_generation; }
	[[nodiscard]] ImageId FinishLookup(ImageId result, const ImageDesc& desc,
	                                   uint32_t metadata_base_layer);

	void               InvalidateCpuAliases(uint64_t address, uint64_t size);
	[[nodiscard]] bool DownloadImageMemory(ImageId id);

	// Registered images per MiB of guest memory, readable without m_lock: guest threads take a
	// write fault on every buffer page they rewrite, and most of those pages hold no image.
	class ImageRegionCounts {
	public:
		// Under m_lock, before the image's pages are watched or read.
		void               Add(GuestRange range, int32_t delta);
		[[nodiscard]] bool MayHoldImage(uint64_t address, uint64_t size) const;

	private:
		static constexpr uint64_t RegionBits = 20;
		static constexpr uint64_t ChunkBits  = 12;
		using Chunk                          = std::atomic<uint32_t>;
		std::array<std::atomic<Chunk*>, (TRACKER_ADDRESS_SIZE >> (RegionBits + ChunkBits))>
		                                      m_chunks {};
		std::vector<std::unique_ptr<Chunk[]>> m_storage;
	};

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	ImageRegionCounts                                             m_image_regions;
	std::map<std::pair<vk::Format, Prospero::ImageType>, ImageId> m_null_images;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	mutable uint32_t m_image_query_epoch      = 0;
	bool             m_readback_linear_images = false;
	// Moves whenever a lookup could find a different image: on registration and when a cached
	// image's stencil, metadata or depth association changes.
	uint64_t                                    m_structure_generation = 1;
	std::unique_ptr<std::array<LookupMemo, 512>> m_lookup_memo;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
	friend bool BcAlias::Matches(const ImageInfo&, const ImageInfo&);
	friend bool BcAlias::Publish(TextureCache&, Image&);
	friend void BcAlias::Resolve(TextureCache&, ImageId, ImageId);
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
