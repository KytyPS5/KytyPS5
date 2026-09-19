#include "common/asyncWriter.h"

#include "common/stringUtils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <thread>

namespace Common {

namespace {

struct WriterState {
	std::mutex                    mutex;
	std::condition_variable       cv_work;
	std::condition_variable       cv_flush;
	std::deque<AsyncWriter::Task> queue;
	std::thread                   worker_thread;
	std::atomic_bool              running {false};
	std::atomic_bool              is_working {false};
	std::atomic_bool              in_emergency {false};
	std::atomic_size_t            dropped_count {0};
	std::atomic_size_t            queue_bytes {0};
	AsyncWriter::QueueConfig      queue_config;
	AsyncWriter::RetryPolicy      retry_policy;
	std::atomic_size_t            in_flight {0};
	bool                          accepting = true;
};

static std::shared_ptr<WriterState> g_writer;
static std::mutex                   g_init_mutex;
static AsyncWriter::QueueConfig     s_queue_config;
static AsyncWriter::RetryPolicy     s_retry_policy;
static bool                         s_config_set = false;
static std::atomic_size_t           g_final_dropped_count {0}; // Snapshot taken at shutdown

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define KYTY_ASYNC_WRITER_HAS_EXCEPTIONS 1
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <share.h>
#endif

static FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	wchar_t wmode[8] = {};
	for (size_t i = 0; mode[i] != '\0' && i < 7; ++i) {
		wmode[i] = static_cast<wchar_t>(mode[i]);
	}
	return _wfsopen(path.c_str(), wmode, _SH_DENYNO);
#else
	return std::fopen(path.c_str(), mode);
#endif
}

static void ProcessTask(const AsyncWriter::Task& task, WriterState* state,
                        bool emergency_mode) noexcept {
	// Pass state explicitly to avoid race with Shutdown (1.1)
	// Catch all exceptions to honor noexcept (1.3)
#if defined(KYTY_ASYNC_WRITER_HAS_EXCEPTIONS)
	try {
#endif
		switch (task.type) {
			case AsyncWriter::TaskType::FileWrite: {
				if (!task.path.empty()) {
					std::error_code ec;
					const auto      parent = task.path.parent_path();
					if (!parent.empty()) {
						std::filesystem::create_directories(parent, ec);
					}

					int retry_count = 0;
					int max_retries =
					    emergency_mode ? 0 : (state ? state->retry_policy.max_retries : 3);
					int backoff_ms = state ? state->retry_policy.backoff_ms : 100;

					while (retry_count <= max_retries) {
						FILE* fp = OpenFile(task.path, "wb");
						if (fp != nullptr) {
							bool write_success = true;
							if (!task.data.empty()) {
								size_t written =
								    std::fwrite(task.data.data(), 1, task.data.size(), fp);
								if (written != task.data.size()) {
									write_success = false; // Partial write (2.1)
								}
							}
							int close_result = std::fclose(fp);
							if (close_result != 0) {
								write_success = false; // fclose error (2.1)
							}
							if (write_success) {
								break; // Success
							}
						}

						retry_count++;
						if (retry_count <= max_retries && !emergency_mode) {
							std::this_thread::sleep_for(
							    std::chrono::milliseconds(backoff_ms * retry_count));
						}
					}

					if (retry_count > max_retries && state) {
						state->dropped_count.fetch_add(1, std::memory_order_relaxed);
					}
				}
				break;
			}
			case AsyncWriter::TaskType::FileAppend: {
				if (!task.path.empty()) {
					std::error_code ec;
					const auto      parent = task.path.parent_path();
					if (!parent.empty()) {
						std::filesystem::create_directories(parent, ec);
					}

					int retry_count = 0;
					int max_retries =
					    emergency_mode ? 0 : (state ? state->retry_policy.max_retries : 3);
					int backoff_ms = state ? state->retry_policy.backoff_ms : 100;

					while (retry_count <= max_retries) {
						FILE* fp = OpenFile(task.path, "ab");
						if (fp != nullptr) {
							bool write_success = true;
							if (!task.data.empty()) {
								size_t written =
								    std::fwrite(task.data.data(), 1, task.data.size(), fp);
								if (written != task.data.size()) {
									write_success = false; // Partial write (2.1)
								}
							}
							int close_result = std::fclose(fp);
							if (close_result != 0) {
								write_success = false; // fclose error (2.1)
							}
							if (write_success) {
								break; // Success
							}
						}

						retry_count++;
						if (retry_count <= max_retries && !emergency_mode) {
							std::this_thread::sleep_for(
							    std::chrono::milliseconds(backoff_ms * retry_count));
						}
					}

					if (retry_count > max_retries && state) {
						state->dropped_count.fetch_add(1, std::memory_order_relaxed);
					}
				}
				break;
			}
			case AsyncWriter::TaskType::CustomTask: {
				if (task.custom_fn) {
					task.custom_fn();
				}
				break;
			}
			case AsyncWriter::TaskType::DualOutput: {
				// Write to file
				if (!task.path.empty()) {
					std::error_code ec;
					const auto      parent = task.path.parent_path();
					if (!parent.empty()) {
						std::filesystem::create_directories(parent, ec);
					}

					int retry_count = 0;
					int max_retries =
					    emergency_mode ? 0 : (state ? state->retry_policy.max_retries : 3);
					int backoff_ms = state ? state->retry_policy.backoff_ms : 100;

					while (retry_count <= max_retries) {
						FILE* fp = OpenFile(task.path, "ab");
						if (fp != nullptr) {
							bool write_success = true;
							if (!task.data.empty()) {
								size_t written =
								    std::fwrite(task.data.data(), 1, task.data.size(), fp);
								if (written != task.data.size()) {
									write_success = false; // Partial write (2.1)
								}
							}
							int close_result = std::fclose(fp);
							if (close_result != 0) {
								write_success = false; // fclose error (2.1)
							}
							if (write_success) {
								break; // Success
							}
						}

						retry_count++;
						if (retry_count <= max_retries && !emergency_mode) {
							std::this_thread::sleep_for(
							    std::chrono::milliseconds(backoff_ms * retry_count));
						}
					}

					if (retry_count > max_retries && state) {
						state->dropped_count.fetch_add(1, std::memory_order_relaxed);
					}
				}

				// Write to console
				if (!task.console_text.empty()) {
					if (task.console_style != fmt::text_style {}) {
						fmt::print(stdout, task.console_style, "{}", task.console_text);
					} else {
						std::fwrite(task.console_text.data(), 1, task.console_text.size(), stdout);
					}
					std::fflush(stdout);
				}
				break;
			}
		}
#if defined(KYTY_ASYNC_WRITER_HAS_EXCEPTIONS)
	} catch (...) {
		// Catch all exceptions to honor noexcept (1.3)
		if (state) {
			state->dropped_count.fetch_add(1, std::memory_order_relaxed);
		}
	}
#endif
}

static void WorkerLoop(WriterState* state) {
	while (true) {
		std::deque<AsyncWriter::Task> batch;
		{
			std::unique_lock lock(state->mutex);
			state->cv_work.wait(lock, [state] {
				return !state->running.load(std::memory_order_acquire) || !state->queue.empty();
			});

			if (!state->running.load(std::memory_order_acquire) && state->queue.empty()) {
				break;
			}

			if (state->queue.empty()) {
				continue;
			}

			state->in_flight.fetch_add(batch.size(), std::memory_order_release);
			batch.swap(state->queue);
			state->queue_bytes.store(0, std::memory_order_relaxed);
			state->is_working.store(true, std::memory_order_release);
		}

		for (const auto& task: batch) {
			ProcessTask(task, state, false);
		}

		{
			std::lock_guard lock(state->mutex);
			state->in_flight.fetch_sub(batch.size(), std::memory_order_release);
			state->is_working.store(false, std::memory_order_release);
			if (state->queue.empty()) {
				state->cv_flush.notify_all();
			}
		}
	}
}

} // namespace

void Common::AsyncWriter::SetQueueConfig(const QueueConfig& config) {
	std::lock_guard init_lock(g_init_mutex);
	if (g_writer != nullptr) {
		return; // Cannot change config after initialization
	}
	s_queue_config = config;
	s_config_set   = true;
}

void Common::AsyncWriter::SetRetryPolicy(const RetryPolicy& policy) {
	std::lock_guard init_lock(g_init_mutex);
	if (g_writer != nullptr) {
		return; // Cannot change policy after initialization
	}
	s_retry_policy = policy;
}

void Common::AsyncWriter::Initialize() {
	std::lock_guard init_lock(g_init_mutex);
	if (g_writer != nullptr) {
		return;
	}
	g_final_dropped_count.store(0, std::memory_order_relaxed);

	auto state = std::make_shared<WriterState>();
	state->running.store(true, std::memory_order_release);
	state->queue_config  = s_config_set ? s_queue_config : QueueConfig {};
	state->retry_policy  = s_retry_policy;
	state->worker_thread = std::thread(WorkerLoop, state.get());
	g_writer             = state;
}

void Common::AsyncWriter::Shutdown() {
	std::shared_ptr<WriterState> state;
	{
		std::lock_guard init_lock(g_init_mutex);
		state = g_writer;
	}

	if (state == nullptr) {
		return;
	}

	{
		std::lock_guard lock(state->mutex);
		state->accepting = false;
		state->running.store(false, std::memory_order_release);
		state->cv_work.notify_all();
	}

	if (state->worker_thread.joinable()) {
		state->worker_thread.join();
	}

	// Producers holding this shared state are rejected by accepting=false. Keep all
	// queue mutations under the queue mutex while draining the final batch.
	for (;;) {
		AsyncWriter::Task task;
		{
			std::lock_guard lock(state->mutex);
			if (state->queue.empty()) {
				break;
			}
			task = std::move(state->queue.front());
			state->queue.pop_front();
		}
		ProcessTask(task, state.get(), false);
	}

	g_final_dropped_count.store(state->dropped_count.load(std::memory_order_relaxed),
	                            std::memory_order_relaxed);
	{
		std::lock_guard init_lock(g_init_mutex);
		if (g_writer == state) {
			g_writer.reset();
		}
	}
}

// Helper function to apply queue limits uniformly (2.3)
static bool ApplyQueueLimits(WriterState* state, size_t data_size, bool bypass_limit) {
	if (bypass_limit) {
		return true;
	}

	// Check if single payload exceeds max_bytes on empty queue (2.3)
	if (state->queue.empty() && data_size > state->queue_config.max_bytes) {
		state->dropped_count.fetch_add(1, std::memory_order_relaxed);
		return false; // Reject task
	}

	while (state->queue.size() >= state->queue_config.max_messages ||
	       state->queue_bytes.load(std::memory_order_relaxed) + data_size >
	           state->queue_config.max_bytes) {
		if (!state->queue.empty()) {
			size_t oldest_size = state->queue.front().data.size();
			state->queue.pop_front();
			state->dropped_count.fetch_add(1, std::memory_order_relaxed);
			state->queue_bytes.fetch_sub(oldest_size, std::memory_order_relaxed);
		} else {
			state->dropped_count.fetch_add(1, std::memory_order_relaxed);
			return false; // Cannot fit even after dropping all
		}
	}
	return true;
}

static std::shared_ptr<WriterState> AcquireWriterState() {
	std::lock_guard init_lock(g_init_mutex);
	if (g_writer == nullptr) {
		auto state = std::make_shared<WriterState>();
		state->running.store(true, std::memory_order_release);
		state->queue_config  = s_config_set ? s_queue_config : AsyncWriter::QueueConfig {};
		state->retry_policy  = s_retry_policy;
		state->worker_thread = std::thread(WorkerLoop, state.get());
		g_writer             = std::move(state);
	}
	return g_writer;
}

void Common::AsyncWriter::EnqueueFileWrite(const std::filesystem::path& path,
                                           std::vector<uint8_t> data, bool append) {
	std::shared_ptr<WriterState> state = AcquireWriterState();

	size_t data_size = data.size();
	Task   task {
	    .type      = append ? TaskType::FileAppend : TaskType::FileWrite,
	    .path      = path,
	    .data      = std::move(data),
	    .custom_fn = nullptr,
	};

	{
		std::lock_guard lock(state->mutex);
		if (!state->accepting) {
			return;
		}
		if (!ApplyQueueLimits(state.get(), data_size, false)) {
			return; // Task rejected due to queue limits
		}
		state->queue.push_back(std::move(task));
		state->queue_bytes.fetch_add(data_size, std::memory_order_relaxed);
		state->cv_work.notify_one();
	}
}

void Common::AsyncWriter::EnqueueFileWrite(const std::filesystem::path& path, std::string_view text,
                                           bool append) {
	std::vector<uint8_t> bytes(text.begin(), text.end());
	EnqueueFileWrite(path, std::move(bytes), append);
}

void Common::AsyncWriter::EnqueueDualOutput(const std::filesystem::path& file_path,
                                            std::string_view text, const std::string& console_text,
                                            fmt::text_style style, bool bypass_queue_limit) {
	std::shared_ptr<WriterState> state = AcquireWriterState();

	size_t data_size = text.size();
	Task   task {
	    .type          = TaskType::DualOutput,
	    .path          = file_path,
	    .data          = std::vector<uint8_t>(text.begin(), text.end()),
	    .custom_fn     = nullptr,
	    .console_text  = console_text,
	    .console_style = style,
	    .timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                           std::chrono::steady_clock::now().time_since_epoch())
	                                           .count()),
	};

	{
		std::lock_guard lock(state->mutex);
		if (!state->accepting) {
			return;
		}
		if (!ApplyQueueLimits(state.get(), data_size, bypass_queue_limit)) {
			return; // Task rejected due to queue limits
		}
		state->queue.push_back(std::move(task));
		state->queue_bytes.fetch_add(data_size, std::memory_order_relaxed);
		state->cv_work.notify_one();
	}
}

void Common::AsyncWriter::EnqueueTask(std::function<void()> task) {
	std::shared_ptr<WriterState> state = AcquireWriterState();

	Task t {
	    .type      = TaskType::CustomTask,
	    .path      = {},
	    .data      = {},
	    .custom_fn = std::move(task),
	};

	{
		std::lock_guard lock(state->mutex);
		if (!state->accepting) {
			return;
		}
		// Apply queue limits to custom tasks too (2.3)
		if (!ApplyQueueLimits(state.get(), 0, false)) {
			return; // Task rejected due to queue limits
		}
		state->queue.push_back(std::move(t));
		state->cv_work.notify_one();
	}
}

void Common::AsyncWriter::Flush() {
	std::shared_ptr<WriterState> state;
	{
		std::lock_guard init_lock(g_init_mutex);
		state = g_writer;
	}

	if (state == nullptr) {
		return;
	}

	std::unique_lock lock(state->mutex);
	state->cv_flush.wait(lock, [state] {
		return state->queue.empty() && !state->is_working.load(std::memory_order_acquire);
	});
}

void Common::AsyncWriter::EmergencyFlush() noexcept {
	std::shared_ptr<WriterState> state;
	{
		std::lock_guard init_lock(g_init_mutex);
		state = g_writer;
	}

	if (state == nullptr) {
		std::fflush(nullptr);
		return;
	}

	// Prevent re-entry or recursive deadlocks during crash handling
	bool expected = false;
	if (!state->in_emergency.compare_exchange_strong(expected, true)) {
		std::fflush(nullptr);
		return;
	}

	// Process tasks synchronously WITHOUT swapping the queue (1.2)
	// We process only what we can safely access
	{
		std::unique_lock lock(state->mutex, std::defer_lock);
		if (lock.try_lock()) {
			// Lock acquired - process queue safely
			while (!state->queue.empty()) {
				ProcessTask(state->queue.front(), state.get(), true); // Emergency mode (2.2)
				state->queue.pop_front();
			}
		}
		// If lock cannot be acquired, we cannot safely access the queue (1.2)
		// In this case, we just flush stdout and return
	}

	std::fflush(nullptr);
}

size_t Common::AsyncWriter::GetPendingCount() {
	std::shared_ptr<WriterState> state;
	{
		std::lock_guard init_lock(g_init_mutex);
		state = g_writer;
	}

	if (state == nullptr) {
		return 0;
	}
	std::lock_guard lock(state->mutex);
	return state->queue.size() + state->in_flight.load(std::memory_order_acquire);
}

size_t Common::AsyncWriter::GetDroppedCount() {
	std::shared_ptr<WriterState> state;
	{
		std::lock_guard init_lock(g_init_mutex);
		state = g_writer;
	}

	if (state == nullptr) {
		return g_final_dropped_count.load(
		    std::memory_order_relaxed); // Return snapshot after shutdown (2.4)
	}
	return state->dropped_count.load(std::memory_order_relaxed);
}

size_t Common::AsyncWriter::GetFinalDroppedCount() {
	return g_final_dropped_count.load(std::memory_order_relaxed);
}

} // namespace Common
