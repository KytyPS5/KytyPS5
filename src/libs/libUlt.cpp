#include "common/assert.h"
#include "common/logging/log.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Libs {

namespace LibUlt {

LIB_VERSION("Ult", 1, "Ult", 1, 1);

constexpr int ULT_ERROR_NULL      = -2139029503; // 0x80810001
constexpr int ULT_ERROR_ALIGNMENT = -2139029502; // 0x80810002
constexpr int ULT_ERROR_RANGE     = -2139029501; // 0x80810003
constexpr int ULT_ERROR_INVALID   = -2139029500; // 0x80810004
constexpr int ULT_ERROR_STATE     = -2139029498; // 0x80810006
constexpr int ULT_ERROR_BUSY      = -2139029497; // 0x80810007
constexpr int ULT_ERROR_AGAIN     = -2139029496; // 0x80810008

struct UltMutexOptParam {
	uint32_t reserved_header[2];
	uint32_t attribute;
	uint32_t reserved0;
};

struct UltUlthreadRuntimeOptParam {
	uint8_t bytes[128];
};

struct UltMutexState {
	std::recursive_mutex mutex;
	uint32_t             attribute = 0;
};

struct UltSemaphoreState {
	std::mutex              mutex;
	std::condition_variable available;
	int32_t                 resources = 0;
	uint32_t                waiters   = 0;
	bool                    alive     = true;
};

struct UltResourcePoolState {
	uint32_t num_threads      = 0;
	uint32_t num_sync_objects = 0;
	void*    work_area        = nullptr;
};

struct UltQueueDataResourcePoolState {
	uint32_t num_data         = 0;
	uint64_t data_size        = 0;
	uint32_t num_queue_object = 0;
	void*    waiting_pool     = nullptr;
	void*    work_area        = nullptr;
};

struct UltQueueState {
	std::mutex                       mutex;
	std::deque<std::vector<uint8_t>> items;
	uint64_t                         data_size    = 0;
	uint32_t                         capacity     = 0;
	void*                            waiting_pool = nullptr;
	void*                            data_pool    = nullptr;
};

struct UltRuntimeState {
	uint32_t max_num_ulthread  = 0;
	uint32_t num_worker_thread = 0;
	void*    work_area         = nullptr;
};

using UltUlthreadEntry = KYTY_SYSV_ABI int32_t (*)(uint64_t);

struct UltUlthreadState {
	UltUlthreadEntry   entry  = nullptr;
	uint64_t           arg    = 0;
	LibKernel::Pthread thread = nullptr;
};

static std::mutex                                                    g_ult_mutex;
static std::unordered_map<void*, std::shared_ptr<UltMutexState>>     g_ult_mutexes;
static std::unordered_map<void*, std::shared_ptr<UltSemaphoreState>> g_ult_semaphores;
static std::unordered_map<void*, UltResourcePoolState>               g_ult_resource_pools;
static std::unordered_map<void*, UltQueueDataResourcePoolState>      g_ult_queue_data_pools;
static std::unordered_map<void*, std::shared_ptr<UltQueueState>>     g_ult_queues;
static std::unordered_map<void*, UltRuntimeState>                    g_ult_runtimes;
static std::unordered_map<void*, std::shared_ptr<UltUlthreadState>>  g_ult_ulthreads;

static uint64_t ult_align_up(uint64_t value, uint64_t alignment) {
	return (value + alignment - 1u) & ~(alignment - 1u);
}

static int UltQueueGetState(void* queue, std::shared_ptr<UltQueueState>* state) {
	EXIT_IF(state == nullptr);

	std::scoped_lock lock(g_ult_mutex);
	auto             it = g_ult_queues.find(queue);
	if (it == g_ult_queues.end()) {
		return (queue == nullptr ? ULT_ERROR_NULL : ULT_ERROR_STATE);
	}

	*state = it->second;
	return OK;
}

static int KYTY_SYSV_ABI UltInitialize() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI UltFinalize() {
	PRINT_NAME();

	std::vector<std::shared_ptr<UltSemaphoreState>> semaphores;
	{
		std::scoped_lock lock(g_ult_mutex);
		semaphores.reserve(g_ult_semaphores.size());
		for (auto& entry: g_ult_semaphores) {
			auto&            state = entry.second;
			std::scoped_lock state_lock(state->mutex);
			state->alive = false;
			semaphores.push_back(state);
		}
		g_ult_semaphores.clear();
		g_ult_mutexes.clear();
		g_ult_resource_pools.clear();
		g_ult_queue_data_pools.clear();
		g_ult_queues.clear();
		g_ult_runtimes.clear();
		g_ult_ulthreads.clear();
	}
	for (const auto& state: semaphores) {
		state->available.notify_all();
	}

	return OK;
}

static int KYTY_SYSV_ABI UltUlthreadRuntimeOptParamInitialize(UltUlthreadRuntimeOptParam* opt_param,
                                                              uint32_t build_version) {
	PRINT_NAME();

	if (opt_param == nullptr) {
		return ULT_ERROR_NULL;
	}

	LOGF("\t opt_param     = 0x%016" PRIx64 "\n"
	     "\t build_version = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(opt_param), build_version);

	std::memset(opt_param, 0, sizeof(*opt_param));

	return OK;
}

static uint64_t KYTY_SYSV_ABI UltUlthreadRuntimeGetWorkAreaSize(uint32_t max_num_ulthread,
                                                                uint32_t num_worker_thread) {
	PRINT_NAME();

	return ult_align_up(static_cast<uint64_t>(max_num_ulthread) * 256u +
	                        static_cast<uint64_t>(num_worker_thread) * 16u * 1024u,
	                    8u);
}

static int KYTY_SYSV_ABI UltUlthreadRuntimeCreate(void* runtime, const char* name,
                                                  uint32_t max_num_ulthread,
                                                  uint32_t num_worker_thread, void* work_area,
                                                  const void* opt_param, uint32_t build_version) {
	PRINT_NAME();

	if (runtime == nullptr) {
		return ULT_ERROR_NULL;
	}

	LOGF("\t runtime           = 0x%016" PRIx64 "\n"
	     "\t name              = 0x%016" PRIx64 "\n"
	     "\t max_num_ulthread  = %" PRIu32 "\n"
	     "\t num_worker_thread = %" PRIu32 "\n"
	     "\t work_area         = 0x%016" PRIx64 "\n"
	     "\t opt_param         = 0x%016" PRIx64 "\n"
	     "\t build_version     = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(runtime), reinterpret_cast<uint64_t>(name), max_num_ulthread,
	     num_worker_thread, reinterpret_cast<uint64_t>(work_area),
	     reinterpret_cast<uint64_t>(opt_param), build_version);

	std::memset(runtime, 0, 4096);

	std::scoped_lock lock(g_ult_mutex);
	g_ult_runtimes[runtime] = {max_num_ulthread, num_worker_thread, work_area};

	return OK;
}

static KYTY_SYSV_ABI void* UltUlthreadRun(void* arg) {
	auto* state = static_cast<UltUlthreadState*>(arg);
	return reinterpret_cast<void*>(static_cast<intptr_t>(state->entry(state->arg)));
}

static int KYTY_SYSV_ABI UltUlthreadCreate(void* ulthread, const char* name, UltUlthreadEntry entry,
                                           uint64_t arg, void* context, uint64_t size_context,
                                           void* runtime, const void* opt_param,
                                           uint32_t build_version) {
	PRINT_NAME();

	LOGF("\t ulthread      = 0x%016" PRIx64 "\n"
	     "\t name          = 0x%016" PRIx64 "\n"
	     "\t entry         = 0x%016" PRIx64 "\n"
	     "\t arg           = 0x%016" PRIx64 "\n"
	     "\t context       = 0x%016" PRIx64 "\n"
	     "\t size_context  = 0x%016" PRIx64 "\n"
	     "\t runtime       = 0x%016" PRIx64 "\n"
	     "\t opt_param     = 0x%016" PRIx64 "\n"
	     "\t build_version = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(ulthread), reinterpret_cast<uint64_t>(name),
	     reinterpret_cast<uint64_t>(entry), arg, reinterpret_cast<uint64_t>(context), size_context,
	     reinterpret_cast<uint64_t>(runtime), reinterpret_cast<uint64_t>(opt_param), build_version);

	if (ulthread == nullptr || entry == nullptr || context == nullptr || runtime == nullptr) {
		return ULT_ERROR_NULL;
	}

	auto state   = std::make_shared<UltUlthreadState>();
	state->entry = entry;
	state->arg   = arg;

	{
		std::scoped_lock lock(g_ult_mutex);
		if (g_ult_runtimes.find(runtime) == g_ult_runtimes.end()) {
			return ULT_ERROR_STATE;
		}
		if (g_ult_ulthreads.find(ulthread) != g_ult_ulthreads.end()) {
			return ULT_ERROR_STATE;
		}
		std::memset(ulthread, 0, 512);
		g_ult_ulthreads[ulthread] = state;
	}

	const auto result = LibKernel::PthreadCreate(&state->thread, nullptr, UltUlthreadRun,
	                                             state.get(), name != nullptr ? name : "");
	if (result != OK) {
		std::scoped_lock lock(g_ult_mutex);
		g_ult_ulthreads.erase(ulthread);
		return ULT_ERROR_AGAIN;
	}

	return OK;
}

static int KYTY_SYSV_ABI UltUlthreadJoin(void* ulthread, int32_t* status) {
	PRINT_NAME();

	LOGF("\t ulthread = 0x%016" PRIx64 "\n"
	     "\t status   = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(ulthread), reinterpret_cast<uint64_t>(status));

	if (ulthread == nullptr) {
		return ULT_ERROR_NULL;
	}

	std::shared_ptr<UltUlthreadState> state;
	{
		std::scoped_lock lock(g_ult_mutex);
		auto             it = g_ult_ulthreads.find(ulthread);
		if (it == g_ult_ulthreads.end()) {
			return ULT_ERROR_STATE;
		}
		state = it->second;
	}

	void* result = nullptr;
	if (LibKernel::PthreadJoin(state->thread, &result) != OK) {
		return ULT_ERROR_STATE;
	}
	if (status != nullptr) {
		*status = static_cast<int32_t>(reinterpret_cast<intptr_t>(result));
	}

	std::scoped_lock lock(g_ult_mutex);
	g_ult_ulthreads.erase(ulthread);
	return OK;
}

static uint64_t KYTY_SYSV_ABI
UltWaitingQueueResourcePoolGetWorkAreaSize(uint32_t num_threads, uint32_t num_sync_objects) {
	PRINT_NAME();

	return ult_align_up(static_cast<uint64_t>(num_threads + num_sync_objects) * 256u, 8u);
}

static int KYTY_SYSV_ABI UltWaitingQueueResourcePoolCreate(void* pool, const char* name,
                                                           uint32_t num_threads,
                                                           uint32_t num_sync_objects,
                                                           void* work_area, const void* opt_param,
                                                           uint32_t build_version) {
	PRINT_NAME();

	if (pool == nullptr) {
		return ULT_ERROR_NULL;
	}

	LOGF("\t pool             = 0x%016" PRIx64 "\n"
	     "\t name             = 0x%016" PRIx64 "\n"
	     "\t num_threads      = %" PRIu32 "\n"
	     "\t num_sync_objects = %" PRIu32 "\n"
	     "\t work_area        = 0x%016" PRIx64 "\n"
	     "\t opt_param        = 0x%016" PRIx64 "\n"
	     "\t build_version    = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(pool), reinterpret_cast<uint64_t>(name), num_threads,
	     num_sync_objects, reinterpret_cast<uint64_t>(work_area),
	     reinterpret_cast<uint64_t>(opt_param), build_version);

	std::memset(pool, 0, 256);

	std::scoped_lock lock(g_ult_mutex);
	g_ult_resource_pools[pool] = {num_threads, num_sync_objects, work_area};

	return OK;
}

static uint64_t KYTY_SYSV_ABI UltQueueDataResourcePoolGetWorkAreaSize(uint32_t num_data,
                                                                      uint64_t data_size,
                                                                      uint32_t num_queue_object) {
	PRINT_NAME();

	LOGF("\t num_data         = %" PRIu32 "\n"
	     "\t data_size        = 0x%016" PRIx64 "\n"
	     "\t num_queue_object = %" PRIu32 "\n",
	     num_data, data_size, num_queue_object);

	const uint64_t data_area  = static_cast<uint64_t>(num_data) * ult_align_up(data_size, 8u);
	const uint64_t queue_area = static_cast<uint64_t>(num_queue_object) * 512u;

	return ult_align_up(data_area + queue_area, 8u);
}

static int KYTY_SYSV_ABI UltQueueDataResourcePoolCreate(void* pool, const char* name,
                                                        uint32_t num_data, uint64_t data_size,
                                                        uint32_t num_queue_object,
                                                        void*    waiting_queue_resource_pool,
                                                        void* work_area, const void* opt_param,
                                                        uint32_t build_version) {
	PRINT_NAME();

	if (pool == nullptr) {
		return ULT_ERROR_NULL;
	}

	LOGF("\t pool                        = 0x%016" PRIx64 "\n"
	     "\t name                        = 0x%016" PRIx64 "\n"
	     "\t num_data                    = %" PRIu32 "\n"
	     "\t data_size                   = 0x%016" PRIx64 "\n"
	     "\t num_queue_object            = %" PRIu32 "\n"
	     "\t waiting_queue_resource_pool = 0x%016" PRIx64 "\n"
	     "\t work_area                   = 0x%016" PRIx64 "\n"
	     "\t opt_param                   = 0x%016" PRIx64 "\n"
	     "\t build_version               = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(pool), reinterpret_cast<uint64_t>(name), num_data, data_size,
	     num_queue_object, reinterpret_cast<uint64_t>(waiting_queue_resource_pool),
	     reinterpret_cast<uint64_t>(work_area), reinterpret_cast<uint64_t>(opt_param),
	     build_version);

	std::scoped_lock lock(g_ult_mutex);
	if (waiting_queue_resource_pool != nullptr &&
	    g_ult_resource_pools.find(waiting_queue_resource_pool) == g_ult_resource_pools.end()) {
		return ULT_ERROR_INVALID;
	}

	std::memset(pool, 0, 512);
	g_ult_queue_data_pools[pool] = {num_data, data_size, num_queue_object,
	                                waiting_queue_resource_pool, work_area};

	return OK;
}

static int KYTY_SYSV_ABI UltQueueCreate(void* queue, const char* name, uint64_t data_size,
                                        void* waiting_queue_resource_pool,
                                        void* queue_data_resource_pool, const void* opt_param,
                                        uint32_t build_version) {
	PRINT_NAME();

	if (queue == nullptr) {
		return ULT_ERROR_NULL;
	}

	LOGF("\t queue                       = 0x%016" PRIx64 "\n"
	     "\t name                        = 0x%016" PRIx64 "\n"
	     "\t data_size                   = 0x%016" PRIx64 "\n"
	     "\t waiting_queue_resource_pool = 0x%016" PRIx64 "\n"
	     "\t queue_data_resource_pool    = 0x%016" PRIx64 "\n"
	     "\t opt_param                   = 0x%016" PRIx64 "\n"
	     "\t build_version               = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(queue), reinterpret_cast<uint64_t>(name), data_size,
	     reinterpret_cast<uint64_t>(waiting_queue_resource_pool),
	     reinterpret_cast<uint64_t>(queue_data_resource_pool),
	     reinterpret_cast<uint64_t>(opt_param), build_version);

	auto state          = std::make_shared<UltQueueState>();
	state->data_size    = data_size;
	state->waiting_pool = waiting_queue_resource_pool;
	state->data_pool    = queue_data_resource_pool;

	std::scoped_lock lock(g_ult_mutex);
	auto             data_pool = g_ult_queue_data_pools.find(queue_data_resource_pool);
	if (data_pool == g_ult_queue_data_pools.end() ||
	    (waiting_queue_resource_pool != nullptr &&
	     g_ult_resource_pools.find(waiting_queue_resource_pool) == g_ult_resource_pools.end())) {
		return ULT_ERROR_INVALID;
	}
	state->capacity = data_pool->second.num_data;

	std::memset(queue, 0, 512);
	g_ult_queues[queue] = std::move(state);

	return OK;
}

static int KYTY_SYSV_ABI UltQueuePush(void* queue, const void* data) {
	PRINT_NAME();

	std::shared_ptr<UltQueueState> state;
	if (auto ret = UltQueueGetState(queue, &state); ret != OK) {
		return ret;
	}

	if (data == nullptr && state->data_size != 0) {
		return ULT_ERROR_NULL;
	}

	std::scoped_lock lock(state->mutex);
	if (state->capacity != 0 && state->items.size() >= state->capacity) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 32) {
			LOGF("\t queue full, dropping pushed item: queue=0x%016" PRIx64 " capacity=%" PRIu32
			     " data_size=0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(queue), state->capacity, state->data_size);
		}
		return OK;
	}

	auto& item = state->items.emplace_back(static_cast<size_t>(state->data_size));
	if (!item.empty()) {
		std::memcpy(item.data(), data, item.size());
	}

	return OK;
}

static int KYTY_SYSV_ABI UltQueueTryPop(void* queue, void* data) {
	PRINT_NAME();

	std::shared_ptr<UltQueueState> state;
	if (auto ret = UltQueueGetState(queue, &state); ret != OK) {
		return ret;
	}

	if (data == nullptr && state->data_size != 0) {
		return ULT_ERROR_NULL;
	}

	std::scoped_lock lock(state->mutex);
	if (state->items.empty()) {
		return ULT_ERROR_AGAIN;
	}

	auto item = std::move(state->items.front());
	state->items.pop_front();
	if (!item.empty()) {
		std::memcpy(data, item.data(), item.size());
	}

	return OK;
}

static int KYTY_SYSV_ABI UltMutexOptParamInitialize(UltMutexOptParam* opt_param,
                                                    uint32_t          build_version) {
	PRINT_NAME();

	if (opt_param == nullptr) {
		return ULT_ERROR_NULL;
	}

	LOGF("\t opt_param     = 0x%016" PRIx64 "\n"
	     "\t build_version = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(opt_param), build_version);

	std::memset(opt_param, 0, sizeof(*opt_param));

	return OK;
}

static int KYTY_SYSV_ABI UltMutexCreate(void* mutex, const char* name,
                                        void*                   waiting_queue_resource_pool,
                                        const UltMutexOptParam* opt_param, uint32_t build_version) {
	PRINT_NAME();

	if (mutex == nullptr) {
		return ULT_ERROR_NULL;
	}

	auto state       = std::make_shared<UltMutexState>();
	state->attribute = (opt_param != nullptr ? opt_param->attribute : 0);

	LOGF("\t mutex                       = 0x%016" PRIx64 "\n"
	     "\t name                        = 0x%016" PRIx64 "\n"
	     "\t waiting_queue_resource_pool = 0x%016" PRIx64 "\n"
	     "\t attribute                   = 0x%08" PRIx32 "\n"
	     "\t build_version               = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(mutex), reinterpret_cast<uint64_t>(name),
	     reinterpret_cast<uint64_t>(waiting_queue_resource_pool), state->attribute, build_version);

	std::memset(mutex, 0, 256);

	std::scoped_lock lock(g_ult_mutex);
	if (waiting_queue_resource_pool != nullptr &&
	    g_ult_resource_pools.find(waiting_queue_resource_pool) == g_ult_resource_pools.end()) {
		return ULT_ERROR_INVALID;
	}
	g_ult_mutexes[mutex] = state;

	return OK;
}

static int KYTY_SYSV_ABI UltMutexLock(void* mutex) {
	PRINT_NAME();

	std::shared_ptr<UltMutexState> state;
	{
		std::scoped_lock lock(g_ult_mutex);
		auto             it = g_ult_mutexes.find(mutex);
		if (it == g_ult_mutexes.end()) {
			return (mutex == nullptr ? ULT_ERROR_NULL : ULT_ERROR_STATE);
		}
		state = it->second;
	}

	state->mutex.lock();

	return OK;
}

static int KYTY_SYSV_ABI UltMutexUnlock(void* mutex) {
	PRINT_NAME();

	std::shared_ptr<UltMutexState> state;
	{
		std::scoped_lock lock(g_ult_mutex);
		auto             it = g_ult_mutexes.find(mutex);
		if (it == g_ult_mutexes.end()) {
			return (mutex == nullptr ? ULT_ERROR_NULL : ULT_ERROR_STATE);
		}
		state = it->second;
	}

	state->mutex.unlock();

	return OK;
}

static int UltSemaphoreGetState(void* semaphore, std::shared_ptr<UltSemaphoreState>* state) {
	EXIT_IF(state == nullptr);

	std::scoped_lock lock(g_ult_mutex);
	auto             it = g_ult_semaphores.find(semaphore);
	if (it == g_ult_semaphores.end()) {
		return (semaphore == nullptr ? ULT_ERROR_NULL : ULT_ERROR_STATE);
	}
	*state = it->second;
	return OK;
}

static int KYTY_SYSV_ABI UltSemaphoreCreate(void* semaphore, const char* name,
                                            int32_t     num_initial_resource,
                                            void*       waiting_queue_resource_pool,
                                            const void* opt_param, uint32_t build_version) {
	PRINT_NAME();
	(void)name;
	(void)build_version;

	if (semaphore == nullptr) {
		return ULT_ERROR_NULL;
	}
	if ((reinterpret_cast<uintptr_t>(semaphore) & 7u) != 0 ||
	    (opt_param != nullptr && (reinterpret_cast<uintptr_t>(opt_param) & 7u) != 0)) {
		return ULT_ERROR_ALIGNMENT;
	}
	if (num_initial_resource < 0) {
		return ULT_ERROR_RANGE;
	}

	auto state       = std::make_shared<UltSemaphoreState>();
	state->resources = num_initial_resource;

	std::scoped_lock lock(g_ult_mutex);
	if (waiting_queue_resource_pool != nullptr &&
	    g_ult_resource_pools.find(waiting_queue_resource_pool) == g_ult_resource_pools.end()) {
		return ULT_ERROR_INVALID;
	}
	if (g_ult_semaphores.find(semaphore) != g_ult_semaphores.end()) {
		return ULT_ERROR_STATE;
	}
	std::memset(semaphore, 0, 256);
	g_ult_semaphores[semaphore] = std::move(state);
	return OK;
}

static int KYTY_SYSV_ABI UltSemaphoreAcquire(void* semaphore, int32_t num_resource) {
	PRINT_NAME();

	if (num_resource <= 0) {
		return ULT_ERROR_RANGE;
	}
	std::shared_ptr<UltSemaphoreState> state;
	if (auto ret = UltSemaphoreGetState(semaphore, &state); ret != OK) {
		return ret;
	}

	std::unique_lock lock(state->mutex);
	if (!state->alive) {
		return ULT_ERROR_STATE;
	}
	state->waiters++;
	state->available.wait(lock, [&] { return !state->alive || state->resources >= num_resource; });
	state->waiters--;
	if (!state->alive) {
		return ULT_ERROR_STATE;
	}
	state->resources -= num_resource;
	return OK;
}

static int KYTY_SYSV_ABI UltSemaphoreTryAcquire(void* semaphore, int32_t num_resource) {
	PRINT_NAME();

	if (num_resource <= 0) {
		return ULT_ERROR_RANGE;
	}
	std::shared_ptr<UltSemaphoreState> state;
	if (auto ret = UltSemaphoreGetState(semaphore, &state); ret != OK) {
		return ret;
	}

	std::scoped_lock lock(state->mutex);
	if (!state->alive) {
		return ULT_ERROR_STATE;
	}
	if (state->resources < num_resource) {
		return ULT_ERROR_AGAIN;
	}
	state->resources -= num_resource;
	return OK;
}

static int KYTY_SYSV_ABI UltSemaphoreRelease(void* semaphore, int32_t num_resource) {
	PRINT_NAME();

	if (num_resource <= 0) {
		return ULT_ERROR_RANGE;
	}
	std::shared_ptr<UltSemaphoreState> state;
	if (auto ret = UltSemaphoreGetState(semaphore, &state); ret != OK) {
		return ret;
	}

	{
		std::scoped_lock lock(state->mutex);
		if (!state->alive) {
			return ULT_ERROR_STATE;
		}
		if (state->resources > INT32_MAX - num_resource) {
			return ULT_ERROR_RANGE;
		}
		state->resources += num_resource;
	}
	state->available.notify_all();
	return OK;
}

static int KYTY_SYSV_ABI UltSemaphoreDestroy(void* semaphore) {
	PRINT_NAME();

	if (semaphore == nullptr) {
		return ULT_ERROR_NULL;
	}
	std::scoped_lock global_lock(g_ult_mutex);
	auto             it = g_ult_semaphores.find(semaphore);
	if (it == g_ult_semaphores.end()) {
		return ULT_ERROR_STATE;
	}
	auto             state = it->second;
	std::scoped_lock state_lock(state->mutex);
	if (state->waiters != 0) {
		return ULT_ERROR_BUSY;
	}
	state->alive = false;
	g_ult_semaphores.erase(it);
	return OK;
}

LIB_DEFINE(InitUlt_1) {
	LIB_FUNC("hZIg1EWGsHM", UltInitialize);
	LIB_FUNC("d-kSG2fLrvI", UltFinalize);
	LIB_FUNC("V2u3WLrwh64", UltUlthreadRuntimeOptParamInitialize);
	LIB_FUNC("grs2pbc2awM", UltUlthreadRuntimeGetWorkAreaSize);
	LIB_FUNC("jw9FkZBXo-g", UltUlthreadRuntimeCreate);
	LIB_FUNC("znI3q8S7KQ4", UltUlthreadCreate);
	LIB_FUNC("gCeAI57LGgI", UltUlthreadJoin);
	LIB_FUNC("WIWV1Qd7PFU", UltWaitingQueueResourcePoolGetWorkAreaSize);
	LIB_FUNC("YiHujOG9vXY", UltWaitingQueueResourcePoolCreate);
	LIB_FUNC("evj9YPkS8s4", UltQueueDataResourcePoolGetWorkAreaSize);
	LIB_FUNC("TFHm6-N6vks", UltQueueDataResourcePoolCreate);
	LIB_FUNC("9Y5keOvb6ok", UltQueueCreate);
	LIB_FUNC("dUwpX3e5NDE", UltQueuePush);
	LIB_FUNC("uZz3ci7XYqc", UltQueueTryPop);
	LIB_FUNC("1+8t9aHLiz8", UltMutexOptParamInitialize);
	LIB_FUNC("mmt8Sa6tL6c", UltMutexCreate);
	LIB_FUNC("8hEGkR1pfr8", UltMutexLock);
	LIB_FUNC("h0XebKiMBtk", UltMutexUnlock);
	LIB_FUNC("h5QlIYj+Ro8", UltSemaphoreCreate);
	LIB_FUNC("QAH1ofI97vU", UltSemaphoreAcquire);
	LIB_FUNC("HA1Ldbi3lPY", UltSemaphoreTryAcquire);
	LIB_FUNC("lbtk5X1mecw", UltSemaphoreRelease);
	LIB_FUNC("izXyehpoZGo", UltSemaphoreDestroy);
}

} // namespace LibUlt

} // namespace Libs
