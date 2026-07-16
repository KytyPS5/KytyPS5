#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

namespace Libs {

LIB_VERSION("Rudp", 1, "Rudp", 1, 1);

namespace Rudp {

using RudpEventHandler = void (*)(int ctx_id, int event_id, int error_code, void* arg);

static RudpEventHandler g_event_handler = nullptr;
static void*            g_event_arg     = nullptr;
static bool             g_initialized   = false;

static KYTY_SYSV_ABI int RudpInit(void* mem_pool, int mem_pool_size) {
	PRINT_NAME();

	LOGF("\t mem_pool      = 0x%016" PRIx64 "\n"
	     "\t mem_pool_size = %d\n",
	     reinterpret_cast<uint64_t>(mem_pool), mem_pool_size);

	g_initialized = true;

	return OK;
}

static KYTY_SYSV_ABI int RudpEnableInternalIOThread(uint32_t stack_size, uint32_t priority) {
	PRINT_NAME();

	LOGF("\t stack_size = %" PRIu32 "\n"
	     "\t priority   = %" PRIu32 "\n",
	     stack_size, priority);

	return OK;
}

static KYTY_SYSV_ABI int RudpSetEventHandler(RudpEventHandler handler, void* arg) {
	PRINT_NAME();

	LOGF("\t handler = 0x%016" PRIx64 "\n"
	     "\t arg     = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(handler), reinterpret_cast<uint64_t>(arg));

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
