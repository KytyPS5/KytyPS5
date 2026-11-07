// Tests for the lock that keeps two emulators from writing the same pipeline lists.

#include "common/fileLock.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace {

using Common::FileLock;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		g_failures++;
	}
}

std::filesystem::path LockPath() {
	return std::filesystem::temp_directory_path() / "kyty-file-lock-test" / "nested" / "T.lock";
}

void TestOneHolderAtATime() {
	std::error_code error;
	std::filesystem::remove_all(LockPath().parent_path().parent_path(), error);
	{
		FileLock first;
		Check(first.TryAcquire(LockPath()) && first.Held(),
		      "the first taker gets the lock, and its folder is created");
		FileLock second;
		Check(!second.TryAcquire(LockPath()) && !second.Held(),
		      "a second taker does not get it while the first holds it");
		Check(first.TryAcquire(LockPath()), "the holder taking it again keeps it");
	}
	FileLock after;
	Check(after.TryAcquire(LockPath()), "a lock is free again once its holder is gone");
}

// This test run as a second process: it tries the lock once and says whether it got it.
constexpr const char* kTryLockArgument = "--try-lock";
constexpr int         kGotTheLock      = 10;
constexpr int         kLockWasTaken    = 20;

// Runs this test again as another process that tries the lock; its exit code, or -1.
int TryLockInAnotherProcess(const char* self) {
#if defined(_WIN32)
	// cmd drops the outer quotes of a command line that starts with one.
	const auto command = std::string("\"\"") + self + "\" " + kTryLockArgument + "\"";
	return std::system(command.c_str());
#else
	const auto command = std::string("\"") + self + "\" " + kTryLockArgument;
	const int  status  = std::system(command.c_str());
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// The lock is for two emulators: a second process does not get it while the first holds it.
void TestAnotherProcessWaitsForTheHolder(const char* self) {
	{
		FileLock holder;
		Check(holder.TryAcquire(LockPath()), "the first process gets the lock");
		Check(TryLockInAnotherProcess(self) == kLockWasTaken,
		      "another process got the lock while this one held it");
	}
	Check(TryLockInAnotherProcess(self) == kGotTheLock,
	      "another process did not get the lock once it was free");
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 2 && std::strcmp(argv[1], kTryLockArgument) == 0) {
		FileLock lock;
		return lock.TryAcquire(LockPath()) ? kGotTheLock : kLockWasTaken;
	}
	TestOneHolderAtATime();
	TestAnotherProcessWaitsForTheHolder(argv[0]);
	std::error_code error;
	std::filesystem::remove_all(LockPath().parent_path().parent_path(), error);
	if (g_failures != 0) {
		std::cerr << g_failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "file lock tests passed\n";
	return 0;
}
