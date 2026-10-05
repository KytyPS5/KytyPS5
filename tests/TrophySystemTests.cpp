#include "common/file.h"
#include "common/trophies.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "TrophySystemTests: failed: %s\n", text);
    std::abort();
  }
}

class TempDirectory {
public:
  TempDirectory() {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path() /
             ("kyty_trophy_test_" + std::to_string(unique));
    Check(std::filesystem::create_directories(m_path),
          "create temporary directory");
  }
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }
  [[nodiscard]] const std::filesystem::path &Path() const { return m_path; }

private:
  std::filesystem::path m_path;
};

void WriteBigEndian(std::vector<char> &bytes, size_t offset, uint64_t value,
                    size_t length) {
  Check(offset <= bytes.size() && length <= bytes.size() - offset,
        "write within package");
  for (size_t i = 0; i < length; ++i) {
    bytes[offset + length - 1 - i] = static_cast<char>(value & 0xff);
    value >>= 8;
  }
}

std::filesystem::path WritePackage(const std::filesystem::path &directory) {
  const std::string conf =
      R"({"defaultLanguage":"en-US","trophies":[{"id":"30","grade":"B","unlockCondition":{"udsStatId":"308","targetValue":"1","progressive":false}}]})";
  const std::string meta =
      R"({"metadata":{"titleMetadata":{"name":"Test game"},"trophyMetadata":[{"id":"30","name":"Hell Diver","detail":"Dived into the water from the diving board."}]}})";
  const size_t table_offset = 0x40;
  const size_t data_offset = table_offset + 2 * 0x40;
  std::vector<char> bytes(data_offset + conf.size() + meta.size());
  WriteBigEndian(bytes, 0, 0xb228c60a, 4);
  WriteBigEndian(bytes, 4, 1, 4);
  WriteBigEndian(bytes, 8, bytes.size(), 8);
  WriteBigEndian(bytes, 0x10, 2, 4);
  WriteBigEndian(bytes, 0x14, 0x20, 4);
  const auto write_entry = [&](size_t index, const char *name, size_t offset,
                               const std::string &contents) {
    const auto entry = table_offset + index * 0x40;
    std::memcpy(bytes.data() + entry, name, std::strlen(name));
    WriteBigEndian(bytes, entry + 0x20, offset, 8);
    WriteBigEndian(bytes, entry + 0x28, contents.size(), 8);
    std::memcpy(bytes.data() + offset, contents.data(), contents.size());
  };
  write_entry(0, "tropconf.json", data_offset, conf);
  write_entry(1, "tropmeta_en-US.json", data_offset + conf.size(), meta);

  const auto path = directory / "trophy00.ucp";
  Common::File file;
  Check(file.Create(path), "create package fixture");
  uint32_t written = 0;
  file.Write(bytes.data(), static_cast<uint32_t>(bytes.size()), &written);
  Check(written == bytes.size(), "write package fixture");
  return path;
}

void TestPackageTargetAndEventMapping(const std::filesystem::path &directory) {
  const auto package =
      Common::Trophies::LoadPackage(WritePackage(directory), 1);
  Check(package.trophies.size() == 1, "parse package trophy");
  const auto &trophy = package.trophies.at(30);
  Check(trophy.uds_stat_id == 308 && trophy.target == 1 && !trophy.progressive,
        "read UDS target even when progressive is false");
  Check(!Common::Trophies::FindUdsTrophy(package,
                                         "COOLING__DIVED_FROM_DIVING_BOARD", 0),
        "do not unlock before target");
  Check(Common::Trophies::FindUdsTrophy(
            package, "COOLING__DIVED_FROM_DIVING_BOARD", 1) == 30,
        "match named event and unlock at target");
  Check(Common::Trophies::FindUdsTrophy(
            package, "COOLING__DIVED_FROM_DIVING_BOARD", 3) == 30,
        "unlock when the counter exceeds its target");
  Check(!Common::Trophies::FindUdsTrophy(package,
                                         "MEMORY_MEADOW__FOUND_A_SECRET", 1),
        "ignore unrelated event names");

  const std::string rules =
      R"({"statsExtractionRuleArray":[{"ruleId":302,"condition":{"eventName":"COOLING__DIVED_FROM_DIVING_BOARD"},"action":{"input":"$.counter","output":{"statId":308}}},{"ruleId":9,"condition":{"eventName":"XYZ"},"action":{"output":{"statId":999}}},{"bad":1}]})";
  std::vector<char> bytes(0x40 + 0x40 + rules.size());
  WriteBigEndian(bytes, 0, 0xb228c60a, 4);
  WriteBigEndian(bytes, 4, 1, 4);
  WriteBigEndian(bytes, 8, bytes.size(), 8);
  WriteBigEndian(bytes, 0x10, 1, 4);
  WriteBigEndian(bytes, 0x14, 0x20, 4);
  std::memcpy(bytes.data() + 0x40, "stats_extraction.json", 21);
  WriteBigEndian(bytes, 0x60, 0x80, 8);
  WriteBigEndian(bytes, 0x68, rules.size(), 8);
  std::memcpy(bytes.data() + 0x80, rules.data(), rules.size());
  auto mapped = package;
  mapped.event_stats = Common::Trophies::ParseUdsEventStats(
      std::as_bytes(std::span(bytes)));
  Check(mapped.event_stats.size() == 2 && mapped.event_stats.at("XYZ").contains(999),
        "parse UDS event stats");
  Check(Common::Trophies::FindUdsTrophies(
            mapped, "COOLING__DIVED_FROM_DIVING_BOARD", 2) ==
            std::vector<int>{30},
        "map event to trophy through stat ID at counter 2");
  Check(Common::Trophies::FindUdsTrophies(
            mapped, "COOLING__DIVED_FROM_DIVING_BOARD", 0).empty(),
        "no unlock below target");
  Check(Common::Trophies::FindUdsTrophies(mapped, "XYZ", 5).empty(),
        "mapped stat without trophy does not fall back");
  Check(Common::Trophies::ParseUdsEventStats({}).empty(),
        "reject unparseable UDS package");
}

void TestUnlockPersistence(const std::filesystem::path &directory) {
  namespace Trophies = Common::Trophies;
  const auto path = Trophies::UnlocksPath(directory, "PPSA00001", 1000, 0);
  Check(path.generic_string().find("_Trophies") != std::string::npos &&
            path.generic_string().find("_SaveData") == std::string::npos,
        "store trophies outside save data");
  Check(Trophies::UnlocksPath(directory, "../bad", 1000, 0).empty(),
        "reject unsafe title IDs");

  Trophies::UnlockData unlocks;
  unlocks.unlocked.insert(4);
  Check(Trophies::LoadUnlockData(path).unlocked.empty(),
        "missing unlock file loads as empty");
  unlocks.unlocked.insert(42);
  unlocks.dates.emplace(42, "2026-10-05 14:32:09");
  Check(Trophies::SaveUnlockData(path, unlocks), "save dated unlocks");
  auto loaded = Trophies::LoadUnlockData(path);
  Check(loaded.unlocked == unlocks.unlocked && loaded.dates == unlocks.dates,
        "load unlock dates alongside IDs");
  loaded.unlocked.insert(43);
  loaded.dates.emplace(43, "2026-10-06 08:01:00");
  Check(Trophies::SaveUnlockData(path, loaded), "replace existing unlock file");
  Check(Trophies::LoadUnlockData(path).dates.at(42) == "2026-10-05 14:32:09",
        "retain original dates when adding unlocks");
}

} // namespace

int main() {
  TempDirectory directory;
  TestPackageTargetAndEventMapping(directory.Path());
  TestUnlockPersistence(directory.Path());
  std::printf("TrophySystemTests: all cases passed\n");
  return 0;
}
