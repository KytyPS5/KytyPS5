#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <algorithm>
#include <mutex>
#include <string>

namespace Libs::Graphics {
namespace {

constexpr uint32_t   DescriptorHeapCount = 1024;
constexpr std::array DescriptorPoolSizes = {
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eUniformBuffer, 4096},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampledImage, DescriptorHeap::MaxSampledImages},
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampler, 1024},
};

// Sets of a layout that does not fit a default pool that its own pool holds.
constexpr uint32_t LargeSetsPerPool = 4;

std::mutex                                                                       g_layout_mutex;
std::unordered_map<vk::DescriptorSetLayout, std::vector<vk::DescriptorPoolSize>> g_layout_sizes;

vk::DescriptorPoolSize* FindSize(std::vector<vk::DescriptorPoolSize>& sizes,
                                 vk::DescriptorType                   type) {
	const auto size = std::find_if(sizes.begin(), sizes.end(),
	                               [&](const auto& size) { return size.type == type; });
	return size == sizes.end() ? nullptr : &*size;
}

// The default pool sizes, raised to hold LargeSetsPerPool sets of the layout.
std::vector<vk::DescriptorPoolSize> LargePoolSizes(std::span<const vk::DescriptorPoolSize> layout) {
	std::vector<vk::DescriptorPoolSize> sizes(DescriptorPoolSizes.begin(),
	                                          DescriptorPoolSizes.end());
	for (const auto& needed: layout) {
		const auto count = needed.descriptorCount * LargeSetsPerPool;
		if (auto* size = FindSize(sizes, needed.type); size != nullptr) {
			size->descriptorCount = std::max(size->descriptorCount, count);
		} else {
			sizes.push_back({needed.type, count});
		}
	}
	return sizes;
}

} // namespace

void DescriptorHeap::RegisterLayout(vk::DescriptorSetLayout                         layout,
                                    std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	std::vector<vk::DescriptorPoolSize> sizes;
	for (const auto& binding: bindings) {
		if (binding.descriptorCount == 0) {
			continue;
		}
		if (auto* size = FindSize(sizes, binding.descriptorType); size != nullptr) {
			size->descriptorCount += binding.descriptorCount;
		} else {
			sizes.push_back({binding.descriptorType, binding.descriptorCount});
		}
	}
	std::lock_guard lock(g_layout_mutex);
	g_layout_sizes[layout] = std::move(sizes);
}

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore) {
	CreateDescriptorPool(DescriptorPoolSizes);
}

DescriptorHeap::~DescriptorHeap() {
	m_graphics.device.destroyDescriptorPool(m_current_pool.pool, nullptr);
	for (const auto pool: m_persistent_pools) {
		m_graphics.device.destroyDescriptorPool(pool, nullptr);
	}
	for (const auto& [pool, tick]: m_pending_pools) {
		m_master_semaphore.Wait(tick);
		m_graphics.device.destroyDescriptorPool(pool.pool, nullptr);
	}
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout layout) {
	KYTY_PROFILER_FUNCTION();
	auto& batch = m_sets[layout];
	if (batch.size != 0) {
		return batch.sets[--batch.size];
	}
	if (Allocate(layout, batch)) {
		return batch.sets[--batch.size];
	}

	RetireCurrentPool();
	if (!m_master_semaphore.IsFree(m_pending_pools.front().second)) {
		m_master_semaphore.Refresh();
	}
	if (m_master_semaphore.IsFree(m_pending_pools.front().second)) {
		auto pool = std::move(m_pending_pools.front().first);
		m_pending_pools.pop_front();
		EXIT_IF(m_graphics.device.resetDescriptorPool(pool.pool, {}) != vk::Result::eSuccess);
		UsePool(std::move(pool));
	} else {
		CreateDescriptorPool(DescriptorPoolSizes);
	}

	m_sets.clear();
	auto& fresh_batch = m_sets[layout];
	if (Allocate(layout, fresh_batch)) {
		return fresh_batch.sets[--fresh_batch.size];
	}

	// The set has more descriptors of a type than an empty default pool holds (drivers such as
	// AMD's enforce the pool sizes): give it a pool that also holds a few of its sets.
	const auto sizes = LargePoolSizes(LayoutSizes(layout));
	RetireCurrentPool();
	CreateDescriptorPool(sizes);
	m_sets.clear();
	auto& large_batch = m_sets[layout];
	if (!Allocate(layout, large_batch)) {
		std::string counts;
		for (const auto& size: sizes) {
			counts += " " + vk::to_string(size.type) + "=" + std::to_string(size.descriptorCount);
		}
		EXIT("descriptor set does not fit a pool sized for it:%s\n", counts.c_str());
	}
	return large_batch.sets[--large_batch.size];
}

vk::DescriptorSet DescriptorHeap::CommitPersistent(vk::DescriptorSetLayout layout) {
	const auto&                  sizes = LayoutSizes(layout);
	vk::DescriptorPoolCreateInfo create {};
	create.maxSets       = 1;
	create.poolSizeCount = static_cast<uint32_t>(sizes.size());
	create.pPoolSizes    = sizes.data();
	vk::DescriptorPool pool;
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &pool) !=
	        vk::Result::eSuccess);
	m_persistent_pools.push_back(pool);
	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool     = pool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts        = &layout;
	vk::DescriptorSet set;
	EXIT_IF(m_graphics.device.allocateDescriptorSets(&allocate, &set) != vk::Result::eSuccess);
	return set;
}

const std::vector<vk::DescriptorPoolSize>&
DescriptorHeap::LayoutSizes(vk::DescriptorSetLayout layout) {
	auto found = m_layout_sizes.find(layout);
	if (found == m_layout_sizes.end()) {
		std::vector<vk::DescriptorPoolSize> sizes;
		{
			std::lock_guard lock(g_layout_mutex);
			if (const auto registered = g_layout_sizes.find(layout);
			    registered != g_layout_sizes.end()) {
				sizes = registered->second;
			}
		}
		found = m_layout_sizes.emplace(layout, std::move(sizes)).first;
	}
	return found->second;
}

void DescriptorHeap::RetireCurrentPool() {
	m_pending_pools.emplace_back(std::move(m_current_pool), m_master_semaphore.CurrentTick());
	m_current_pool = {};
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, Batch& batch) {
	// Ask only for the sets that the pool has room for: a failed allocation can keep the room of
	// the sets it did create (AMD's driver does), so asking an empty default pool for 32 sets of
	// GTA V's bindless ray tracing table (20000-41000 sampled images each) left it unable to hold
	// even one.
	const auto& needs = LayoutSizes(layout);
	uint32_t    count = std::min(DescriptorSetBatch, m_free_sets);
	for (const auto& needed: needs) {
		const auto* free = FindSize(m_free, needed.type);
		count =
		    std::min(count, free == nullptr ? 0 : free->descriptorCount / needed.descriptorCount);
	}
	if (count == 0) {
		return false;
	}

	std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
	layouts.fill(layout);

	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool     = m_current_pool.pool;
	allocate.descriptorSetCount = count;
	allocate.pSetLayouts        = layouts.data();

	const auto result = m_graphics.device.allocateDescriptorSets(&allocate, batch.sets.data());
	if (result != vk::Result::eSuccess) {
		EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
		        result != vk::Result::eErrorFragmentedPool);
		// The driver needs more room than the descriptor counts say, and the failure may have
		// used some of it up: take no more sets from this pool.
		m_free_sets = 0;
		return false;
	}
	m_free_sets -= count;
	for (const auto& needed: needs) {
		FindSize(m_free, needed.type)->descriptorCount -= needed.descriptorCount * count;
	}
	batch.size = count;
	return true;
}

void DescriptorHeap::CreateDescriptorPool(std::span<const vk::DescriptorPoolSize> sizes) {
	vk::DescriptorPoolCreateInfo create {};
	create.maxSets       = DescriptorHeapCount;
	create.poolSizeCount = static_cast<uint32_t>(sizes.size());
	create.pPoolSizes    = sizes.data();
	Pool pool;
	pool.sizes.assign(sizes.begin(), sizes.end());
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &pool.pool) !=
	        vk::Result::eSuccess);
	UsePool(std::move(pool));
}

void DescriptorHeap::UsePool(Pool pool) {
	m_current_pool = std::move(pool);
	m_free         = m_current_pool.sizes;
	m_free_sets    = DescriptorHeapCount;
}

} // namespace Libs::Graphics
