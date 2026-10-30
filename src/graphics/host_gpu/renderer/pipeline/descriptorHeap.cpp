#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

namespace Libs::Graphics {
namespace {

constexpr uint32_t   DescriptorHeapCount = 1024;
// A pool holds at most this many times the sizes below: no layout of a device needs more.
constexpr uint32_t   MaxDescriptorPoolScale = 64;
constexpr std::array DescriptorPoolSizes = {
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampler, 1024},
};

} // namespace

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore) {
	CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
	m_graphics.device.destroyDescriptorPool(m_current_pool, nullptr);
	for (const auto& pending: m_pending_pools) {
		m_master_semaphore.Wait(pending.tick);
		m_graphics.device.destroyDescriptorPool(pending.pool, nullptr);
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

	m_pending_pools.push_back({m_current_pool, m_master_semaphore.CurrentTick(), m_pool_scale});
	// Pools of an older, smaller size are no use once their work is done.
	while (!m_pending_pools.empty() && m_pending_pools.front().scale < m_pool_scale &&
	       m_master_semaphore.IsFree(m_pending_pools.front().tick)) {
		m_graphics.device.destroyDescriptorPool(m_pending_pools.front().pool, nullptr);
		m_pending_pools.pop_front();
	}
	if (const auto front = m_pending_pools.empty() ? PendingPool {nullptr, 0, 0} : m_pending_pools.front();
	    front.scale == m_pool_scale && m_master_semaphore.IsFree(front.tick)) {
		m_current_pool = front.pool;
		m_pending_pools.pop_front();
		EXIT_IF(m_graphics.device.resetDescriptorPool(m_current_pool, {}) != vk::Result::eSuccess);
	} else {
		CreateDescriptorPool();
	}

	m_sets.clear();
	auto* fresh_batch = &m_sets[layout];
	while (!Allocate(layout, *fresh_batch)) {
		// Not even one set fits an empty pool: the layout has more descriptors of a kind than the
		// pool has. Pools of the next size take it, and so the sets that follow.
		EXIT_IF(m_pool_scale >= MaxDescriptorPoolScale);
		GrowDescriptorPool();
		m_sets.clear();
		fresh_batch = &m_sets[layout];
	}
	return fresh_batch->sets[--fresh_batch->size];
}

void DescriptorHeap::GrowDescriptorPool() {
	m_pending_pools.push_back({m_current_pool, m_master_semaphore.CurrentTick(), m_pool_scale});
	m_pool_scale *= 2;
	CreateDescriptorPool();
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, Batch& batch) {
	std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
	layouts.fill(layout);

	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool = m_current_pool;
	allocate.pSetLayouts    = layouts.data();

	for (;;) {
		allocate.descriptorSetCount = batch.allocation;
		const auto result = m_graphics.device.allocateDescriptorSets(&allocate, batch.sets.data());
		if (result == vk::Result::eSuccess) {
			batch.size = batch.allocation;
			return true;
		}
		EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
		        result != vk::Result::eErrorFragmentedPool);
		if (batch.allocation == 1) {
			return false;
		}
		batch.allocation /= 2;
	}
}

void DescriptorHeap::CreateDescriptorPool() {
	auto sizes = DescriptorPoolSizes;
	for (auto& size: sizes) {
		size.descriptorCount *= m_pool_scale;
	}
	vk::DescriptorPoolCreateInfo create {};
	create.maxSets       = DescriptorHeapCount;
	create.poolSizeCount = static_cast<uint32_t>(sizes.size());
	create.pPoolSizes    = sizes.data();
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &m_current_pool) !=
	        vk::Result::eSuccess);
}

} // namespace Libs::Graphics
