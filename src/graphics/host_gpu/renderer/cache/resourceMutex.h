#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCEMUTEX_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCEMUTEX_H_

#include "common/common.h"

#include <mutex>
#include <thread>

namespace Libs::Graphics {

// Owner-tracked shared buffer/image transaction.
class ResourceMutex final {
public:
	ResourceMutex() = default;
	~ResourceMutex();
	KYTY_CLASS_NO_COPY(ResourceMutex);

	void               lock();
	void               unlock();
	[[nodiscard]] bool IsOwnedByCurrentThread();

private:
	std::mutex      m_resource;
	std::mutex      m_state;
	std::thread::id m_resource_owner;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCEMUTEX_H_
