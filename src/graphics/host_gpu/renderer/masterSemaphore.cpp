#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cinttypes>
#include <fmt/format.h>
#include <vector>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::ReportDeviceFault() {
	if (!m_graphics.device_fault_enabled) {
		Log::WriteToConsoleAndLog("device fault: VK_EXT_device_fault unavailable\n");
		return;
	}
	const auto device         = static_cast<VkDevice>(m_graphics.device);
	const auto get_fault_info = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT;
	if (get_fault_info == nullptr) {
		Log::WriteToConsoleAndLog("device fault: vkGetDeviceFaultInfoEXT missing\n");
		return;
	}
	VkDeviceFaultCountsEXT counts {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
	if (get_fault_info(device, &counts, nullptr) != VK_SUCCESS) {
		Log::WriteToConsoleAndLog("device fault: count query failed\n");
		return;
	}
	std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	VkDeviceFaultInfoEXT                     info {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
	info.pAddressInfos      = addresses.data();
	info.pVendorInfos       = vendors.data();
	counts.vendorBinarySize = 0;
	const auto result       = get_fault_info(device, &counts, &info);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		Log::WriteToConsoleAndLog("device fault: info query failed\n");
		return;
	}
	Log::WriteToConsoleAndLog(fmt::format("device fault: {}\n", info.description));
	for (const auto& address: addresses) {
		Log::WriteToConsoleAndLog(
		    fmt::format("device fault address: type={} address=0x{:016x} precision=0x{:x}\n",
		                static_cast<int>(address.addressType), address.reportedAddress,
		                address.addressPrecision));
	}
	for (const auto& vendor: vendors) {
		Log::WriteToConsoleAndLog(fmt::format("device fault vendor: {} code=0x{:x} data=0x{:x}\n",
		                                      vendor.description, vendor.vendorFaultCode,
		                                      vendor.vendorFaultData));
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	KYTY_PROFILER_BLOCK("MasterSemaphore::Wait");
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		if (result == vk::Result::eErrorDeviceLost) {
			ReportDeviceFault();
		}
		EXIT("timeline wait failed: result=%s tick=%" PRIu64 " gpu_tick=%" PRIu64
		     " current_tick=%" PRIu64 "\n",
		     vk::to_string(result).c_str(), tick, KnownGpuTick(), CurrentTick());
	}
	Refresh();
}

} // namespace Libs::Graphics
