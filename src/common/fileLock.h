#ifndef KYTY_COMMON_FILELOCK_H_
#define KYTY_COMMON_FILELOCK_H_

#include "common/common.h"

#include <filesystem>

namespace Common {

// An exclusive lock on a file, between processes. The host releases it when the process ends,
// also when it stops abnormally, so a lock is never left behind.
class FileLock final {
public:
	FileLock() = default;
	~FileLock();
	KYTY_CLASS_NO_COPY(FileLock);

	// Takes the lock without waiting, creating the file and its folder when needed. False when
	// another process holds it or the file cannot be opened.
	[[nodiscard]] bool TryAcquire(const std::filesystem::path& path);

	[[nodiscard]] bool Held() const noexcept { return m_held; }

private:
	bool m_held = false;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	void* m_handle = nullptr;
#else
	int m_fd = -1;
#endif
};

} // namespace Common

#endif /* KYTY_COMMON_FILELOCK_H_ */
