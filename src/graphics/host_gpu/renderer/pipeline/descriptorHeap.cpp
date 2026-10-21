#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <algorithm>

namespace Libs::Graphics {
namespace {

constexpr uint32_t   DescriptorHeapCount = 1024;
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
	m_graphics.device.destroyDescriptorPool(m_current_pool.handle, nullptr);
	for (const auto& [pool, tick]: m_pending_pools) {
		m_master_semaphore.Wait(tick);
		m_graphics.device.destroyDescriptorPool(pool.handle, nullptr);
	}
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout layout,
                                          std::span<const vk::DescriptorPoolSize> sizes) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(layout == nullptr);

	auto& batch = m_sets[layout];
	if (batch.size != 0) {
		return batch.sets[--batch.size];
	}
	if (Allocate(layout, batch, sizes)) {
		return batch.sets[--batch.size];
	}

	m_pending_pools.emplace_back(m_current_pool, m_master_semaphore.CurrentTick());
	if (const auto& [pool, tick] = m_pending_pools.front(); m_master_semaphore.IsFree(tick)) {
		m_current_pool = pool;
		m_pending_pools.pop_front();
		EXIT_IF(m_graphics.device.resetDescriptorPool(m_current_pool.handle, {}) !=
		        vk::Result::eSuccess);
		m_remaining = m_current_pool.sizes;
		m_remaining_sets = DescriptorHeapCount;
	} else {
		CreateDescriptorPool(sizes);
	}

	m_sets.clear();
	auto& fresh_batch = m_sets[layout];
	if (!Allocate(layout, fresh_batch, sizes)) {
		// A recycled pool may predate a larger layout.
		m_graphics.device.destroyDescriptorPool(m_current_pool.handle, nullptr);
		CreateDescriptorPool(sizes);
		EXIT_IF(!Allocate(layout, fresh_batch, sizes));
	}
	return fresh_batch.sets[--fresh_batch.size];
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, Batch& batch,
                              std::span<const vk::DescriptorPoolSize> sizes) {
	auto count = std::min(DescriptorSetBatch, m_remaining_sets);
	for (const auto& size: sizes) {
		EXIT_IF(size.descriptorCount == 0);
		const auto available = std::ranges::find(m_remaining, size.type, &vk::DescriptorPoolSize::type);
		EXIT_IF(available == m_remaining.end());
		count = std::min(count, available->descriptorCount / size.descriptorCount);
	}
	if (count == 0) {
		return false;
	}
	std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
	layouts.fill(layout);

	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool     = m_current_pool.handle;
	allocate.pSetLayouts        = layouts.data();
	allocate.descriptorSetCount = count;
	const auto result = m_graphics.device.allocateDescriptorSets(&allocate, batch.sets.data());
	if (result != vk::Result::eSuccess) {
		EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
		        result != vk::Result::eErrorFragmentedPool);
		return false;
	}
	for (const auto& size: sizes) {
		auto available = std::ranges::find(m_remaining, size.type, &vk::DescriptorPoolSize::type);
		available->descriptorCount -= count * size.descriptorCount;
	}
	m_remaining_sets -= count;
	batch.size = count;
	return true;
}

void DescriptorHeap::CreateDescriptorPool(std::span<const vk::DescriptorPoolSize> sizes) {
	m_current_pool.sizes = DescriptorPoolSizes;
	for (const auto& size: sizes) {
		auto capacity = std::ranges::find(m_current_pool.sizes, size.type, &vk::DescriptorPoolSize::type);
		EXIT_IF(capacity == m_current_pool.sizes.end());
		capacity->descriptorCount = std::max(capacity->descriptorCount, size.descriptorCount);
	}
	vk::DescriptorPoolCreateInfo create {};
	create.maxSets       = DescriptorHeapCount;
	create.poolSizeCount = static_cast<uint32_t>(m_current_pool.sizes.size());
	create.pPoolSizes    = m_current_pool.sizes.data();
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &m_current_pool.handle) !=
	        vk::Result::eSuccess);
	m_remaining = m_current_pool.sizes;
	m_remaining_sets = DescriptorHeapCount;
}

} // namespace Libs::Graphics
