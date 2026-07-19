#include "graphics/host_gpu/vulkanCommon.h"

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
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vma.h"

#include <atomic>
#include <cinttypes>

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

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = ctx->instance;
	info.physicalDevice   = ctx->physical_device;
	info.device           = ctx->device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = ctx->memory_budget_ext_enabled ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &ctx->allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", VulkanToString(result).c_str());
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

	const auto& properties = ctx->GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(ctx->allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

void VulkanCreateBuffer(GraphicContext* ctx, uint64_t size, VulkanBuffer* buffer) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || ctx->allocator == nullptr || buffer == nullptr ||
	        buffer->buffer != nullptr || buffer->memory.allocation != nullptr || size == 0);

	vk::BufferCreateInfo buffer_info {};
	buffer_info.sType       = vk::StructureType::eBufferCreateInfo;
	buffer_info.size        = size;
	buffer_info.usage       = buffer->usage;
	buffer_info.sharingMode = vk::SharingMode::eExclusive;

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags =
	    static_cast<vk::MemoryPropertyFlags::MaskType>(buffer->memory.property);

	vk::Buffer::CType native_buffer = VK_NULL_HANDLE;
	const auto        result        = static_cast<vk::Result>(vmaCreateBuffer(
	    ctx->allocator, static_cast<const vk::BufferCreateInfo::NativeType*>(buffer_info),
	    &alloc_info, &native_buffer, &buffer->memory.allocation, &buffer->memory.allocation_info));
	buffer->buffer                  = native_buffer;
	if (result != vk::Result::eSuccess) {
		VulkanLogMemoryBudget(ctx);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	ctx->device.getBufferMemoryRequirements(buffer->buffer, &buffer->memory.requirements);
	buffer->memory.type      = buffer->memory.allocation_info.memoryType;
	buffer->memory.memory    = buffer->memory.allocation_info.deviceMemory;
	buffer->memory.offset    = buffer->memory.allocation_info.offset;
	buffer->memory.unique_id = VulkanNextMemoryUniqueId();
	buffer->buffer_size      = size;
	VulkanTrackAllocation(&buffer->memory);
}

void VulkanDeleteBuffer(GraphicContext* ctx, VulkanBuffer* buffer) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || ctx->allocator == nullptr || buffer == nullptr ||
	        buffer->buffer == nullptr || buffer->memory.allocation == nullptr);

	VulkanUntrackAllocation(&buffer->memory);
	vmaDestroyBuffer(ctx->allocator, buffer->buffer, buffer->memory.allocation);
	buffer->buffer                 = nullptr;
	buffer->memory.memory          = nullptr;
	buffer->memory.allocation      = nullptr;
	buffer->memory.allocation_info = {};
	buffer->memory.offset          = 0;
}

bool VulkanCreateImage(GraphicContext* ctx, const vk::ImageCreateInfo& image_info,
                       VulkanImage* image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || ctx->allocator == nullptr || image == nullptr ||
	        image->image != nullptr || image->memory.allocation != nullptr);

	auto&                   memory = image->memory;
	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = static_cast<vk::MemoryPropertyFlags::MaskType>(memory.property);

	vk::Image::CType native_image = VK_NULL_HANDLE;
	const auto       result       = static_cast<vk::Result>(vmaCreateImage(
	    ctx->allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
	    &alloc_info, &native_image, &memory.allocation, &memory.allocation_info));
	image->image                  = native_image;
	if (result != vk::Result::eSuccess) {
		VulkanLogMemoryBudget(ctx);
		return false;
	}

	ctx->device.getImageMemoryRequirements(image->image, &memory.requirements);
	memory.type      = memory.allocation_info.memoryType;
	memory.memory    = memory.allocation_info.deviceMemory;
	memory.offset    = memory.allocation_info.offset;
	memory.unique_id = VulkanNextMemoryUniqueId();
	VulkanTrackAllocation(&memory);
	return true;
}

void VulkanDeleteImage(GraphicContext* ctx, VulkanImage* image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || ctx->allocator == nullptr || image == nullptr ||
	        image->image == nullptr || image->memory.allocation == nullptr);

	auto& memory = image->memory;
	VulkanUntrackAllocation(&memory);
	vmaDestroyImage(ctx->allocator, image->image, memory.allocation);
	image->image           = nullptr;
	memory.memory          = nullptr;
	memory.allocation      = nullptr;
	memory.allocation_info = {};
	memory.offset          = 0;
}

void VulkanMapMemory(GraphicContext* ctx, VulkanMemory* memory, void** data) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || memory == nullptr || data == nullptr || ctx->allocator == nullptr ||
	        memory->allocation == nullptr);
	EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(vmaMapMemory(ctx->allocator, memory->allocation,
	                                                          data)) != vk::Result::eSuccess);
}

void VulkanUnmapMemory(GraphicContext* ctx, VulkanMemory* memory) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(ctx == nullptr || memory == nullptr || ctx->allocator == nullptr ||
	        memory->allocation == nullptr);
	vmaUnmapMemory(ctx->allocator, memory->allocation);
}

} // namespace Libs::Graphics
