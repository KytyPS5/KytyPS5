#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
class RenderContext;

// The guest's occlusion counters. A ZPASS_DONE event dumps the sample counter of each of the 16
// DBs to address + db * 16, setting bit 63 once the value is there; a query is the difference of a
// dump at A (begin) and one at A + 8 (end), summed over the DBs.
namespace OcclusionCounters {

constexpr uint32_t DbCount  = 16;
constexpr uint64_t DbStride = 16;
// Bytes a dump may write.
constexpr uint64_t DumpSize  = (DbCount - 1u) * DbStride + sizeof(uint64_t);
constexpr uint64_t ReadyBit  = uint64_t {1} << 63u;
constexpr uint64_t ValueMask = ReadyBit - 1u;
// Added to a segment whose samples could not all be counted, so its queries read visible.
constexpr uint64_t UncountedSamples = uint64_t {1} << 24u;

// The value a dump writes for one DB: the whole count lives in DB 0, the others stay at zero.
[[nodiscard]] constexpr uint64_t DbValue(uint32_t db, uint64_t total) {
	return ReadyBit | (db == 0 ? (total & ValueMask) : 0u);
}

// Splits the stream of draws into segments at every dump. The samples of a segment are counted
// by host queries only while a guest query is open (a begin dump at A without the end dump at
// A + 8); the value published for a dump is the running total of every earlier segment, so any
// begin/end pairing (also nested or interleaved queries) reads the samples between its dumps.
class Segments {
public:
	struct Dump {
		uint64_t address    = 0;
		uint64_t first_slot = 0; // Host query slots of the segment the dump ends, [first, end).
		uint64_t end_slot   = 0;
		bool     uncounted  = false; // Some samples of the segment had no host query.
	};

	// `capacity`: slots of the host query pool; `max_open_frames`: frames after which a begin
	// dump that never saw its end stops keeping the counters running.
	explicit Segments(uint32_t capacity, uint32_t max_open_frames = 8)
	    : m_capacity(capacity), m_max_open_frames(max_open_frames) {}

	[[nodiscard]] Dump OnDump(uint64_t address) {
		Dump dump {.address    = address,
		           .first_slot = m_segment_first,
		           .end_slot   = m_next_slot,
		           .uncounted  = m_uncounted};
		m_segment_first  = m_next_slot;
		m_uncounted      = false;
		const auto begin = std::find_if(m_open.begin(), m_open.end(), [&](const Open& open) {
			return open.address + sizeof(uint64_t) == address;
		});
		if (begin != m_open.end()) {
			m_open.erase(begin);
		} else {
			m_open.push_back({address, m_frame});
		}
		return dump;
	}

	// Whether the samples of draws are needed now.
	[[nodiscard]] bool Counting() const noexcept { return !m_open.empty(); }

	// A slot for a host query of the current segment; false when every slot waits for its
	// results, and then the segment reads visible.
	[[nodiscard]] bool AllocateSlot(uint32_t& slot) {
		if (m_next_slot - m_released >= m_capacity) {
			m_uncounted = true;
			return false;
		}
		slot = static_cast<uint32_t>(m_next_slot % m_capacity);
		m_next_slot++;
		return true;
	}

	// Slots before `end_slot` are free again.
	void Release(uint64_t end_slot) { m_released = std::max(m_released, end_slot); }

	void OnFrame() {
		m_frame++;
		std::erase_if(
		    m_open, [this](const Open& open) { return open.frame + m_max_open_frames < m_frame; });
	}

	[[nodiscard]] uint32_t Capacity() const noexcept { return m_capacity; }
	[[nodiscard]] size_t   OpenQueries() const noexcept { return m_open.size(); }

private:
	struct Open {
		uint64_t address = 0;
		uint64_t frame   = 0;
	};

	uint32_t          m_capacity        = 0;
	uint32_t          m_max_open_frames = 0;
	uint64_t          m_next_slot       = 0;
	uint64_t          m_released        = 0;
	uint64_t          m_segment_first   = 0;
	uint64_t          m_frame           = 0;
	bool              m_uncounted       = false;
	std::vector<Open> m_open;
};

} // namespace OcclusionCounters

// Host occlusion queries behind the guest's ZPASS_DONE counters. Everything runs on the GPU
// thread, in command order: Vulkan occlusion queries are active only inside render pass
// instances (they never split one), and the counters reach guest memory once the GPU is known
// to have finished the queries (checked at dumps and, with a fresh GPU tick, once per frame).
// Only at a flip, and only while the GPU is more than a frame behind, the GPU thread waits for
// it: the counters of a frame reach guest memory by the next frame's flip.
class OcclusionQueries {
public:
	explicit OcclusionQueries(RenderContext& context);
	~OcclusionQueries();
	KYTY_CLASS_NO_COPY(OcclusionQueries);

	// Whether host queries back the counters (the device supports hostQueryReset); otherwise a
	// dump publishes a count that always reads visible.
	[[nodiscard]] bool Enabled() const noexcept { return m_pool != nullptr; }

	// A ZPASS_DONE dump to `address` (8-byte aligned), in the command buffer being recorded.
	void Dump(CommandScheduler& scheduler, uint64_t address);
	// Called by the command buffer right after it began and before it ends a render pass instance.
	void OnBeginRendering(vk::CommandBuffer recorder);
	void OnEndRendering(vk::CommandBuffer recorder);
	// Once per guest frame, before its flip: publishes the dumps the GPU finished and those of
	// earlier frames.
	void OnFrame();
	// Publishes the dumps of command buffers the GPU finished; `refresh` queries the GPU's
	// progress first (a driver call).
	void PublishCompleted(bool refresh);
	// The guest unmaps [address, address + size): pending dumps there are not written.
	void OnUnmap(uint64_t address, uint64_t size);
	// Whether a dump waits for the GPU before its counters reach guest memory.
	[[nodiscard]] bool HasPendingDumps() const noexcept { return !m_pending.empty(); }

private:
	void BeginQuery(vk::CommandBuffer recorder);
	void EndQuery(vk::CommandBuffer recorder);
	struct PendingDump {
		OcclusionCounters::Segments::Dump dump;
		uint64_t                          tick  = 0;
		uint64_t                          frame = 0;
		bool                              write = true;
	};

	void                   Publish(const PendingDump& pending);
	[[nodiscard]] uint64_t ReadSamples(uint64_t first_slot, uint64_t end_slot);
	void                   ResetSlots(uint64_t first_slot, uint64_t end_slot);

	RenderContext&              m_context;
	vk::Device                  m_device = nullptr;
	vk::QueryPool               m_pool   = nullptr;
	vk::QueryControlFlags       m_control;
	OcclusionCounters::Segments m_segments;
	bool                        m_rendering    = false;
	bool                        m_query_active = false;
	uint32_t                    m_active_slot  = 0;
	uint64_t                    m_frame        = 0;
	uint64_t                    m_total        = 0;
	std::deque<PendingDump>     m_pending;
	std::vector<uint64_t>       m_results;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_
