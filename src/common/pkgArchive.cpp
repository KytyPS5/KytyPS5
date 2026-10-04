// SPDX-License-Identifier: GPL-2.0-only
#include "common/pkgArchive.h"

#include "common/archiveReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace Common {
namespace {
// Resolve from the executable, never the working directory: launcher shortcuts
// and the emulator child may start in different directories.
std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
	std::wstring buffer(32768, L'\0');
	const auto   length =
	    GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || length >= buffer.size()) return {};
	buffer.resize(length);
	return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buffer(size, '\0');
	if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
	return std::filesystem::path(buffer.c_str()).parent_path();
#else
	std::error_code error;
	const auto      executable = std::filesystem::read_symlink("/proc/self/exe", error);
	return error ? std::filesystem::path {} : executable.parent_path();
#endif
}

std::filesystem::path HelperPath() {
#ifdef _WIN32
	const wchar_t* override_path = _wgetenv(L"KYTY_PKG_HELPER");
#else
	const char* override_path = std::getenv("KYTY_PKG_HELPER");
#endif
	if (override_path != nullptr && *override_path != 0) {
		const std::filesystem::path helper(override_path);
		if (!std::filesystem::is_regular_file(helper))
			throw std::runtime_error("KYTY_PKG_HELPER points to a missing file");
		return helper;
	}
	const auto directory = ExecutableDirectory();
	if (!directory.empty()) {
		// The parent location also supports the build-tree launcher subdirectory.
		for (const auto& root: {directory, directory.parent_path()}) {
			const auto helper = root / "pkg" / "KytyPkgMount.dll";
			if (std::filesystem::is_regular_file(helper)) return helper;
		}
	}
	throw std::runtime_error(
	    "PKG reader missing: build/install the pkg helper, or set KYTY_PKG_HELPER");
}

class Pipe {
public:
	Pipe()                       = default;
	Pipe(const Pipe&)            = delete;
	Pipe& operator=(const Pipe&) = delete;
	~Pipe() { Close(); }
	void Open(const std::filesystem::path& package,
	          std::chrono::milliseconds    startup_timeout = std::chrono::seconds(60)) {
		const auto helper = HelperPath();
		// Cover creation, spawn and closing the child ends: another PKG helper
		// must never inherit these temporarily inheritable pipe handles.
		static std::mutex spawn_mutex;
		std::lock_guard   spawn_lock(spawn_mutex);
		deadline_        = std::chrono::steady_clock::now() + startup_timeout;
		deadline_active_ = true;
#ifdef _WIN32
		// Windows filenames cannot contain a double quote. Explicit quoted arguments,
		// not cmd.exe/PowerShell, carry native Unicode package/helper filenames.
		auto quote = [](const std::wstring& value) {
			if (value.find(L'"') != std::wstring::npos)
				throw std::runtime_error("Invalid quote in filename");
			return L"\"" + value + L"\"";
		};
		std::wstring command = L"dotnet " + quote(helper.native()) + L" " + quote(package.native());
		SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
		HANDLE              input_read = nullptr, output_write = nullptr;
		auto                fail = [&] {
			if (input_read) CloseHandle(input_read);
			if (output_write) CloseHandle(output_write);
			Close();
			throw std::runtime_error(
			    "Unable to start package helper; check .NET 10 and KYTY_PKG_HELPER");
		};
		// An overlapped parent write handle lets a blocked request obey its deadline.
		static uint64_t pipe_serial = 0; // Protected by spawn_mutex.
		const auto pipe_name = L"\\\\.\\pipe\\kyty-pkg-" + std::to_wstring(GetCurrentProcessId()) +
		                       L"-" + std::to_wstring(++pipe_serial);
		input_write_         = CreateNamedPipeW(
		    pipe_name.c_str(),
		    PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
		    PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
		if (input_write_ == INVALID_HANDLE_VALUE) {
			input_write_ = nullptr;
			fail();
		}
		input_read =
		    CreateFileW(pipe_name.c_str(), GENERIC_READ, 0, &attributes, OPEN_EXISTING, 0, nullptr);
		if (input_read == INVALID_HANDLE_VALUE) {
			input_read = nullptr;
			fail();
		}
		// Wake on actual response data, not on a timer polling the child process.
		const auto response_name = pipe_name + L"-response";
		output_read_             = CreateNamedPipeW(
		    response_name.c_str(),
		    PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
		    PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
		if (output_read_ == INVALID_HANDLE_VALUE) {
			output_read_ = nullptr;
			fail();
		}
		output_write = CreateFileW(response_name.c_str(), GENERIC_WRITE, 0, &attributes,
		                           OPEN_EXISTING, 0, nullptr);
		if (output_write == INVALID_HANDLE_VALUE) {
			output_write = nullptr;
			fail();
		}
		STARTUPINFOW startup {};
		startup.cb         = sizeof(startup);
		startup.dwFlags    = STARTF_USESTDHANDLES;
		startup.hStdInput  = input_read;
		startup.hStdOutput = output_write;
		startup.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION process {};
		if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
		                    nullptr, nullptr, &startup, &process))
			fail();
		process_ = process.hProcess;
		CloseHandle(process.hThread);
		CloseHandle(input_read);
		CloseHandle(output_write);
#else
		// Build arguments before fork: the child uses only async-signal-safe calls.
		const std::string helper_arg = helper.string(), package_arg = package.string();
		int               input[2], output[2];
		if (pipe(input) != 0) throw std::runtime_error("Cannot create helper input pipe");
		if (pipe(output) != 0) {
			close(input[0]);
			close(input[1]);
			throw std::runtime_error("Cannot create helper output pipe");
		}
		for (int fd: {input[0], input[1], output[0], output[1]}) {
			if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
				for (int end: {input[0], input[1], output[0], output[1]})
					close(end);
				throw std::runtime_error("Cannot protect helper pipe inheritance");
			}
		}
		process_ = fork();
		if (process_ < 0) {
			for (int fd: {input[0], input[1], output[0], output[1]})
				close(fd);
			throw std::runtime_error("Cannot fork package helper");
		}
		if (process_ == 0) {
			close(input[1]);
			close(output[0]);
			if (dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0) _exit(127);
			if (input[0] != STDIN_FILENO) close(input[0]);
			if (output[1] != STDOUT_FILENO) close(output[1]);
			fcntl(STDIN_FILENO, F_SETFD, 0);
			fcntl(STDOUT_FILENO, F_SETFD, 0);
			execlp("dotnet", "dotnet", helper_arg.c_str(), package_arg.c_str(),
			       static_cast<char*>(nullptr));
			_exit(127);
		}
		close(input[0]);
		close(output[1]);
		if (fcntl(input[1], F_SETFL, O_NONBLOCK) < 0) {
			close(input[1]);
			close(output[0]);
			Close();
			throw std::runtime_error("Cannot make helper requests nonblocking");
		}
		input_write_ = input[1];
		output_read_ = output[0];
#endif
	}
	void Close() noexcept {
#ifdef _WIN32
		if (input_write_) {
			CloseHandle(input_write_);
			input_write_ = nullptr;
		}
		if (output_read_) {
			CloseHandle(output_read_);
			output_read_ = nullptr;
		}
		if (process_) {
			if (WaitForSingleObject(process_, 1000) == WAIT_TIMEOUT) TerminateProcess(process_, 2);
			CloseHandle(process_);
			process_ = nullptr;
		}
#else
		if (input_write_ >= 0) {
			close(input_write_);
			input_write_ = -1;
		}
		if (output_read_ >= 0) {
			close(output_read_);
			output_read_ = -1;
		}
		if (process_ > 0) {
			int status = 0;
			// Closing the stream normally ends the helper. If it is still busy,
			// terminate only the child process we spawned and reap it.
			pid_t result;
			do {
				result = waitpid(process_, &status, WNOHANG);
			} while (result < 0 && errno == EINTR);
			if (result == 0) {
				// A stalled helper can ignore SIGTERM. Force termination so cleanup
				// cannot defeat the startup deadline.
				kill(process_, SIGKILL);
				while (waitpid(process_, &status, 0) < 0 && errno == EINTR) {
				}
			}
			process_ = -1;
		}
#endif
	}
	void Write(const void* data, size_t size) {
		auto* p = static_cast<const uint8_t*>(data);
		while (size != 0) {
#ifdef _WIN32
			CheckDeadline();
			OVERLAPPED operation {};
			operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!operation.hEvent) throw std::runtime_error("Cannot create helper write event");
			DWORD wrote = 0;
			bool ok = WriteFile(input_write_, p, static_cast<DWORD>(size), &wrote, &operation) != 0;
			if (!ok && GetLastError() == ERROR_IO_PENDING) {
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
				                           deadline_ - std::chrono::steady_clock::now())
				                           .count();
				const auto state     = WaitForSingleObject(
				    operation.hEvent, static_cast<DWORD>(std::clamp<int64_t>(remaining, 0, 60000)));
				if (state != WAIT_OBJECT_0) {
					CancelIoEx(input_write_, &operation);
					// Cancellation must finish before the OVERLAPPED or buffer goes out of scope.
					GetOverlappedResult(input_write_, &operation, &wrote, TRUE);
					CloseHandle(operation.hEvent);
					throw std::runtime_error("Package helper request timed out or wait failed");
				}
				ok = GetOverlappedResult(input_write_, &operation, &wrote, FALSE) != 0;
			}
			CloseHandle(operation.hEvent);
			if (!ok || wrote == 0) throw std::runtime_error("Package helper write failed");
#else
			CheckDeadline();
			pollfd     writable {input_write_, POLLOUT, 0};
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			                           deadline_ - std::chrono::steady_clock::now())
			                           .count();
			const int  ready =
			    poll(&writable, 1, static_cast<int>(std::clamp<int64_t>(remaining, 1, 1000)));
			if (ready < 0 && errno == EINTR) continue;
			if (ready < 0) throw std::runtime_error("Cannot poll helper request");
			if (ready == 0) continue;
			// Suppress only this thread's newly generated SIGPIPE, preserving the caller's signal
			// state.
			sigset_t blocked, previous, pending;
			sigemptyset(&blocked);
			sigaddset(&blocked, SIGPIPE);
			if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0)
				throw std::runtime_error("Cannot protect package pipe write");
			sigpending(&pending);
			const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
			const auto wrote           = write(input_write_, p, size);
			const int  saved_errno     = errno;
			if (wrote < 0 && saved_errno == EPIPE && !already_pending) {
				sigpending(&pending);
				if (sigismember(&pending, SIGPIPE) == 1) {
					int signal_number = 0;
					sigwait(&blocked, &signal_number);
				}
			}
			pthread_sigmask(SIG_SETMASK, &previous, nullptr);
			errno = saved_errno;
			if (wrote < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
			if (wrote <= 0) throw std::runtime_error("Package helper write failed");
#endif
			p += wrote;
			size -= static_cast<size_t>(wrote);
		}
	}
	// A single deadline covers the complete request and response, including partial I/O.
	void BeginTransaction(std::chrono::milliseconds timeout) {
		deadline_        = std::chrono::steady_clock::now() + timeout;
		deadline_active_ = true;
	}
	void FinishStartup() { deadline_active_ = false; }
	void Read(void* data, size_t size) {
		auto* p = static_cast<uint8_t*>(data);
		while (size != 0) {
#ifdef _WIN32
			CheckDeadline();
			OVERLAPPED operation {};
			operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!operation.hEvent) throw std::runtime_error("Cannot create helper read event");
			DWORD got = 0;
			bool  ok  = ReadFile(output_read_, p, static_cast<DWORD>(size), &got, &operation) != 0;
			if (!ok && GetLastError() == ERROR_IO_PENDING) {
				const auto  remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
				                            deadline_ - std::chrono::steady_clock::now())
				                            .count();
				const DWORD wait_ms =
				    deadline_active_ ? static_cast<DWORD>(std::clamp<int64_t>(remaining, 0, 60000))
					                 : INFINITE;
				const auto state = WaitForSingleObject(operation.hEvent, wait_ms);
				if (state != WAIT_OBJECT_0) {
					CancelIoEx(output_read_, &operation);
					// Drain cancellation before releasing the operation and caller's buffer.
					GetOverlappedResult(output_read_, &operation, &got, TRUE);
					CloseHandle(operation.hEvent);
					throw std::runtime_error("Package helper response timed out or wait failed");
				}
				ok = GetOverlappedResult(output_read_, &operation, &got, FALSE) != 0;
			}
			CloseHandle(operation.hEvent);
			if (!ok || got == 0) throw std::runtime_error("Package helper closed the stream");
#else
			if (deadline_active_) {
				CheckDeadline();
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
				                           deadline_ - std::chrono::steady_clock::now())
				                           .count();
				pollfd     ready {output_read_, POLLIN, 0};
				const int  result =
				    poll(&ready, 1, static_cast<int>(std::clamp<int64_t>(remaining, 1, 1000)));
				if (result < 0 && errno == EINTR) continue;
				if (result < 0) throw std::runtime_error("Cannot poll package helper");
				if (result == 0) continue;
			}
			const auto got = read(output_read_, p, size);
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) throw std::runtime_error("Package helper closed the stream");
#endif
			p += got;
			size -= static_cast<size_t>(got);
		}
	}
	uint64_t Number(size_t bytes) {
		std::array<uint8_t, 8> data {};
		Read(data.data(), bytes);
		uint64_t value = 0;
		for (size_t i = 0; i < bytes; ++i)
			value |= static_cast<uint64_t>(data[i]) << (8 * i);
		return value;
	}
	void Number(uint64_t value, size_t bytes) {
		std::array<uint8_t, 8> data {};
		for (size_t i = 0; i < bytes; ++i)
			data[i] = static_cast<uint8_t>(value >> (8 * i));
		Write(data.data(), bytes);
	}
	std::string String(size_t limit) {
		const auto length = Number(4);
		if (length > limit) throw std::runtime_error("Package helper string exceeds limit");
		std::string value(static_cast<size_t>(length), '\0');
		Read(value.data(), value.size());
		return value;
	}

private:
	void CheckDeadline() {
		if (deadline_active_ && std::chrono::steady_clock::now() >= deadline_)
			throw std::runtime_error("Package helper transaction timed out");
	}
	bool                                  deadline_active_ = false;
	std::chrono::steady_clock::time_point deadline_;
#ifdef _WIN32
	HANDLE input_write_ = nullptr, output_read_ = nullptr, process_ = nullptr;
#else
	int   input_write_ = -1, output_read_ = -1;
	pid_t process_ = -1;
#endif
};

class PkgArchiveBackend final: public ArchiveReader {
public:
	explicit PkgArchiveBackend(const std::filesystem::path& package,
	                           std::chrono::milliseconds startup_timeout = std::chrono::seconds(60),
	                           std::chrono::milliseconds read_timeout    = std::chrono::seconds(60))
	    : read_timeout_(read_timeout) {
		pipe_.Open(package, startup_timeout);
		std::array<char, 4> magic {};
		pipe_.Read(magic.data(), magic.size());
		if (std::memcmp(magic.data(), "KPE1", 4) == 0)
			throw std::runtime_error(pipe_.String(65536));
		if (std::memcmp(magic.data(), "KPK1", 4) != 0)
			throw std::runtime_error("Invalid package-helper protocol");
		const auto count = pipe_.Number(4);
		if (count == 0 || count > 200000) throw std::runtime_error("Invalid package catalog size");
		size_t total_names = 0;
		for (uint64_t id = 0; id < count; ++id) {
			auto name = pipe_.String(4096);
			total_names += name.size();
			if (total_names > 32 * 1024 * 1024 || name.find('\0') != std::string::npos ||
			    name.find('\\') != std::string::npos)
				throw std::runtime_error("Package catalog names exceed limits");
			const auto size = pipe_.Number(8), kind = pipe_.Number(1);
			if (kind > 1 || !entries_.emplace(name, Entry {id, size, kind == 1}).second)
				throw std::runtime_error("Duplicate/invalid catalog entry");
		}
		auto root = entries_.find("");
		if (root == entries_.end() || root->second.is_file)
			throw std::runtime_error("Package catalog has no root");
		for (const auto& [path, entry]: entries_) {
			if (path.empty()) continue;
			auto split  = path.rfind('/');
			auto parent = split == std::string::npos ? std::string {} : path.substr(0, split);
			auto name   = split == std::string::npos ? path : path.substr(split + 1);
			if (name.empty() || name == "." || name == ".." || path.front() == '/')
				throw std::runtime_error("Invalid package member path");
			auto directory = entries_.find(parent);
			if (directory == entries_.end() || directory->second.is_file)
				throw std::runtime_error("Package member has no directory parent");
			children_[parent].push_back({name, entry.is_file});
		}
		pipe_.FinishStartup();
		std::fprintf(stderr, "PKG mounted read-only: %zu entries (on-demand reads)\n",
		             entries_.size());
	}
	std::optional<Entry> Find(std::string_view member) override {
		auto found = entries_.find(std::string(member));
		return found == entries_.end() ? std::nullopt : std::optional<Entry>(found->second);
	}
	std::vector<File::DirEntry> List(std::string_view member) override {
		auto found = children_.find(std::string(member));
		return found == children_.end() ? std::vector<File::DirEntry> {} : found->second;
	}
	uint64_t Read(uint64_t id, uint64_t offset, uint32_t size, void* data) override {
		if (size == 0) return 0;
		if (data == nullptr || size > 1024 * 1024) return 0;
		std::lock_guard lock(mutex_);
		if (failed_) return 0;
		try {
			pipe_.BeginTransaction(read_timeout_);
			pipe_.Number(1, 1);
			pipe_.Number(id, 8);
			pipe_.Number(offset, 8);
			pipe_.Number(size, 4);
			const auto got = pipe_.Number(4);
			if (got == std::numeric_limits<uint32_t>::max()) {
				std::fprintf(stderr, "PKG read failed: %s\n", pipe_.String(65536).c_str());
				return 0;
			}
			if (got > size) throw std::runtime_error("Oversized package read reply");
			pipe_.Read(data, static_cast<size_t>(got));
			return got;
		} catch (const std::exception& ex) {
			std::fprintf(stderr, "PKG reader failed: %s\n", ex.what());
			pipe_.Close();
			failed_ = true;
			return 0;
		}
	}

private:
	std::chrono::milliseconds                                    read_timeout_;
	bool                                                         failed_ = false;
	Pipe                                                         pipe_;
	std::mutex                                                   mutex_;
	std::map<std::string, Entry>                                 entries_;
	std::unordered_map<std::string, std::vector<File::DirEntry>> children_;
};
} // namespace
std::shared_ptr<ArchiveReader> OpenPkgArchive(const std::filesystem::path& path) {
	try {
		return std::make_shared<PkgArchiveBackend>(path);
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "Cannot mount PKG: %s\n", ex.what());
		return {};
	}
}
} // namespace Common
