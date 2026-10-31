#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

namespace Libs::Graphics {
namespace {

constexpr uint32_t DescriptorHeapCount = 1024;
// Sized for layouts with large texture arrays: a set must fit in a fresh pool.
constexpr std::array DescriptorPoolSizes = {
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 32768},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampledImage, 65536},
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampler, 8192},
};

} // namespace

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore) {
	CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
	m_graphics.device.destroyDescriptorPool(m_current_pool, nullptr);
	for (const auto& [pool, tick]: m_pending_pools) {
		m_master_semaphore.Wait(tick);
		m_graphics.device.destroyDescriptorPool(pool, nullptr);
	}
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout layout) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(layout == nullptr);

	auto& batch = m_sets[layout];
	if (batch.size != 0) {
		return batch.sets[--batch.size];
	}
	if (Allocate(layout, batch)) {
		return batch.sets[--batch.size];
	}

	m_pending_pools.emplace_back(m_current_pool, m_master_semaphore.CurrentTick());
	if (const auto& [pool, tick] = m_pending_pools.front(); m_master_semaphore.IsFree(tick)) {
		m_current_pool = pool;
		m_pending_pools.pop_front();
		EXIT_IF(m_graphics.device.resetDescriptorPool(m_current_pool, {}) != vk::Result::eSuccess);
	} else {
		CreateDescriptorPool();
	}

	m_sets.clear();
	auto& fresh_batch = m_sets[layout];
	EXIT_IF(!Allocate(layout, fresh_batch));
	return fresh_batch.sets[--fresh_batch.size];
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, Batch& batch) {
	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool     = m_current_pool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts        = &layout;

	// Allocate one set at a time: a multi-set allocation that runs out of pool memory can keep
	// the memory of the sets it made before failing, leaving no room for a smaller retry.
	batch.size = 0;
	while (batch.size < batch.allocation) {
		const auto result =
		    m_graphics.device.allocateDescriptorSets(&allocate, &batch.sets[batch.size]);
		if (result != vk::Result::eSuccess) {
			EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
			        result != vk::Result::eErrorFragmentedPool);
			break;
		}
		batch.size++;
	}
	return batch.size != 0;
}

void DescriptorHeap::CreateDescriptorPool() {
	vk::DescriptorPoolCreateInfo create {};
	create.maxSets       = DescriptorHeapCount;
	create.poolSizeCount = static_cast<uint32_t>(DescriptorPoolSizes.size());
	create.pPoolSizes    = DescriptorPoolSizes.data();
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &m_current_pool) !=
	        vk::Result::eSuccess);
}

} // namespace Libs::Graphics
