#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs {

LIB_VERSION("Rudp", 1, "Rudp", 1, 1);

namespace Rudp {

using EventHandler = int(KYTY_SYSV_ABI*)(int, int, const uint8_t*, size_t, const void*, uint32_t,
                                         void*);
using ContextEventHandler = void(KYTY_SYSV_ABI*)(int, int, int, void*);

constexpr int ERR_NOT_INIT       = static_cast<int>(0x80770001u);
constexpr int ERR_ALREADY_INIT   = static_cast<int>(0x80770002u);
constexpr int ERR_INVALID_CTX    = static_cast<int>(0x80770003u);
constexpr int ERR_INVALID_ARG    = static_cast<int>(0x80770004u);
constexpr int ERR_INVALID_OPTION = static_cast<int>(0x80770005u);
constexpr int ERR_NOT_ACCEPTABLE = static_cast<int>(0x8077000du);
constexpr int ERR_THREAD_IN_USE  = static_cast<int>(0x80770010u);
constexpr int ERR_CANCELLED      = static_cast<int>(0x80770014u);
constexpr int ERR_WOULD_BLOCK    = static_cast<int>(0x80770016u);
constexpr int ERR_BUFFER_SMALL   = static_cast<int>(0x8077001au);
constexpr int ERR_ALREADY_BOUND  = static_cast<int>(0x8077001du);
constexpr int ERR_ALREADY_EXISTS = static_cast<int>(0x8077001eu);
constexpr int ERR_INVALID_POLL   = static_cast<int>(0x8077001fu);
constexpr int ERR_IN_PROGRESS    = static_cast<int>(0x80770021u);
constexpr int ERR_NO_HANDLER     = static_cast<int>(0x80770022u);
constexpr int ERR_END_OF_DATA    = static_cast<int>(0x80770024u);
constexpr int ERR_ESTABLISHED    = static_cast<int>(0x80770025u);

constexpr uint16_t POLL_READ   = 0x0001;
constexpr uint16_t POLL_WRITE  = 0x0002;
constexpr uint16_t POLL_FLUSH  = 0x0004;
constexpr uint16_t POLL_ERROR  = 0x0008;
constexpr size_t   BUFFER_SIZE = 65536;

struct ReadInfo {
	uint8_t  size;
	uint8_t  retransmission_count;
	uint16_t retransmission_delay;
	uint8_t  retransmission_delay_high;
	uint8_t  flags;
	uint16_t sequence_number;
	uint32_t timestamp;
};
static_assert(sizeof(ReadInfo) == 12);

struct PollEvent {
	int      context_id;
	uint16_t requested_events;
	uint16_t returned_events;
};
static_assert(sizeof(PollEvent) == 8);

struct ContextStatus {
	uint32_t state;
	int      parent_id;
	uint32_t children;
	uint32_t lost_packets;
	uint32_t sent_packets;
	uint32_t received_packets;
	uint64_t sent_bytes;
	uint64_t received_bytes;
	uint32_t retransmissions;
	uint32_t rtt;
};
static_assert(sizeof(ContextStatus) == 48);

struct Message {
	std::vector<uint8_t> data;
	uint8_t              flags     = 0;
	uint16_t             sequence  = 0;
	uint32_t             timestamp = 0;
};

struct Context {
	int                                           id            = 0;
	ContextEventHandler                           handler       = nullptr;
	void*                                         handler_arg   = nullptr;
	int                                           socket        = -1;
	uint16_t                                      vport         = 0;
	uint8_t                                       mux_mode      = 0;
	bool                                          bound         = false;
	bool                                          active        = false;
	bool                                          established   = false;
	bool                                          peer_closed   = false;
	int                                           peer_id       = 0;
	int                                           last_error    = OK;
	uint16_t                                      next_sequence = 0;
	uint32_t                                      sent_packets  = 0;
	uint32_t                                      recv_packets  = 0;
	uint64_t                                      sent_bytes    = 0;
	uint64_t                                      recv_bytes    = 0;
	std::deque<Message>                           messages;
	std::unordered_map<int, std::vector<uint8_t>> options;
};

struct Poll {
	std::unordered_map<int, uint16_t> contexts;
	bool                              cancelled = false;
};

struct PendingEvent {
	ContextEventHandler handler;
	int                 context_id;
	int                 event_id;
	int                 error;
	void*               arg;
};

static std::mutex                       g_mutex;
static std::condition_variable          g_cv;
static bool                             g_initialized   = false;
static bool                             g_internal_io   = false;
static EventHandler                     g_event_handler = nullptr;
static void*                            g_event_arg     = nullptr;
static int                              g_next_context  = 1;
static int                              g_next_poll     = 1;
static std::unordered_map<int, Context> g_contexts;
static std::unordered_map<int, Poll>    g_polls;
static std::vector<PendingEvent>        g_events;

static Context* GetContext(int id) {
	const auto it = g_contexts.find(id);
	return it == g_contexts.end() ? nullptr : &it->second;
}

static void QueueEvent(const Context& context, int event_id, int error = OK) {
	if (context.handler != nullptr) {
		g_events.push_back({context.handler, context.id, event_id, error, context.handler_arg});
	}
}

static std::vector<PendingEvent> TakeInternalEvents() {
	std::vector<PendingEvent> result;
	if (g_internal_io) {
		result.swap(g_events);
	}
	return result;
}

static void Dispatch(std::vector<PendingEvent> events) {
	for (const auto& event: events) {
		event.handler(event.context_id, event.event_id, event.error, event.arg);
	}
}

static int Connect(Context* context, uint16_t peer_vport) {
	if (!context->bound) {
		return ERR_NOT_ACCEPTABLE;
	}
	if (context->established) {
		return ERR_ESTABLISHED;
	}
	Context* peer = nullptr;
	for (auto& [id, candidate]: g_contexts) {
		if (id != context->id && candidate.bound && !candidate.established &&
		    (peer_vport == 0 || candidate.vport == peer_vport)) {
			peer = &candidate;
			if (candidate.active) {
				break;
			}
		}
	}
	if (peer == nullptr) {
		context->last_error = ERR_IN_PROGRESS;
		return ERR_IN_PROGRESS;
	}
	context->peer_id     = peer->id;
	peer->peer_id        = context->id;
	context->established = peer->established = true;
	context->peer_closed = peer->peer_closed = false;
	context->last_error = peer->last_error = OK;
	QueueEvent(*context, 2);
	QueueEvent(*context, 4);
	QueueEvent(*peer, 2);
	QueueEvent(*peer, 4);
	g_cv.notify_all();
	return OK;
}

static KYTY_SYSV_ABI int Init(void* memory, int size) {
	PRINT_NAME();
	if (memory == nullptr || size <= 0) {
		return ERR_INVALID_ARG;
	}
	std::scoped_lock lock(g_mutex);
	if (g_initialized) {
		return ERR_ALREADY_INIT;
	}
	g_initialized  = true;
	g_internal_io  = false;
	g_next_context = 1;
	g_next_poll    = 1;
	return OK;
}

static KYTY_SYSV_ABI int End() {
	PRINT_NAME();
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	g_initialized   = false;
	g_internal_io   = false;
	g_event_handler = nullptr;
	g_event_arg     = nullptr;
	g_contexts.clear();
	g_polls.clear();
	g_events.clear();
	g_cv.notify_all();
	return OK;
}

static KYTY_SYSV_ABI int EnableInternalIOThread(uint32_t stack_size, uint32_t priority) {
	PRINT_NAME();
	(void)stack_size;
	(void)priority;
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	if (g_internal_io) {
		return ERR_THREAD_IN_USE;
	}
	if (!g_contexts.empty()) {
		return ERR_NOT_ACCEPTABLE;
	}
	g_internal_io = true;
	return OK;
}

static KYTY_SYSV_ABI int SetEventHandler(EventHandler handler, void* arg) {
	PRINT_NAME();
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	if (handler == nullptr) {
		return ERR_NO_HANDLER;
	}
	g_event_handler = handler;
	g_event_arg     = arg;
	return OK;
}

static KYTY_SYSV_ABI int ProcessEvents(uint64_t timeout) {
	PRINT_NAME();
	std::vector<PendingEvent> events;
	{
		std::unique_lock lock(g_mutex);
		if (!g_initialized) {
			return ERR_NOT_INIT;
		}
		if (g_internal_io) {
			return ERR_THREAD_IN_USE;
		}
		if (g_events.empty() && timeout != 0) {
			g_cv.wait_for(lock, std::chrono::microseconds(timeout),
			              [] { return !g_events.empty() || !g_initialized; });
		}
		events.swap(g_events);
	}
	Dispatch(std::move(events));
	return OK;
}

static KYTY_SYSV_ABI int CreateContext(ContextEventHandler handler, void* arg, int* id) {
	PRINT_NAME();
	if (id == nullptr) {
		return ERR_INVALID_ARG;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	Context context {};
	context.id          = g_next_context++;
	context.handler     = handler;
	context.handler_arg = arg;
	const int nonblock  = 1;
	context.options[7].assign(reinterpret_cast<const uint8_t*>(&nonblock),
	                          reinterpret_cast<const uint8_t*>(&nonblock) + sizeof(nonblock));
	for (const int option: {2, 3}) {
		const uint32_t bytes = BUFFER_SIZE;
		context.options[option].assign(reinterpret_cast<const uint8_t*>(&bytes),
		                               reinterpret_cast<const uint8_t*>(&bytes) + sizeof(bytes));
	}
	*id = context.id;
	g_contexts.emplace(context.id, std::move(context));
	return OK;
}

static KYTY_SYSV_ABI int Bind(int id, int socket, uint16_t vport, uint8_t mux_mode) {
	PRINT_NAME();
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	auto* context = GetContext(id);
	if (context == nullptr) {
		return ERR_INVALID_CTX;
	}
	if (context->bound) {
		return ERR_ALREADY_BOUND;
	}
	context->socket   = socket;
	context->vport    = vport;
	context->mux_mode = mux_mode;
	context->bound    = true;
	return OK;
}

static KYTY_SYSV_ABI int Activate(int id, const void* address, uint32_t address_size) {
	PRINT_NAME();
	(void)address;
	(void)address_size;
	std::vector<PendingEvent> events;
	int                       result = ERR_IN_PROGRESS;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_initialized) {
			return ERR_NOT_INIT;
		}
		if (g_event_handler == nullptr) {
			return ERR_NO_HANDLER;
		}
		auto* context = GetContext(id);
		if (context == nullptr) {
			return ERR_INVALID_CTX;
		}
		if (!context->bound) {
			return ERR_NOT_ACCEPTABLE;
		}
		context->active     = true;
		context->last_error = ERR_IN_PROGRESS;
		for (auto& [candidate_id, candidate]: g_contexts) {
			if (candidate_id != id && candidate.last_error == ERR_IN_PROGRESS && candidate.bound &&
			    !candidate.established) {
				result = Connect(&candidate, context->vport);
				break;
			}
		}
		events = TakeInternalEvents();
	}
	Dispatch(std::move(events));
	return result;
}

static KYTY_SYSV_ABI int Initiate(int id, const void* address, uint32_t address_size,
                                  uint16_t vport) {
	PRINT_NAME();
	(void)address;
	(void)address_size;
	std::vector<PendingEvent> events;
	int                       result = OK;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_initialized) {
			return ERR_NOT_INIT;
		}
		if (g_event_handler == nullptr) {
			return ERR_NO_HANDLER;
		}
		auto* context = GetContext(id);
		if (context == nullptr) {
			return ERR_INVALID_CTX;
		}
		result = Connect(context, vport);
		events = TakeInternalEvents();
	}
	Dispatch(std::move(events));
	return result;
}

static KYTY_SYSV_ABI int Terminate(int id) {
	PRINT_NAME();
	std::vector<PendingEvent> events;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_initialized) {
			return ERR_NOT_INIT;
		}
		auto it = g_contexts.find(id);
		if (it == g_contexts.end()) {
			return ERR_INVALID_CTX;
		}
		if (auto* peer = GetContext(it->second.peer_id); peer != nullptr) {
			peer->peer_closed = true;
			peer->peer_id     = 0;
			peer->established = false;
			QueueEvent(*peer, 1);
		}
		QueueEvent(it->second, 1);
		g_contexts.erase(it);
		for (auto& [poll_id, poll]: g_polls) {
			(void)poll_id;
			poll.contexts.erase(id);
		}
		g_cv.notify_all();
		events = TakeInternalEvents();
	}
	Dispatch(std::move(events));
	return OK;
}

static KYTY_SYSV_ABI int Write(int id, const void* data, size_t size, uint8_t flags) {
	PRINT_NAME();
	if (data == nullptr || size == 0 ||
	    size > static_cast<size_t>(std::numeric_limits<int>::max())) {
		return ERR_INVALID_ARG;
	}
	std::vector<PendingEvent> events;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_initialized) {
			return ERR_NOT_INIT;
		}
		auto* context = GetContext(id);
		if (context == nullptr) {
			return ERR_INVALID_CTX;
		}
		auto* peer = GetContext(context->peer_id);
		if (!context->established || peer == nullptr) {
			return context->peer_closed ? ERR_END_OF_DATA : ERR_NOT_ACCEPTABLE;
		}
		size_t queued = 0;
		for (const auto& message: peer->messages) {
			queued += message.data.size();
		}
		if (size > BUFFER_SIZE - std::min(queued, BUFFER_SIZE)) {
			return ERR_WOULD_BLOCK;
		}
		Message message;
		message.data.resize(size);
		std::memcpy(message.data.data(), data, size);
		message.flags    = flags;
		message.sequence = context->next_sequence++;
		message.timestamp =
		    static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		                              std::chrono::steady_clock::now().time_since_epoch())
		                              .count());
		peer->messages.push_back(std::move(message));
		context->sent_packets++;
		context->sent_bytes += size;
		QueueEvent(*peer, 5);
		g_cv.notify_all();
		events = TakeInternalEvents();
	}
	Dispatch(std::move(events));
	return static_cast<int>(size);
}

static KYTY_SYSV_ABI int Read(int id, void* data, size_t size, uint8_t flags, ReadInfo* info) {
	PRINT_NAME();
	(void)flags;
	if (data == nullptr || size == 0) {
		return ERR_INVALID_ARG;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	auto* context = GetContext(id);
	if (context == nullptr) {
		return ERR_INVALID_CTX;
	}
	if (context->messages.empty()) {
		return context->peer_closed ? ERR_END_OF_DATA : ERR_WOULD_BLOCK;
	}
	const auto& message = context->messages.front();
	if (size < message.data.size() || (info != nullptr && info->size < sizeof(ReadInfo))) {
		return ERR_BUFFER_SMALL;
	}
	std::memcpy(data, message.data.data(), message.data.size());
	if (info != nullptr) {
		*info                 = {};
		info->size            = sizeof(ReadInfo);
		info->flags           = message.flags;
		info->sequence_number = message.sequence;
		info->timestamp       = message.timestamp;
	}
	const int result = static_cast<int>(message.data.size());
	context->recv_packets++;
	context->recv_bytes += message.data.size();
	context->messages.pop_front();
	return result;
}

static KYTY_SYSV_ABI int GetSizeWritable(int id) {
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	const auto* context = GetContext(id);
	if (context == nullptr) {
		return ERR_INVALID_CTX;
	}
	return context->established ? static_cast<int>(BUFFER_SIZE) : ERR_NOT_ACCEPTABLE;
}

static KYTY_SYSV_ABI int GetSizeReadable(int id) {
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	const auto* context = GetContext(id);
	if (context == nullptr) {
		return ERR_INVALID_CTX;
	}
	if (context->messages.empty()) {
		return context->peer_closed ? ERR_END_OF_DATA : 0;
	}
	return static_cast<int>(context->messages.front().data.size());
}

static KYTY_SYSV_ABI int GetNumberOfPacketsToRead(int id) {
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	const auto* context = GetContext(id);
	return context == nullptr ? ERR_INVALID_CTX : static_cast<int>(context->messages.size());
}

static KYTY_SYSV_ABI int GetNumberOfPacketsToWrite(int id) {
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	const auto* context = GetContext(id);
	if (context == nullptr) {
		return ERR_INVALID_CTX;
	}
	return context->established ? static_cast<int>(BUFFER_SIZE / 1346) : ERR_NOT_ACCEPTABLE;
}

static KYTY_SYSV_ABI int Flush(int id) {
	std::vector<PendingEvent> events;
	{
		std::scoped_lock lock(g_mutex);
		if (!g_initialized) {
			return ERR_NOT_INIT;
		}
		auto* context = GetContext(id);
		if (context == nullptr) {
			return ERR_INVALID_CTX;
		}
		QueueEvent(*context, 6);
		events = TakeInternalEvents();
	}
	Dispatch(std::move(events));
	return OK;
}

static KYTY_SYSV_ABI int SetOption(int id, int option, const void* value, size_t size) {
	if (value == nullptr || size == 0) {
		return ERR_INVALID_ARG;
	}
	if (option < 1 || option > 20 || option == 14) {
		return ERR_INVALID_OPTION;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	auto* context = GetContext(id);
	if (context == nullptr) {
		return ERR_INVALID_CTX;
	}
	auto* bytes = static_cast<const uint8_t*>(value);
	context->options[option].assign(bytes, bytes + size);
	return OK;
}

static KYTY_SYSV_ABI int GetOption(int id, int option, void* value, size_t size) {
	if (value == nullptr || size == 0) {
		return ERR_INVALID_ARG;
	}
	if (option < 1 || option > 20) {
		return ERR_INVALID_OPTION;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	const auto* context = GetContext(id);
	if (context == nullptr) {
		return ERR_INVALID_CTX;
	}
	if (option == 14) {
		if (size < sizeof(int)) {
			return ERR_BUFFER_SMALL;
		}
		std::memcpy(value, &context->last_error, sizeof(int));
		return OK;
	}
	const auto it = context->options.find(option);
	if (it == context->options.end()) {
		return ERR_INVALID_OPTION;
	}
	if (size < it->second.size()) {
		return ERR_BUFFER_SMALL;
	}
	std::memcpy(value, it->second.data(), it->second.size());
	return OK;
}

static KYTY_SYSV_ABI int GetContextStatus(int id, ContextStatus* status, size_t size) {
	if (status == nullptr || size < sizeof(ContextStatus)) {
		return ERR_INVALID_ARG;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	const auto* context = GetContext(id);
	if (context == nullptr) {
		return ERR_INVALID_CTX;
	}
	*status                  = {};
	status->state            = context->established ? 3u : (context->bound ? 1u : 0u);
	status->parent_id        = -1;
	status->sent_packets     = context->sent_packets;
	status->received_packets = context->recv_packets;
	status->sent_bytes       = context->sent_bytes;
	status->received_bytes   = context->recv_bytes;
	return OK;
}

static KYTY_SYSV_ABI int PollCreate(size_t size) {
	if (size == 0) {
		return ERR_INVALID_ARG;
	}
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	const int id = g_next_poll++;
	g_polls.emplace(id, Poll {});
	return id;
}

static KYTY_SYSV_ABI int PollDestroy(int id) {
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	if (g_polls.erase(id) == 0) {
		return ERR_INVALID_POLL;
	}
	g_cv.notify_all();
	return OK;
}

static KYTY_SYSV_ABI int PollControl(int poll_id, int operation, int context_id, uint16_t events) {
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	auto poll = g_polls.find(poll_id);
	if (poll == g_polls.end()) {
		return ERR_INVALID_POLL;
	}
	if (GetContext(context_id) == nullptr) {
		return ERR_INVALID_CTX;
	}
	if (operation == 1) {
		if (!poll->second.contexts.emplace(context_id, events).second) {
			return ERR_ALREADY_EXISTS;
		}
	} else if (operation == 2) {
		auto item = poll->second.contexts.find(context_id);
		if (item == poll->second.contexts.end()) {
			return ERR_INVALID_CTX;
		}
		item->second = events;
	} else if (operation == 3) {
		poll->second.contexts.erase(context_id);
	} else {
		return ERR_INVALID_ARG;
	}
	g_cv.notify_all();
	return OK;
}

static uint16_t ReadyEvents(int id, uint16_t requested) {
	const auto* context = GetContext(id);
	if (context == nullptr) {
		return 0;
	}
	uint16_t result = 0;
	if ((requested & POLL_READ) != 0 && (!context->messages.empty() || context->peer_closed)) {
		result |= POLL_READ;
	}
	if ((requested & POLL_WRITE) != 0 && context->established) {
		result |= POLL_WRITE;
	}
	if ((requested & POLL_FLUSH) != 0) {
		result |= POLL_FLUSH;
	}
	if ((requested & POLL_ERROR) != 0 && context->last_error < 0 &&
	    context->last_error != ERR_IN_PROGRESS) {
		result |= POLL_ERROR;
	}
	return result;
}

static KYTY_SYSV_ABI int PollWait(int id, PollEvent* events, size_t count, uint64_t timeout) {
	if (events == nullptr || count == 0) {
		return ERR_INVALID_ARG;
	}
	std::unique_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	auto poll = g_polls.find(id);
	if (poll == g_polls.end()) {
		return ERR_INVALID_POLL;
	}
	const auto ready = [&] {
		return poll->second.cancelled || !g_initialized ||
		       std::any_of(
		           poll->second.contexts.begin(), poll->second.contexts.end(),
		           [](const auto& item) { return ReadyEvents(item.first, item.second) != 0; });
	};
	if (!ready() && timeout != 0) {
		g_cv.wait_for(lock, std::chrono::microseconds(timeout), ready);
	}
	if (!g_initialized) {
		return ERR_CANCELLED;
	}
	if (poll->second.cancelled) {
		poll->second.cancelled = false;
		return ERR_CANCELLED;
	}
	size_t written = 0;
	for (const auto& [context_id, requested]: poll->second.contexts) {
		const auto returned = ReadyEvents(context_id, requested);
		if (returned != 0 && written < count) {
			events[written++] = {context_id, requested, returned};
		}
	}
	return static_cast<int>(written);
}

static KYTY_SYSV_ABI int PollCancel(int id) {
	std::scoped_lock lock(g_mutex);
	if (!g_initialized) {
		return ERR_NOT_INIT;
	}
	auto poll = g_polls.find(id);
	if (poll == g_polls.end()) {
		return ERR_INVALID_POLL;
	}
	poll->second.cancelled = true;
	g_cv.notify_all();
	return OK;
}

static KYTY_SYSV_ABI int NetReceived(int socket, const uint8_t* data, size_t size,
                                     const void* address, uint32_t address_size) {
	PRINT_NAME();
	(void)socket;
	(void)data;
	(void)size;
	(void)address;
	(void)address_size;
	std::scoped_lock lock(g_mutex);
	return g_initialized ? OK : ERR_NOT_INIT;
}

static KYTY_SYSV_ABI int NetFlush() {
	PRINT_NAME();
	std::scoped_lock lock(g_mutex);
	return g_initialized ? OK : ERR_NOT_INIT;
}

} // namespace Rudp

LIB_DEFINE(InitRudp_1) {
	LIB_FUNC("amuBfI-AQc4", Rudp::Init);
	LIB_FUNC("3hBvwqEwqj8", Rudp::End);
	LIB_FUNC("6PBNpsgyaxw", Rudp::EnableInternalIOThread);
	LIB_FUNC("SUEVes8gvmw", Rudp::SetEventHandler);
	LIB_FUNC("9U9m1YH0ScQ", Rudp::ProcessEvents);
	LIB_FUNC("CAbbX6BuQZ0", Rudp::CreateContext);
	LIB_FUNC("l4SLBpKUDK4", Rudp::Bind);
	LIB_FUNC("J-6d0WTjzMc", Rudp::Activate);
	LIB_FUNC("szEVu+edXV4", Rudp::Initiate);
	LIB_FUNC("OMYRTU0uc4w", Rudp::Terminate);
	LIB_FUNC("KaPL3fbTLCA", Rudp::Write);
	LIB_FUNC("rZqWV3eXgOA", Rudp::Read);
	LIB_FUNC("fRc1ahQppR4", Rudp::GetSizeWritable);
	LIB_FUNC("sAZqO2+5Qqo", Rudp::GetSizeReadable);
	LIB_FUNC("Px0miD2LuW0", Rudp::GetNumberOfPacketsToRead);
	LIB_FUNC("ZUTAzIbwxJ0", Rudp::GetNumberOfPacketsToWrite);
	LIB_FUNC("Ms0cLK8sTtE", Rudp::Flush);
	LIB_FUNC("0yzYdZf0IwE", Rudp::SetOption);
	LIB_FUNC("mCQIhSmCP6o", Rudp::GetOption);
	LIB_FUNC("wIJsiqY+BMk", Rudp::GetContextStatus);
	LIB_FUNC("MVbmLASjn5M", Rudp::PollCreate);
	LIB_FUNC("LjwbHpEeW0A", Rudp::PollDestroy);
	LIB_FUNC("haMpc7TFx0A", Rudp::PollControl);
	LIB_FUNC("M6ggviwXpLs", Rudp::PollWait);
	LIB_FUNC("yzeXuww-UWg", Rudp::PollCancel);
	LIB_FUNC("vPzJldDSxXc", Rudp::NetReceived);
	LIB_FUNC("+BJ9svDmjYs", Rudp::NetFlush);
}

} // namespace Libs
