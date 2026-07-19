#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/renderer/imageInfo.h"
#include "graphics/host_gpu/renderer/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/tiler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

struct DepthStencilVulkanImage;
struct GpuTextureVulkanImage;
struct GraphicContext;
struct ImageViewInfo;
struct RenderTextureVulkanImage;
struct StorageTextureVulkanImage;
struct VideoOutVulkanImage;
struct VulkanImage;
struct VulkanMemory;
class BufferCache;
class CommandBuffer;
class DummyTextureCache;
class ResourceMutex;

class TextureCache {
public:
	struct RegionInfo {
		bool image_pages        = false;
		bool image_bytes        = false;
		bool gpu_image_bytes    = false;
		bool non_sampled_pages  = false;
		bool metadata_pages     = false;
		bool metadata_bytes     = false;
		bool gpu_metadata_bytes = false;
	};

	TextureCache(PageManager& page_manager, BufferCache& buffer_cache,
	             ResourceMutex& resource_mutex);
	~TextureCache();
	KYTY_CLASS_NO_COPY(TextureCache);

	[[nodiscard]] VulkanImage* FindTexture(CommandBuffer* command, GraphicContext* ctx,
	                                       const ImageInfo& info, bool metadata_read);
	[[nodiscard]] StorageTextureVulkanImage*
	FindStorageTexture(CommandBuffer* command, GraphicContext* ctx, const ImageInfo& info);
	[[nodiscard]] RenderTextureVulkanImage*
	FindRenderTarget(CommandBuffer* command, GraphicContext* ctx, const RenderTargetInfo& info);
	[[nodiscard]] DepthStencilVulkanImage*
	FindDepthTarget(CommandBuffer* command, GraphicContext* ctx, const DepthTargetInfo& info);
	[[nodiscard]] std::vector<VideoOutVulkanImage*>
	     RegisterVideoOutSurfaces(GraphicContext* ctx, const std::vector<VideoOutInfo>& infos);
	void RefreshVideoOut(VideoOutVulkanImage* image, bool render_target = false);
	void UnregisterVideoOutSurfaces(const std::vector<VideoOutVulkanImage*>& images);
	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer* command, uint64_t vaddr, uint64_t size,
	                                        uint32_t packed_clear);
	void               MarkGpuWritten(VulkanImage* image);
	void               PrepareHostWrite(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool InvalidateMemoryFromGPU(uint64_t vaddr, uint64_t size,
	                                           bool formatted_buffer_write = false);
	[[nodiscard]] RenderTextureVulkanImage* FindRenderTargetByRange(CommandBuffer* command,
	                                                                uint64_t vaddr, uint64_t size);
	[[nodiscard]] vk::ImageView GetRenderTargetAttachmentView(GraphicContext*           ctx,
	                                                          RenderTextureVulkanImage* image,
	                                                          vk::Format format, uint32_t level,
	                                                          uint32_t base_layer,
	                                                          uint32_t layer_count);
	[[nodiscard]] vk::ImageView GetDepthTargetAttachmentView(GraphicContext*          ctx,
	                                                         DepthStencilVulkanImage* image,
	                                                         uint32_t                 base_layer,
	                                                         uint32_t                 layer_count);
	[[nodiscard]] vk::ImageView
	GetDepthTargetSampledView(GraphicContext* ctx, DepthStencilVulkanImage* image,
	                          vk::Format view_format, uint32_t swizzle, uint32_t base_level,
	                          uint32_t level_count, vk::ImageViewType type, uint32_t base_layer,
	                          uint32_t layer_count);
	[[nodiscard]] vk::ImageView GetSampledColorView(GraphicContext* ctx, VulkanImage* image,
	                                                vk::Format view_format, uint32_t swizzle,
	                                                uint32_t base_level, uint32_t level_count,
	                                                vk::ImageViewType type, uint32_t base_layer,
	                                                uint32_t layer_count);
	[[nodiscard]] vk::ImageView
	GetRenderTargetStorageView(GraphicContext* ctx, RenderTextureVulkanImage* image,
	                           vk::Format view_format, uint32_t base_level, uint32_t level_count,
	                           vk::ImageViewType type, uint32_t base_layer, uint32_t layer_count);
	[[nodiscard]] vk::ImageView GetStorageTextureSampledView(GraphicContext*            ctx,
	                                                         StorageTextureVulkanImage* image,
	                                                         const ImageInfo&           info);
	[[nodiscard]] vk::ImageView GetStorageTextureStorageView(GraphicContext*            ctx,
	                                                         StorageTextureVulkanImage* image,
	                                                         uint32_t                   base_level);
	[[nodiscard]] DepthStencilVulkanImage*
	FindDepthTargetByRange(CommandBuffer* command, uint64_t vaddr, uint64_t size,
	                       bool allow_containing_sampled = false);
	[[nodiscard]] RegionInfo QueryRegion(uint64_t vaddr, uint64_t size);
	void                     RegisterMeta(uint64_t vaddr, uint64_t size, uint32_t layers = 1);
	[[nodiscard]] bool       IsMeta(uint64_t vaddr);
	[[nodiscard]] bool       IsMetaRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool       IsMetaCleared(uint64_t vaddr, uint32_t slice);
	[[nodiscard]] bool       ClearMeta(uint64_t vaddr);
	[[nodiscard]] bool       TouchMeta(uint64_t vaddr, uint32_t slice, bool is_clear);
	[[nodiscard]] bool       InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
	                                          PageFaultPhase phase) noexcept;
	void                     UnmapMemory(uint64_t vaddr, uint64_t size);

	VulkanImage* GetDummySampledTexture(bool uint_format, bool image_3d);
	VulkanImage* GetDummyStorageTexture(bool uint_format, bool image_3d);

private:
	struct CachedImage;
	using ImageOwnerIndex = MultiRangePageOwnerIndex<CachedImage*>;
	struct ReadbackWorker;
	struct MetaDataInfo {
		uint64_t size         = 0;
		uint32_t layers       = 1;
		uint32_t clear_mask   = 0;
		bool     gpu_modified = false;
	};
	[[nodiscard]] vk::ImageView GetImageView(GraphicContext* ctx, VulkanImage* image,
	                                         const ImageViewInfo& info);
	[[nodiscard]] bool          HasMetaOverlapLocked(uint64_t vaddr, uint64_t size) const;
	[[nodiscard]] CachedImage*  FindGpuReadbackPageCandidateLocked(uint64_t vaddr, uint64_t size);
	void                        RequireNoMetaOverlapLocked(uint64_t vaddr, uint64_t size) const;
	void                        MarkSampledAliasesCpuDirtyLocked(uint64_t vaddr, uint64_t size);
	void RetireSampledTargetAliases(GraphicContext* ctx, const ImageInfo& requested);
	void ResolveStorageImageOverlaps(GraphicContext* ctx, const ImageInfo& requested);
	void RetireStorageDepthAliasLocked(GraphicContext* ctx, const ImageInfo& requested);
	void RegisterImageLocked(CachedImage& image);
	void UnregisterImageLocked(CachedImage& image, bool release_tracking);
	[[nodiscard]] VulkanImage*              PublishImage(CommandBuffer*               command,
	                                                     std::shared_ptr<CachedImage> image);
	[[nodiscard]] std::vector<CachedImage*> FindImagesInRegionLocked(uint64_t vaddr, uint64_t size,
	                                                                 bool page_overlap);
	void RequireRetirementIsolation(const std::vector<CachedImage*>& retire, const char* operation,
	                                uint64_t address, uint64_t size) const;
	void RetireImages(const std::vector<CachedImage*>& retire,
	                  const CachedImage*               native_image_source = nullptr);
	void SynchronizeColorImageToBufferLocked(CachedImage& cached, uint64_t write_address,
	                                         uint64_t write_size);
	void SynchronizeDepthImageToBufferLocked(CachedImage& cached, uint64_t write_address,
	                                         uint64_t write_size);

	std::unique_ptr<DummyTextureCache>        m_dummy_textures;
	TrackingSpinLock                          m_lock;
	std::mutex                                m_fault_mutex;
	MemoryTracker                             m_memory_tracker;
	MemoryTracker                             m_metadata_tracker;
	Tiler                                     m_tiler;
	BufferCache&                              m_buffer_cache;
	ResourceMutex&                            m_resource_mutex;
	std::vector<std::shared_ptr<CachedImage>> m_images;
	ImageOwnerIndex                           m_image_owner_index;
	std::map<uint64_t, MetaDataInfo>          m_surface_metas;
	std::unique_ptr<ReadbackWorker>           m_readback;
	std::vector<uint8_t>                      m_buffer_transition_linear;
	std::vector<uint8_t>                      m_buffer_transition_guest;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
