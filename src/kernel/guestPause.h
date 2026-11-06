#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_GUESTPAUSE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_GUESTPAUSE_H_

#include "common/common.h"

#include <cstdint>
#include <memory>

namespace Libs::LibKernel {

// Stops the guest's threads while the emulator does work the game must not compete with (the
// pipelines PipelineCache has the player wait for). A thread is only suspended while it runs the
// guest's own code, where it holds none of the emulator's locks: one inside the emulator (in a
// call, or waiting there) is left alone and suspended later, once it is back in guest code.
// Windows only; elsewhere nothing is stopped.
class GuestPause {
public:
	GuestPause();
	// Resumes the threads it suspended.
	~GuestPause();
	KYTY_CLASS_NO_COPY(GuestPause);

	[[nodiscard]] static bool Supported() noexcept;

	// Suspends the guest threads that run guest code now. Call it often while paused: threads
	// leave the emulator, and the guest may have started others.
	void Poll();
	[[nodiscard]] uint32_t Suspended() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::LibKernel

#endif // EMULATOR_INCLUDE_EMULATOR_KERNEL_GUESTPAUSE_H_
