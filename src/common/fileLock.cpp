#include "common/fileLock.h"

#include <system_error>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Common {

FileLock::~FileLock() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (m_handle != nullptr) {
		CloseHandle(static_cast<HANDLE>(m_handle));
	}
#else
	if (m_fd >= 0) {
		// Closing the descriptor releases the lock.
		close(m_fd);
	}
#endif
}

bool FileLock::TryAcquire(const std::filesystem::path& path) {
	if (m_held) {
		return true;
	}
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// No sharing: a second process cannot open the file while this handle is open.
	HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
	                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	m_handle = handle;
#else
	const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		return false;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		close(fd);
		return false;
	}
	m_fd = fd;
#endif
	m_held = true;
	return true;
}

} // namespace Common
