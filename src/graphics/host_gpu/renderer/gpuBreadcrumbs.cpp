#include "graphics/host_gpu/renderer/gpuBreadcrumbs.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "kernel/memory.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Libs::Graphics::GpuBreadcrumbs {
namespace {

constexpr uint32_t Slots = 4096;
// Per slot after the marks: three dwords of copied indirect arguments and one spare.
constexpr uint64_t ArgsStart = uint64_t {Slots} * 2u * sizeof(uint32_t);

struct State {
	std::mutex                         mutex;
	vk::Buffer                         buffer = nullptr;
	vk::DeviceMemory                   memory = nullptr;
	volatile uint32_t*                 marks  = nullptr; // per slot: id at start, id at end
	uint32_t                           next   = 1;
	std::array<std::string, Slots>     descriptions;
	std::array<uint32_t, Slots>        ids {};
	std::array<uint64_t, Slots>        args {};
	std::array<bool, Slots>            copied {};
	const char*                        watch_name   = nullptr;
	const uint32_t*                    watch_data   = nullptr;
	uint32_t                           watch_dwords = 0;
};

State& Get() {
	static State state;
	return state;
}

bool Create(GraphicContext& graphics, State& state) {
	vk::BufferCreateInfo info {};
	info.size  = ArgsStart + uint64_t {Slots} * 4u * sizeof(uint32_t);
	info.usage = vk::BufferUsageFlagBits::eTransferDst;
	if (graphics.device.createBuffer(&info, nullptr, &state.buffer) != vk::Result::eSuccess) return false;
	vk::MemoryRequirements requirements {};
	graphics.device.getBufferMemoryRequirements(state.buffer, &requirements);
	const auto properties = graphics.physical_device.getMemoryProperties();
	const auto wanted = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
		    (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) return false;
	vk::MemoryAllocateInfo allocate {};
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type;
	if (graphics.device.allocateMemory(&allocate, nullptr, &state.memory) != vk::Result::eSuccess) return false;
	graphics.device.bindBufferMemory(state.buffer, state.memory, 0);
	void* mapped = nullptr;
	if (graphics.device.mapMemory(state.memory, 0, info.size, {}, &mapped) != vk::Result::eSuccess) return false;
	std::fill_n(static_cast<uint32_t*>(mapped), info.size / sizeof(uint32_t), 0u);
	state.marks = static_cast<volatile uint32_t*>(mapped);
	return true;
}

} // namespace

bool Enabled(const GraphicContext& graphics) {
	return graphics.buffer_marker_enabled;
}

uint32_t Begin(GraphicContext& graphics, vk::CommandBuffer command, std::string description,
               uint64_t indirect_args, vk::Buffer args_buffer, uint64_t args_offset) {
	auto&           state = Get();
	std::lock_guard lock(state.mutex);
	if (state.marks == nullptr && !Create(graphics, state)) {
		EXIT("GPU breadcrumbs: no host-visible marker buffer\n");
	}
	const auto id   = state.next++;
	const auto slot = id % Slots;
	state.ids[slot]          = id;
	state.descriptions[slot] = std::move(description);
	state.args[slot]         = indirect_args;
	state.copied[slot]       = args_buffer != nullptr;
	if (args_buffer != nullptr) {
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {},
		                        1, &barrier, 0, nullptr, 0, nullptr);
		const vk::BufferCopy copy {args_offset, ArgsStart + uint64_t {slot} * 16u, 3u * sizeof(uint32_t)};
		command.copyBuffer(args_buffer, state.buffer, 1, &copy);
	}
	command.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eTopOfPipe, state.buffer,
	                             uint64_t {slot} * 8u, id);
	return id;
}

void End(vk::CommandBuffer command, uint32_t id) {
	auto& state = Get();
	command.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, state.buffer,
	                             uint64_t {id % Slots} * 8u + 4u, id);
}

void Report() {
	auto&           state = Get();
	std::lock_guard lock(state.mutex);
	if (state.marks == nullptr) return;
	// The newest id whose end the GPU wrote: what follows it had not finished.
	uint32_t last_ended = 0;
	for (uint32_t slot = 0; slot < Slots; slot++) {
		if (state.marks[slot * 2u + 1u] == state.ids[slot]) last_ended = std::max(last_ended, state.ids[slot]);
	}
	std::printf("GPU breadcrumbs: last command that ended: %u, last recorded: %u\n", last_ended, state.next - 1);
	const uint32_t first = last_ended > 8u ? last_ended - 8u : 1u;
	for (uint32_t id = first; id < state.next && id < last_ended + 24u; id++) {
		const auto slot    = id % Slots;
		if (state.ids[slot] != id) continue;
		const bool started = state.marks[slot * 2u] == id;
		const bool ended   = state.marks[slot * 2u + 1u] == id;
		std::printf("GPU breadcrumbs: %s %u %s\n", ended ? "ended  " : started ? "RUNNING" : "pending", id,
		            state.descriptions[slot].c_str());
		// The arguments as guest memory holds them now: what the GPU read, unless rewritten since.
		if (state.copied[slot]) {
			const auto* read = state.marks + ArgsStart / sizeof(uint32_t) + slot * 4u;
			std::printf("GPU breadcrumbs:     arguments read: %u %u %u\n", read[0], read[1], read[2]);
		}
		std::array<uint32_t, 5> args {};
		if (state.args[slot] != 0 &&
		    LibKernel::Memory::TryReadBacking(state.args[slot], args.data(), sizeof(args))) {
			std::printf("GPU breadcrumbs:     arguments now: %u %u %u %u %u\n", args[0], args[1], args[2], args[3], args[4]);
		}
	}
	// A command of another queue can hang long before the newest one that ended.
	for (uint32_t slot = 0; slot < Slots; slot++) {
		const auto id = state.ids[slot];
		if (id != 0 && id < first && state.marks[slot * 2u] == id && state.marks[slot * 2u + 1u] != id) {
			std::printf("GPU breadcrumbs: RUNNING (earlier) %u %s\n", id, state.descriptions[slot].c_str());
		}
	}
	if (state.watch_data != nullptr) {
		std::printf("GPU breadcrumbs: %s, nonzero dwords:", state.watch_name);
		for (uint32_t i = 0; i < state.watch_dwords; i++) {
			if (state.watch_data[i] != 0) std::printf(" [%u]=%u", i, state.watch_data[i]);
		}
		std::printf("\n");
	}
	std::fflush(stdout);
}

void Watch(const char* name, const uint32_t* data, uint32_t dwords) {
	auto&           state = Get();
	std::lock_guard lock(state.mutex);
	state.watch_name   = name;
	state.watch_data   = data;
	state.watch_dwords = dwords;
}

} // namespace Libs::Graphics::GpuBreadcrumbs
