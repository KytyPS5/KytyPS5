#ifndef KYTY_COMMON_ASYNC_WRITER_H_
#define KYTY_COMMON_ASYNC_WRITER_H_

#include "common/common.h"

#include <cstdint>
#include <filesystem>
#include <fmt/color.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

class AsyncWriter {
public:
	enum class TaskType {
		FileWrite,
		FileAppend,
		CustomTask,
		DualOutput // Write to both file and console
	};

	struct Task {
		TaskType              type = TaskType::FileWrite;
		std::filesystem::path path;
		std::vector<uint8_t>  data;
		std::function<void()> custom_fn;
		std::string           console_text;
		fmt::text_style       console_style;
		uint64_t              timestamp = 0; // Generated at enqueue time
	};

	struct RetryPolicy {
		int max_retries = 3;
		int backoff_ms  = 100; // Exponential backoff base
	};

	struct QueueConfig {
		size_t max_messages = 10000;               // Maximum number of messages in queue
		size_t max_bytes    = 10ULL * 1024 * 1024; // Maximum bytes in queue (10MB)
	};

	static void Initialize();
	static void Shutdown();

	// Configure queue limits and retry policy (must be called before Initialize)
	static void SetQueueConfig(const QueueConfig& config);
	static void SetRetryPolicy(const RetryPolicy& policy);

	// Get final dropped count even after shutdown (snapshot taken at shutdown time)
	static size_t GetFinalDroppedCount();

	// Enqueue writing data to a file asynchronously
	static void EnqueueFileWrite(const std::filesystem::path& path, std::vector<uint8_t> data,
	                             bool append = false);
	static void EnqueueFileWrite(const std::filesystem::path& path, std::string_view text,
	                             bool append = false);

	// Enqueue a custom background task (e.g. command buffer decoding)
	static void EnqueueTask(std::function<void()> task);

	// Enqueue dual-output task (file + console) with timestamp
	static void EnqueueDualOutput(const std::filesystem::path& file_path, std::string_view text,
	                              const std::string& console_text, fmt::text_style style = {},
	                              bool bypass_queue_limit = false);

	// Wait until all currently queued items have been written
	static void Flush();

	// Synchronous best-effort flush for terminate/SEH handlers. It may use C++ locks and
	// filesystem APIs; POSIX signal handlers must not call it.
	static void EmergencyFlush() noexcept;

	// Query pending task count
	static size_t GetPendingCount();

	// Get count of dropped messages (due to queue limits or write failures)
	static size_t GetDroppedCount();

	struct Lifecycle {
		static constexpr char const* name = "AsyncWriter";
		static void                  initialize() { AsyncWriter::Initialize(); }
		static void                  shutdown() { AsyncWriter::Shutdown(); }
		static void                  emergency_shutdown() { AsyncWriter::EmergencyFlush(); }
	};
};

} // namespace Common

#endif /* KYTY_COMMON_ASYNC_WRITER_H_ */
