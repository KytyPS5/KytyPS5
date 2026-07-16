#include "graphics/host_gpu/renderer/resourceMutex.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

namespace {

using Libs::Graphics::ResourceMutex;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ResourceMutexTests: failed: %s\n", text);
		std::abort();
	}
}

void YieldMany() {
	for (uint32_t i = 0; i < 4096; i++) {
		std::this_thread::yield();
	}
}

void TestFaultBlocksPublisher() {
	ResourceMutex   mutex;
	std::atomic_bool publisher_started {false};
	std::atomic_bool publisher_entered {false};
	std::thread      publisher;
	{
		ResourceMutex::FaultScope fault(mutex);
		publisher = std::thread([&] {
			publisher_started.store(true, std::memory_order_release);
			std::lock_guard lock(mutex);
			publisher_entered.store(true, std::memory_order_release);
		});
		while (!publisher_started.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		YieldMany();
		Check(!publisher_entered.load(std::memory_order_acquire),
		      "publisher entered during active fault transaction");
	}
	publisher.join();
	Check(publisher_entered.load(std::memory_order_acquire),
	      "publisher did not resume after fault transaction");
}

void TestFaultDrainsExistingOwner() {
	ResourceMutex    mutex;
	std::unique_lock owner(mutex);
	std::atomic_bool fault_started {false};
	std::atomic_bool fault_entered {false};
	std::atomic_bool release_fault {false};
	std::atomic_bool publisher_started {false};
	std::atomic_bool publisher_entered {false};
	std::thread fault([&] {
		fault_started.store(true, std::memory_order_release);
		ResourceMutex::FaultScope scope(mutex);
		fault_entered.store(true, std::memory_order_release);
		while (!release_fault.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
	});
	while (!fault_started.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	YieldMany();
	Check(!fault_entered.load(std::memory_order_acquire),
	      "fault transaction did not wait for existing owner");
	owner.unlock();
	while (!fault_entered.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	std::thread publisher([&] {
		publisher_started.store(true, std::memory_order_release);
		std::lock_guard lock(mutex);
		publisher_entered.store(true, std::memory_order_release);
	});
	while (!publisher_started.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	YieldMany();
	Check(!publisher_entered.load(std::memory_order_acquire),
	      "publisher entered before drained fault completed");
	release_fault.store(true, std::memory_order_release);
	fault.join();
	publisher.join();
	Check(publisher_entered.load(std::memory_order_acquire),
	      "publisher did not resume after drained fault");
}

void TestFaultScopesSerialize() {
	ResourceMutex    mutex;
	std::atomic_bool first_entered {false};
	std::atomic_bool second_started {false};
	std::atomic_bool second_entered {false};
	std::atomic_bool release_first {false};
	std::atomic_bool release_second {false};
	std::atomic_bool publisher_entered {false};
	std::thread first([&] {
		ResourceMutex::FaultScope scope(mutex);
		first_entered.store(true, std::memory_order_release);
		while (!release_first.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
	});
	while (!first_entered.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	std::thread second([&] {
		second_started.store(true, std::memory_order_release);
		ResourceMutex::FaultScope scope(mutex);
		second_entered.store(true, std::memory_order_release);
		while (!release_second.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
	});
	while (!second_started.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	YieldMany();
	Check(!second_entered.load(std::memory_order_acquire),
	      "second fault transaction entered before first completed");
	release_first.store(true, std::memory_order_release);
	while (!second_entered.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	std::thread publisher([&] {
		std::lock_guard lock(mutex);
		publisher_entered.store(true, std::memory_order_release);
	});
	YieldMany();
	Check(!publisher_entered.load(std::memory_order_acquire),
	      "publisher entered during second fault transaction");
	release_second.store(true, std::memory_order_release);
	first.join();
	second.join();
	publisher.join();
	Check(publisher_entered.load(std::memory_order_acquire),
	      "publisher did not resume after serialized faults");
}

void TestPreownedFaultKeepsResourceTransaction() {
	ResourceMutex   mutex;
	std::atomic_bool publisher_started {false};
	std::atomic_bool publisher_entered {false};
	std::thread      publisher;
	std::unique_lock owner(mutex);
	{
		ResourceMutex::FaultScope fault(mutex);
		publisher = std::thread([&] {
			publisher_started.store(true, std::memory_order_release);
			std::lock_guard lock(mutex);
			publisher_entered.store(true, std::memory_order_release);
		});
		while (!publisher_started.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		YieldMany();
		Check(!publisher_entered.load(std::memory_order_acquire),
		      "publisher entered during preowned fault transaction");
	}
	YieldMany();
	Check(!publisher_entered.load(std::memory_order_acquire),
	      "preowned fault released its outer resource transaction");
	owner.unlock();
	publisher.join();
	Check(publisher_entered.load(std::memory_order_acquire),
	      "publisher did not resume after outer transaction");
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
[[noreturn]] void RunDeathCase(const char* name) {
	ResourceMutex mutex;
	if (std::strcmp(name, "recursive-lock") == 0) {
		std::lock_guard first(mutex);
		std::lock_guard second(mutex);
	} else if (std::strcmp(name, "nested-fault") == 0) {
		ResourceMutex::FaultScope first(mutex);
		ResourceMutex::FaultScope second(mutex);
	}
	std::_Exit(0x7f);
}

void CheckDeathCase(const char* name) {
	char path[MAX_PATH] {};
	Check(GetModuleFileNameA(nullptr, path, MAX_PATH) != 0, "GetModuleFileName failed");
	std::string command = std::string("\"") + path + "\" --death " + name;
	std::vector<char> mutable_command(command.begin(), command.end());
	mutable_command.push_back('\0');
	STARTUPINFOA startup {sizeof(startup)};
	PROCESS_INFORMATION process {};
	Check(CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
	                     nullptr, nullptr, &startup, &process) != 0,
	      "CreateProcess failed");
	const auto wait = WaitForSingleObject(process.hProcess, 10000);
	if (wait != WAIT_OBJECT_0) {
		TerminateProcess(process.hProcess, 0x7e);
	}
	Check(wait == WAIT_OBJECT_0, "ResourceMutex death test timed out");
	DWORD exit_code = 0;
	Check(GetExitCodeProcess(process.hProcess, &exit_code) != 0 && exit_code == 321,
	      "ResourceMutex death path used the wrong exit");
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
}
#endif

} // namespace

int main(int argc, char** argv) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (argc == 3 && std::strcmp(argv[1], "--death") == 0) {
		RunDeathCase(argv[2]);
	}
#else
	(void)argc;
	(void)argv;
#endif
	TestFaultBlocksPublisher();
	TestFaultDrainsExistingOwner();
	TestFaultScopesSerialize();
	TestPreownedFaultKeepsResourceTransaction();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	CheckDeathCase("recursive-lock");
	CheckDeathCase("nested-fault");
#endif
	std::puts("ResourceMutexTests: all cases passed");
	return 0;
}
