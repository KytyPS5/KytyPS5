#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <compare>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class Buffer;
class BufferCache;
class CommandBuffer;
class CommandScheduler;
class RenderExecutor;
class StreamBuffer;
class TileManager;
struct TextureCacheTestAccess;

class TextureCache {
public:
	enum class BindingType : uint8_t { Texture, Storage, RenderTarget, DepthTarget, VideoOut };

	struct ImageDesc {
		ImageInfo     info;
		ImageViewInfo view_info;
		BindingType   type = BindingType::Texture;
	};

	struct RegionInfo {
		bool image_pages     = false;
		bool image_bytes     = false;
		bool gpu_image_bytes = false;
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
	[[nodiscard]] Image&        GetImage(ImageId id);
	[[nodiscard]] const Image&  GetImage(ImageId id) const;
	void                        MarkGpuWritten(ImageId id);

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	[[nodiscard]] bool InvalidateMemoryFromGPU(uint64_t address, uint64_t size,
	                                           bool formatted_buffer_write = false);
	[[nodiscard]] RegionInfo QueryRegion(uint64_t address, uint64_t size);

	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void RunGarbageCollector();

private:
	enum class TransferDirection { Upload, Download };
	struct ColorTransferPlan;
	struct DownloadPlan;

	struct Slot {
		std::shared_ptr<Image> image;
		uint32_t               generation = 1;
	};

	struct MetaDataInfo {
		uint32_t clear_mask = 0;
	};

	struct OverlapResult {
		ImageId image;
		int32_t mip   = -1;
		int32_t layer = -1;
	};

	using ImageIds       = InlinePageOwnerList<ImageId, 16>;
	using ImagePageTable = MultiLevelPageTable<ImageIds, 20, 40, 10>;

	[[nodiscard]] Image&                 ResolveImage(ImageId id);
	[[nodiscard]] const Image&           ResolveImage(ImageId id) const;
	[[nodiscard]] std::shared_ptr<Image> ResolveOwner(ImageId id) const;
	[[nodiscard]] ImageId                InsertImage(const ImageInfo& info);
	[[nodiscard]] ImageId                GetNullImage(const ImageDesc& desc);
	void                                 RegisterImage(ImageId id);
	void                                 UnregisterImage(ImageId id);
	void                                 DeleteImage(ImageId id);
	void                                 FreeImage(ImageId id);
	void DeleteImages(std::span<const ImageId> ids, std::optional<ImageId> native_source = {});
	void RetainImage(CommandBuffer& command, ImageId id);
	void TouchImage(Image& image);
	void TrackImage(ImageId id);
	void TrackImageHead(ImageId id);
	void TrackImageTail(ImageId id);
	void UntrackImage(ImageId id);
	void UntrackImageHead(ImageId id);
	void UntrackImageTail(ImageId id);
	void MarkAsMaybeDirty(ImageId id, Image& image);
	void TrackImageDownload(ImageId id);
	void TrackImageDownloadLocked(ImageId id, Image& image);
	[[nodiscard]] static bool SameBacking(const ImageInfo& cached, const ImageInfo& requested,
	                                      bool exact_format);
	[[nodiscard]] static BindingType UploadBinding(const Image& image);
	[[nodiscard]] bool               SafeToDownload(const Image& image);

	// Caller holds m_lock; it also serializes the per-image query epoch.
	[[nodiscard]] ImageIds      FindImagesInRegion(uint64_t address, uint64_t size,
	                                               bool page_overlap) const;
	[[nodiscard]] OverlapResult ResolveOverlap(const ImageInfo& requested, BindingType binding,
	                                           ImageId cached, ImageId merged);
	[[nodiscard]] ImageId       ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
	                                                ImageId cached);
	[[nodiscard]] ImageId       ExpandImage(const ImageInfo& info, ImageId source);
	void                        RefreshImage(ImageId id, const ImageDesc& desc);
	void                        InitializeImage(ImageId id, const ImageDesc& desc);
	[[nodiscard]] ColorTransferPlan BuildColorTransfer(const Image& image, BindingType binding,
	                                                   TransferDirection direction) const;
	[[nodiscard]] DownloadPlan      BuildDownload(const Image& image) const;
	void UploadImage(Image& image, const ImageDesc& desc, Buffer& source, uint64_t source_offset);
	void DownloadImageData(Image& image, Buffer& destination, uint64_t destination_offset,
	                       uint64_t destination_size, DownloadPlan plan);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	void CommitGpuWrite(Image& image);
	void PrepareImageCopy(Image& image);
	void RefreshCopySource(ImageId id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	void               AssociateStencil(ImageId depth, GuestRange stencil);
	void               AssociateStencilLocked(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	void ValidateImageDesc(const ImageDesc& desc) const;

	void InvalidateCpuAliases(uint64_t address, uint64_t size);
	void ClearGpuModified(ImageId id);

	void                                        DownloadImage(ImageId id);
	[[nodiscard]] bool                          TryDownloadImage(ImageId id);
	[[nodiscard]] std::pair<uint8_t*, uint64_t> MapDownload(uint64_t size, uint64_t alignment);
	void QueueDownload(GuestRange range, StreamBuffer& download, uint8_t* mapped, uint64_t offset);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	std::unique_ptr<TileManager>                      m_tiler;
	BufferCache&                                      m_buffer_cache;
	std::vector<Slot>                                 m_slots;
	std::vector<uint32_t>                             m_free_slots;
	ImagePageTable                                    m_image_page_table;
	std::map<vk::Format, ImageId>                     m_null_images;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::set<ImageId>                                 m_download_images;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	mutable uint32_t m_image_query_epoch      = 0;
	bool             m_readback_linear_images = false;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
