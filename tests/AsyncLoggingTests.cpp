#include "common/asyncWriter.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AsyncLoggingTests: failed: %s\n", message);
		std::abort();
	}
}

void TestBasicConsoleAndFile() {
	std::printf("AsyncLoggingTests: TestBasicConsoleAndFile...\n");

	// Test AsyncWriter directly
	Common::AsyncWriter::Initialize();

	// Write some test messages
	Common::AsyncWriter::EnqueueFileWrite("_test_async_log.txt", "Test message 1\n", true);
	Common::AsyncWriter::EnqueueFileWrite("_test_async_log.txt", "Test message 2\n", true);
	Common::AsyncWriter::EnqueueFileWrite("_test_async_log.txt", "Test message 3\n", true);

	Common::AsyncWriter::Flush();
	Common::AsyncWriter::Shutdown();

	// Check if file was created
	Check(std::filesystem::exists("_test_async_log.txt"), "Log file was not created");

	// Clean up
	std::filesystem::remove("_test_async_log.txt");

	std::printf("AsyncLoggingTests: TestBasicConsoleAndFile passed\n");
}

void TestQueueLimits() {
	std::printf("AsyncLoggingTests: TestQueueLimits...\n");

	// Set small queue limits
	Common::AsyncWriter::QueueConfig queue_config;
	queue_config.max_messages = 10;
	queue_config.max_bytes = 8;
	Common::AsyncWriter::SetQueueConfig(queue_config);

	Common::AsyncWriter::Initialize();

	// Enqueue more messages than the limit
	for (int i = 0; i < 20; i++) {
		std::string msg = "Test message " + std::to_string(i) + "\n";
		Common::AsyncWriter::EnqueueFileWrite("_test_queue_limits.txt", msg, true);
	}

	Common::AsyncWriter::Flush();
	Common::AsyncWriter::Shutdown();

	// Check that some messages were dropped
	size_t dropped = Common::AsyncWriter::GetDroppedCount();
	Check(dropped > 0, "No messages were dropped despite queue limit");

	// Clean up
	std::filesystem::remove("_test_queue_limits.txt");
	Common::AsyncWriter::SetQueueConfig(Common::AsyncWriter::QueueConfig {});

	std::printf("AsyncLoggingTests: TestQueueLimits passed (dropped: %zu)\n", dropped);
}

void TestLoadScenario() {
	std::printf("AsyncLoggingTests: TestLoadScenario...\n");

	Common::AsyncWriter::Initialize();

	const int message_count = 10000;
	auto start = std::chrono::high_resolution_clock::now();

	// Enqueue many messages rapidly
	for (int i = 0; i < message_count; i++) {
		std::string msg = "Load test message " + std::to_string(i) + "\n";
		Common::AsyncWriter::EnqueueFileWrite("_test_load.txt", msg, true);
	}

	Common::AsyncWriter::Flush();
	auto end = std::chrono::high_resolution_clock::now();

	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	std::printf("AsyncLoggingTests: Processed %d messages in %lld ms\n", message_count,
	             static_cast<long long>(duration.count()));

	Common::AsyncWriter::Shutdown();

	// Clean up
	std::filesystem::remove("_test_load.txt");

	std::printf("AsyncLoggingTests: TestLoadScenario passed\n");
}

void TestDualOutput() {
	std::printf("AsyncLoggingTests: TestDualOutput...\n");

	Common::AsyncWriter::Initialize();

	// Test dual-output task
	std::string console_text = "Console output\n";
	std::string file_text = "File output\n";
	Common::AsyncWriter::EnqueueDualOutput("_test_dual.txt", file_text, console_text);

	Common::AsyncWriter::Flush();
	Common::AsyncWriter::Shutdown();

	// Check if file was created
	Check(std::filesystem::exists("_test_dual.txt"), "Dual output file was not created");

	// Clean up
	std::filesystem::remove("_test_dual.txt");

	std::printf("AsyncLoggingTests: TestDualOutput passed\n");
}

void TestRetryLogic() {
	std::printf("AsyncLoggingTests: TestRetryLogic...\n");

	// Set retry policy
	Common::AsyncWriter::RetryPolicy retry_policy;
	retry_policy.max_retries = 2;
	retry_policy.backoff_ms = 10;
	Common::AsyncWriter::SetRetryPolicy(retry_policy);

	Common::AsyncWriter::Initialize();

	// A regular file cannot be a directory, so this fails with ENOTDIR on every platform.
	const std::filesystem::path blocker = "_test_retry_blocker";
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
	} cleanup {blocker};
	std::error_code ec;
	std::filesystem::remove(blocker, ec);
	std::ofstream(blocker).put('x');
	Common::AsyncWriter::EnqueueFileWrite(blocker / "subpath.txt", "test", false);

	Common::AsyncWriter::Flush();
	Common::AsyncWriter::Shutdown();

	// Check that message was dropped after retries
	size_t dropped = Common::AsyncWriter::GetDroppedCount();
	Check(dropped > 0, "Message was not dropped after failed retries");

	std::printf("AsyncLoggingTests: TestRetryLogic passed (dropped: %zu)\n", dropped);
}

void TestConcurrentShutdown() {
	std::printf("AsyncLoggingTests: TestConcurrentShutdown...\n");

	Common::AsyncWriter::Initialize();
	std::thread producer([&] {
		for (int i = 0; i < 10000; ++i) {
			Common::AsyncWriter::EnqueueTask([] {});
		}
	});

	std::thread shutdown_thread([] { Common::AsyncWriter::Shutdown(); });
	producer.join();
	shutdown_thread.join();
	// A producer that starts after the first lifecycle has ended may create the
	// next lifecycle; close it as well before checking the final state.
	Common::AsyncWriter::Shutdown();

	Check(Common::AsyncWriter::GetPendingCount() == 0,
	      "Pending tasks remained after concurrent shutdown");
	std::printf("AsyncLoggingTests: TestConcurrentShutdown passed\n");
}

void TestLogLevelFiltering() {
	std::printf("AsyncLoggingTests: TestLogLevelFiltering...\n");

	// This test would require full Log system initialization
	// For now, we'll skip it and test the underlying AsyncWriter directly
	std::printf("AsyncLoggingTests: TestLogLevelFiltering skipped (requires full Log system)\n");
}

void TestEmergencyFlush() {
	std::printf("AsyncLoggingTests: TestEmergencyFlush...\n");

	Common::AsyncWriter::Initialize();

	// Enqueue some messages
	for (int i = 0; i < 5; i++) {
		std::string msg = "Emergency test message " + std::to_string(i) + "\n";
		Common::AsyncWriter::EnqueueFileWrite("_test_emergency.txt", msg, true);
	}

	// Call emergency flush (simulating crash scenario)
	Common::AsyncWriter::EmergencyFlush();

	Common::AsyncWriter::Shutdown();

	// Check if file was created with messages
	Check(std::filesystem::exists("_test_emergency.txt"), "Emergency flush file was not created");

	// Clean up
	std::filesystem::remove("_test_emergency.txt");

	std::printf("AsyncLoggingTests: TestEmergencyFlush passed\n");
}

void TestExplicitFlush() {
	std::printf("AsyncLoggingTests: TestExplicitFlush...\n");

	Common::AsyncWriter::Initialize();

	// Enqueue messages
	for (int i = 0; i < 10; i++) {
		std::string msg = "Flush test message " + std::to_string(i) + "\n";
		Common::AsyncWriter::EnqueueFileWrite("_test_flush.txt", msg, true);
	}

	// Explicit flush
	Common::AsyncWriter::Flush();

	Common::AsyncWriter::Shutdown();

	// Check if file was created
	Check(std::filesystem::exists("_test_flush.txt"), "Flush file was not created");

	// Clean up
	std::filesystem::remove("_test_flush.txt");

	std::printf("AsyncLoggingTests: TestExplicitFlush passed\n");
}

} // namespace

int main() {
	std::printf("AsyncLoggingTests: Starting...\n");

	TestBasicConsoleAndFile();
	TestQueueLimits();
	TestLoadScenario();
	TestDualOutput();
	TestRetryLogic();
	TestConcurrentShutdown();
	TestLogLevelFiltering();
	TestEmergencyFlush();
	TestExplicitFlush();

	std::printf("AsyncLoggingTests: All tests passed\n");
	return 0;
}
