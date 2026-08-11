#ifndef KYTY_COMMON_SUBSYSTEMS_H_
#define KYTY_COMMON_SUBSYSTEMS_H_

#include "common/common.h"

#include <cstdio>
#include <vector>

namespace Common {

class Subsystems {
public:
	using Callback = void (*)();

	explicit Subsystems(bool print = false);
	~Subsystems();

	template <typename Lifecycle>
	void Initialize() {
		Lifecycle::initialize();
		m_active.push_back({ShutdownCallback<Lifecycle>(), EmergencyCallback<Lifecycle>()});
		if (m_print) {
			std::printf("Initialized: %s\n", Lifecycle::name);
		}
	}

	void Destroy();
	void EmergencyShutdown();

	static void EmergencyShutdownActive();

	KYTY_CLASS_NO_COPY(Subsystems);

private:
	template <typename Lifecycle>
	static consteval Callback ShutdownCallback() {
		if constexpr (requires { Lifecycle::shutdown; }) {
			return Lifecycle::shutdown;
		}
		return nullptr;
	}

	template <typename Lifecycle>
	static consteval Callback EmergencyCallback() {
		if constexpr (requires { Lifecycle::emergency_shutdown; }) {
			return Lifecycle::emergency_shutdown;
		}
		return nullptr;
	}

	struct Entry {
		Callback shutdown;
		Callback emergency_shutdown;
	};

	std::vector<Entry> m_active;
	bool               m_print;
};

} // namespace Common

#endif /* KYTY_COMMON_SUBSYSTEMS_H_ */
