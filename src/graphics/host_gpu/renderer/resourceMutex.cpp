#include "graphics/host_gpu/renderer/resourceMutex.h"

#include "common/assert.h"

namespace Libs::Graphics {

ResourceMutex::FaultScope::FaultScope(ResourceMutex& mutex): m_mutex(mutex) {
	m_resource_preowned = m_mutex.BeginFault();
}

ResourceMutex::FaultScope::~FaultScope() {
	m_mutex.EndFault(m_resource_preowned);
}

ResourceMutex::~ResourceMutex() {
	std::lock_guard state(m_state);
	if (m_resource_owner != std::thread::id {} || m_fault_owner != std::thread::id {}) {
		EXIT("ResourceMutex destroyed with an active owner or fault transaction\n");
	}
}

void ResourceMutex::lock() {
	const auto current = std::this_thread::get_id();
	{
		std::lock_guard state(m_state);
		if (m_resource_owner == current) {
			EXIT("recursive resource transaction\n");
		}
		if (m_fault_owner == current) {
			EXIT("resource transaction re-entered from a page-fault callback\n");
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

bool ResourceMutex::BeginFault() {
	const auto current = std::this_thread::get_id();
	{
		std::lock_guard state(m_state);
		if (m_fault_owner == current) {
			EXIT("nested resource page-fault transaction\n");
		}
		if (m_resource_owner == current) {
			m_fault_owner = current;
			return true;
		}
	}
	lock();
	std::lock_guard state(m_state);
	if (m_resource_owner != current || m_fault_owner != std::thread::id {}) {
		EXIT("resource fault transaction acquired inconsistent ownership\n");
	}
	m_fault_owner = current;
	return false;
}

void ResourceMutex::EndFault(bool resource_preowned) {
	const auto current = std::this_thread::get_id();
	{
		std::lock_guard state(m_state);
		if (m_fault_owner != current || m_resource_owner != current) {
			EXIT("resource page-fault transaction ended with inconsistent ownership\n");
		}
		m_fault_owner = {};
	}
	if (!resource_preowned) {
		unlock();
	}
}

} // namespace Libs::Graphics
