#include "graphics/host_gpu/renderer/cache/resourceMutex.h"

#include "common/assert.h"

namespace Libs::Graphics {

ResourceMutex::~ResourceMutex() {
	std::lock_guard state(m_state);
	if (m_resource_owner != std::thread::id {}) {
		EXIT("ResourceMutex destroyed with an active owner\n");
	}
}

void ResourceMutex::lock() {
	const auto current = std::this_thread::get_id();
	{
		std::lock_guard state(m_state);
		if (m_resource_owner == current) {
			EXIT("recursive resource transaction\n");
		}
	}
	m_resource.lock();
	std::lock_guard state(m_state);
	if (m_resource_owner != std::thread::id {}) {
		EXIT("resource mutex acquired with a stale owner\n");
	}
	m_resource_owner = current;
}

void ResourceMutex::unlock() {
	const auto current = std::this_thread::get_id();
	{
		std::lock_guard state(m_state);
		if (m_resource_owner != current) {
			EXIT("resource transaction released without ownership\n");
		}
		m_resource_owner = {};
	}
	m_resource.unlock();
}

bool ResourceMutex::IsOwnedByCurrentThread() {
	std::lock_guard state(m_state);
	return m_resource_owner == std::this_thread::get_id();
}

} // namespace Libs::Graphics
