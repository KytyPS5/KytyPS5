#include "common/subsystems.h"

#include <atomic>

namespace Common {

static Subsystems*       g_active_subsystems = nullptr;
static std::atomic<bool> g_exiting {false};

Subsystems::Subsystems(bool print): m_print(print) {
	m_active.reserve(16);
	g_active_subsystems = this;
}

Subsystems::~Subsystems() {
	Destroy();
	if (g_active_subsystems == this) {
		g_active_subsystems = nullptr;
	}
}

void Subsystems::Destroy() {
	for (auto it = m_active.rbegin(); it != m_active.rend(); ++it) {
		if (it->shutdown != nullptr) {
			it->shutdown();
		}
	}
	m_active.clear();
}

void Subsystems::EmergencyShutdown() {
	for (auto it = m_active.rbegin(); it != m_active.rend(); ++it) {
		if (it->emergency_shutdown != nullptr) {
			it->emergency_shutdown();
		}
	}
	m_active.clear();
}

void Subsystems::EmergencyShutdownActive() {
	if (g_exiting.exchange(true)) {
		return;
	}
	if (g_active_subsystems != nullptr) {
		g_active_subsystems->EmergencyShutdown();
	}
}

bool Subsystems::Exiting() {
	return g_exiting.load(std::memory_order_acquire);
}

} // namespace Common
