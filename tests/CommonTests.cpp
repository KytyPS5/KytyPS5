#include "FileTimeTestUtil.h"
#include "common/file.h"
#include "common/platform/sysDbg.h"
#include "common/virtualMemory.h"

#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

namespace {

namespace VirtualMemory = Common::VirtualMemory;

constexpr uint64_t PAGE = 0x10000;
constexpr uint64_t BASE = 0x0000'0600'0000'0000ull;

TEST(VirtualMemory, AllocFixedRefusesMappedPagesAndFreeReleasesOnlyItsRange) {
	ASSERT_TRUE(VirtualMemory::AllocFixed(BASE, PAGE, VirtualMemory::Mode::ReadWrite));
	ASSERT_TRUE(VirtualMemory::AllocFixed(BASE + PAGE, PAGE, VirtualMemory::Mode::ReadWrite));

	EXPECT_FALSE(VirtualMemory::AllocFixed(BASE, PAGE, VirtualMemory::Mode::ReadWrite));
	EXPECT_FALSE(VirtualMemory::AllocFixed(BASE - PAGE, 2 * PAGE, VirtualMemory::Mode::ReadWrite));

	ASSERT_TRUE(VirtualMemory::Free(BASE, PAGE));
	EXPECT_FALSE(VirtualMemory::AllocFixed(BASE + PAGE, PAGE, VirtualMemory::Mode::ReadWrite));
	ASSERT_TRUE(VirtualMemory::AllocFixed(BASE, PAGE, VirtualMemory::Mode::ReadWrite));

	EXPECT_TRUE(VirtualMemory::Free(BASE, PAGE));
	EXPECT_TRUE(VirtualMemory::Free(BASE + PAGE, PAGE));
}

TEST(SysDbg, StackUsageReportsTheCallingThreadStackBase) {
	std::thread([] {
		const auto s = SysStackUsage();
		EXPECT_NE(s.reserved_addr, 0u);
		EXPECT_LT(s.reserved_addr, reinterpret_cast<uintptr_t>(&s));
	}).join();
}

TEST(File, MissingFileStaysInvalidAndCreatedFileRoundTrips) {
	const auto path = std::filesystem::temp_directory_path() / "kyty_common_file_test.bin";
	std::filesystem::remove(path);

	Common::File missing;
	EXPECT_FALSE(missing.Open(path, Common::File::Mode::Read));
	EXPECT_TRUE(missing.IsInvalid());

	const uint32_t value = 0x12345678u;
	{
		Common::File out;
		ASSERT_TRUE(out.Create(path));
		out.Write(&value, sizeof(value));
		EXPECT_EQ(out.Size(), sizeof(value));
	}

	Common::File in;
	ASSERT_TRUE(in.Open(path, Common::File::Mode::Read));
	uint32_t read_value = 0;
	in.Read(&read_value, sizeof(read_value));
	EXPECT_EQ(read_value, value);
	EXPECT_TRUE(in.Seek(2));
	EXPECT_EQ(in.Tell(), 2u);
	in.Close();
	std::filesystem::remove(path);
}

TEST(File, NativeExistenceQueriesFollowTheEntryType) {
	const auto dir  = std::filesystem::temp_directory_path();
	const auto file = dir / "kyty_common_exists_test.bin";
	{
		Common::File out;
		ASSERT_TRUE(out.Create(file));
		const uint8_t byte = 1;
		out.Write(&byte, sizeof(byte));
	}

	EXPECT_TRUE(Common::File::IsDirectoryExisting(dir));
	EXPECT_TRUE(Common::File::IsDirectoryExisting(dir / ""));
	EXPECT_FALSE(Common::File::IsFileExisting(dir));
	EXPECT_EQ(Common::File::Size(dir), 0u);

	EXPECT_TRUE(Common::File::IsFileExisting(file));
	EXPECT_FALSE(Common::File::IsFileExisting(file / ""));
	EXPECT_FALSE(Common::File::IsDirectoryExisting(file));
	EXPECT_EQ(Common::File::Size(file), 1u);

	std::filesystem::remove(file);
	EXPECT_FALSE(Common::File::IsFileExisting(file));
}

TEST(File, GetTimesReportsExactNanoseconds) {
	const auto path = std::filesystem::temp_directory_path() / "kyty_common_time_test.bin";
	ASSERT_TRUE(Common::File().Create(path));
	// A multiple of 100 ns, the Windows file time unit.
	constexpr int64_t WRITE_NS = 1700000000123456700;
	SetUnixWriteTime(path, WRITE_NS);

	const auto times = Common::File::GetTimes(path);
	ASSERT_TRUE(times.has_value());
	EXPECT_EQ(times->write_ns, WRITE_NS);
	EXPECT_GT(times->access_ns, WRITE_NS);

	Common::File in(path, Common::File::Mode::Read);
	const auto   open_times = in.GetTimes();
	ASSERT_TRUE(open_times.has_value());
	EXPECT_EQ(open_times->write_ns, WRITE_NS);
	in.Close();
	std::filesystem::remove(path);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
