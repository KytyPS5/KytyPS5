#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/renderer/cachedImageRegistry.h"
#include "graphics/host_gpu/renderer/imageInfo.h"
#include "graphics/host_gpu/renderer/tiler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct DepthStencilVulkanImage;
struct GpuTextureVulkanImage;
struct ImageViewInfo;
struct RenderTextureVulkanImage;
struct StorageTextureVulkanImage;
struct VideoOutVulkanImage;
struct VulkanImage;
struct VulkanMemory;
class BufferCache;
class CommandBuffer;
class DummyTextureCache;
class GpuResourceManager;
class ImageAliasPlan;
class ResourceMutex;

[[nodiscard]] bool IsExactRenderTargetMipStorage(const ImageInfo& sampled, const ImageInfo& storage,
                                                 vk::Format sampled_view_format,
                                                 vk::Format storage_image_format) noexcept;

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
	struct MetaRangeInfo {
		uint64_t metadata_address = 0;
		uint64_t metadata_size    = 0;
		uint32_t slice            = 0;
		bool     full             = false;
	};

	TextureCache(PageManager& page_manager, BufferCache& buffer_cache, ResourceMutex& resource_mutex);
	~TextureCache();
	KYTY_CLASS_NO_COPY(TextureCache);

	[[nodiscard]] VulkanImage& FindTexture(CommandBuffer& command, const ImageInfo& info,
	                                       bool metadata_read);
	[[nodiscard]] StorageTextureVulkanImage& FindStorageTexture(CommandBuffer&   command,
	                                                            const ImageInfo& info);
	[[nodiscard]] RenderTextureVulkanImage&  FindRenderTarget(CommandBuffer&          command,
	                                                          const RenderTargetInfo& info);
	[[nodiscard]] DepthStencilVulkanImage&   FindDepthTarget(CommandBuffer&         command,
	                                                         const DepthTargetInfo& info);
	[[nodiscard]] std::vector<VideoOutVulkanImage*>
	                   RegisterVideoOutSurfaces(const std::vector<VideoOutInfo>& infos);
	void               RefreshVideoOut(VideoOutVulkanImage& image, bool render_target = false);
	void               UnregisterVideoOutSurfaces(const std::vector<VideoOutVulkanImage*>& images);
	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t vaddr, uint64_t size,
	                                        uint32_t packed_clear);
	void               MarkGpuWritten(VulkanImage& image);
	void               PrepareHostWrite(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool InvalidateMemoryFromGPU(uint64_t vaddr, uint64_t size,
	                                           bool formatted_buffer_write = false);
	[[nodiscard]] RenderTextureVulkanImage* FindRenderTargetByRange(CommandBuffer& command,
	                                                                uint64_t vaddr, uint64_t size);
	[[nodiscard]] vk::ImageView GetRenderTargetAttachmentView(RenderTextureVulkanImage& image,
	                                                          vk::Format format, uint32_t level,
	                                                          uint32_t base_layer,
	                                                          uint32_t layer_count);
	[[nodiscard]] vk::ImageView GetDepthTargetAttachmentView(DepthStencilVulkanImage& image,
	                                                         uint32_t                 base_layer,
	                                                         uint32_t                 layer_count);
	[[nodiscard]] vk::ImageView
	GetDepthTargetSampledView(DepthStencilVulkanImage& image, vk::Format view_format,
	                          uint32_t swizzle, uint32_t base_level, uint32_t level_count,
	                          vk::ImageViewType type, uint32_t base_layer, uint32_t layer_count);
	[[nodiscard]] vk::ImageView GetSampledColorView(VulkanImage& image, vk::Format view_format,
	                                                uint32_t swizzle, uint32_t base_level,
	                                                uint32_t level_count, vk::ImageViewType type,
	                                                uint32_t base_layer, uint32_t layer_count);
	[[nodiscard]] vk::ImageView
	GetRenderTargetStorageView(RenderTextureVulkanImage& image, vk::Format view_format,
	                           uint32_t base_level, uint32_t level_count, vk::ImageViewType type,
	                           uint32_t base_layer, uint32_t layer_count);
	[[nodiscard]] vk::ImageView GetStorageTextureSampledView(StorageTextureVulkanImage& image,
	                                                         const ImageInfo&           info);
	[[nodiscard]] vk::ImageView GetStorageTextureStorageView(StorageTextureVulkanImage& image,
	                                                         uint32_t                   base_level);
	[[nodiscard]] DepthStencilVulkanImage*
	FindDepthTargetByRange(CommandBuffer& command, uint64_t vaddr, uint64_t size,
	                       bool allow_containing_sampled = false);
	[[nodiscard]] RegionInfo QueryRegion(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool       ResolveMetaRange(uint64_t vaddr, uint64_t size, MetaRangeInfo& info);
	void                     RegisterMeta(uint64_t vaddr, uint64_t size, uint32_t layers = 1);
	[[nodiscard]] bool       IsMeta(uint64_t vaddr);
	[[nodiscard]] bool       IsMetaRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool       IsMetaCleared(uint64_t vaddr, uint32_t slice);
	[[nodiscard]] bool       ClearMeta(uint64_t vaddr);
	[[nodiscard]] bool       TouchMeta(uint64_t vaddr, uint32_t slice, bool is_clear);
	[[nodiscard]] bool       InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
	                                          PageFaultPhase phase) noexcept;
	VulkanImage& GetDummySampledTexture(bool uint_format, bool image_3d);
	VulkanImage& GetDummyStorageTexture(bool uint_format, bool image_3d);

private:
	friend class GpuResourceManager;
	using CachedImage = CachedImageRecord;
	using AliasRetirementPlan = ImageAliasPlan;
	struct UnmapPlan;
	struct ReadbackWorker;
	struct MetaDataInfo {
		uint64_t size         = 0;
		uint32_t layers       = 1;
		uint32_t clear_mask   = 0;
		bool     gpu_modified = false;
	};
	[[nodiscard]] vk::ImageView GetImageView(VulkanImage& image, const ImageViewInfo& info);
	[[nodiscard]] bool          HasMetaOverlapLocked(uint64_t vaddr, uint64_t size) const;
	[[nodiscard]] CachedImage*  FindGpuReadbackPageCandidateLocked(uint64_t vaddr, uint64_t size);
	void                        RequireNoMetaOverlapLocked(uint64_t vaddr, uint64_t size) const;
	void                        ResolveImageMetadataOverlapsLocked(uint64_t vaddr, uint64_t size);
	void                        MarkSampledAliasesCpuDirtyLocked(uint64_t vaddr, uint64_t size);
	void                        RetireSampledTargetAliases(const ImageInfo& requested);
	void                        ResolveStorageImageOverlaps(const ImageInfo& requested);
	void                        RetireStorageDepthAliasLocked(const ImageInfo& requested);
	void                        RemoveImageLocked(CachedImage& image, bool release_tracking);
	[[nodiscard]] std::shared_ptr<CachedImage>
	FindImageOwnerLocked(const CachedImage& image) const;
	[[nodiscard]] std::shared_ptr<CachedImage> FindImageOwnerLocked(VulkanImage& image) const;
	[[nodiscard]] VulkanImage&  PublishImage(CommandBuffer&               command,
	                                         std::shared_ptr<CachedImage> image);
	[[nodiscard]] std::vector<CachedImage*> FindImagesInRegionLocked(uint64_t vaddr, uint64_t size,
	                                                                 CachedImageRegistry::QueryMode mode);
	[[nodiscard]] std::vector<CachedImage*>
	FindImagesInRegionsLocked(std::span<const GuestRange> ranges,
	                          CachedImageRegistry::QueryMode mode);
	void RequireRetirementIsolation(const std::vector<CachedImage*>& retire, const char* operation,
	                                uint64_t address, uint64_t size) const;
	void RetireImages(const std::vector<CachedImage*>& retire,
	                  const CachedImage*               native_image_source = nullptr);
	void RetireDepthMetadataLocked(const std::vector<CachedImage*>& retire,
	                               uint64_t                         preserve_address = 0);
	void ExecuteAliasRetirementLocked(const AliasRetirementPlan& plan);
	void MaterializeImagesToGuestLocked(const std::vector<std::shared_ptr<CachedImage>>& images);
	void SynchronizeColorImageToBufferLocked(CachedImage& cached, uint64_t write_address,
	                                         uint64_t write_size);
	void SynchronizeDepthImageToBufferLocked(CachedImage& cached, uint64_t write_address,
	                                         uint64_t write_size);
	[[nodiscard]] UnmapPlan BuildUnmapPlanLocked(uint64_t vaddr, uint64_t size);
	void                    ValidateUnmapMemory(uint64_t vaddr, uint64_t size);
	void                    CommitUnmapMemory(uint64_t vaddr, uint64_t size);

	std::unique_ptr<DummyTextureCache>                          m_dummy_textures;
	TrackingSpinLock                                            m_lock;
	std::mutex                                                  m_fault_mutex;
	MemoryTracker                                               m_memory_tracker;
	MemoryTracker                                               m_metadata_tracker;
	Tiler                                                       m_tiler;
	BufferCache&                                                m_buffer_cache;
	ResourceMutex&                                              m_resource_mutex;
	CachedImageRegistry                                        m_image_registry;
	std::map<uint64_t, MetaDataInfo>                            m_surface_metas;
	std::unique_ptr<ReadbackWorker>                             m_readback;
	std::vector<uint8_t>                                        m_buffer_transition_guest;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
