#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
class RenderContext;
struct GraphicContext;

// Guest GPU clock writes (end-of-pipe timestamps, COPY_DATA of the GPU clock), taken by the host
// GPU when the command executes. They reach guest memory, in the guest 100 MHz reference clock,
// when the command buffer completes and before the interrupts recorded after them.
class GpuTimestamps {
public:
	// Timestamps in flight. A write beyond them waits for the oldest to complete.
	static constexpr uint32_t QueryCount = 4096;

	// reference = reference_base + (ticks - device_base) * rate / 2^32
	struct Segment {
		uint64_t device_base    = 0;
		uint64_t reference_base = 0;
		uint64_t rate           = 0;
	};

	// Host GPU ticks to the guest clock, for a counter with the valid bits of mask.
	class Clock {
	public:
		Clock(uint64_t mask, double nominal_rate): m_mask(mask), m_nominal_rate(nominal_rate) {}

		// Calibrated pair of device ticks and reference time. The first one starts measuring
		// the rate, the second anchors the clock, later ones correct it.
		void Sample(uint64_t ticks, uint64_t reference);
		// Without calibrated pairs: one anchor with the nominal rate, then Advance keeps the
		// segment base recent so that masked ticks stay within half a counter period of it.
		void Anchor(uint64_t ticks, uint64_t reference);
		void Advance(uint64_t reference);

		[[nodiscard]] bool            Anchored() const { return m_anchored; }
		[[nodiscard]] uint64_t        LastReference() const { return m_last_reference; }
		[[nodiscard]] const Segment&  Previous() const { return m_previous; }
		[[nodiscard]] const Segment&  Current() const { return m_current; }
		[[nodiscard]] uint64_t        Convert(uint64_t ticks) const;
		[[nodiscard]] static uint64_t Convert(const Segment& previous, const Segment& current,
		                                      uint64_t ticks, uint64_t mask);

	private:
		void Restart(uint64_t ticks, uint64_t reference);

		uint64_t m_mask         = 0;
		double   m_nominal_rate = 0.0;
		bool     m_sampled      = false;
		bool     m_anchored     = false;
		// Rate measurement: reference time since its start over the masked intervals summed.
		uint64_t m_measure_reference = 0;
		uint64_t m_measure_ticks     = 0;
		uint64_t m_last_ticks        = 0;
		uint64_t m_last_reference    = 0;
		Segment  m_anchor;
		Segment  m_previous;
		Segment  m_current;
	};

	GpuTimestamps(GraphicContext& graphics, CommandScheduler& scheduler, RenderContext& context);
	~GpuTimestamps();
	KYTY_CLASS_NO_COPY(GpuTimestamps);

	// End of pipe: after all earlier commands complete. Otherwise when the command is reached.
	void Write(uint64_t vaddr, uint32_t size, bool end_of_pipe);
	// End-of-pipe label (immediate data). Behind a GPU clock value not stored yet, it is stored
	// after that value, at completion. False when none is pending: the caller writes it now.
	[[nodiscard]] bool WriteLabel(uint64_t vaddr, uint64_t value, uint32_t size);
	// Stores every value recorded so far, before a write the GPU thread makes itself.
	void StoreAll();
	// Runs a guest-visible completion effect, such as an interrupt, from a completion callback
	// after the values before it: now, or on the GPU thread right after the ones left to it.
	void Signal(Common::UniqueFunction<void>&& effect);
	// Stores the values that completions left to the GPU thread and runs the effects queued
	// behind them, in their order. A completion that leaves one queues this on the GPU thread;
	// every write and an unmap call it too.
	void StoreRetries();

	// Conversion of host ticks at or after previous.device_base.
	[[nodiscard]] static uint64_t ToReference(const Segment& previous, const Segment& current,
	                                          uint64_t ticks);

private:
	// A GPU clock value read from query, or a label with its value.
	struct Pending {
		uint64_t vaddr = 0;
		uint64_t value = 0;
		uint32_t query = 0;
		uint32_t size  = 0;
		bool     label = false;
	};

	struct Batch {
		std::vector<Pending> writes;
		uint32_t             first_query = 0;
		uint32_t             queries     = 0;
		Segment              previous;
		Segment              current;
		bool                 resolved = false;
	};

	// A value to store, or an effect to run when it has one.
	struct Retry {
		uint64_t                     vaddr = 0;
		uint64_t                     value = 0;
		uint32_t                     size  = 0;
		Common::UniqueFunction<void> effect;
	};

	[[nodiscard]] bool SampleClocks(uint64_t& ticks, uint64_t& reference) const;
	void               InitializeWithSubmission(bool anchor);
	Batch&             CurrentBatch();
	void               Resolve();
	void               Copy(const Batch& batch);
	void               Complete(const Batch& batch);
	void               Stored(uint64_t count);
	void               QueueRetries();

	GraphicContext&        m_graphics;
	CommandScheduler&      m_scheduler;
	RenderContext&         m_context;
	uint64_t               m_mask = 0;
	Buffer                 m_readback;
	vk::QueryPool          m_pool = nullptr;
	std::shared_ptr<Batch> m_batch;
	uint64_t               m_issued = 0;
	std::atomic<uint64_t>  m_retired {0};
	// Values recorded and not stored yet: a label behind one waits for it.
	std::atomic<uint64_t> m_unstored {0};
	std::mutex            m_mutex;
	std::mutex            m_store_mutex;
	std::vector<Retry>    m_retries;
	bool                  m_retrying     = false;
	bool                  m_retry_queued = false;
	Clock                 m_clock;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
