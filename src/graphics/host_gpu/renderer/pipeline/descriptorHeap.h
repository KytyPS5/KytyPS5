#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <deque>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class MasterSemaphore;

class DescriptorHeap {
public:
	DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore);
	~DescriptorHeap();
	KYTY_CLASS_NO_COPY(DescriptorHeap);

	[[nodiscard]] vk::DescriptorSet Commit(vk::DescriptorSetLayout layout);
	// A set of a pool of its own that lives as long as the heap. It is never bound: it holds
	// descriptors that bindings copy into the sets Commit() gives them.
	[[nodiscard]] vk::DescriptorSet CommitPersistent(vk::DescriptorSetLayout layout);

	// Records the descriptor counts of a layout whose sets come from this heap, so that the heap
	// asks a pool only for the sets it has room for, and a set larger than a default pool gets a
	// pool sized for it.
	static void RegisterLayout(vk::DescriptorSetLayout                         layout,
	                           std::span<const vk::DescriptorSetLayoutBinding> bindings);

	// Sampled images of one pool; bindless image tables take tens of thousands of them per set
	// (GTA V's ray tracing table: ~82000).
	static constexpr uint32_t MaxSampledImages = 262144;

private:
	static constexpr uint32_t DescriptorSetBatch = 32;

	struct Batch {
		std::array<vk::DescriptorSet, DescriptorSetBatch> sets {};
		uint32_t                                          size = 0;
	};

	struct Pool {
		vk::DescriptorPool                  pool = nullptr;
		std::vector<vk::DescriptorPoolSize> sizes;
	};

	[[nodiscard]] bool Allocate(vk::DescriptorSetLayout layout, Batch& batch);
	[[nodiscard]] const std::vector<vk::DescriptorPoolSize>&
	     LayoutSizes(vk::DescriptorSetLayout layout);
	void CreateDescriptorPool(std::span<const vk::DescriptorPoolSize> sizes);
	void UsePool(Pool pool);
	void RetireCurrentPool();

	GraphicContext&  m_graphics;
	MasterSemaphore& m_master_semaphore;
	Pool             m_current_pool;
	// Descriptors and sets that the current pool still has room for.
	std::vector<vk::DescriptorPoolSize>                m_free;
	uint32_t                                           m_free_sets = 0;
	std::deque<std::pair<Pool, uint64_t>>              m_pending_pools;
	std::vector<vk::DescriptorPool>                    m_persistent_pools;
	std::unordered_map<vk::DescriptorSetLayout, Batch> m_sets;
	std::unordered_map<vk::DescriptorSetLayout, std::vector<vk::DescriptorPoolSize>> m_layout_sizes;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORHEAP_H_
