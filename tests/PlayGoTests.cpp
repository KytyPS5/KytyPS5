#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace Libs {
void InitPlayGo_1(Loader::SymbolDatabase* symbols);
} // namespace Libs

namespace {

struct PlayGoInitParams {
	const void* buf_addr;
	uint32_t    buf_size;
	uint32_t    reserved;
};

constexpr int      PLAYGO_ERROR_BAD_CHUNK_ID = static_cast<int>(0x80B2000Cu);
constexpr int8_t   PLAYGO_LOCUS_LOCAL_FAST   = 3;
constexpr uint32_t PLAYGO_BUFFER_SIZE        = 2u * 1024u * 1024u;

class PlayGo: public testing::Test {
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

	void SetUp() override { Libs::InitPlayGo_1(&m_symbols); }

	template <typename F>
	F Find(const char* nid) {
		const auto* symbol = m_symbols.FindByNid(nid, Loader::SymbolType::Func);
		EXPECT_NE(symbol, nullptr);
		return reinterpret_cast<F>(symbol->vaddr);
	}

	uint32_t OpenAndCountChunks(bool playgo_hack) {
		Config::ConfigOptions options;
		options.printf_direction    = Config::LogDirection::Silent;
		options.playgo_hack_enabled = playgo_hack;
		Config::Load(options);

		const auto initialize = Find<int(KYTY_SYSV_ABI*)(const PlayGoInitParams*)>("ts6GlZOKRrE");
		const auto open       = Find<int(KYTY_SYSV_ABI*)(int*, const void*)>("M1Gma1ocrGE");
		const auto chunk_id =
		    Find<int(KYTY_SYSV_ABI*)(int, uint16_t*, uint32_t, uint32_t*)>("73fF1MFU8hA");

		std::vector<uint8_t> buffer(PLAYGO_BUFFER_SIZE);
		const PlayGoInitParams init {buffer.data(), PLAYGO_BUFFER_SIZE, 0};
		EXPECT_EQ(initialize(&init), 0);
		int handle = 0;
		EXPECT_EQ(open(&handle, nullptr), 0);
		uint32_t chunks = 0;
		EXPECT_EQ(chunk_id(handle, nullptr, 0, &chunks), 0);
		return chunks;
	}

private:
	Loader::SymbolDatabase m_symbols;
};

TEST_F(PlayGo, PackageWithoutChunkMetadataHasOnlyChunkZeroInstalled) {
	ASSERT_EQ(OpenAndCountChunks(false), 1u);

	const auto get_locus =
	    Find<int(KYTY_SYSV_ABI*)(int, const uint16_t*, uint32_t, int8_t*)>("uWIYLFkkwqk");
	const uint16_t chunk_zero = 0;
	int8_t         locus      = 0;
	EXPECT_EQ(get_locus(1, &chunk_zero, 1, &locus), 0);
	EXPECT_EQ(locus, PLAYGO_LOCUS_LOCAL_FAST);
	const uint16_t chunk_one = 1;
	EXPECT_EQ(get_locus(1, &chunk_one, 1, &locus), PLAYGO_ERROR_BAD_CHUNK_ID);
}

TEST_F(PlayGo, PlayGoHackReportsDefaultChunksWithoutMetadata) {
	EXPECT_EQ(OpenAndCountChunks(true), 1000u);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
