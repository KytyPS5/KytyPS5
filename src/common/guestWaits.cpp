#include "common/guestWaits.h"

#include "common/logging/log.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace Common::GuestWaits {

namespace Detail {
std::atomic<bool> g_enabled {false};
} // namespace Detail

namespace {

constexpr size_t OBJECT_NAME_SIZE = 64;

// One per thread. Only the owning thread writes it; the dump thread reads it. Both sides take the
// spin lock, which is uncontended except during a dump.
struct Slot {
	std::atomic_flag lock = ATOMIC_FLAG_INIT;
	uint32_t         id   = 0;
	std::string      name;

	Kind        kind     = Kind::None;
	const void* object   = nullptr;
	uint64_t    caller   = 0;
	uint64_t    begin_ns = 0;
	char        object_name[OBJECT_NAME_SIZE] = {};

	Kind        last_kind       = Kind::None;
	const void* last_object     = nullptr;
	uint64_t    last_caller     = 0;
	uint64_t    end_ns          = 0;
	uint64_t    completed_waits = 0;
};

class SlotLock {
public:
	explicit SlotLock(Slot& slot): m_slot(slot) {
		while (m_slot.lock.test_and_set(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
	}
	~SlotLock() { m_slot.lock.clear(std::memory_order_release); }
	SlotLock(const SlotLock&)            = delete;
	SlotLock(SlotLock&&)                 = delete;
	SlotLock& operator=(const SlotLock&) = delete;
	SlotLock& operator=(SlotLock&&)      = delete;

private:
	Slot& m_slot;
};

struct Registry {
	std::mutex                         mutex;
	std::vector<std::shared_ptr<Slot>> slots;
	uint32_t                           next_id = 1;
};

// Never destroyed: guest threads and the dump thread may still touch it during process exit.
Registry& GetRegistry() {
	static auto* registry = new Registry;
	return *registry;
}

struct SlotHolder {
	std::shared_ptr<Slot> slot;

	SlotHolder() = default;
	~SlotHolder() {
		if (slot == nullptr) {
			return;
		}
		auto&            registry = GetRegistry();
		std::lock_guard  lock(registry.mutex);
		auto&            slots = registry.slots;
		slots.erase(std::remove(slots.begin(), slots.end(), slot), slots.end());
	}
	SlotHolder(const SlotHolder&)            = delete;
	SlotHolder(SlotHolder&&)                 = delete;
	SlotHolder& operator=(const SlotHolder&) = delete;
	SlotHolder& operator=(SlotHolder&&)      = delete;
};

thread_local SlotHolder t_holder;

Slot& CurrentSlot() {
	if (t_holder.slot == nullptr) {
		auto  slot     = std::make_shared<Slot>();
		auto& registry = GetRegistry();
		{
			std::lock_guard lock(registry.mutex);
			slot->id = registry.next_id++;
			registry.slots.push_back(slot);
		}
		t_holder.slot = std::move(slot);
	}
	return *t_holder.slot;
}

void CopyName(char (&dst)[OBJECT_NAME_SIZE], const char* src) {
	if (src == nullptr) {
		dst[0] = '\0';
		return;
	}
	const size_t length = strnlen(src, OBJECT_NAME_SIZE - 1);
	std::memcpy(dst, src, length);
	dst[length] = '\0';
}

void DumpLoop(uint32_t seconds) {
	const auto interval = std::chrono::seconds(seconds);
	const auto min_wait = static_cast<uint64_t>(seconds) * 1000000000ull;
	for (;;) {
		std::this_thread::sleep_for(interval);
		const auto text = Format(Snapshot(NowNs()), min_wait);
		LOGF("%s", text.c_str());
	}
}

} // namespace

const char* KindName(Kind kind) {
	switch (kind) {
		case Kind::None: return "none";
		case Kind::Mutex: return "mutex";
		case Kind::Rwlock: return "rwlock";
		case Kind::Cond: return "cond";
		case Kind::Join: return "join";
		case Kind::Equeue: return "equeue";
		case Kind::Sema: return "sema";
		case Kind::EventFlag: return "event_flag";
		case Kind::Address: return "address";
		case Kind::Aio: return "aio";
		case Kind::Ampr: return "ampr";
		case Kind::Sleep: return "sleep";
	}
	return "unknown";
}

void SetEnabled(bool enabled) {
	Detail::g_enabled.store(enabled, std::memory_order_relaxed);
}

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

bool BeginWait(Kind kind, const void* object, const char* object_name, uint64_t caller) {
	auto&    slot = CurrentSlot();
	SlotLock lock(slot);
	if (slot.kind != Kind::None) {
		return false;
	}
	slot.kind     = kind;
	slot.object   = object;
	slot.caller   = caller;
	slot.begin_ns = NowNs();
	CopyName(slot.object_name, object_name);
	return true;
}

void EndWait() {
	auto&    slot = CurrentSlot();
	SlotLock lock(slot);
	if (slot.kind == Kind::None) {
		return;
	}
	slot.last_kind   = slot.kind;
	slot.last_object = slot.object;
	slot.last_caller = slot.caller;
	slot.end_ns      = NowNs();
	slot.completed_waits++;
	slot.kind           = Kind::None;
	slot.object         = nullptr;
	slot.caller         = 0;
	slot.object_name[0] = '\0';
}

void SetCurrentThreadName(const char* name) {
	if (!IsEnabled()) {
		return;
	}
	auto&    slot = CurrentSlot();
	SlotLock lock(slot);
	slot.name = (name != nullptr ? name : "");
}

std::vector<ThreadState> Snapshot(uint64_t now_ns) {
	std::vector<std::shared_ptr<Slot>> slots;
	{
		auto&           registry = GetRegistry();
		std::lock_guard lock(registry.mutex);
		slots = registry.slots;
	}

	std::vector<ThreadState> states;
	states.reserve(slots.size());
	for (const auto& slot: slots) {
		SlotLock    lock(*slot);
		ThreadState state;
		state.name        = slot->name;
		state.id          = slot->id;
		state.kind        = slot->kind;
		state.object      = reinterpret_cast<uint64_t>(slot->object);
		state.object_name = slot->object_name;
		state.caller      = slot->caller;
		if (slot->kind != Kind::None && now_ns > slot->begin_ns) {
			state.waiting_ns = now_ns - slot->begin_ns;
		}
		state.last_kind   = slot->last_kind;
		state.last_object = reinterpret_cast<uint64_t>(slot->last_object);
		state.last_caller = slot->last_caller;
		if (slot->completed_waits != 0 && now_ns > slot->end_ns) {
			state.since_last_ns = now_ns - slot->end_ns;
		}
		state.completed_waits = slot->completed_waits;
		states.push_back(std::move(state));
	}

	std::sort(states.begin(), states.end(),
	          [](const ThreadState& a, const ThreadState& b) { return a.id < b.id; });
	return states;
}

std::string Format(const std::vector<ThreadState>& states, uint64_t min_wait_ns) {
	size_t long_waits = 0;
	for (const auto& state: states) {
		if (state.kind != Kind::None && state.waiting_ns >= min_wait_ns) {
			long_waits++;
		}
	}

	std::string text;
	char        line[512];
	std::snprintf(line, sizeof(line),
	              "GuestWaits: %zu threads, %zu blocked for at least %.3f s\n", states.size(),
	              long_waits, static_cast<double>(min_wait_ns) / 1e9);
	text += line;

	for (const auto& state: states) {
		const char* name = state.name.empty() ? "<unnamed>" : state.name.c_str();
		if (state.kind != Kind::None) {
			std::snprintf(line, sizeof(line),
			              "\t%s [%u]: %s %s '%s' object=0x%016" PRIx64 " caller=0x%016" PRIx64
			              " for %.3f s",
			              name, state.id, state.waiting_ns >= min_wait_ns ? "BLOCKED" : "waiting",
			              KindName(state.kind), state.object_name.c_str(), state.object,
			              state.caller, static_cast<double>(state.waiting_ns) / 1e9);
		} else {
			std::snprintf(line, sizeof(line), "\t%s [%u]: running", name, state.id);
		}
		text += line;

		if (state.completed_waits != 0) {
			std::snprintf(line, sizeof(line),
			              "; completed=%" PRIu64 " last=%s object=0x%016" PRIx64
			              " caller=0x%016" PRIx64 " ended %.3f s ago",
			              state.completed_waits, KindName(state.last_kind), state.last_object,
			              state.last_caller, static_cast<double>(state.since_last_ns) / 1e9);
			text += line;
		}
		text += "\n";
	}
	return text;
}

uint32_t ParseDumpSeconds(const char* value) {
	if (value == nullptr || *value == '\0') {
		return 0;
	}
	char*      end    = nullptr;
	const auto parsed = std::strtoul(value, &end, 10);
	if (end == value || *end != '\0' || parsed == 0 || parsed > 3600) {
		return 0;
	}
	return static_cast<uint32_t>(parsed);
}

void InitFromEnvironment() {
	const uint32_t seconds = ParseDumpSeconds(std::getenv("KYTY_GUEST_WAIT_DUMP_SECONDS"));
	if (seconds == 0) {
		return;
	}
	SetEnabled(true);
	LOGF("GuestWaits: enabled, dumping every %u s\n", seconds);
	std::thread(DumpLoop, seconds).detach();
}

} // namespace Common::GuestWaits
