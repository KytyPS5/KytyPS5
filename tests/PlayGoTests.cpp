#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
	void SetUp() override { Libs::InitPlayGo_1(&m_symbols); }

	template <typename F>
	F Find(const char* nid) {
		const auto* symbol = m_symbols.FindByNid(nid, Loader::SymbolType::Func);
		EXPECT_NE(symbol, nullptr);
		return reinterpret_cast<F>(symbol->vaddr);
	}

	int Open(bool playgo_hack, int* handle) {
		Config::ConfigOptions options;
		options.printf_direction    = Config::LogDirection::Silent;
		options.playgo_hack_enabled = playgo_hack;
		Config::Load(options);

		const auto initialize = Find<int(KYTY_SYSV_ABI*)(const PlayGoInitParams*)>("ts6GlZOKRrE");
		const auto open       = Find<int(KYTY_SYSV_ABI*)(int*, const void*)>("M1Gma1ocrGE");

		std::vector<uint8_t> buffer(PLAYGO_BUFFER_SIZE);
		const PlayGoInitParams init {buffer.data(), PLAYGO_BUFFER_SIZE, 0};
		EXPECT_EQ(initialize(&init), 0);
		return open(handle, nullptr);
	}

private:
	Loader::SymbolDatabase m_symbols;
};

TEST_F(PlayGo, OpenRequiresChunkMetadata) {
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto root = std::filesystem::temp_directory_path() / ("kyty_playgo_" + std::to_string(stamp));
	ASSERT_TRUE(std::filesystem::create_directory(root));
	const auto sys = root / "sce_sys";
	std::filesystem::create_directory(sys);
	const auto param = sys / "param.json";
	std::ofstream(param) << "{}";
	const auto metadata = root / "playgo-chunks.xml";
	std::ofstream(metadata) << R"(<psproject fmt="playgo-chunks" version="3000"><volume>
<chunk_info chunk_count="1" scenario_count="1"><chunks/><scenarios default_id="0">
<scenario id="0" type="playmode" initial_chunk_count="1">0</scenario></scenarios></chunk_info>
</volume><files><file dst_path="sce_sys/param.json"/></files></psproject>)";

	Loader::SystemContentLoadParamSfo(param);
	int handle = -1;
	EXPECT_EQ(Open(false, &handle), 0);

	const auto get_locus =
	    Find<int(KYTY_SYSV_ABI*)(int, const uint16_t*, uint32_t, int8_t*)>("uWIYLFkkwqk");
	const uint16_t chunk_zero = 0;
	int8_t         locus      = 0;
	EXPECT_EQ(get_locus(handle, &chunk_zero, 1, &locus), 0);
	EXPECT_EQ(locus, PLAYGO_LOCUS_LOCAL_FAST);
	const uint16_t chunk_one = 1;
	EXPECT_EQ(get_locus(handle, &chunk_one, 1, &locus), PLAYGO_ERROR_BAD_CHUNK_ID);

	std::filesystem::remove(metadata);
	Loader::SystemContentLoadParamSfo(param);
	handle = -1;
	EXPECT_EQ(Open(false, &handle), static_cast<int>(0x80B20008u));
	EXPECT_EQ(handle, -1);
	std::filesystem::remove_all(root);
}

TEST_F(PlayGo, PlayGoHackReportsDefaultChunksWithoutMetadata) {
	int handle = -1;
	ASSERT_EQ(Open(true, &handle), 0);
	const auto chunk_id =
	    Find<int(KYTY_SYSV_ABI*)(int, uint16_t*, uint32_t, uint32_t*)>("73fF1MFU8hA");
	uint32_t chunks = 0;
	EXPECT_EQ(chunk_id(handle, nullptr, 0, &chunks), 0);
	EXPECT_EQ(chunks, 1000u);
}

} // namespace

int main(int argc, char** argv) {
	Common::Subsystems subsystems;
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
