#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

namespace Libs {
void InitUserService_1(Loader::SymbolDatabase* symbols);
} // namespace Libs

namespace {

using namespace Libs::UserService;

class UserService: public testing::Test {
protected:
	void SetUp() override { Libs::InitUserService_1(&m_symbols); }

	template <typename F>
	F Find(const char* nid) {
		const auto* symbol = m_symbols.FindByNid(nid, Loader::SymbolType::Func);
		EXPECT_NE(symbol, nullptr);
		return reinterpret_cast<F>(symbol->vaddr);
	}

private:
	Loader::SymbolDatabase m_symbols;
};

TEST_F(UserService, TerminateIgnoresArguments) {
	const auto terminate = Find<int(KYTY_SYSV_ABI*)(int, int32_t*)>("bwFjS+bX9mA");

	EXPECT_EQ(terminate(Config::GetUserId(), nullptr), OK);
}

TEST_F(UserService, ForegroundUserIsThePrimaryUser) {
	const auto get_foreground_user = Find<int(KYTY_SYSV_ABI*)(int*)>("eNb53LQJmIM");

	int user_id = -1;
	EXPECT_EQ(get_foreground_user(nullptr), USER_SERVICE_ERROR_INVALID_ARGUMENT);
	EXPECT_EQ(get_foreground_user(&user_id), OK);
	EXPECT_EQ(user_id, Config::GetUserId());
}

TEST_F(UserService, UserIdListsHoldLoggedInUsersThenInvalidIds) {
	const auto get_login_users      = Find<int(KYTY_SYSV_ABI*)(int*)>("fPhymKNvK-A");
	const auto get_registered_users = Find<int(KYTY_SYSV_ABI*)(int*)>("5EiQCnL2G1Y");

	int login_users[4];
	int registered_users[20];
	std::memset(login_users, 0x7f, sizeof(login_users));
	std::memset(registered_users, 0x7f, sizeof(registered_users));
	EXPECT_EQ(get_registered_users(nullptr), USER_SERVICE_ERROR_INVALID_ARGUMENT);
	EXPECT_EQ(get_login_users(login_users), OK);
	EXPECT_EQ(get_registered_users(registered_users), OK);

	// Without connected controllers only the primary user is logged in.
	EXPECT_EQ(login_users[0], Config::GetUserId());
	EXPECT_EQ(registered_users[0], Config::GetUserId());
	for (int i = 1; i < 4; i++) {
		EXPECT_EQ(login_users[i], -1);
	}
	for (int i = 1; i < 20; i++) {
		EXPECT_EQ(registered_users[i], -1);
	}
}

} // namespace

int main(int argc, char** argv) {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	subsystems.Initialize<Log::Lifecycle>();
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
