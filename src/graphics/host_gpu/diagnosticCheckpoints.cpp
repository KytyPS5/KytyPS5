#include "graphics/host_gpu/vulkanCommon.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr size_t CHECKPOINT_RING_SIZE = 1u << 16u;

std::array<DiagnosticCheckpoint, CHECKPOINT_RING_SIZE> g_checkpoints {};
std::atomic<uint64_t>                                  g_checkpoint_sequence {0};

const char* OpName(uint32_t op) {
	switch (op) {
		case 0: return "DispatchDirect";
		case 1: return "DrawIndex";
		case 2: return "DrawIndexAuto";
		case 3: return "EopWrite";
		case 4: return "EopInterrupt";
		case 5: return "EopWriteBack";
		case 6: return "EopFlip";
		case 7: return "EopWriteBackFlip";
		case 8: return "EopOnlyFlip";
		default: return "Unknown";
	}
}

void Print(const char* stage, const DiagnosticCheckpoint& checkpoint) {
	std::printf("  [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64
	            "\n",
	            stage, checkpoint.sequence, OpName(checkpoint.op), checkpoint.submit_id,
	            checkpoint.arg0, checkpoint.arg1, checkpoint.arg2, checkpoint.arg3,
	            checkpoint.arg4);
	LOGF("  [%s] seq=%" PRIu64 " op=%s submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     stage, checkpoint.sequence, OpName(checkpoint.op), checkpoint.submit_id, checkpoint.arg0,
	     checkpoint.arg1, checkpoint.arg2, checkpoint.arg3, checkpoint.arg4);
}

}

const DiagnosticCheckpoint* RecordDiagnosticCheckpoint(const DiagnosticCheckpoint& checkpoint) {
	const auto sequence = g_checkpoint_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	auto&      slot     = g_checkpoints[sequence % CHECKPOINT_RING_SIZE];
	slot                = checkpoint;
	slot.sequence       = sequence;
	return &slot;
}

static void DumpDeviceFault(GraphicContext& graphics) {
	if (!graphics.device_fault_enabled) {
		return;
	}
	vk::DeviceFaultCountsEXT counts {};
	auto result = graphics.device.getFaultInfoEXT(&counts, nullptr);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		std::printf("--- Device fault: query failed: %s ---\n", vk::to_string(result).c_str());
		return;
	}
	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	std::vector<uint8_t>                       binary(static_cast<size_t>(counts.vendorBinarySize));
	vk::DeviceFaultInfoEXT                     info {};
	info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
	info.pVendorInfos  = vendors.empty() ? nullptr : vendors.data();
	info.pVendorBinaryData = binary.empty() ? nullptr : binary.data();
	result = graphics.device.getFaultInfoEXT(&counts, &info);
	std::printf("--- Device fault (%s): \"%s\" addresses=%u vendor=%u binary=%" PRIu64 " ---\n",
	            vk::to_string(result).c_str(), info.description.data(), counts.addressInfoCount,
	            counts.vendorInfoCount, static_cast<uint64_t>(counts.vendorBinarySize));
	LOGF("--- Device fault (%s): \"%s\" addresses=%u vendor=%u binary=%" PRIu64 " ---\n",
	     vk::to_string(result).c_str(), info.description.data(), counts.addressInfoCount,
	     counts.vendorInfoCount, static_cast<uint64_t>(counts.vendorBinarySize));
	if (!binary.empty()) {
		if (auto* file = std::fopen("_device_fault.nv-gpudmp", "wb"); file != nullptr) {
			std::fwrite(binary.data(), 1, binary.size(), file);
			std::fclose(file);
			std::printf("  vendor binary written to _device_fault.nv-gpudmp\n");
		}
	}
	for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
		const auto& address = addresses[i];
		std::printf("  address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		            vk::to_string(address.addressType).c_str(),
		            static_cast<uint64_t>(address.reportedAddress),
		            static_cast<uint64_t>(address.addressPrecision));
		LOGF("  address[%u]: %s at 0x%016" PRIx64 " (precision 0x%" PRIx64 ")\n", i,
		     vk::to_string(address.addressType).c_str(),
		     static_cast<uint64_t>(address.reportedAddress),
		     static_cast<uint64_t>(address.addressPrecision));
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		const auto& vendor = vendors[i];
		std::printf("  vendor[%u]: \"%s\" code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n", i,
		            vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
		            static_cast<uint64_t>(vendor.vendorFaultData));
		LOGF("  vendor[%u]: \"%s\" code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n", i,
		     vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
		     static_cast<uint64_t>(vendor.vendorFaultData));
	}
	std::fflush(stdout);
}

void DumpDeviceLossDiagnostics(GraphicContext& graphics) {
	DumpDeviceFault(graphics);
	if (!graphics.diagnostic_checkpoints_enabled || graphics.queue == nullptr) {
		return;
	}
	std::vector<vk::CheckpointDataNV> data;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		data = graphics.queue.getCheckpointDataNV();
	}
	std::printf("--- Diagnostic checkpoints (%zu, last recorded seq=%" PRIu64 ") ---\n",
	            data.size(), g_checkpoint_sequence.load(std::memory_order_relaxed));
	LOGF("--- Diagnostic checkpoints (%zu, last recorded seq=%" PRIu64 ") ---\n", data.size(),
	     g_checkpoint_sequence.load(std::memory_order_relaxed));
	for (const auto& entry: data) {
		const auto* checkpoint = static_cast<const DiagnosticCheckpoint*>(entry.pCheckpointMarker);
		const auto  stage      = vk::to_string(entry.stage);
		const bool  in_ring    = checkpoint >= g_checkpoints.data() &&
		                      checkpoint < g_checkpoints.data() + g_checkpoints.size();
		std::printf("  [%s] marker=%p ring=[%p, %p) %s\n", stage.c_str(),
		            static_cast<const void*>(checkpoint),
		            static_cast<const void*>(g_checkpoints.data()),
		            static_cast<const void*>(g_checkpoints.data() + g_checkpoints.size()),
		            in_ring ? "inside" : "OUTSIDE");
		if (checkpoint == nullptr || !in_ring) {
			continue;
		}
		Print(stage.c_str(), *checkpoint);
	}
	std::fflush(stdout);
}

}
