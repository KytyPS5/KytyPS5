#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <chrono>
#include <vector>

namespace Libs::Graphics {

namespace {

// A lost device says nothing about what went wrong on its own. VK_EXT_device_fault, when the
// driver exposes it, reports the GPU addresses that faulted and any vendor-specific detail --
// the only practical way to find a shader that walks off its resources, since GPU-assisted
// validation shuts itself down once the device is already gone.
// With VK_NV_device_diagnostic_checkpoints the driver remembers the markers the GPU last
// passed. renderDraw tags every draw with its pixel shader hash, so the newest marker names
// the draw that was in flight when the device died.
void ReportCheckpoints(const GraphicContext& graphics) {
	if (!graphics.nv_diagnostics_enabled || graphics.queue == nullptr) {
		return;
	}
	auto* get_data = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(
	    graphics.device.getProcAddr("vkGetQueueCheckpointDataNV"));
	if (get_data == nullptr) {
		return;
	}
	uint32_t count = 0;
	get_data(graphics.queue, &count, nullptr);
	if (count == 0) {
		LOGF("device checkpoints: none reported\n");
		return;
	}
	std::vector<VkCheckpointDataNV> data(count);
	for (auto& entry: data) {
		entry.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
		entry.pNext = nullptr;
	}
	get_data(graphics.queue, &count, data.data());
	LOGF("--- Device checkpoints (%u) --- newest marker is the draw that was executing\n",
	     count);
	for (uint32_t i = 0; i < count; i++) {
		LOGF("  checkpoint[%u]: stage=0x%08x ps_hash=0x%016llx\n", i,
		     static_cast<uint32_t>(data[i].stage),
		     static_cast<unsigned long long>(
		         reinterpret_cast<uintptr_t>(data[i].pCheckpointMarker)));
	}
}

void ReportDeviceFault(const GraphicContext& graphics) {
	if (!graphics.device_fault_enabled) {
		LOGF("device fault: VK_EXT_device_fault is not enabled, no detail available\n");
		return;
	}
	auto* get_info = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
	    graphics.device.getProcAddr("vkGetDeviceFaultInfoEXT"));
	if (get_info == nullptr) {
		LOGF("device fault: vkGetDeviceFaultInfoEXT is unavailable\n");
		return;
	}
	VkDeviceFaultCountsEXT counts {};
	counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
	if (get_info(graphics.device, &counts, nullptr) != VK_SUCCESS) {
		LOGF("device fault: counts query failed\n");
		return;
	}
	std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<VkDeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	std::vector<uint8_t>                     binary(counts.vendorBinarySize);
	VkDeviceFaultInfoEXT info {};
	info.sType             = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
	info.pAddressInfos     = addresses.empty() ? nullptr : addresses.data();
	info.pVendorInfos      = vendors.empty() ? nullptr : vendors.data();
	info.pVendorBinaryData = binary.empty() ? nullptr : binary.data();
	if (get_info(graphics.device, &counts, &info) != VK_SUCCESS) {
		LOGF("device fault: info query failed\n");
		return;
	}
	LOGF("--- Device fault --- %s\n", info.description);
	for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
		const auto& a = addresses[i];
		LOGF("  address[%u]: type=%u reported=0x%016llx precision=0x%016llx\n", i,
		     static_cast<uint32_t>(a.addressType),
		     static_cast<unsigned long long>(a.reportedAddress),
		     static_cast<unsigned long long>(a.addressPrecision));
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		const auto& v = vendors[i];
		LOGF("  vendor[%u]: %s code=%llu data=%llu\n", i, v.description,
		     static_cast<unsigned long long>(v.vendorFaultCode),
		     static_cast<unsigned long long>(v.vendorFaultData));
	}
	if (counts.addressInfoCount == 0 && counts.vendorInfoCount == 0) {
		LOGF("  (driver reported no address or vendor detail)\n");
	}
}

} // namespace

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

void MasterSemaphore::Wait(uint64_t tick) {
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

	const auto wait_start = std::chrono::steady_clock::now();
	const auto result     = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (result == vk::Result::eErrorDeviceLost) {
		// A hung shader keeps the wait pending for the whole TDR budget; an illegal access
		// tears the device down at once. The elapsed time tells the two apart.
		LOGF("device lost after waiting %lld ms on the timeline semaphore\n" ,
		     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
		                                std::chrono::steady_clock::now() - wait_start)
		                                .count()));
		ReportCheckpoints(m_graphics);
		ReportDeviceFault(m_graphics);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
