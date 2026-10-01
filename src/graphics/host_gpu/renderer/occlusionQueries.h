#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

class Buffer;
class CommandRecorder;
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

// Host occlusion queries behind the guest's ZPASS_DONE counters. Queries and dumps are recorded
// on the GPU execution thread, in operation order: Vulkan occlusion queries are active only inside
// render pass instances (they never split one). As on the console, a dump's counters reach guest
// memory when the GPU finishes the work before it: a priority completion queued with the dump
// writes them, in dump order and ahead of any later completion that tells the guest the GPU got
// there (a suspend point, a flip). The execution thread also publishes the dumps it finds
// finished, and at a flip, while the GPU is more than a frame behind, waits for it: the counters
// of a frame reach guest memory by the next frame's flip. GPU work that reads the counters later
// in the stream does not wait for that: the copy of memory it reads gets them on the GPU, from
// the host query results, right before it runs (see PrepareRead()).
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
	void OnBeginRendering(const CommandRecorder& recorder);
	void OnEndRendering(const CommandRecorder& recorder);
	// Once per guest frame, before its flip: publishes the dumps the GPU finished and those of
	// earlier frames.
	void OnFrame();
	// Publishes the dumps of command buffers the GPU finished; `refresh` queries the GPU's
	// progress first (a driver call). Any thread.
	void PublishCompleted(bool refresh);
	// The guest unmaps [address, address + size): pending dumps there are not written.
	void OnUnmap(uint64_t address, uint64_t size);
	// Whether a dump waits for the GPU before its counters reach guest memory.
	[[nodiscard]] bool HasPendingDumps() const {
		std::lock_guard lock(m_mutex);
		return !m_pending.empty();
	}
	// Whether the samples of the draws recorded now reach a guest occlusion counter.
	[[nodiscard]] bool Counting() const noexcept { return Enabled() && m_segments.Counting(); }

	// GPU work reads guest memory after earlier dumps there, as on the console, where a dump's
	// counters are in memory before later work of the stream runs. Execution thread, for each
	// range the operation being prepared reads, before its copy of the range is obtained: true
	// when dumps the GPU may not have finished are under it, and then NoteReadTarget() gives the
	// copy the operation reads, which WriteForReaders() fills before the operation's commands.
	[[nodiscard]] bool PrepareRead(uint64_t address, uint64_t size);
	void               NoteReadTarget(uint64_t address, uint64_t size, const Buffer& buffer,
	                                  uint64_t offset);
	void               WriteForReaders();
	// A new operation: copies noted for an operation that recorded no commands are dropped.
	void DropReadTargets() noexcept {
		m_read_targets.clear();
		m_read_dumps.clear();
	}
	// A read whose copy cannot be filled on the GPU (a CP DMA source): waits for the GPU to get past
	// the dumps under [address, address + size) and publishes them.
	void WaitForCounters(uint64_t address, uint64_t size);

private:
	void BeginQuery(const CommandRecorder& recorder);
	void EndQuery(const CommandRecorder& recorder);
	struct PendingDump {
		OcclusionCounters::Segments::Dump dump;
		uint64_t                          tick  = 0;
		uint64_t                          frame = 0;
		uint64_t                          seq   = 0;
		bool                              write = true;
	};
	// Pages with dumps the GPU may not have finished, for a cheap first look at every read. An
	// entry stays valid until the dumps up to `last_seq` are published; `chunks` marks the 64-byte
	// chunks of the page that those dumps write.
	struct PendingPage {
		uint64_t page     = UINT64_MAX;
		uint64_t last_seq = 0;
		uint64_t chunks   = 0;
	};
	static constexpr size_t PendingPageCount = 4096;
	struct ReadDump {
		uint64_t address = 0;
		uint64_t seq     = 0;
	};
	struct ReadTarget {
		uint64_t address        = 0;
		uint64_t size           = 0;
		uint64_t device_address = 0;
		size_t   first_dump     = 0;
		size_t   end_dump       = 0;
	};

	// Publishes the first `count` pending dumps, which the GPU finished.
	void                   PublishFront(size_t count);
	// The results of the host queries in slots [first_slot, end_slot) into m_results; false
	// when the driver could not give them.
	[[nodiscard]] bool     ReadResults(uint64_t first_slot, uint64_t end_slot);
	// The counters of a dump at `address` with the running total.
	void                   WriteCounters(uint64_t address);
	void                   ResetSlots(uint64_t first_slot, uint64_t end_slot);
	// Resets and frees the slots of published dumps before `end_slot`, except those a command
	// buffer the GPU has not finished copies the results of.
	void                   ReleaseSlots(uint64_t end_slot);
	void                   NotePendingPages(uint64_t address, uint64_t seq);
	[[nodiscard]] bool     MayReadPendingDump(uint64_t address, uint64_t size) const;
	// Waits for the GPU to finish `tick` and publishes what it finished.
	void                   WaitAndPublish(uint64_t tick);
	void                   CreateCounterPipeline();

	RenderContext&              m_context;
	vk::Device                  m_device = nullptr;
	vk::QueryPool               m_pool   = nullptr;
	vk::QueryControlFlags       m_control;
	bool                        m_rendering    = false;
	bool                        m_query_active = false;
	uint32_t                    m_active_slot  = 0;
	uint64_t                    m_frame        = 0;
	// The execution thread records dumps and the priority completions publish them: the pending
	// dumps, the running total, the result scratch and the slots published dumps release.
	mutable std::mutex          m_mutex;
	OcclusionCounters::Segments m_segments;
	uint64_t                    m_total = 0;
	// Tick of the command buffer whose completion publishes the dumps recorded into it.
	uint64_t                    m_completion_tick = 0;
	std::deque<PendingDump>     m_pending;
	std::vector<uint64_t>       m_results;
	// Slots of published dumps end at m_published_slot, reset ones at m_reset_slot. A hold keeps
	// the slots from `slot` on until the GPU finishes `tick`, whose command buffer copies their
	// results (WriteForReaders()).
	struct SlotHold {
		uint64_t tick = 0;
		uint64_t slot = 0;
	};
	uint64_t                    m_published_slot = 0;
	uint64_t                    m_reset_slot     = 0;
	std::deque<SlotHold>        m_holds;
	// Sequence number of the last dump (execution thread) and of the last one published.
	uint64_t                    m_dump_seq = 0;
	std::atomic<uint64_t>       m_published_seq {0};
	// Execution thread: pages of unpublished dumps, and the newest dump whose page entry another
	// page took (until it is published every read takes the exact look).
	std::vector<PendingPage>    m_pending_pages;
	uint64_t                    m_evicted_seq = 0;
	// Execution thread: bounds of the dumps recorded since all earlier ones were published.
	uint64_t                    m_pending_low  = 0;
	uint64_t                    m_pending_high = 0;
	// Execution thread: the copies the operation being prepared reads under dumps of the command
	// buffer being recorded, and those dumps.
	std::vector<ReadTarget>     m_read_targets;
	std::vector<ReadDump>       m_read_dumps;
	// GPU side of WriteForReaders(): the host query results by slot and the running totals.
	std::unique_ptr<Buffer>     m_slot_results;
	std::unique_ptr<Buffer>     m_totals;
	vk::PipelineLayout          m_counter_layout   = nullptr;
	vk::Pipeline                m_counter_pipeline = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_
