#include "updater/UpdateInstaller.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using Kyty::Updater::Install;
using Kyty::Updater::Validate;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "UpdateInstallerTests: %s\n", message);
		std::abort();
	}
}

std::string Read(const fs::path& path) {
	std::ifstream input(path);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::string& content) {
	fs::create_directories(path.parent_path());
	std::ofstream(path) << content;
	fs::permissions(path, fs::perms::owner_all, fs::perm_options::add);
}

struct Fixture {
	fs::path root   = fs::temp_directory_path() /
	                  ("kyty-updater-test-" +
	                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::path source = root / "release";
	fs::path target = root / "installed";
#ifdef _WIN32
	fs::path    launcher = "launcher.exe";
	const char* emulator = "kyty_emulator.exe";
	const char* updater  = "kyty_updater.exe";
#else
	fs::path    launcher = "launcher";
	const char* emulator = "kyty_emulator";
	const char* updater  = "kyty_updater";
#endif
	Fixture() {
		fs::create_directories(target);
		Release();
		Write(target / launcher, "old launcher");
	}
	void Release() {
		Write(source / launcher, "new launcher");
		Write(source / emulator, "new emulator");
		Write(source / updater, "new updater");
	}
	~Fixture() {
		std::error_code ignored;
		fs::remove_all(root, ignored);
	}
};

void TestOverlayPreservesUserFiles() {
	Fixture fixture;
	Write(fixture.target / "_SaveData" / "save.dat", "valuable save");
	Write(fixture.target / "config.ini", "user settings");
	Write(fixture.source / "lib" / "runtime", "new runtime");
	Check(Validate(fixture.source, fixture.target, fixture.launcher).success,
	      "valid release rejected");
	Check(Install(fixture.source, fixture.target, fixture.launcher).success,
	      "valid release failed to install");
	Check(Read(fixture.target / fixture.launcher) == "new launcher", "launcher was not updated");
	Check(!fs::exists(fixture.source / fixture.launcher),
	      "update copied rather than moved its staged executable");
	Check(Read(fixture.target / "lib" / "runtime") == "new runtime",
	      "nested runtime was not installed");
	Check(Read(fixture.target / "_SaveData" / "save.dat") == "valuable save",
	      "savedata was removed");
	Check(Read(fixture.target / "config.ini") == "user settings", "settings were overwritten");
	for (const auto& entry: fs::directory_iterator(fixture.target)) {
		Check(!entry.path().filename().string().starts_with(".kyty-update"),
		      "successful update left a backup");
	}
}

void TestExclusiveInstallLock() {
	Fixture fixture;
	fs::create_directory(fixture.target / ".kyty-update-lock");
	Check(!Validate(fixture.source, fixture.target, fixture.launcher).success,
	      "validation ignored existing install lock");
	const auto result = Install(fixture.source, fixture.target, fixture.launcher);
	Check(!result.success, "concurrent install lock was ignored");
	Check(Read(fixture.target / fixture.launcher) == "old launcher",
	      "locked update changed installed files");
	Check(fs::exists(fixture.target / ".kyty-update-lock"), "another updater's lock was removed");
}

void TestValidation() {
	Fixture fixture;
	Check(!Validate(fixture.source, fixture.target, "../launcher").success,
	      "parent traversal accepted");
	Check(!Validate(fixture.source, fixture.target, fixture.source / fixture.launcher).success,
	      "absolute launcher accepted");
	Check(!Validate(fixture.source, fixture.source, fixture.launcher).success,
	      "overlapping source accepted");
	fs::remove(fixture.source / fixture.updater);
	Check(!Validate(fixture.source, fixture.target, fixture.launcher).success,
	      "missing updater accepted");
	Write(fixture.source / fixture.updater, "updater");
	Write(fixture.source / ".kyty-update-result", "spoofed result");
	Check(!Validate(fixture.source, fixture.target, fixture.launcher).success,
	      "reserved updater file accepted");
	fs::remove(fixture.source / ".kyty-update-result");
	Write(fixture.target / "lib", "user file");
	Write(fixture.source / "lib" / "runtime", "runtime");
	Check(!Install(fixture.source, fixture.target, fixture.launcher).success,
	      "directory/file collision accepted");
	Check(Read(fixture.target / fixture.launcher) == "old launcher",
	      "validation failure changed existing launcher");
}

#ifndef _WIN32
void TestSymlinks() {
	Fixture fixture;
	Write(fixture.source / "lib" / "runtime.2", "runtime v2");
	fs::create_symlink("runtime.2", fixture.source / "lib" / "runtime");
	const auto first = Install(fixture.source, fixture.target, fixture.launcher);
	if (!first.success) {
		std::fprintf(stderr, "%s\n", first.message.c_str());
	}
	Check(first.success, "internal release symlink rejected");
	Check(fs::is_symlink(fixture.target / "lib" / "runtime"), "release symlink was dereferenced");
	Check(Read(fixture.target / "lib" / "runtime") == "runtime v2",
	      "installed symlink target is wrong");
	fixture.Release();
	Write(fixture.source / "lib" / "runtime.2", "runtime v3");
	fs::create_symlink("runtime.2", fixture.source / "lib" / "runtime");
	Check(Install(fixture.source, fixture.target, fixture.launcher).success,
	      "second update rejected existing internal link");
	fixture.Release();
	fs::create_symlink("../../installed/launcher", fixture.source / "lib" / "escape");
	Check(!Validate(fixture.source, fixture.target, fixture.launcher).success,
	      "escaping source symlink accepted");
	fs::remove(fixture.source / "lib" / "escape");
	fs::create_symlink(fixture.source / fixture.launcher, fixture.source / "absolute-link");
	Check(!Validate(fixture.source, fixture.target, fixture.launcher).success,
	      "absolute source symlink accepted");
	fs::remove(fixture.source / "absolute-link");
	fs::remove_all(fixture.target / "lib");
	fs::create_directory(fixture.root / "user-data");
	fs::create_directory_symlink(fixture.root / "user-data", fixture.target / "lib");
	Check(!Validate(fixture.source, fixture.target, fixture.launcher).success,
	      "destination symlink directory accepted");
	Check(fs::is_empty(fixture.root / "user-data"), "validation wrote through destination symlink");
}

void TestExecutePermissions() {
	Fixture fixture;
	fs::permissions(fixture.source / fixture.launcher,
	                fs::perms::owner_read | fs::perms::owner_write);
	Check(!Validate(fixture.source, fixture.target, fixture.launcher).success,
	      "non-executable launcher accepted");
}

#endif

void TestRollback() {
#ifndef _WIN32
	// A root test runner bypasses file permissions; normal users exercise a real
	// rename failure after the launcher and a new nested file have been
	// installed.
	if (geteuid() == 0) {
		return;
	}
#endif
	Fixture fixture;
	Write(fixture.source / "a-new" / "runtime", "new file");
	Write(fixture.source / "z-locked" / "runtime", "new library");
	Write(fixture.target / "z-locked" / "runtime", "old library");
#ifdef _WIN32
	HANDLE locked_file =
	    CreateFileW((fixture.target / "z-locked" / "runtime").c_str(), GENERIC_READ,
	                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	Check(locked_file != INVALID_HANDLE_VALUE, "could not lock destination for rollback test");
#else
	fs::permissions(fixture.target / "z-locked", fs::perms::owner_read | fs::perms::owner_exec);
#endif
	const auto result = Install(fixture.source, fixture.target, fixture.launcher);
#ifdef _WIN32
	CloseHandle(locked_file);
#else
	fs::permissions(fixture.target / "z-locked", fs::perms::owner_all);
#endif
	Check(!result.success, "locked destination did not fail the transaction");
	Check(Read(fixture.source / fixture.launcher) == "new launcher",
	      "rollback did not restore the staged release");
	Check(Read(fixture.target / fixture.launcher) == "old launcher",
	      "rollback did not restore the launcher");
	Check(Read(fixture.target / "z-locked" / "runtime") == "old library",
	      "rollback damaged an unchanged file");
	Check(!fs::exists(fixture.target / "a-new"), "rollback retained newly installed directories");
	Check(!fs::exists(fixture.target / fixture.emulator),
	      "rollback retained a newly installed executable");
}

#ifndef _WIN32
void TestCommandLine(const fs::path& helper) {
	for (int scenario = 0; scenario < 3; ++scenario) {
		Fixture    fixture;
		const auto workspace = fixture.target / ".kyty-update-test";
		fs::create_directory(workspace);
		fs::rename(fixture.source, workspace / "payload");
		fixture.source          = workspace / "payload";
		const auto original_cwd = fixture.root / "original working directory";
		fs::create_directory(original_cwd);
		const std::string old_launcher =
		    "#!/bin/sh\nprintf 'old\\n' > restarted-version\npwd > restarted-cwd\n";
		Write(fixture.target / fixture.launcher, old_launcher);
		Write(fixture.source / fixture.launcher,
		      "#!/bin/sh\nprintf 'new\\n' > restarted-version\npwd > "
		      "restarted-cwd\n");
		if (scenario == 1) {
			// Invalid payload exercises the CLI's failed-install/old-launcher path.
			fs::remove(fixture.source / fixture.updater);
		} else if (scenario == 2) {
			Write(fixture.target / ".kyty-update-result" / "blocker", "keep");
		}
		int parent_pipe[2];
		Check(pipe(parent_pipe) == 0, "cannot create parent synchronization pipe");
		const auto parent = fork();
		Check(parent >= 0, "cannot fork simulated launcher");
		if (parent == 0) {
			close(parent_pipe[1]);
			char signal;
			static_cast<void>(read(parent_pipe[0], &signal, 1));
			_exit(0);
		}
		close(parent_pipe[0]);
		const auto parent_pid = std::to_string(parent);
		const auto updater    = fork();
		Check(updater >= 0, "cannot fork update helper");
		if (updater == 0) {
			close(parent_pipe[1]);
			execl(helper.c_str(), helper.c_str(), "--install", fixture.source.c_str(),
			      fixture.target.c_str(), fixture.launcher.c_str(), parent_pid.c_str(),
			      original_cwd.c_str(), static_cast<char*>(nullptr));
			_exit(127);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		int status = 0;
		Check(waitpid(updater, &status, WNOHANG) == 0, "helper exited before its parent");
		Check(Read(fixture.target / fixture.launcher) == old_launcher,
		      "helper updated a running launcher");
		close(parent_pipe[1]);
		waitpid(parent, nullptr, 0);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		pid_t      finished;
		while ((finished = waitpid(updater, &status, WNOHANG)) == 0 &&
		       std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (finished == 0) {
			kill(updater, SIGKILL);
			waitpid(updater, nullptr, 0);
		}
		Check(finished == updater && WIFEXITED(status) &&
		          WEXITSTATUS(status) == (scenario == 1 ? 1 : 0),
		      "helper did not complete with the expected status");
		while (Read(original_cwd / "restarted-cwd").empty() &&
		       std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		Check(Read(original_cwd / "restarted-version") == (scenario == 1 ? "old\n" : "new\n"),
		      "wrong launcher restarted");
		Check(Read(original_cwd / "restarted-cwd") == fs::canonical(original_cwd).string() + "\n",
		      "restart changed the working directory");
		if (scenario != 2) {
			Check(Read(fixture.target / ".kyty-update-result")
			          .starts_with(scenario == 1 ? "failure\n" : "success\n"),
			      "helper did not record the update result");
		}
		if (scenario != 1) {
			Check(!fs::exists(workspace), "helper did not clean the successful download");
		}
	}
}

void TestMacBundleScope() {
	Fixture        fixture;
	const fs::path launcher = "KytyPS5.app/Contents/MacOS/KytyPS5";
	Write(fixture.source / launcher, "new mac launcher");
	Write(fixture.source / launcher.parent_path() / "kyty_emulator", "new mac emulator");
	Write(fixture.source / launcher.parent_path() / "kyty_updater", "new mac updater");
	Write(fixture.target / launcher, "old mac launcher");
	Write(fixture.target / launcher.parent_path() / "_SaveData" / "save", "mac save");
	Write(fixture.source / "unrelated.txt", "must stay outside installation");
	Check(Install(fixture.source, fixture.target, launcher).success, "mac bundle update failed");
	Check(Read(fixture.target / launcher) == "new mac launcher", "mac launcher not replaced");
	Check(Read(fixture.target / launcher.parent_path() / "_SaveData" / "save") == "mac save",
	      "mac savedata was removed");
	Check(!fs::exists(fixture.target / "unrelated.txt"), "mac update installed archive siblings");
	Check(Read(fixture.target / fixture.launcher) == "old launcher",
	      "mac update changed flat launcher sibling");
}
#endif
} // namespace

int main(int argc, char* argv[]) {
	TestOverlayPreservesUserFiles();
	TestValidation();
	TestExclusiveInstallLock();
	TestRollback();
#ifndef _WIN32
	TestSymlinks();
	TestExecutePermissions();
	TestMacBundleScope();
	if (argc == 2) {
		TestCommandLine(fs::absolute(argv[1]));
	}
#else
	static_cast<void>(argc);
	static_cast<void>(argv);
#endif
	return 0;
}
