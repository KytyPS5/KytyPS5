#include "graphics/host_gpu/renderer/queueSubmitter.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/frameStats.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics {

namespace {

bool EnvFlag(const char* name) {
	const char* v = std::getenv(name);
	return v != nullptr && v[0] != '\0' && v[0] != '0';
}

} // namespace

QueueSubmitter::QueueSubmitter(GraphicContext& graphics) : m_graphics(graphics) {
	m_inline = EnvFlag("KYTY_SYNC_SUBMIT") || GpuSyncDebugEnabled();
	if (!m_inline) {
		m_thread = std::jthread([this](std::stop_token stop) { Run(stop); });
	}
}

QueueSubmitter::~QueueSubmitter() {
	if (m_thread.joinable()) {
		Drain();
		m_thread.request_stop();
		m_work_available.notify_all();
		m_thread.join();
	}
}

uint64_t QueueSubmitter::Enqueue(MasterSemaphore& master, SubmitInfo info, vk::CommandBuffer buffer,
                                 const DebugInfo& debug) {
	std::unique_lock lock(m_mutex);
	Item             item;
	item.tick = master.NextTick();
	info.AddSignal(master.Handle(), item.tick);
	item.info   = info;
	item.buffer = buffer;
	item.debug  = debug;
	RecordSubmitHistory(item.tick, debug.op, debug.submit_id, debug.arg0, debug.arg1, debug.arg2,
	                    debug.arg3, debug.arg4);
	const auto tick = item.tick;
	if (m_inline) {
		std::vector<Item> batch;
		batch.push_back(std::move(item));
		SubmitBatch(batch);
		return tick;
	}
	m_pending.push_back(std::move(item));
	++m_enqueued;
	lock.unlock();
	m_work_available.notify_one();
	return tick;
}

void QueueSubmitter::Drain() {
	if (m_inline) {
		return;
	}
	EXIT_IF(std::this_thread::get_id() == m_thread.get_id());
	std::unique_lock lock(m_mutex);
	const auto       target = m_enqueued;
	m_progress.wait(lock, [&] { return m_submitted >= target; });
}

void QueueSubmitter::Run(std::stop_token stop) {
	constexpr size_t MaxBatch = 64;
	std::vector<Item> batch;
	batch.reserve(MaxBatch);
	for (;;) {
		batch.clear();
		{
			std::unique_lock lock(m_mutex);
			m_work_available.wait(lock, stop, [this] { return !m_pending.empty(); });
			if (m_pending.empty()) {
				return; // stop requested and nothing left
			}
			while (!m_pending.empty() && batch.size() < MaxBatch) {
				batch.push_back(std::move(m_pending.front()));
				m_pending.pop_front();
			}
		}
		SubmitBatch(batch);
		{
			std::lock_guard lock(m_mutex);
			m_submitted += batch.size();
		}
		m_progress.notify_all();
	}
}

void QueueSubmitter::SubmitBatch(std::vector<Item>& batch) {
	const size_t count = batch.size();
	std::vector<vk::TimelineSemaphoreSubmitInfo> timeline(count);
	std::vector<vk::SubmitInfo>                  submits(count);
	for (size_t i = 0; i < count; i++) {
		auto& item = batch[i];
		auto& t    = timeline[i];
		t.waitSemaphoreValueCount   = item.info.num_wait_semaphores;
		t.pWaitSemaphoreValues      = item.info.wait_ticks.data();
		t.signalSemaphoreValueCount = item.info.num_signal_semaphores;
		t.pSignalSemaphoreValues    = item.info.signal_ticks.data();

		auto& s                = submits[i];
		s.pNext                = &t;
		s.waitSemaphoreCount   = item.info.num_wait_semaphores;
		s.pWaitSemaphores      = item.info.wait_semaphores.data();
		s.pWaitDstStageMask    = item.info.wait_stages.data();
		s.commandBufferCount   = 1;
		s.pCommandBuffers      = &item.buffer;
		s.signalSemaphoreCount = item.info.num_signal_semaphores;
		s.pSignalSemaphores    = item.info.signal_semaphores.data();
	}

	const auto begin = std::chrono::steady_clock::now();
	vk::Result result;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		result = m_graphics.queue.submit(static_cast<uint32_t>(count), submits.data(), nullptr);
	}
	g_frame_stats.submits.fetch_add(count, std::memory_order_relaxed);
	g_frame_stats.submit_batches.fetch_add(1, std::memory_order_relaxed);
	g_frame_stats.submit_us.fetch_add(
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                              std::chrono::steady_clock::now() - begin)
	                              .count()),
	    std::memory_order_relaxed);

	if (result != vk::Result::eSuccess) {
		const auto& d = batch.front().debug;
		LOGF("vkQueueSubmit failed: %s (%d), batch of %zu starting at tick=%" PRIu64
		     " debug_op=%u debug_submit=%" PRIu64 "\n",
		     vk::to_string(result).c_str(), static_cast<int>(result), count, batch.front().tick,
		     d.op, d.submit_id);
		std::printf("vkQueueSubmit failed: %s (%d), batch of %zu starting at tick=%" PRIu64
		            " debug_op=%u debug_submit=%" PRIu64 "\n",
		            vk::to_string(result).c_str(), static_cast<int>(result), count,
		            batch.front().tick, d.op, d.submit_id);
		std::fflush(stdout);
		if (result == vk::Result::eErrorDeviceLost) {
			DumpSubmitHistory();
		}
		EXIT("vkQueueSubmit failed (see log above)\n");
	}
}

void DrainQueueSubmits(GraphicContext& graphics) {
	if (graphics.submitter != nullptr) {
		graphics.submitter->Drain();
	}
}

} // namespace Libs::Graphics
