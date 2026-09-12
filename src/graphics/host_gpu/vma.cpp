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

#include <algorithm>
#include <cinttypes>

namespace Libs::Graphics {

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}

	const auto budget = GetMemoryBudget();
	LOGF("Vulkan device memory: heap=%" PRIu64 " MiB budget=%" PRIu64 " MiB usage=%" PRIu64
	     " MiB working-ceiling=%" PRIu64 " MiB (VK_EXT_memory_budget %s)\n",
	     budget.heap_size >> 20u, budget.budget >> 20u, budget.usage >> 20u,
	     GetTotalMemoryBudget() >> 20u, memory_budget_ext_enabled ? "on" : "off");
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

Headroom::MemoryBudget GraphicContext::GetMemoryBudget() const {
	Headroom::MemoryBudget snapshot {};
	if (allocator == nullptr) {
		return snapshot;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	snapshot.reported = CanReportMemoryUsage();
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			snapshot.heap_size += properties.size;
		}
		if (!discrete || device_local) {
			snapshot.budget += snapshot.reported ? budgets[heap].budget : properties.size;
			snapshot.usage += snapshot.reported ? budgets[heap].usage : 0;
		}
	}
	return snapshot;
}

uint64_t GraphicContext::ImageMemorySize(const vk::ImageCreateInfo& image_info) const {
	if (device == nullptr) {
		return 0;
	}
	// An unbacked VkImage answers what the allocation would have asked for, including the driver's
	// own alignment and padding.
	vk::Image probe = nullptr;
	if (device.createImage(&image_info, nullptr, &probe) != vk::Result::eSuccess ||
	    probe == nullptr) {
		return 0;
	}
	const auto requirements = device.getImageMemoryRequirements(probe);
	device.destroyImage(probe, nullptr);
	return requirements.size;
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image,
                                 ImageAllocationReport* report) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	const auto usage_mask =
	    static_cast<uint32_t>(static_cast<vk::ImageUsageFlags::MaskType>(image_info.usage));
	const bool host_fallback_allowed = Headroom::HostFallbackAllowedForImageUsage(usage_mask);

	// `preferred` rather than `required` lets VMA place the image in a host-visible heap when no
	// device memory is left.
	const auto attempt = [&](bool device_local_required) {
		VmaAllocationCreateInfo alloc_info {};
		if (device_local_required) {
			alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		} else {
			alloc_info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		}
		vk::Image::CType native_image = VK_NULL_HANDLE;
		const auto       result       = static_cast<vk::Result>(vmaCreateImage(
		    allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info), &alloc_info,
		    &native_image, &image.allocation, nullptr));
		if (result != vk::Result::eSuccess) {
			// VMA leaves both outputs untouched on failure; make sure a retry starts clean.
			image.image      = nullptr;
			image.allocation = nullptr;
			return result;
		}
		image.image = native_image;
		return result;
	};

	auto result = attempt(true);
	auto tier   = Headroom::AllocationTier::DeviceLocal;
	if (result != vk::Result::eSuccess && host_fallback_allowed) {
		result = attempt(false);
		tier   = Headroom::AllocationTier::HostFallback;
	}

	if (report != nullptr) {
		report->result                = result;
		report->tier                  = tier;
		report->host_fallback_allowed = host_fallback_allowed;
		report->budget                = GetMemoryBudget();
		report->size = result == vk::Result::eSuccess ? 0 : ImageMemorySize(image_info);
	}

	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}
	if (tier == Headroom::AllocationTier::HostFallback) {
		LOGF("image allocation fell back to host-visible memory: extent=%ux%ux%u format=%d "
		     "usage=0x%x\n",
		     image_info.extent.width, image_info.extent.height, image_info.extent.depth,
		     static_cast<int>(image_info.format), usage_mask);
	}

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
