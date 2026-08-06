#include "common/fatalLog.h"

#include "common/common.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Common {

namespace {

std::mutex  g_fatal_log_mutex;
std::string g_fatal_log_mirror_path;
char        g_exe_dir[512] {};
char        g_fatal_abs[560] {};
char        g_crash_last_abs[560] {};
char        g_heartbeat_abs[560] {};
volatile long g_paths_ready = 0;

void InitPathsOnce() {
	if (g_paths_ready != 0) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	char module_path[MAX_PATH] {};
	const DWORD n = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
	if (n > 0 && n < MAX_PATH) {
		char* slash = std::strrchr(module_path, '\\');
		if (slash != nullptr) {
			*(slash + 1) = '\0';
			std::snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", module_path);
			std::snprintf(g_fatal_abs, sizeof(g_fatal_abs), "%s_kyty_fatal.txt", g_exe_dir);
			std::snprintf(g_crash_last_abs, sizeof(g_crash_last_abs), "%s_kyty_crash_last.txt",
			              g_exe_dir);
			std::snprintf(g_heartbeat_abs, sizeof(g_heartbeat_abs), "%s_kyty_heartbeat.txt",
			              g_exe_dir);
			InterlockedExchange(&g_paths_ready, 1);
			return;
		}
	}
#endif
	std::snprintf(g_fatal_abs, sizeof(g_fatal_abs), "%s", "_kyty_fatal.txt");
	std::snprintf(g_crash_last_abs, sizeof(g_crash_last_abs), "%s", "_kyty_crash_last.txt");
	std::snprintf(g_heartbeat_abs, sizeof(g_heartbeat_abs), "%s", "_kyty_heartbeat.txt");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	InterlockedExchange(&g_paths_ready, 1);
#else
	g_paths_ready = 1;
#endif
}

void AppendFatalLine(const char* path, const char* message) {
	if (path == nullptr || path[0] == '\0' || message == nullptr) {
		return;
	}
	if (FILE* file = std::fopen(path, "a")) {
		std::fputs(message, file);
		std::fputc('\n', file);
		std::fclose(file);
	}
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// No CRT locks / mutex — safe from VEH / stack-overflow adjacent paths.
void WriteRawLine(const char* path, const char* message, bool truncate) {
	if (path == nullptr || path[0] == '\0' || message == nullptr) {
		return;
	}
	const DWORD access = truncate ? GENERIC_WRITE : FILE_APPEND_DATA;
	const DWORD creat  = truncate ? CREATE_ALWAYS : OPEN_ALWAYS;
	HANDLE      file =
	    CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, creat,
	                FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	DWORD written = 0;
	const DWORD len = static_cast<DWORD>(std::strlen(message));
	(void)WriteFile(file, message, len, &written, nullptr);
	(void)WriteFile(file, "\r\n", 2, &written, nullptr);
	FlushFileBuffers(file);
	CloseHandle(file);
}
#endif

} // namespace

void SetFatalLogMirrorPath(const char* path) {
	std::lock_guard lock(g_fatal_log_mutex);
	if (path != nullptr && path[0] != '\0') {
		g_fatal_log_mirror_path = path;
	} else {
		g_fatal_log_mirror_path.clear();
	}
}

void LogFatalToFile(const char* message) {
	if (message == nullptr) {
		return;
	}

	std::fputs(message, stderr);
	std::fputc('\n', stderr);
	std::fflush(stderr);

	std::lock_guard lock(g_fatal_log_mutex);
	InitPathsOnce();
	AppendFatalLine(g_fatal_abs, message);
	AppendFatalLine("_kyty_fatal.txt", message); // cwd fallback
	if (!g_fatal_log_mirror_path.empty()) {
		AppendFatalLine(g_fatal_log_mirror_path.c_str(), message);
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	WriteRawLine(g_crash_last_abs, message, /*truncate=*/false);
#endif
}

void EmergencyLogRaw(const char* message) {
	if (message == nullptr) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	InitPathsOnce();
	WriteRawLine(g_crash_last_abs, message, /*truncate=*/false);
	WriteRawLine(g_fatal_abs, message, /*truncate=*/false);
	// Best-effort console (may fail under hard faults).
	DWORD written = 0;
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	if (err != nullptr && err != INVALID_HANDLE_VALUE) {
		(void)WriteFile(err, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
		(void)WriteFile(err, "\r\n", 2, &written, nullptr);
		FlushFileBuffers(err);
	}
#else
	LogFatalToFile(message);
#endif
}

void HeartbeatLog(const char* message) {
	if (message == nullptr) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	InitPathsOnce();
	WriteRawLine(g_heartbeat_abs, message, /*truncate=*/true);
#else
	(void)message;
#endif
}

} // namespace Common
