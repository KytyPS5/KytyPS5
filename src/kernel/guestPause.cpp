#include "kernel/guestPause.h"

#include "common/singleton.h"
#include "kernel/pthread.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Libs::LibKernel {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

struct GuestPause::Impl {
	// The executable segments of the guest's programs, sorted.
	std::vector<std::pair<uint64_t, uint64_t>> code;
	// By host thread id.
	std::unordered_map<uint64_t, HANDLE> suspended;

	[[nodiscard]] bool InGuestCode(uint64_t address) const {
		const auto next = std::ranges::upper_bound(code, address, {}, &std::pair<uint64_t, uint64_t>::first);
		return next != code.begin() && address < std::prev(next)->second;
	}
};

GuestPause::GuestPause(): m_impl(std::make_unique<Impl>()) {
	// Taken before any thread is suspended: the linker's lock may be held by a guest thread.
	m_impl->code = Common::Singleton<Loader::RuntimeLinker>::Instance()->GetCodeRanges();
	std::ranges::sort(m_impl->code);
	m_impl->suspended.reserve(256);
}

GuestPause::~GuestPause() {
	for (const auto handle: m_impl->suspended | std::views::values) {
		::ResumeThread(handle);
		::CloseHandle(handle);
	}
}

bool GuestPause::Supported() noexcept {
	return true;
}

void GuestPause::Poll() {
	const auto self = static_cast<uint64_t>(::GetCurrentThreadId());
	for (const auto id: PthreadGuestHostThreadIds()) {
		if (id == self || m_impl->suspended.contains(id)) {
			continue;
		}
		HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, static_cast<DWORD>(id));
		if (thread == nullptr) {
			continue;
		}
		if (::SuspendThread(thread) == static_cast<DWORD>(-1)) {
			::CloseHandle(thread);
			continue;
		}
		// Returns once the thread has stopped. Nothing here takes a lock (nor allocates) while a
		// thread that may hold one is suspended: only threads in guest code stay suspended.
		CONTEXT context {};
		context.ContextFlags = CONTEXT_CONTROL;
		if (::GetThreadContext(thread, &context) != 0 && m_impl->InGuestCode(context.Rip)) {
			m_impl->suspended.emplace(id, thread);
			continue;
		}
		::ResumeThread(thread);
		::CloseHandle(thread);
	}
}

uint32_t GuestPause::Suspended() const noexcept {
	return static_cast<uint32_t>(m_impl->suspended.size());
}

#else

struct GuestPause::Impl {};

GuestPause::GuestPause(): m_impl(std::make_unique<Impl>()) {}

GuestPause::~GuestPause() = default;

bool GuestPause::Supported() noexcept {
	return false;
}

void GuestPause::Poll() {}

uint32_t GuestPause::Suspended() const noexcept {
	return 0;
}

#endif

} // namespace Libs::LibKernel
