#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/timer.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"

#include <gtest/gtest.h>

namespace {

namespace LibKernel = Libs::LibKernel;

void* KYTY_SYSV_ABI ReturnNull(void*) {
	return nullptr;
}

TEST(KernelThread, SleepEndsAtItsDeadline) {
	const auto start = Common::SteadyNs();
	EXPECT_EQ(LibKernel::KernelUsleep(1000000), 0);
	const auto elapsed = Common::SteadyNs() - start;
	EXPECT_GE(elapsed, 1000000000u);
	EXPECT_LT(elapsed, 1004000000u);
}

TEST(KernelThread, PosixSoloschedAttributeRoundTrips) {
	LibKernel::PthreadAttr attr = nullptr;
	ASSERT_EQ(LibKernel::PthreadAttrInit(&attr), 0);
	int solosched = 0;
	EXPECT_EQ(Libs::Posix::pthread_attr_setsolosched_np(&attr, 1), 0);
	EXPECT_EQ(LibKernel::PthreadAttrGetsolosched(&attr, &solosched), 0);
	EXPECT_EQ(solosched, 1);
	EXPECT_EQ(LibKernel::PthreadAttrDestroy(&attr), 0);
}

TEST(KernelThread, ThreadsWithoutACpuSetTakeTheCreatorsCpus) {
	LibKernel::KernelCpumask mask = 0;
	EXPECT_EQ(LibKernel::PthreadGetaffinity(LibKernel::PthreadSelf(), &mask), 0);
	EXPECT_EQ(mask, 0x1fffu);

	LibKernel::PthreadAttr attr = nullptr;
	ASSERT_EQ(LibKernel::PthreadAttrInit(&attr), 0);
	EXPECT_EQ(LibKernel::PthreadAttrGetaffinity(&attr, &mask), 0);
	EXPECT_EQ(mask, ~LibKernel::KernelCpumask {0});

	ASSERT_EQ(LibKernel::PthreadSetaffinity(LibKernel::PthreadSelf(), 0x24), 0);
	const auto child_affinity = [&mask](const LibKernel::PthreadAttr* child_attr) {
		LibKernel::Pthread thread = nullptr;
		EXPECT_EQ(LibKernel::PthreadCreate(&thread, child_attr, ReturnNull, nullptr, "Affinity"), 0);
		EXPECT_EQ(LibKernel::PthreadGetaffinity(thread, &mask), 0);
		EXPECT_EQ(LibKernel::PthreadJoin(thread, nullptr), 0);
		return mask;
	};
	EXPECT_EQ(child_affinity(&attr), 0x24u);
	EXPECT_EQ(child_affinity(nullptr), 0x24u);
	ASSERT_EQ(LibKernel::PthreadAttrSetaffinity(&attr, 0x3), 0);
	EXPECT_EQ(child_affinity(&attr), 0x3u);
	EXPECT_EQ(LibKernel::PthreadAttrDestroy(&attr), 0);
	EXPECT_EQ(LibKernel::PthreadSetaffinity(LibKernel::PthreadSelf(), 0x1fff), 0);
}

} // namespace

int main(int argc, char** argv) {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	subsystems.Initialize<Log::Lifecycle>();
	// Guest threads allocate their TLS in guest memory.
	subsystems.Initialize<LibKernel::Memory::Lifecycle>();
	LibKernel::Initialize();
	LibKernel::PthreadInitSelfForMainThread();
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
