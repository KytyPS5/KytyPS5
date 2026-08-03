#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCEMUTEX_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCEMUTEX_H_

#include "common/common.h"

#include <mutex>
#include <thread>

namespace Libs::Graphics {

// Owner-tracked shared buffer/image transaction. External faults pause GPU submissions first;
// command-processor faults drain pending guest processors before entering this transaction.
class ResourceMutex final {
public:
	class FaultScope final {
	public:
		explicit FaultScope(ResourceMutex& mutex);
		~FaultScope();
		KYTY_CLASS_NO_COPY(FaultScope);

	private:
		ResourceMutex& m_mutex;
		bool           m_resource_preowned = false;
	};

	ResourceMutex() = default;
	~ResourceMutex();
	KYTY_CLASS_NO_COPY(ResourceMutex);

	void               lock();
	void               unlock();
	[[nodiscard]] bool IsOwnedByCurrentThread();

private:
	friend class FaultScope;

	[[nodiscard]] bool BeginFault();
	void               EndFault(bool resource_preowned);

	std::mutex      m_resource;
	std::mutex      m_state;
	std::thread::id m_resource_owner;
	std::thread::id m_fault_owner;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RESOURCEMUTEX_H_
