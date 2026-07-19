#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <memory>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
struct DepthRenderTarget;
} // namespace HW

struct GraphicContext;
struct ShaderBufferResource;
struct ShaderComputeInputInfo;
struct VideoOutVulkanImage;
struct DepthStencilVulkanImage;
struct TextureVulkanImage;
struct StorageTextureVulkanImage;
struct RenderTextureVulkanImage;
struct VulkanCommandPool;
struct VulkanBuffer;
struct VulkanDescriptorSet;
struct VulkanFramebuffer;
struct RenderDepthInfo;
struct RenderColorInfo;
class BufferCache;

struct HtileClearTarget {
	uint64_t address = 0;
	uint64_t size    = 0;
};

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	EopWrite,
	EopInterrupt,
	EopWriteBack,
	EopFlip,
	EopWriteBackFlip,
	EopOnlyFlip,
	Unknown,
};

class FenceResourceRetainer {
public:
	FenceResourceRetainer() = default;
	~FenceResourceRetainer();
	KYTY_CLASS_NO_COPY(FenceResourceRetainer);

	void               Retain(std::shared_ptr<void> resource);
	void               ReleaseAfterFence() noexcept;
	[[nodiscard]] bool Empty() const noexcept { return m_resources.empty(); }

private:
	std::vector<std::shared_ptr<void>> m_resources;
};

class CommandBuffer {
public:
	explicit CommandBuffer(int queue): m_queue(queue) { Allocate(); }
	~CommandBuffer() { Free(); }

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const;

	void Allocate();
	void Free();
	void Begin() const;
	void End() const;
	void Execute();
	void ExecuteWithSemaphore(vk::Semaphore signal_semaphore = nullptr);
	void ExecuteWithSemaphore(vk::Semaphore wait_semaphore, vk::PipelineStageFlags wait_stage,
	                          vk::Semaphore signal_semaphore);
	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0,
	                  uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	void BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* colors,
	                     uint32_t color_count, RenderDepthInfo* depth) const;
	void EndRenderPass() const;
	void WaitForFenceOnly();
	void WaitForFence();
	void WaitForFenceAndReset();
	void DeleteAfterFence(VulkanBuffer* buffer);
	void RetainResourceUntilFence(std::shared_ptr<void> resource);
	void RecycleDescriptorAfterFence(VulkanDescriptorSet* set);

	[[nodiscard]] vk::CommandBuffer Handle() const;
	[[nodiscard]] int               GetQueue() const { return m_queue; }
	[[nodiscard]] bool              IsExecute() const { return m_execute; }
	[[nodiscard]] uint64_t GetRecordingGeneration() const { return m_recording_generation; }

private:
	friend class BufferCache;

	void Submit(vk::Semaphore wait_semaphore, vk::PipelineStageFlags wait_stage,
	            vk::Semaphore signal_semaphore);
	[[nodiscard]] vk::Semaphore ResolveSignalSemaphore(vk::Semaphore semaphore) const;
	void                        FinalizeFence(bool reset_recording);
	void                        ReleaseResourcesAfterFence();
	void                        DeleteBuffersAfterFence();
	void                        RecycleDescriptorsAfterFence();

	VulkanCommandPool*                m_pool                 = nullptr;
	uint32_t                          m_index                = static_cast<uint32_t>(-1);
	int                               m_queue                = -1;
	bool                              m_execute              = false;
	bool                              m_fence_waited         = false;
	uint64_t                          m_submit_seq           = 0;
	uint64_t                          m_recording_generation = 0;
	uint32_t                          m_debug_op             = 0;
	uint64_t                          m_debug_submit_id      = 0;
	uint32_t                          m_debug_arg0           = 0;
	uint32_t                          m_debug_arg1           = 0;
	uint32_t                          m_debug_arg2           = 0;
	uint32_t                          m_debug_arg3           = 0;
	uint64_t                          m_debug_arg4           = 0;
	std::vector<VulkanBuffer*>        m_delete_after_fence;
	FenceResourceRetainer             m_fence_resources;
	std::vector<VulkanDescriptorSet*> m_descriptor_sets_after_fence;
	HostStreamBuffer                  m_host_stream;
};

void RenderDrawIndex(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                     HW::UserConfig* ucfg, HW::Shader* sh_ctx, uint32_t index_type_and_size,
                     uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
                     uint32_t instance_count = 1, uint32_t render_target_slice_offset = 0,
                     int32_t vertex_offset_add = 0, uint32_t first_instance = 0);
void RenderDrawIndexAuto(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                         HW::UserConfig* ucfg, HW::Shader* sh_ctx, uint32_t index_count,
                         uint32_t flags, uint32_t render_target_slice_offset = 0,
                         uint32_t instance_count = 1, uint32_t first_vertex = 0,
                         uint32_t first_instance = 0);
void RenderDispatchDirect(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx,
                          HW::Shader* sh_ctx, uint32_t thread_group_x, uint32_t thread_group_y,
                          uint32_t thread_group_z, uint32_t mode);

void GraphicsRenderInit();
void GraphicsRenderCreateContext();
void GraphicsRenderReleaseThreadCommandPools();

[[nodiscard]] bool ResolveComputeImageClear(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource* descriptor,
                                            uint32_t* packed_clear, uint64_t* size);
[[nodiscard]] bool ResolveHtileClearTarget(const HW::DepthRenderTarget& target,
                                           uint64_t descriptor_size, HtileClearTarget* resolved);

void GraphicsRenderMemoryBarrier(CommandBuffer* buffer);
void GraphicsRenderTextureBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size);
void GraphicsRenderDepthStencilBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
