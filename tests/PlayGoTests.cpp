#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <gtest/gtest.h>

#include <array>
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
constexpr uint16_t PLAYGO_CHUNK_ID_LIMIT     = 1000;
constexpr uint32_t PLAYGO_BUFFER_SIZE        = 2u * 1024u * 1024u;
constexpr size_t   PLAYGO_CHUNK_COUNT_OFFSET = 10;
constexpr uint16_t PACKAGE_CHUNKS_NUM        = 2;

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

TEST_F(PlayGo, PackageWithoutChunkMetadataListsChunkZeroAndInstallsChunksBelowTheLimit) {
	ASSERT_EQ(OpenAndCountChunks(false), 1u);

	const auto get_locus =
	    Find<int(KYTY_SYSV_ABI*)(int, const uint16_t*, uint32_t, int8_t*)>("uWIYLFkkwqk");
	const uint16_t chunk_zero = 0;
	int8_t         locus      = 0;
	EXPECT_EQ(get_locus(1, &chunk_zero, 1, &locus), 0);
	EXPECT_EQ(locus, PLAYGO_LOCUS_LOCAL_FAST);
	const uint16_t chunk_one       = 1;
	int8_t         chunk_one_locus = 0;
	EXPECT_EQ(get_locus(1, &chunk_one, 1, &chunk_one_locus), 0);
	EXPECT_EQ(chunk_one_locus, PLAYGO_LOCUS_LOCAL_FAST);
	const uint16_t first_chunk_past_limit = PLAYGO_CHUNK_ID_LIMIT;
	EXPECT_EQ(get_locus(1, &first_chunk_past_limit, 1, &locus), PLAYGO_ERROR_BAD_CHUNK_ID);
}

TEST_F(PlayGo, PlayGoHackReportsDefaultChunksWithoutMetadata) {
	EXPECT_EQ(OpenAndCountChunks(true), 1000u);
}

TEST_F(PlayGo, PackageWithChunkMetadataRejectsChunksPastItsTable) {
	const auto package      = std::filesystem::temp_directory_path() / "kyty_playgo_chunk_table";
	const auto bare_package = package / "bare";
	std::filesystem::create_directories(bare_package);
	std::ofstream(package / "param.json") << "{}";
	std::ofstream(bare_package / "param.json") << "{}";
	std::array<char, PLAYGO_CHUNK_COUNT_OFFSET + sizeof(uint16_t)> chunk_table {'p', 'l', 'g', 'o'};
	chunk_table[PLAYGO_CHUNK_COUNT_OFFSET] = PACKAGE_CHUNKS_NUM;
	std::ofstream(package / "playgo-chunk.dat", std::ios::binary)
	    .write(chunk_table.data(), chunk_table.size());
	Loader::SystemContentLoadParamSfo(package / "param.json");

	EXPECT_EQ(OpenAndCountChunks(false), PACKAGE_CHUNKS_NUM);
	const auto get_locus =
	    Find<int(KYTY_SYSV_ABI*)(int, const uint16_t*, uint32_t, int8_t*)>("uWIYLFkkwqk");
	const uint16_t last_chunk = PACKAGE_CHUNKS_NUM - 1;
	int8_t         locus      = 0;
	EXPECT_EQ(get_locus(1, &last_chunk, 1, &locus), 0);
	EXPECT_EQ(locus, PLAYGO_LOCUS_LOCAL_FAST);
	const uint16_t first_chunk_past_table = PACKAGE_CHUNKS_NUM;
	EXPECT_EQ(get_locus(1, &first_chunk_past_table, 1, &locus), PLAYGO_ERROR_BAD_CHUNK_ID);

	Loader::SystemContentLoadParamSfo(bare_package / "param.json");
	std::filesystem::remove_all(package);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
