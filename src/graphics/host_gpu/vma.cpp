#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vma.h"

#include <atomic>
#include <cinttypes>
#include <fmt/format.h>
#include <string>
#include <vector>
#include <vulkan/vk_enum_string_helper.h>

namespace Libs::Graphics {

namespace {

struct MemoryStats {
	std::atomic_uint64_t allocated[VK_MAX_MEMORY_TYPES] {};
	std::atomic_uint64_t count[VK_MAX_MEMORY_TYPES] {};
};

MemoryStats g_memory_stats;

void TrackAllocationImpl(const VulkanMemory& memory) {
	g_memory_stats.allocated[memory.type] += memory.requirements.size;
	g_memory_stats.count[memory.type]++;
}

void UntrackAllocationImpl(const VulkanMemory& memory) {
	EXIT_IF(g_memory_stats.allocated[memory.type] < memory.requirements.size);
	EXIT_IF(g_memory_stats.count[memory.type] == 0);
	g_memory_stats.allocated[memory.type] -= memory.requirements.size;
	g_memory_stats.count[memory.type]--;
}

} // namespace

void VulkanTrackAllocation(const VulkanMemory* memory) {
	EXIT_IF(memory == nullptr);
	TrackAllocationImpl(*memory);
}

void VulkanUntrackAllocation(const VulkanMemory* memory) {
	EXIT_IF(memory == nullptr);
	UntrackAllocationImpl(*memory);
}

bool VulkanCreateAllocator(GraphicContext* ctx) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || ctx->instance == nullptr || ctx->physical_device == nullptr ||
	        ctx->device == nullptr || ctx->allocator != nullptr);

	VmaAllocatorCreateInfo info {};
	info.instance         = ctx->instance;
	info.physicalDevice   = ctx->physical_device;
	info.device           = ctx->device;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = ctx->memory_budget_ext_enabled ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;

	const auto result = vmaCreateAllocator(&info, &ctx->allocator);
	if (result != VK_SUCCESS) {
		LOGF("vmaCreateAllocator failed: %s\n", string_VkResult(result));
		return false;
	}
	return true;
}

void VulkanDestroyAllocator(GraphicContext* ctx) {
	if (ctx == nullptr || ctx->allocator == nullptr) {
		return;
	}
	vmaDestroyAllocator(ctx->allocator);
	ctx->allocator = nullptr;
}

uint64_t VulkanNextMemoryUniqueId() {
	static std::atomic_uint64_t sequence = 0;
	return ++sequence;
}

void VulkanLogMemoryBudget(GraphicContext* ctx) {
	if (ctx == nullptr || ctx->allocator == nullptr || ctx->physical_device == nullptr) {
		return;
	}

	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &properties);
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(ctx->allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

bool VulkanAllocate(GraphicContext* ctx, VulkanMemory* memory) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || memory == nullptr || memory->memory != nullptr ||
	        memory->allocation != nullptr || ctx->allocator == nullptr ||
	        memory->requirements.size == 0);

	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &properties);
	uint32_t index = 0;
	for (; index < properties.memoryTypeCount; index++) {
		if ((memory->requirements.memoryTypeBits & (uint32_t {1} << index)) != 0 &&
		    (properties.memoryTypes[index].propertyFlags & memory->property) == memory->property) {
			break;
		}
	}

	VmaAllocationCreateInfo info {};
	info.requiredFlags = memory->property;
	memory->unique_id  = VulkanNextMemoryUniqueId();
	const auto result  = vmaAllocateMemory(ctx->allocator, &memory->requirements, &info,
	                                       &memory->allocation, &memory->allocation_info);
	if (result == VK_SUCCESS) {
		memory->type   = memory->allocation_info.memoryType;
		memory->memory = memory->allocation_info.deviceMemory;
		memory->offset = memory->allocation_info.offset;
		VulkanTrackAllocation(memory);
		return true;
	}

	VulkanLogMemoryBudget(ctx);
	std::vector<std::string> stats;
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
		stats.push_back(fmt::format("{}, {}, {}", i, g_memory_stats.count[i].load(),
		                            g_memory_stats.allocated[i].load()));
	}
	EXIT("size = %" PRIu64 ", index = %u, error: %s:%s\n", memory->requirements.size, index,
	     string_VkResult(result), Common::Concat(stats, '\n').c_str());
	return false;
}

void VulkanFree(GraphicContext* ctx, VulkanMemory* memory) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || memory == nullptr || ctx->allocator == nullptr ||
	        memory->allocation == nullptr);
	vmaFreeMemory(ctx->allocator, memory->allocation);
	VulkanUntrackAllocation(memory);
	memory->memory          = nullptr;
	memory->allocation      = nullptr;
	memory->allocation_info = {};
	memory->offset          = 0;
}

void VulkanMapMemory(GraphicContext* ctx, VulkanMemory* memory, void** data) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || memory == nullptr || data == nullptr || ctx->allocator == nullptr ||
	        memory->allocation == nullptr);
	EXIT_NOT_IMPLEMENTED(vmaMapMemory(ctx->allocator, memory->allocation, data) != VK_SUCCESS);
}

void VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* memory) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || memory == nullptr || ctx->allocator == nullptr ||
	        memory->allocation == nullptr);
	vmaUnmapMemory(ctx->allocator, memory->allocation);
}

void VulkanBindImageMemory(GraphicContext* ctx, VulkanImage* image, VulkanMemory* memory) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || image == nullptr || memory == nullptr || ctx->allocator == nullptr ||
	        memory->allocation == nullptr);
	EXIT_NOT_IMPLEMENTED(vmaBindImageMemory(ctx->allocator, memory->allocation, image->image) !=
	                     VK_SUCCESS);
}

void VulkanBindBufferMemory(GraphicContext* ctx, VulkanBuffer* buffer, VulkanMemory* memory) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || buffer == nullptr || memory == nullptr || ctx->allocator == nullptr ||
	        memory->allocation == nullptr);
	EXIT_NOT_IMPLEMENTED(vmaBindBufferMemory(ctx->allocator, memory->allocation, buffer->buffer) !=
	                     VK_SUCCESS);
}

} // namespace Libs::Graphics
