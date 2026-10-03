#include "graphics/host_gpu/renderer/cache/indirectKeyFeedback.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <bit>
#include <cstring>

namespace Libs::Graphics {

IndirectKeyFeedback::IndirectKeyFeedback(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_download(graphics, scheduler, MemoryUsage::Download, 0, AllFlags, MaxSlots * SlotBytes) {
	SetVulkanObjectNameF(m_graphics.device, m_download.Handle(), "Indirect Key Feedback");
}

void IndirectKeyFeedback::Queue(uint64_t signature, vk::Buffer source, uint64_t source_offset,
                                uint32_t words) {
	if (source == nullptr || words == 0 || words * sizeof(uint32_t) > SlotBytes) {
		return;
	}
	m_queued.push_back({signature, source, source_offset, words});
}

void IndirectKeyFeedback::Flush(vk::CommandBuffer command) {
	if (m_queued.empty()) {
		return;
	}
	if (!m_scheduler.AcceptsOperations()) {
		m_queued.clear();
		return;
	}
	vk::MemoryBarrier2 barrier {};
	barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	barrier.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
	bool barrier_recorded = false;
	for (size_t entry = 0; entry < m_queued.size(); entry++) {
		const auto& pending = m_queued[entry];
		const auto slot = m_next_slot;
		// A slot still owned by an unretired submit is left alone: dropping this draw's keys only
		// delays a material by a frame, while overwriting one would corrupt a pending read.
		// Allocation is round-robin and slots retire in allocation order, so the slot under
		// m_next_slot is the oldest: if it is busy every slot is, and nothing else queued here
		// can be served either.
		if (m_slot_ticks[slot] != 0 && !m_scheduler.IsFree(m_slot_ticks[slot])) {
			break;
		}
		const auto bytes  = static_cast<uint64_t>(pending.words) * sizeof(uint32_t);
		const auto offset = static_cast<uint64_t>(slot) * SlotBytes;

		if (!barrier_recorded) {
			vk::DependencyInfo dependency {};
			dependency.memoryBarrierCount = 1;
			dependency.pMemoryBarriers    = &barrier;
			command.pipelineBarrier2(dependency);
			barrier_recorded = true;
		}

		vk::BufferCopy region {};
		region.srcOffset = pending.offset;
		region.dstOffset = offset;
		region.size      = bytes;
		command.copyBuffer(pending.source, m_download.Handle(), 1, &region);

		const auto signature = pending.signature;
		const auto words     = pending.words;
		m_scheduler.DeferOperation([this, slot, offset, bytes, words, signature] {
			m_download.Invalidate(offset, bytes);
			const auto*           mapped = m_download.Mapped().data() + offset;
			std::vector<uint32_t> keys;
			for (uint32_t word = 0; word < words; word++) {
				uint32_t bits = 0;
				std::memcpy(&bits, mapped + word * sizeof(uint32_t), sizeof(bits));
				while (bits != 0) {
					const auto bit = static_cast<uint32_t>(std::countr_zero(bits));
					keys.push_back(word * 32u + bit);
					bits &= bits - 1u;
				}
			}
			ShaderRecompiler::IR::AddObservedIndirectKeys(signature, keys);
			m_slot_ticks[slot] = 0;
		});

		m_slot_ticks[slot] = m_scheduler.CurrentTick();
		m_next_slot        = (slot + 1u) % MaxSlots;
	}
	m_queued.clear();
}

} // namespace Libs::Graphics
