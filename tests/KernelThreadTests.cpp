#include "common/timer.h"
#include "kernel/pthread.h"

#include <gtest/gtest.h>

namespace {

TEST(KernelThread, SleepEndsAtItsDeadline) {
	const auto start = Common::SteadyNs();
	EXPECT_EQ(Libs::LibKernel::KernelUsleep(1000000), 0);
	const auto elapsed = Common::SteadyNs() - start;
	EXPECT_GE(elapsed, 1000000000u);
	EXPECT_LT(elapsed, 1004000000u);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
