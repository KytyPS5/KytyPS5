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
void InitUserService_1(Loader::SymbolDatabase* symbols);
} // namespace Libs

namespace {

using namespace Libs::UserService;

struct UserServiceLoginUserIdList {
	int user_id[4];
};

struct UserServiceRegisteredUserIdList {
	int user_id[16];
};

class UserService: public testing::Test {
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

TEST_F(UserService, InitializeAndTerminateSucceed) {
	const auto initialize = Find<int(KYTY_SYSV_ABI*)(const void*)>("j3YMu1MVNNo");
	const auto terminate  = Find<int(KYTY_SYSV_ABI*)()>("bwFjS+bX9mA");

	EXPECT_EQ(initialize(nullptr), OK);
	EXPECT_EQ(terminate(), OK);
}

TEST_F(UserService, ForegroundUserMatchesInitialUser) {
	const auto get_initial_user    = Find<int(KYTY_SYSV_ABI*)(int*)>("CdWp0oHWGr0");
	const auto get_foreground_user = Find<int(KYTY_SYSV_ABI*)(int*)>("eNb53LQJmIM");

	EXPECT_EQ(get_foreground_user(nullptr), USER_SERVICE_ERROR_INVALID_ARGUMENT);

	int initial_user    = -1;
	int foreground_user = -1;
	EXPECT_EQ(get_initial_user(&initial_user), OK);
	EXPECT_EQ(get_foreground_user(&foreground_user), OK);

	EXPECT_EQ(initial_user, Config::GetUserId());
	EXPECT_EQ(foreground_user, Config::GetUserId());
}

TEST_F(UserService, RegisteredAndLoginUserIdListsIncludePrimaryUser) {
	const auto get_login_users      = Find<int(KYTY_SYSV_ABI*)(UserServiceLoginUserIdList*)>("fPhymKNvK-A");
	const auto get_registered_users = Find<int(KYTY_SYSV_ABI*)(UserServiceRegisteredUserIdList*)>("5EiQCnL2G1Y");

	EXPECT_EQ(get_registered_users(nullptr), USER_SERVICE_ERROR_INVALID_ARGUMENT);

	UserServiceLoginUserIdList      login_users {};
	UserServiceRegisteredUserIdList registered_users {};

	EXPECT_EQ(get_login_users(&login_users), OK);
	EXPECT_EQ(get_registered_users(&registered_users), OK);

	EXPECT_EQ(login_users.user_id[0], Config::GetUserId());
	EXPECT_EQ(registered_users.user_id[0], Config::GetUserId());
	for (int i = 1; i < 4; i++) {
		EXPECT_EQ(registered_users.user_id[i], Config::GetPlayerUserId(i));
	}
	for (int i = 4; i < 16; i++) {
		EXPECT_EQ(registered_users.user_id[i], -1);
	}
}

TEST_F(UserService, UserNumberAndNameReturnValidDataForPrimaryUser) {
	const auto get_user_number = Find<int(KYTY_SYSV_ABI*)(int, int32_t*)>("qbwy0Ub8b3M");
	const auto get_user_name   = Find<int(KYTY_SYSV_ABI*)(int, char*, size_t)>("1xxcMiGu2fo");

	EXPECT_EQ(get_user_number(Config::GetUserId(), nullptr), USER_SERVICE_ERROR_INVALID_ARGUMENT);
	EXPECT_EQ(get_user_name(Config::GetUserId(), nullptr, 0), USER_SERVICE_ERROR_INVALID_ARGUMENT);

	int32_t number = 0;
	EXPECT_EQ(get_user_number(Config::GetUserId(), &number), OK);
	EXPECT_EQ(number, 1);

	char name_buf[128] = {};
	EXPECT_EQ(get_user_name(Config::GetUserId(), name_buf, sizeof(name_buf)), OK);
	EXPECT_STREQ(name_buf, Config::GetUserName().c_str());

	EXPECT_EQ(get_user_number(0xdeadbeef, &number), USER_SERVICE_ERROR_NOT_LOGGED_IN);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
