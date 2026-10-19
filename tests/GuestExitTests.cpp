#include "common/subsystems.h"
#include "libs/guestExit.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::FILE* g_report = nullptr;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "GuestExitTests: %s\n", message);
		std::abort();
	}
}

struct ExitLifecycle {
	static constexpr const char* name = "GuestExitTests";
	static void                  initialize() {}
	static void                  shutdown() { std::_Exit(96); }
	static void emergency_shutdown() { std::fputs("emergency cleanup\n", g_report); }
};

void Child(const char* report, int status, bool from_worker) {
	g_report = std::fopen(report, "wb");
	Check(g_report != nullptr, "open report");
	std::array<char, 4096> buffer {};
	Check(std::setvbuf(g_report, buffer.data(), _IOFBF, buffer.size()) == 0, "buffer report");
	std::fputs("guest output\n", g_report);
	Check(std::atexit([] { std::_Exit(97); }) == 0, "register host teardown guard");
	Common::Subsystems subsystems;
	subsystems.Initialize<ExitLifecycle>();
	std::atomic<bool> ready {false};
	std::thread       worker([&] {
		ready.store(true, std::memory_order_release);
		if (from_worker) {
			Libs::GuestExit(status);
			std::_Exit(98);
		}
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
	worker.detach();
	while (!ready.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	if (!from_worker) {
		Libs::GuestExit(status);
		std::_Exit(98);
	}
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void Run(const std::filesystem::path& executable, const std::filesystem::path& report, int status,
         const char* mode) {
	const auto code = std::to_string(status);
#ifdef _WIN32
	const auto program       = '"' + executable.string() + '"';
	const auto quoted_report = '"' + report.string() + '"';
	const auto result = _spawnl(_P_WAIT, executable.string().c_str(), program.c_str(), "child",
	                            quoted_report.c_str(), code.c_str(), mode, nullptr);
	Check(result == status, "preserve guest exit status without host teardown");
#else
	const auto child = fork();
	Check(child >= 0, "fork child");
	if (child == 0) {
		execl(executable.c_str(), executable.c_str(), "child", report.c_str(), code.c_str(), mode,
		      nullptr);
		std::_Exit(95);
	}
	int result = 0;
	Check(waitpid(child, &result, 0) == child && WIFEXITED(result) && WEXITSTATUS(result) == status,
	      "preserve guest exit status without host teardown");
#endif
	std::ifstream     input(report, std::ios::binary);
	const std::string output((std::istreambuf_iterator<char>(input)),
	                         std::istreambuf_iterator<char>());
	Check(output == "guest output\nemergency cleanup\n",
	      "run safe cleanup and flush buffered guest output before terminating");
	input.close();
	Check(std::filesystem::remove(report), "remove child report");
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 5 && std::string_view(argv[1]) == "child") {
		Child(argv[2], std::atoi(argv[3]), std::string_view(argv[4]) == "worker");
	}
	Check(argc == 1, "expected parent or child arguments");
	const auto executable = std::filesystem::absolute(argv[0]);
	const auto unique     = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto directory =
	    std::filesystem::temp_directory_path() / ("kyty-guest-exit-" + std::to_string(unique));
	Check(std::filesystem::create_directory(directory), "create child report directory");
	for (const int status: {0, 37, 255}) {
		for (const auto* mode: {"main", "worker"}) {
			Run(executable, directory / "report", status, mode);
		}
	}
	Check(std::filesystem::remove(directory), "remove child report directory");
	std::puts("GuestExitTests: all 6 cases passed");
	return 0;
}
