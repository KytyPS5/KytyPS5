#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_INDIRECTKEYFEEDBACK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_INDIRECTKEYFEEDBACK_H_

#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// Carries back the descriptor-heap keys a draw selected, for a table too large for the host to
// enumerate. The draw writes a bitmap into its flattened SRT; this copies that bitmap somewhere the
// host can read and hands the set bits to the materializer once the submit retires.
class IndirectKeyFeedback {
	static constexpr uint32_t MaxSlots     = 64;
	static constexpr uint64_t SlotBytes    = 8u * 1024u;

public:
	IndirectKeyFeedback(GraphicContext& graphics, CommandScheduler& scheduler);
	KYTY_CLASS_NO_COPY(IndirectKeyFeedback);

	// Registers one table's bitmap. The copy is recorded later, after the draw that fills it.
	void Queue(uint64_t signature, vk::Buffer source, uint64_t source_offset, uint32_t words);

	// Records the copies for everything queued since the last call. Must follow the draw or
	// dispatch in the same command buffer, or the bitmap is read before the shader writes it.
	// Outside a render pass only: CommandBuffer runs it whenever a pass ends and before End.
	void Flush(vk::CommandBuffer command);

	[[nodiscard]] bool HasQueued() const noexcept { return !m_queued.empty(); }

private:
	struct Pending {
		uint64_t   signature   = 0;
		vk::Buffer source      = nullptr;
		uint64_t   offset      = 0;
		uint32_t   words       = 0;
	};

	GraphicContext&              m_graphics;
	CommandScheduler&            m_scheduler;
	Buffer                       m_download;
	std::array<uint64_t, MaxSlots> m_slot_ticks {};
	uint32_t                     m_next_slot = 0;
	std::vector<Pending>         m_queued;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_INDIRECTKEYFEEDBACK_H_
