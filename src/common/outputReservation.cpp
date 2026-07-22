#include "common/outputReservation.h"

#include "common/file.h"

#include <algorithm>
#include <cctype>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // IWYU pragma: keep
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Common {

struct OutputReservation::Private {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int descriptor = -1;
#endif
};

OutputReservation::OutputReservation(): m_private(std::make_unique<Private>()) {}

OutputReservation::~OutputReservation() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (m_private->handle != INVALID_HANDLE_VALUE) {
		CloseHandle(m_private->handle);
	}
#else
	if (m_private->descriptor >= 0) {
		flock(m_private->descriptor, LOCK_UN);
		close(m_private->descriptor);
	}
#endif
}

std::filesystem::path OutputReservation::LockPath(const std::filesystem::path& output) {
	auto path = output;
	path += ".kyty-lock";
	return path;
}

bool OutputReservation::IsReservedPath(const std::filesystem::path& output) {
	auto filename = output.filename().generic_string();
	std::transform(filename.begin(), filename.end(), filename.begin(),
	               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	return filename.ends_with(".kyty-lock");
}

std::unique_ptr<OutputReservation> OutputReservation::Acquire(
    const std::filesystem::path& output, std::string& error) {
	error.clear();
	if (output.empty()) {
		error = "output path is empty";
		return {};
	}
	if (IsReservedPath(output)) {
		error = "output path uses the reserved .kyty-lock suffix";
		return {};
	}
	const auto lock = LockPath(output);
	if (!lock.parent_path().empty() && !File::CreateDirectories(lock.parent_path())) {
		error = "cannot create output reservation directory";
		return {};
	}
	auto reservation = std::unique_ptr<OutputReservation>(new OutputReservation());
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	reservation->m_private->handle =
	    CreateFileW(lock.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
	                FILE_ATTRIBUTE_NORMAL, nullptr);
	if (reservation->m_private->handle == INVALID_HANDLE_VALUE) {
		error = "output is already in use";
		return {};
	}
#else
	reservation->m_private->descriptor =
	    open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
	if (reservation->m_private->descriptor < 0 ||
	    flock(reservation->m_private->descriptor, LOCK_EX | LOCK_NB) != 0) {
		error = "output is already in use";
		return {};
	}
#endif
	return reservation;
}

} // namespace Common
