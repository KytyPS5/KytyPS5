#include "graphics/host_gpu/renderer/deviceLossReport.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Graphics {

namespace {

struct Checkpoint {
	uint64_t sequence     = 0;
	uint64_t submit_id    = 0;
	uint64_t arg4         = 0;
	uint64_t pixel_shader = 0;
	uint64_t shader_hash  = 0;
	uint32_t op           = 0;
	uint32_t arg0         = 0;
	uint32_t arg1         = 0;
	uint32_t arg2         = 0;
	uint32_t arg3         = 0;
};

// A checkpoint marker is its sequence number; the ring keeps the most recent checkpoints.
constexpr uint64_t CheckpointRingSize     = 16384;
constexpr uint64_t MaxReportedCheckpoints = 32;

constexpr uint32_t MaxReportedAddresses = 8;

std::mutex              g_checkpoint_mutex;
std::vector<Checkpoint> g_checkpoints;
uint64_t                g_checkpoint_sequence = 0;
// Several threads can see the loss at once. The report holds this lock, so the others wait for it
// before their EXIT ends the process.
std::mutex g_report_mutex;
bool       g_reported = false;

void Write(const std::string& text) {
	Log::WriteToConsoleAndLog(text);
}

std::string DescribeCheckpoint(const Checkpoint& checkpoint) {
	switch (static_cast<CommandBufferDebugOp>(checkpoint.op)) {
		case CommandBufferDebugOp::DispatchDirect:
			return fmt::format("DispatchDirect submit={} groups={}x{}x{} mode=0x{:08x} "
			                   "cs=0x{:016x} hash=0x{:016x}",
			                   checkpoint.submit_id, checkpoint.arg0, checkpoint.arg1,
			                   checkpoint.arg2, checkpoint.arg3, checkpoint.arg4,
			                   checkpoint.shader_hash);
		case CommandBufferDebugOp::DispatchIndirect:
			return fmt::format(
			    "DispatchIndirect submit={} args=0x{:x} mode=0x{:08x} cs=0x{:016x} hash=0x{:016x}",
			    checkpoint.submit_id, (uint64_t {checkpoint.arg1} << 32u) | checkpoint.arg0,
			    checkpoint.arg3, checkpoint.arg4, checkpoint.shader_hash);
		case CommandBufferDebugOp::DrawIndex:
		case CommandBufferDebugOp::DrawIndexAuto:
			return fmt::format(
			    "{} submit={} args={},{},{},{},0x{:x} ps=0x{:016x}",
			    static_cast<CommandBufferDebugOp>(checkpoint.op) == CommandBufferDebugOp::DrawIndex
			        ? "DrawIndex"
			        : "DrawIndexAuto",
			    checkpoint.submit_id, checkpoint.arg0, checkpoint.arg1, checkpoint.arg2,
			    checkpoint.arg3, checkpoint.arg4, checkpoint.pixel_shader);
		default:
			return fmt::format("op={} submit={} args={},{},{},{},0x{:x}", checkpoint.op,
			                   checkpoint.submit_id, checkpoint.arg0, checkpoint.arg1,
			                   checkpoint.arg2, checkpoint.arg3, checkpoint.arg4);
	}
}

void ReportFaultInfo(vk::Device device) {
	vk::DeviceFaultCountsEXT counts {};
	auto                     result = device.getFaultInfoEXT(&counts, nullptr);
	if (result != vk::Result::eSuccess) {
		Write(fmt::format("VK_EXT_device_fault: vkGetDeviceFaultInfoEXT failed: {}\n",
		                  vk::to_string(result)));
		return;
	}

	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
	counts.vendorBinarySize = 0;

	vk::DeviceFaultInfoEXT info {};
	info.pAddressInfos = addresses.data();
	info.pVendorInfos  = vendors.data();
	result             = device.getFaultInfoEXT(&counts, &info);
	if (result != vk::Result::eSuccess && result != vk::Result::eIncomplete) {
		Write(fmt::format("VK_EXT_device_fault: vkGetDeviceFaultInfoEXT failed: {}\n",
		                  vk::to_string(result)));
		return;
	}

	const char* description = info.description.data();
	Write(fmt::format("VK_EXT_device_fault: \"{}\", {} address records, {} vendor records\n",
	                  description, counts.addressInfoCount, counts.vendorInfoCount));
	for (uint32_t i = 0; i < std::min(counts.addressInfoCount, MaxReportedAddresses); i++) {
		const auto& address = addresses[i];
		// The reported address is only accurate to addressPrecision, a power of two.
		const auto precision = std::max<vk::DeviceSize>(address.addressPrecision, 1);
		const auto begin     = address.reportedAddress & ~(precision - 1);
		Write(fmt::format("  address[{}]: {} reported=0x{:016x} range=[0x{:016x}, 0x{:016x})\n", i,
		                  vk::to_string(address.addressType), address.reportedAddress, begin,
		                  begin + precision));
	}
	if (counts.addressInfoCount > MaxReportedAddresses) {
		// Many records of one type usually come from the waves of one stuck shader.
		std::vector<vk::DeviceFaultAddressTypeEXT> types;
		for (const auto& address: addresses) {
			if (std::ranges::find(types, address.addressType) == types.end()) {
				types.push_back(address.addressType);
			}
		}
		for (const auto type: types) {
			uint32_t       count = 0;
			vk::DeviceSize low   = UINT64_MAX;
			vk::DeviceSize high  = 0;
			for (const auto& address: addresses) {
				if (address.addressType == type) {
					count++;
					low  = std::min(low, address.reportedAddress);
					high = std::max(high, address.reportedAddress);
				}
			}
			Write(fmt::format("  all {}: {} records, 0x{:016x}..0x{:016x}\n", vk::to_string(type),
			                  count, low, high));
		}
	}
	for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
		const auto& vendor = vendors[i];
		Write(fmt::format("  vendor[{}]: {} code=0x{:016x} data=0x{:016x}\n", i,
		                  vendor.description.data(), vendor.vendorFaultCode,
		                  vendor.vendorFaultData));
	}
}

void ReportCheckpoints(vk::Queue queue) {
	uint32_t count = 0;
	queue.getCheckpointDataNV(&count, nullptr);
	std::vector<vk::CheckpointDataNV> data(count);
	queue.getCheckpointDataNV(&count, data.data());
	data.resize(count);
	if (data.empty()) {
		Write("GPU checkpoints: the driver reported none\n");
		return;
	}

	// Each record is the last checkpoint that reached its stage. The operation that did not
	// finish follows the bottom-of-pipe checkpoint; work up to the top-of-pipe one had started.
	uint64_t first = UINT64_MAX;
	uint64_t last  = 0;
	for (const auto& record: data) {
		const auto sequence = reinterpret_cast<uint64_t>(record.pCheckpointMarker);
		Write(fmt::format("GPU checkpoint: stage={} checkpoint={}\n", vk::to_string(record.stage),
		                  sequence));
		first = std::min(first, sequence);
		last  = std::max(last, sequence);
	}

	std::scoped_lock lock(g_checkpoint_mutex);
	if (g_checkpoints.empty()) {
		return;
	}
	const auto oldest = g_checkpoint_sequence > CheckpointRingSize
	                        ? g_checkpoint_sequence - CheckpointRingSize + 1
	                        : uint64_t {1};
	first             = std::max(first, oldest);
	last              = std::min({last, g_checkpoint_sequence, first + MaxReportedCheckpoints - 1});
	Write(fmt::format("GPU checkpoints {}..{} of {} recorded:\n", first, last,
	                  g_checkpoint_sequence));
	for (uint64_t sequence = first; sequence <= last; sequence++) {
		const auto& checkpoint = g_checkpoints[sequence % CheckpointRingSize];
		if (checkpoint.sequence == sequence) {
			Write(fmt::format("  [{}] {}\n", sequence, DescribeCheckpoint(checkpoint)));
		}
	}
}

} // namespace

bool DeviceLossCheckpointsRequested() {
	static const bool requested = [] {
		const char* env = std::getenv("KYTY_GPU_CHECKPOINTS");
		return env != nullptr && std::strcmp(env, "0") != 0;
	}();
	return requested;
}

void DeviceLossRecordCheckpoint(vk::CommandBuffer buffer, uint32_t op, uint64_t submit_id,
                                uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
                                uint64_t arg4, uint64_t pixel_shader) {
	uint64_t sequence = 0;
	{
		std::scoped_lock lock(g_checkpoint_mutex);
		if (g_checkpoints.empty()) {
			g_checkpoints.resize(CheckpointRingSize);
		}
		sequence                                     = ++g_checkpoint_sequence;
		g_checkpoints[sequence % CheckpointRingSize] = {
		    .sequence     = sequence,
		    .submit_id    = submit_id,
		    .arg4         = arg4,
		    .pixel_shader = pixel_shader,
		    .op           = op,
		    .arg0         = arg0,
		    .arg1         = arg1,
		    .arg2         = arg2,
		    .arg3         = arg3,
		};
	}
	buffer.setCheckpointNV(reinterpret_cast<const void*>(sequence));
}

void DeviceLossAnnotateShader(uint64_t shader_hash) {
	std::scoped_lock lock(g_checkpoint_mutex);
	if (!g_checkpoints.empty()) {
		g_checkpoints[g_checkpoint_sequence % CheckpointRingSize].shader_hash = shader_hash;
	}
}

void DeviceLossReport(GraphicContext& graphics, vk::Result result) {
	if (result != vk::Result::eErrorDeviceLost) {
		return;
	}
	std::scoped_lock lock(g_report_mutex);
	if (g_reported) {
		return;
	}
	g_reported = true;
	Write("--- Device loss report ---\n");
	if (graphics.device_fault_enabled) {
		ReportFaultInfo(graphics.device);
	} else {
		Write("VK_EXT_device_fault: not supported by this device\n");
	}
	if (graphics.device_checkpoints_enabled) {
		ReportCheckpoints(graphics.queue);
	} else {
		Write("GPU checkpoints: off (KYTY_GPU_CHECKPOINTS=1 enables them on NVIDIA)\n");
	}
	Log::Flush();
}

} // namespace Libs::Graphics
