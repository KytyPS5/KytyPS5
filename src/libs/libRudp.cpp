#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <mutex>

namespace Libs {

LIB_VERSION("Rudp", 1, "Rudp", 1, 1);

namespace Rudp {

using RudpEventHandler = int(KYTY_SYSV_ABI *)(int event_id, int socket, const uint8_t* data,
	                                          size_t data_len, const void* address,
	                                          uint32_t address_len, void* arg);

constexpr int RUDP_ERROR_NOT_INITIALIZED    = static_cast<int>(0x80770001u);
constexpr int RUDP_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80770002u);
constexpr int RUDP_ERROR_INVALID_ARGUMENT   = static_cast<int>(0x80770004u);
constexpr int RUDP_ERROR_THREAD_IN_USE      = static_cast<int>(0x80770010u);
constexpr int RUDP_ERROR_NO_EVENT_HANDLER   = static_cast<int>(0x80770022u);

static RudpEventHandler g_event_handler = nullptr;
static void*            g_event_arg     = nullptr;
static bool             g_initialized   = false;
static bool             g_io_thread_enabled = false;
static void*            g_memory_pool       = nullptr;
static int              g_memory_pool_size  = 0;
static std::mutex       g_state_mutex;

static KYTY_SYSV_ABI int RudpInit(void* mem_pool, int mem_pool_size) {
	PRINT_NAME();

	LOGF("\t mem_pool      = 0x%016" PRIx64 "\n"
	     "\t mem_pool_size = %d\n",
	     reinterpret_cast<uint64_t>(mem_pool), mem_pool_size);

	if (mem_pool == nullptr || mem_pool_size <= 0) {
		return RUDP_ERROR_INVALID_ARGUMENT;
	}

	std::scoped_lock lock(g_state_mutex);
	if (g_initialized) {
		return RUDP_ERROR_ALREADY_INITIALIZED;
	}
	g_memory_pool      = mem_pool;
	g_memory_pool_size = mem_pool_size;
	g_initialized      = true;

	return OK;
}

static KYTY_SYSV_ABI int RudpEnableInternalIOThread(uint32_t stack_size, uint32_t priority) {
	PRINT_NAME();

	LOGF("\t stack_size = %" PRIu32 "\n"
	     "\t priority   = %" PRIu32 "\n",
	     stack_size, priority);

	std::scoped_lock lock(g_state_mutex);
	if (!g_initialized) {
		return RUDP_ERROR_NOT_INITIALIZED;
	}
	if (g_io_thread_enabled) {
		return RUDP_ERROR_THREAD_IN_USE;
	}
	g_io_thread_enabled = true;
	return OK;
}

static KYTY_SYSV_ABI int RudpSetEventHandler(RudpEventHandler handler, void* arg) {
	PRINT_NAME();

	LOGF("\t handler = 0x%016" PRIx64 "\n"
	     "\t arg     = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(handler), reinterpret_cast<uint64_t>(arg));

	std::scoped_lock lock(g_state_mutex);
	if (!g_initialized) {
		return RUDP_ERROR_NOT_INITIALIZED;
	}
	if (handler == nullptr) {
		return RUDP_ERROR_NO_EVENT_HANDLER;
	}
	g_event_handler = handler;
	g_event_arg     = arg;

	return OK;
}

} // namespace Rudp

LIB_DEFINE(InitRudp_1) {
	LIB_FUNC("amuBfI-AQc4", Rudp::RudpInit);
	LIB_FUNC("6PBNpsgyaxw", Rudp::RudpEnableInternalIOThread);
	LIB_FUNC("SUEVes8gvmw", Rudp::RudpSetEventHandler);
}

} // namespace Libs
