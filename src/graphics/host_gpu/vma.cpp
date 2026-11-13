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
	CreateSparseBinding();
	return true;
}

void GraphicContext::CreateSparseBinding() {
	EXIT_IF(allocator == nullptr || sparse_binding != nullptr);
	if (sparse_residency_image2d_enabled || sparse_residency_buffer_enabled) {
		sparse_binding = new SparseBinding(*this);
	}
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	delete sparse_binding;
	sparse_binding = nullptr;
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = physical_device_memory_properties;
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
	if (allocator == nullptr) {
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
			usage += budgets[heap].statistics.blockBytes;
		}
	}
	return usage;
}

bool GraphicContext::DeviceBudgetAllows(uint64_t size) const {
	if (allocator == nullptr) {
		return false;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	uint64_t usage  = 0;
	uint64_t budget = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		if (physical_device_memory_properties.memoryHeaps[heap].flags &
		    vk::MemoryHeapFlagBits::eDeviceLocal) {
			usage += budgets[heap].statistics.blockBytes;
			budget += budgets[heap].budget;
		}
	}
	if (device_budget_override != 0) {
		budget = device_budget_override;
	}
	return size <= budget && usage <= budget - size;
}

uint32_t GraphicContext::GetHeapUsage(std::span<HeapUsage, VK_MAX_MEMORY_HEAPS> heaps) const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const auto count = physical_device_memory_properties.memoryHeapCount;
	for (uint32_t heap = 0; heap < count; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		heaps[heap] = {.usage        = budgets[heap].usage,
		               .budget       = budgets[heap].budget,
		               .allocations  = budgets[heap].statistics.allocationCount,
		               .device_local = static_cast<bool>(properties.flags &
		                                                 vk::MemoryHeapFlagBits::eDeviceLocal)};
	}
	return count;
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

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image,
                                 uint32_t first_bound_level) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);
	if (first_bound_level != 0 && CreatePartialImage(image_info, first_bound_level, image)) {
		return true;
	}

	vk::Image::CType native_image = VK_NULL_HANDLE;
	const auto       allocate     = [&](const VmaAllocationCreateInfo& info) {
		return vmaCreateImage(allocator,
		                      static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
		                      &info, &native_image, &image.allocation, nullptr);
	};
	VmaAllocationCreateInfo alloc_info {};
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	const vk::DeviceImageMemoryRequirements query {.pCreateInfo = &image_info};
	// Device memory while it is in budget, host memory past it.
	if (!AllocateDeviceLocal(device.getImageMemoryRequirements(query).memoryRequirements.size,
	                         alloc_info, allocate)) {
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
		if (allocate(alloc_info) != VK_SUCCESS) {
			LogMemoryBudget();
			return false;
		}
	}
	image.image = native_image;

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

	if (image.sparse) {
		// Binds still queued would name a destroyed image.
		sparse_binding->Forget(image.image);
		device.destroyImage(image.image, nullptr);
		vmaFreeMemory(allocator, image.allocation);
	} else {
		vmaDestroyImage(allocator, image.image, image.allocation);
	}
	image.image      = nullptr;
	image.allocation = nullptr;
	image.sparse     = false;
}

bool GraphicContext::CreatePartialImage(const vk::ImageCreateInfo& base, uint32_t first_level,
                                        VulkanImage& image) {
	if (!sparse_residency_image2d_enabled || base.imageType != vk::ImageType::e2D ||
	    base.samples != vk::SampleCountFlagBits::e1 || base.tiling != vk::ImageTiling::eOptimal ||
	    first_level == 0 || first_level >= base.mipLevels) {
		return false;
	}
	auto info = base;
	info.flags |=
	    vk::ImageCreateFlagBits::eSparseBinding | vk::ImageCreateFlagBits::eSparseResidency;
	if (GetImageFormatProperties(info.format, info.imageType, info.tiling, info.usage, info.flags,
	                             nullptr) != vk::Result::eSuccess ||
	    physical_device
	        .getSparseImageFormatProperties(info.format, info.imageType, info.samples, info.usage,
	                                        info.tiling)
	        .empty()) {
		return false;
	}
	vk::Image native = nullptr;
	if (device.createImage(&info, nullptr, &native) != vk::Result::eSuccess) {
		return false;
	}
	const auto requirements = device.getImageMemoryRequirements(native);
	const auto sparse       = device.getImageSparseMemoryRequirements(native);
	const auto color        = std::ranges::find_if(sparse, [](const auto& entry) {
		return static_cast<bool>(entry.formatProperties.aspectMask &
		                         vk::ImageAspectFlagBits::eColor);
	});
	if (color == sparse.end() || requirements.alignment == 0) {
		device.destroyImage(native, nullptr);
		return false;
	}
	const auto block = requirements.alignment;
	const auto align = [block](uint64_t size) { return (size + block - 1u) / block * block; };

	// Lay the bound levels and the mip tails out in one allocation.
	SparseImageBinds binds {.image = native};
	uint64_t         size        = 0;
	const auto&      granularity = color->formatProperties.imageGranularity;
	const auto       tail_first  = std::min(color->imageMipTailFirstLod, info.mipLevels);
	for (uint32_t level = first_level; level < tail_first; ++level) {
		const vk::Extent3D extent {std::max(info.extent.width >> level, 1u),
		                           std::max(info.extent.height >> level, 1u), 1u};
		const uint64_t     blocks =
		    uint64_t {(extent.width + granularity.width - 1u) / granularity.width} *
		    ((extent.height + granularity.height - 1u) / granularity.height);
		for (uint32_t layer = 0; layer < info.arrayLayers; ++layer) {
			vk::SparseImageMemoryBind bind {};
			bind.subresource  = {vk::ImageAspectFlagBits::eColor, level, layer};
			bind.extent       = extent;
			bind.memoryOffset = size;
			binds.levels.push_back(bind);
			size += blocks * block;
		}
	}
	const auto add_tails = [&](const vk::SparseImageMemoryRequirements& entry,
	                           vk::SparseMemoryBindFlags                flags) {
		if (entry.imageMipTailFirstLod >= info.mipLevels || entry.imageMipTailSize == 0) {
			return;
		}
		const bool     single = static_cast<bool>(entry.formatProperties.flags &
		                                          vk::SparseImageFormatFlagBits::eSingleMiptail);
		const uint32_t tails  = single ? 1u : info.arrayLayers;
		for (uint32_t tail = 0; tail < tails; ++tail) {
			vk::SparseMemoryBind bind {};
			bind.resourceOffset = entry.imageMipTailOffset + tail * entry.imageMipTailStride;
			bind.size           = entry.imageMipTailSize;
			bind.memoryOffset   = size;
			bind.flags          = flags;
			binds.opaque.push_back(bind);
			size += align(entry.imageMipTailSize);
		}
	};
	add_tails(*color, {});
	for (const auto& entry: sparse) {
		if (static_cast<bool>(entry.formatProperties.aspectMask &
		                      vk::ImageAspectFlagBits::eMetadata)) {
			add_tails(entry, vk::SparseMemoryBindFlagBits::eMetadata);
		}
	}

	// A sparse image is bound to device-local memory only. Where the device-local heaps cannot
	// take it, a full image follows.
	uint32_t device_types = 0;
	for (uint32_t type = 0; type < physical_device_memory_properties.memoryTypeCount; ++type) {
		if (physical_device_memory_properties.memoryTypes[type].propertyFlags &
		    vk::MemoryPropertyFlagBits::eDeviceLocal) {
			device_types |= 1u << type;
		}
	}
	const VkMemoryRequirements memory {size, block, requirements.memoryTypeBits & device_types};
	const bool                 allocated =
	    memory.memoryTypeBits != 0 &&
	    AllocateDeviceLocal(size, {}, [&](const VmaAllocationCreateInfo& info) {
		    return vmaAllocateMemory(allocator, &memory, &info, &image.allocation, nullptr);
	    });
	if (!allocated) {
		image.allocation = nullptr;
		device.destroyImage(native, nullptr);
		return false;
	}
	VmaAllocationInfo allocation_info {};
	vmaGetAllocationInfo(allocator, image.allocation, &allocation_info);
	for (auto& bind: binds.levels) {
		bind.memory = allocation_info.deviceMemory;
		bind.memoryOffset += allocation_info.offset;
	}
	for (auto& bind: binds.opaque) {
		bind.memory = allocation_info.deviceMemory;
		bind.memoryOffset += allocation_info.offset;
	}
	sparse_binding->BindImage(std::move(binds));

	image.image      = native;
	image.format     = info.format;
	image.image_type = info.imageType;
	image.extent     = info.extent;
	image.layers     = info.arrayLayers;
	image.mip_levels = info.mipLevels;
	image.samples    = static_cast<uint32_t>(info.samples);
	image.usage      = info.usage;
	image.flags      = info.flags;
	image.state      = {.layout = info.initialLayout};
	image.subresource_states.clear();
	image.sparse = true;
	return true;
}

} // namespace Libs::Graphics
