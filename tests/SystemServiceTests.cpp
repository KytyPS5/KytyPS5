#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Libs {
void InitSystemService_1(Loader::SymbolDatabase* symbols);
} // namespace Libs

namespace {

using namespace Libs::SystemService;

class SystemService: public testing::Test {
protected:
	static void SetUpTestSuite() {
		static Common::Subsystems subsystems;
		Common::InitializeThreads();
		subsystems.Initialize<Config::Lifecycle>();
		Config::ConfigOptions options;
		options.printf_direction = Config::LogDirection::Silent;
		Config::Load(options);
		subsystems.Initialize<Log::Lifecycle>();
	}

	void SetUp() override { Libs::InitSystemService_1(&m_symbols); }

	template <typename F>
	F Find(const char* nid) {
		const auto* symbol = m_symbols.FindByNid(nid, Loader::SymbolType::Func);
		EXPECT_NE(symbol, nullptr);
		return reinterpret_cast<F>(symbol->vaddr);
	}

private:
	Loader::SymbolDatabase m_symbols;
};

TEST_F(SystemService, MediaAndMusicPlaybackControlSucceed) {
	const auto disable_media  = Find<int(KYTY_SYSV_ABI*)()>("64nkF7LGk8w");
	const auto reenable_media = Find<int(KYTY_SYSV_ABI*)()>("uInYhy7xa+U");
	const auto disable_music  = Find<int(KYTY_SYSV_ABI*)()>("x1UB9bwDSOw");
	const auto reenable_music = Find<int(KYTY_SYSV_ABI*)()>("9kPCz7Or+1Y");

	EXPECT_EQ(disable_media(), OK);
	EXPECT_EQ(reenable_media(), OK);
	EXPECT_EQ(disable_music(), OK);
	EXPECT_EQ(reenable_music(), OK);
}

TEST_F(SystemService, RemoteplayStatusValidatesArgumentAndReportsInactive) {
	const auto get_remoteplay_status = Find<int(KYTY_SYSV_ABI*)(bool*)>("Getl+RqEhVQ");

	EXPECT_EQ(get_remoteplay_status(nullptr), SYSTEM_SERVICE_ERROR_PARAMETER);

	bool active = true;
	EXPECT_EQ(get_remoteplay_status(&active), OK);
	EXPECT_FALSE(active);
}

TEST_F(SystemService, SettingsAndActivitiesSucceed) {
	const auto show_safe_area   = Find<int(KYTY_SYSV_ABI*)()>("tPfQU2pD4-M");
	const auto show_controller  = Find<int(KYTY_SYSV_ABI*)()>("w9wlKcHrmm8");
	const auto open_challenge   = Find<int(KYTY_SYSV_ABI*)()>("sPuK5ic3GD4");
	const auto open_tournament  = Find<int(KYTY_SYSV_ABI*)()>("gELp9ue2ccQ");
	const auto ready_to_suspend = Find<int(KYTY_SYSV_ABI*)()>("EqcPA3ugRP8");
	const auto power_tick       = Find<int(KYTY_SYSV_ABI*)()>("XbbJC3E+L5M");

	EXPECT_EQ(show_safe_area(), OK);
	EXPECT_EQ(show_controller(), OK);
	EXPECT_EQ(open_challenge(), OK);
	EXPECT_EQ(open_tournament(), OK);
	EXPECT_EQ(ready_to_suspend(), OK);
	EXPECT_EQ(power_tick(), OK);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
