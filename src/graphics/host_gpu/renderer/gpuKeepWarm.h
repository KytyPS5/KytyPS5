#ifndef KYTY_GRAPHICS_HOST_GPU_RENDERER_GPUKEEPWARM_H_
#define KYTY_GRAPHICS_HOST_GPU_RENDERER_GPUKEEPWARM_H_

// KYTY_GPU_KEEPWARM (macOS, Apple silicon, opt-in): a light, continuous spin on the GPU through a native MTLCommandQueue of its own
// (gpuKeepWarm.mm), so that macOS keeps the GPU clock in the fast state. The guest's workload ping-pongs between the CPU and the GPU,
// and the DVFS heuristic of macOS then leaves the GPU in a low performance state (see Apple Developer Forums, thread 768160: the clock
// goes up when the command buffers of the queue are saturated). The game and its VkQueue are not touched.
// This header is the pure part (tests/GpuKeepWarmTests.cpp pins it):
//   KYTY_GPU_KEEPWARM=<K>     unset / 0 / empty / unknown = OFF (zero cost), K = threadgroups per dispatch, 1..16
//   KYTY_GPU_KEEPWARM_MS=<ms> length of one dispatch, default 1.5, 0.1..2 (watchdog)
// The spin pauses by itself when the guest has not submitted work for kIdleNs and resumes with the next submit.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::GpuKeepWarm {

constexpr uint32_t kMaxGroups   = 16;
constexpr uint32_t kMinInFlight = 2;
constexpr uint32_t kDefaultUs   = 1500;
constexpr uint32_t kMinUs       = 100;
constexpr uint32_t kMaxUs       = 2000;
constexpr uint64_t kIdleNs      = 250ull * 1000 * 1000; // no guest submit for this long -> the spin pauses

// Digits only; anything else is OFF. Above the maximum: clamped.
inline uint32_t ParseGroups(std::string_view text) {
	if (text.empty() || text.size() > 6) return 0;
	uint32_t value = 0;
	for (const char c: text) {
		if (c < '0' || c > '9') return 0;
		value = value * 10 + static_cast<uint32_t>(c - '0');
	}
	return value > kMaxGroups ? kMaxGroups : value;
}

// "1.5" -> 1500 us. Not a number / empty -> default; clamped to [kMinUs, kMaxUs].
inline uint32_t ParseDispatchUs(std::string_view text) {
	if (text.empty()) return kDefaultUs;
	uint64_t whole = 0, frac = 0, scale = 1;
	bool     dot = false, digit = false;
	for (const char c: text) {
		if (c == '.' && !dot) {
			dot = true;
		} else if (c >= '0' && c <= '9') {
			digit = true;
			if (!dot) {
				whole = whole * 10 + static_cast<uint64_t>(c - '0');
				if (whole > 100000) return kMaxUs;
			} else if (scale < 1000) {
				frac = frac * 10 + static_cast<uint64_t>(c - '0');
				scale *= 10;
			}
		} else {
			return kDefaultUs;
		}
	}
	if (!digit) return kDefaultUs;
	const uint64_t us = whole * 1000 + frac * (1000 / scale);
	return us < kMinUs ? kMinUs : (us > kMaxUs ? kMaxUs : static_cast<uint32_t>(us));
}

inline uint32_t GroupsFromEnv() {
	const char* value = std::getenv("KYTY_GPU_KEEPWARM");
	return value == nullptr ? 0 : ParseGroups(value);
}

inline uint32_t DispatchUsFromEnv() {
	const char* value = std::getenv("KYTY_GPU_KEEPWARM_MS");
	return value == nullptr ? kDefaultUs : ParseDispatchUs(value);
}

// ---- Eligibility: the spin runs only on the GPU the emulator renders on, and only on Apple silicon. ----

struct DeviceInfo {
	uint64_t    registry_id = 0; // MTLDevice.registryID
	std::string name;
};

// The 8 bytes of VkPhysicalDeviceIDProperties::deviceLUID. MoltenVK stores the MTLDevice registry id there; the build we looked at
// (Vulkan 1.1.357 instance) writes it big-endian (registry id 0x1000004d3 -> 00 00 00 01 00 00 04 d3), so both byte orders are accepted.
using Luid = std::array<uint8_t, 8>;

inline bool LuidIsRegistryId(const Luid& luid, uint64_t registry_id) {
	uint64_t little = 0, big = 0;
	for (size_t i = 0; i < 8; i++) {
		little |= static_cast<uint64_t>(luid[i]) << (8 * i);
		big = (big << 8) | luid[i];
	}
	return little == registry_id || big == registry_id;
}

// What the Vulkan side (MoltenVK) says about the device the emulator renders on.
struct WantedDevice {
	bool        has_luid = false; // VkPhysicalDeviceIDProperties::deviceLUIDValid
	Luid        luid {};
	std::string name; // VkPhysicalDeviceProperties::deviceName (MoltenVK: the MTLDevice name)
};

struct Eligibility {
	bool        ok     = false;
	size_t      index  = 0;       // into the list of Metal devices when ok
	const char* reason = nullptr; // when !ok
};

// apple_silicon: the host (not the process: the emulator runs translated by Rosetta) is an Apple silicon Mac.
// Matching order: the LUID = registry id (exact); else the name when exactly one Metal device has it; else the only Metal device.
inline Eligibility CheckEligibility(bool apple_silicon, const std::vector<DeviceInfo>& devices, const WantedDevice& wanted) {
	Eligibility r;
	if (!apple_silicon) {
		r.reason = "host is not Apple silicon";
		return r;
	}
	if (devices.empty()) {
		r.reason = "no Metal device";
		return r;
	}
	if (wanted.has_luid) {
		for (size_t i = 0; i < devices.size(); i++) {
			if (LuidIsRegistryId(wanted.luid, devices[i].registry_id)) {
				r.ok    = true;
				r.index = i;
				return r;
			}
		}
		r.reason = "no Metal device with the registry id of the Vulkan device";
		return r;
	}
	if (!wanted.name.empty()) {
		size_t hits = 0, last = 0;
		for (size_t i = 0; i < devices.size(); i++) {
			if (devices[i].name == wanted.name) {
				hits++;
				last = i;
			}
		}
		if (hits == 1) {
			r.ok    = true;
			r.index = last;
			return r;
		}
	}
	if (devices.size() == 1) {
		r.ok    = true;
		r.index = 0;
		return r;
	}
	r.reason = "several GPUs and the Vulkan device cannot be identified";
	return r;
}

// ---- Which command buffers are in flight. Thread-safe; the Metal completion handler calls OnCompleted() from its own thread and
// encodes as many buffers as it returns; the guest's submit path calls NoteActivity(). `now` is a monotonic clock in ns. ----
//
// States: running (kMinInFlight buffers in flight, replaced on completion) -> paused (no replacement; the buffers drain) when the last
// guest activity is older than kIdleNs -> running again at the first NoteActivity() (it returns the buffers to encode).
class InFlightPolicy {
public:
	// The first fill at start (the idle clock starts at `now`).
	uint32_t Fill(uint64_t now) {
		m_last_activity.store(now);
		std::lock_guard<std::mutex> lock(m_mutex);
		return TakeLocked();
	}

	// One buffer has finished: how many to encode to replace it (0 after Stop() and while paused).
	uint32_t OnCompleted(uint64_t now) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_in_flight > 0) m_in_flight--;
		if (!m_paused && !m_stopped && now - m_last_activity.load() >= kIdleNs) {
			m_paused      = true;
			m_paused_flag = true;
			m_pause_start = now;
			m_pauses++;
		}
		return TakeLocked();
	}

	// The guest submitted (or flipped). Cheap when running. When paused: resumes and returns the buffers to encode.
	// No lost wake-up: this stores the time and then reads the paused flag; OnCompleted() sets the flag and then (next time) reads the time
	// under the lock, so a pause and a submit cannot both miss each other.
	uint32_t NoteActivity(uint64_t now) {
		m_last_activity.store(now);
		if (!m_paused_flag.load()) return 0;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_paused || m_stopped) return 0;
		m_paused      = false;
		m_paused_flag = false;
		m_paused_ns += now - m_pause_start;
		m_resumes++;
		return TakeLocked();
	}

	void Stop() {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stopped = true;
	}
	bool     Stopped() const { std::lock_guard<std::mutex> lock(m_mutex); return m_stopped; }
	bool     Idle() const { std::lock_guard<std::mutex> lock(m_mutex); return m_in_flight == 0; }
	bool     Paused() const { std::lock_guard<std::mutex> lock(m_mutex); return m_paused; }
	uint32_t InFlight() const { std::lock_guard<std::mutex> lock(m_mutex); return m_in_flight; }
	uint64_t Pauses() const { std::lock_guard<std::mutex> lock(m_mutex); return m_pauses; }
	uint64_t Resumes() const { std::lock_guard<std::mutex> lock(m_mutex); return m_resumes; }
	// Total time spent paused, including a pause that is still going on at `now`.
	uint64_t PausedNs(uint64_t now) const {
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_paused_ns + (m_paused ? now - m_pause_start : 0);
	}

private:
	uint32_t TakeLocked() {
		if (m_stopped || m_paused) return 0;
		const uint32_t add = m_in_flight < kMinInFlight ? kMinInFlight - m_in_flight : 0;
		m_in_flight += add;
		return add;
	}

	mutable std::mutex    m_mutex;
	std::atomic<uint64_t> m_last_activity {0};
	std::atomic<bool>     m_paused_flag {false}; // mirror of m_paused for the lock-free fast path of NoteActivity()
	uint32_t              m_in_flight   = 0;
	bool                  m_stopped     = false;
	bool                  m_paused      = false;
	uint64_t              m_pause_start = 0;
	uint64_t              m_paused_ns   = 0;
	uint64_t              m_pauses      = 0;
	uint64_t              m_resumes     = 0;
};

// Counters of the spin (cumulative); logged at Stop().
struct Stats {
	uint64_t cb_n      = 0; // command buffers completed
	uint64_t gpu_ns    = 0; // sum of GPUEndTime - GPUStartTime
	uint64_t iters     = 0; // loop iterations of all threadgroups
	uint64_t pauses    = 0;
	uint64_t resumes   = 0;
	uint64_t paused_ns = 0;
};

// Starts the spin when `groups` != 0 and the eligibility check passes; on refusal returns false and sets `reason`.
// Stop() is idempotent and waits for the buffers in flight. NoteActivity() is called on every guest submit and flip.
#if defined(__APPLE__)
bool        Start(uint32_t groups, uint32_t dispatch_us, const WantedDevice& wanted, std::string& reason);
void        Stop();
Stats       GetStats();
const char* DeviceName(); // of the MTLDevice in use ("" before Start)
namespace detail {
inline std::atomic<bool> g_running {false};
void                     NoteActivitySlow();
} // namespace detail
inline void NoteActivity() {
	if (detail::g_running.load(std::memory_order_relaxed)) detail::NoteActivitySlow();
}
#else
inline bool Start(uint32_t, uint32_t, const WantedDevice&, std::string& reason) {
	reason = "not macOS";
	return false;
}
inline void        Stop() {}
inline Stats       GetStats() { return {}; }
inline const char* DeviceName() { return ""; }
inline void        NoteActivity() {}
#endif

} // namespace Libs::Graphics::GpuKeepWarm

#endif
