#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LODSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LODSTATS_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/lodStatsReport.h"

#include <array>
#include <cstdint>
#include <memory>

namespace Libs::Graphics {

class Buffer;
class RenderContext;

// GPU mip statistics for GET_LOD_STATS.
//
// Instrumented pixel shaders record, per T# mip-statistics counter (MipStatsCntId), the finest
// mip level sampled and a count in a device-local buffer. A GET_LOD_STATS packet copies the
// counters into a private host-visible slot in command order and resets them; the report layout
// and the counting rule are in lodStatsReport.h.
//
// ASTRO BOT keeps 16 buckets of 256 counters, each with a ring of 16 reports, and reports one
// bucket per frame: DMA_DATA clears the slot header, GET_LOD_STATS writes the report,
// RELEASE_MEM flushes, then DMA_DATA publishes the slot index. The emulator performs that index
// write when the command is executed, before the GPU has produced the report. The game skips a
// current slot whose header is still zero and keeps the statistics of the previous slot, which is
// what the hardware would still show it, so the report is written when the GPU completes it.
class LodStatsCounter {
public:
	static constexpr uint32_t Counters   = LodStatsReport::Counters;
	static constexpr uint32_t Entries    = LodStatsReport::Entries;
	static constexpr uint32_t ReportSize = LodStatsReport::ReportSize;

	explicit LodStatsCounter(RenderContext& context);
	~LodStatsCounter();
	KYTY_CLASS_NO_COPY(LodStatsCounter);

	// The device-local counter buffer bound to instrumented shaders.
	[[nodiscard]] Buffer& CounterBuffer();
	// Execution thread: a GET_LOD_STATS packet, at the current command position. control is its
	// fourth dword (bit 19 reports and resets, bit 18 forces a reset).
	void Report(uint64_t destination, uint32_t size, uint32_t control);

private:
	static constexpr uint32_t PublishSlots    = 64;
	static constexpr uint64_t PublishSlotSize = 4096;

	RenderContext&                     m_context;
	std::unique_ptr<Buffer>            m_counters;
	std::unique_ptr<Buffer>            m_publish;
	std::array<uint64_t, PublishSlots> m_slot_ticks {};
	uint64_t                           m_issued      = 0;
	bool                               m_initialized = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_LODSTATS_H_
