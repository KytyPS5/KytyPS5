#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <deque>
#include <span>
#include <unordered_map>

namespace Libs::Graphics {

struct GraphicContext;
class MasterSemaphore;

class DescriptorHeap {
public:
	DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore);
	~DescriptorHeap();
	KYTY_CLASS_NO_COPY(DescriptorHeap);

	// Per-set totals, with one entry per descriptor type.
	[[nodiscard]] vk::DescriptorSet Commit(vk::DescriptorSetLayout layout,
	                                       std::span<const vk::DescriptorPoolSize> sizes);

private:
	static constexpr uint32_t DescriptorSetBatch = 32;

	struct Batch {
		std::array<vk::DescriptorSet, DescriptorSetBatch> sets {};
		uint32_t                                          size       = 0;
	};

	struct Pool {
		vk::DescriptorPool                   handle = nullptr;
		std::array<vk::DescriptorPoolSize, 4> sizes {};
	};

	[[nodiscard]] bool Allocate(vk::DescriptorSetLayout layout, Batch& batch,
	                            std::span<const vk::DescriptorPoolSize> sizes);
	void CreateDescriptorPool(std::span<const vk::DescriptorPoolSize> sizes = {});

	GraphicContext&                                     m_graphics;
	MasterSemaphore&                                    m_master_semaphore;
	Pool                                               m_current_pool;
	std::array<vk::DescriptorPoolSize, 4>                m_remaining {};
	uint32_t                                           m_remaining_sets = 0;
	std::deque<std::pair<Pool, uint64_t>>                m_pending_pools;
	std::unordered_map<vk::DescriptorSetLayout, Batch>  m_sets;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_
