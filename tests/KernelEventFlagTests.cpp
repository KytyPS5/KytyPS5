#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/timer.h"
#include "kernel/eventFlag.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

namespace {

using namespace Libs::LibKernel;
using namespace Libs::LibKernel::EventFlag;

constexpr uint32_t AND       = 0x01;
constexpr uint32_t OR        = 0x02;
constexpr uint32_t CLEAR_ALL = 0x10;
constexpr uint32_t SINGLE    = 0x10;
constexpr uint32_t MULTI     = 0x20;

KernelEventFlag Create(uint32_t attr, uint64_t bits) {
	KernelEventFlag ef = nullptr;
	EXPECT_EQ(KernelCreateEventFlag(&ef, "test", attr, bits, nullptr), OK);
	return ef;
}

// A single-waiter flag refuses other waiters while one is queued.
void WaitUntilQueued(KernelEventFlag ef) {
	while (KernelPollEventFlag(ef, 1, OR, nullptr) != KERNEL_ERROR_EPERM) {
		std::this_thread::yield();
	}
}

TEST(KernelEventFlag, ZeroTimeoutWaitSleepsUntilTheNextTick) {
	auto*      ef    = Create(0, 0x4);
	const auto start = Common::SteadyNs();
	for (int i = 0; i < 10; i++) {
		uint64_t       result  = 0;
		KernelUseconds timeout = 0;
		EXPECT_EQ(KernelWaitEventFlag(ef, 1, OR, &result, &timeout), KERNEL_ERROR_ETIMEDOUT);
		EXPECT_EQ(result, 0x4u);
		EXPECT_EQ(timeout, 0u);
	}
	// Every wait after the first starts just after a tick and sleeps a whole one.
	EXPECT_GE(Common::SteadyNs() - start, 9000000u);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
}

TEST(KernelEventFlag, SetReleasesTheWaiterBeforeAClear) {
	auto*       ef     = Create(SINGLE, 0);
	uint64_t    result = 0;
	int         ret    = OK;
	std::thread waiter([&] {
		KernelUseconds timeout = 2000000;
		ret                    = KernelWaitEventFlag(ef, 1, AND, &result, &timeout);
	});
	WaitUntilQueued(ef);
	EXPECT_EQ(KernelSetEventFlag(ef, 1), OK);
	EXPECT_EQ(KernelClearEventFlag(ef, 0), OK);
	waiter.join();
	EXPECT_EQ(ret, OK);
	EXPECT_EQ(result, 1u);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
}

TEST(KernelEventFlag, SetTestsWaitersInQueueOrderAfterEachClear) {
	using namespace std::chrono_literals;
	auto*       ef            = Create(MULTI, 0);
	uint64_t    first_result  = 0;
	uint64_t    second_result = 1;
	int         first         = OK;
	int         second        = OK;
	std::thread first_waiter([&] {
		KernelUseconds timeout = 2000000;
		first = KernelWaitEventFlag(ef, 1, OR | CLEAR_ALL, &first_result, &timeout);
	});
	std::this_thread::sleep_for(50ms);
	std::thread second_waiter([&] {
		KernelUseconds timeout = 200000;
		second                 = KernelWaitEventFlag(ef, 2, OR, &second_result, &timeout);
	});
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ(KernelSetEventFlag(ef, 3), OK);
	EXPECT_EQ(KernelClearEventFlag(ef, 0), OK);
	first_waiter.join();
	second_waiter.join();
	EXPECT_EQ(first, OK);
	EXPECT_EQ(first_result, 3u);
	EXPECT_EQ(second, KERNEL_ERROR_ETIMEDOUT);
	EXPECT_EQ(second_result, 0u);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
}

TEST(KernelEventFlag, CancelReleasesWaitersWithAZeroPattern) {
	auto*       ef     = Create(SINGLE, 0);
	uint64_t    result = 1;
	int         ret    = OK;
	std::thread waiter([&] { ret = KernelWaitEventFlag(ef, 1, AND, &result, nullptr); });
	WaitUntilQueued(ef);
	int count = 0;
	EXPECT_EQ(KernelCancelEventFlag(ef, 0x8, &count), OK);
	waiter.join();
	EXPECT_EQ(count, 1);
	EXPECT_EQ(ret, KERNEL_ERROR_ECANCELED);
	EXPECT_EQ(result, 0u);
	EXPECT_EQ(KernelPollEventFlag(ef, 0x8, AND, &result), OK);
	EXPECT_EQ(result, 0x8u);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
}

TEST(KernelEventFlag, DeleteReleasesWaitersWithAccessDenied) {
	auto*       ef     = Create(SINGLE, 0x4);
	uint64_t    result = 1;
	int         ret    = OK;
	std::thread waiter([&] { ret = KernelWaitEventFlag(ef, 1, AND, &result, nullptr); });
	WaitUntilQueued(ef);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
	waiter.join();
	EXPECT_EQ(ret, KERNEL_ERROR_EACCES);
	EXPECT_EQ(result, 0u);
}

TEST(KernelEventFlag, RejectsInvalidWaits) {
	auto* ef = Create(0, 0);
	for (uint32_t mode: {0x00u, 0x03u, 0x31u}) {
		EXPECT_EQ(KernelWaitEventFlag(ef, 1, mode, nullptr, nullptr), KERNEL_ERROR_EINVAL);
		EXPECT_EQ(KernelPollEventFlag(ef, 1, mode, nullptr), KERNEL_ERROR_EINVAL);
	}
	EXPECT_EQ(KernelPollEventFlag(ef, 0, OR, nullptr), KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelPollEventFlag(nullptr, 1, 0, nullptr), KERNEL_ERROR_EINVAL);
	EXPECT_EQ(KernelPollEventFlag(nullptr, 1, OR, nullptr), KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
}

TEST(KernelEventFlag, NamesHaveAtMost31Characters) {
	KernelEventFlag ef = nullptr;
	EXPECT_EQ(KernelCreateEventFlag(&ef, std::string(32, 'n').c_str(), 0, 0, nullptr),
	          KERNEL_ERROR_ENAMETOOLONG);
	ASSERT_EQ(KernelCreateEventFlag(&ef, std::string(31, 'n').c_str(), 0, 0, nullptr), OK);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
}

} // namespace

int main(int argc, char** argv) {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	subsystems.Initialize<Log::Lifecycle>();
	// Guest threads allocate their TLS in guest memory.
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	Libs::LibKernel::Initialize();
	Libs::LibKernel::PthreadInitSelfForMainThread();
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
