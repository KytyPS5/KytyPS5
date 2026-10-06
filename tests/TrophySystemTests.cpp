#include "common/file.h"
#include "common/trophies.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {
namespace Trophies = Common::Trophies;

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

void WritePackage(const std::filesystem::path &path,
                  const std::map<std::string, std::string> &files) {
  size_t offset = 0x40 + files.size() * 0x40;
  size_t size = offset;
  for (const auto &[name, contents] : files)
    size += contents.size();
  std::vector<char> bytes(size);
  WriteBigEndian(bytes, 0, 0xb228c60a, 4);
  WriteBigEndian(bytes, 4, 1, 4);
  WriteBigEndian(bytes, 8, bytes.size(), 8);
  WriteBigEndian(bytes, 0x10, files.size(), 4);
  WriteBigEndian(bytes, 0x14, 0x20, 4);
  size_t entry = 0x40;
  for (const auto &[name, contents] : files) {
    Check(name.size() < 0x20, "fixture file name fits");
    std::memcpy(bytes.data() + entry, name.data(), name.size());
    WriteBigEndian(bytes, entry + 0x20, offset, 8);
    WriteBigEndian(bytes, entry + 0x28, contents.size(), 8);
    std::memcpy(bytes.data() + offset, contents.data(), contents.size());
    offset += contents.size();
    entry += 0x40;
  }
  Common::File file;
  Check(file.Create(path), "create package fixture");
  uint32_t written = 0;
  file.Write(bytes.data(), static_cast<uint32_t>(bytes.size()), &written);
  Check(written == bytes.size(), "write package fixture");
}

Trophies::Package MakePackage(const std::filesystem::path &directory) {
  const auto path = directory / "trophy00.ucp";
  WritePackage(
      path,
      {{"tropconf.json", R"({"defaultLanguage":"en-US","trophies":[
			{"id":"0","grade":"P"},
			{"id":"1","grade":"G","unlockCondition":{"udsStatId":"10","comparator":"ge","targetValue":"10","progressive":false}},
			{"id":"2","grade":"S","unlockCondition":{"udsStatId":"20","comparator":"gt","targetValue":"100","progressive":false}},
			{"id":"3","grade":"B","groupId":"1","unlockCondition":{"udsStatId":"30","comparator":"le","targetValue":"5","progressive":false}},
			{"id":"4","grade":"B","unlockCondition":{"udsStatId":"10","comparator":"lt","targetValue":"-1","progressive":false}}
		]})"},
       {"tropmeta_en-US.json",
        R"({"metadata":{"titleMetadata":{"name":"Test game"},"trophyMetadata":[
			{"id":"1","name":"Find All Coins","detail":"Find all coins in the level."}
		]}})"}});
  return Trophies::LoadPackage(path, 1);
}

void TestEventExtraction(const std::filesystem::path &directory) {
  auto package = MakePackage(directory);
  Check(package.trophies.size() == 5,
        "unsupported signed target does not discard trophy metadata");
  Check(package.trophies.at(1).uds_stat_id == 10 &&
            package.trophies.at(1).target == 10 &&
            !package.trophies.at(1).progressive,
        "non-progressive trophy retains its stat target");
  Check(!package.trophies.at(4).target,
        "negative target is not converted to unsigned");
  Check(Trophies::FindUdsTrophies(package, "FIND_ALL_COINS",
                                  {{"counter", uint64_t{100}}})
            .empty(),
        "missing mapping never guesses from trophy text");
  const auto path = directory / "uds00.ucp";
  WritePackage(path, {{"stats_definition.json", R"({"statDefinitionArray":[
			{"statId":10,"dataType":"uint64","aggregation":"latest","minValue":"0"},
			{"statId":20,"dataType":"uint64","aggregation":"latest"},
			{"statId":30,"dataType":"uint32","aggregation":"latest"},
			{"statId":40,"dataType":"uint64","aggregation":"sum"},
			{"statId":50,"dataType":"uint64","aggregation":"count"},
			{"statId":60,"dataType":"uint64","aggregation":"max"},
			{"statId":70,"dataType":"uint64","aggregation":"latest","maxValue":"3"}
		]})"},
                      {"stats_extraction.json", R"({"statsExtractionRuleArray":[
			{"condition":{"eventName":"COLLECT"},"action":{"input":"$.counter","output":{"statId":10}}},
			{"condition":{"eventName":"COLLECT"},"action":{"input":"$.counterBase","output":{"statId":20}}},
			{"condition":{"eventName":"TIME"},"action":{"input":"$.seconds","output":{"statId":30}}},
			{"condition":{"eventName":"FILTERED","property":{"path":"$.level","comparator":"==","value":1}},"action":{"input":"$.counter","output":{"statId":10}}},
			{"condition":{"eventName":"NESTED"},"action":{"input":"$.player.counter","output":{"statId":10}}},
			{"condition":{"eventName":"SUM"},"action":{"input":"$.counter","output":{"statId":40}}},
			{"condition":{"eventName":"COUNT"},"action":{"output":{"statId":50}}},
			{"condition":{"eventName":"MAX"},"action":{"input":"$.counter","output":{"statId":60}}},
			{"condition":{"eventName":"BOUNDED"},"action":{"input":"$.counter","output":{"statId":70}}},
			{"bad":1}
		]})"}});
  package.event_rules = Trophies::LoadUdsRules(path);
  Check(package.event_rules.size() == 2,
        "skip unsupported extraction and aggregation");
  Check(Trophies::FindUdsTrophies(
            package, "COLLECT",
            {{"counter", uint64_t{9}}, {"counterBase", uint64_t{100}}})
            .empty(),
        "do not add independent counters or treat gt as ge");
  Check(Trophies::FindUdsTrophies(
            package, "COLLECT",
            {{"counter", uint64_t{10}}, {"counterBase", uint64_t{100}}}) ==
            std::vector<int>{1},
        "each stat reads its configured property");
  Check(Trophies::FindUdsTrophies(
            package, "COLLECT",
            {{"counter", uint64_t{0}}, {"counterBase", uint64_t{101}}}) ==
            std::vector<int>{2},
        "strict comparison requires greater value");
  Check(Trophies::FindUdsTrophies(
            package, "COLLECT",
            {{"counter", uint64_t{10}}, {"counterBase", uint64_t{101}}}) ==
            std::vector<int>({1, 2}),
        "one event can unlock multiple mapped trophies");
  Check(Trophies::FindUdsTrophies(
            package, "TIME", {{"seconds", uint32_t{5}}}) == std::vector<int>{3},
        "less-equal condition accepts its boundary");
  Check(Trophies::FindUdsTrophies(package, "TIME", {{"seconds", uint32_t{6}}})
            .empty(),
        "less-equal condition rejects larger input");
  Check(Trophies::FindUdsTrophies(package, "TIME", {{"counter", uint64_t{0}}})
            .empty(),
        "missing property is not zero");
  package.trophies.at(3).comparison = Trophies::Comparison::Less;
  Check(Trophies::FindUdsTrophies(package, "TIME", {{"seconds", uint32_t{5}}})
            .empty(),
        "strict less rejects its boundary");
  Check(Trophies::FindUdsTrophies(
            package, "TIME", {{"seconds", uint32_t{4}}}) == std::vector<int>{3},
        "strict less accepts lower input");
  package.trophies.at(3).comparison = Trophies::Comparison::GreaterEqual;
  Check(Trophies::FindUdsTrophies(package, "TIME",
                                  {{"seconds", uint64_t{UINT32_MAX} + 1}})
            .empty(),
        "uint64 input cannot update a uint32 stat");
  Check(
      Trophies::FindUdsTrophies(package, "COLLECT", {{"counter", int32_t{100}}})
          .empty(),
      "signed inputs cannot update unsigned stats");
  Check(Trophies::FindUdsTrophies(package, "COLLECT",
                                  {{"counter", uint32_t{100}}})
            .empty(),
        "unsigned input width must match the stat");
  package.trophies.at(1).comparison = Trophies::Comparison::None;
  Check(Trophies::FindUdsTrophies(package, "COLLECT",
                                  {{"counter", uint64_t{100}}})
            .empty(),
        "unknown comparator cannot unlock");
  WritePackage(path, {{"stats_extraction.json", "{}"}});
  Check(Trophies::LoadUdsRules(path).empty(),
        "missing stat definitions cannot infer aggregation");
  Check(Trophies::LoadUdsRules(directory / "missing.ucp").empty(),
        "missing UDS package has no rules");
}

void TestProgress(const std::filesystem::path &directory) {
  const auto package = MakePackage(directory);
  Trophies::UnlockData unlocks;
  unlocks.unlocked = {0, 1, 999};
  const auto progress = Trophies::GetProgress(package, unlocks);
  Check(progress.total == 5 && progress.earned == 2,
        "ignore unlock IDs absent from package");
  Check(progress.earned_grade[1] == 1 && progress.earned_grade[2] == 1 &&
            progress.Percentage() == 60,
        "percentage uses 6:2:1 grade points and excludes platinum");
  const auto group = Trophies::GetProgress(package, unlocks, 1);
  Check(group.total == 1 && group.earned == 0 && group.Percentage() == 0,
        "group counts filter trophies");
  Check(Trophies::GetProgress({}, {}).Percentage() == 0,
        "empty package progress is zero");
}

void TestUnlockPersistence(const std::filesystem::path &directory) {
  const auto path = Trophies::UnlocksPath(directory, "PPSA00001", 1000, 0);
  Check(path == directory / "_Trophies/PPSA00001/trophies_1000_0.json",
        "store trophies outside save data");
  Check(Trophies::UnlocksPath(directory, "../bad", 1000, 0).empty(),
        "reject unsafe title IDs");
  Check(Trophies::LoadUnlockData(path).unlocked.empty(),
        "missing unlock file loads as empty");
  Trophies::UnlockData unlocks;
  unlocks.unlocked = {4, 42};
  const auto tick = Trophies::UnixEpochTick + 1791288000123456ULL;
  unlocks.timestamps.emplace(42, tick);
  Check(Trophies::SaveUnlockData(path, unlocks), "save timestamped unlocks");
  auto loaded = Trophies::LoadUnlockData(path);
  Check(loaded.unlocked == unlocks.unlocked &&
            loaded.timestamps == unlocks.timestamps,
        "persist full UTC microsecond timestamp");
  loaded.unlocked.insert(43);
  loaded.timestamps.emplace(43, tick + 1000000);
  Check(Trophies::SaveUnlockData(path, loaded), "replace existing unlock file");
  Check(Trophies::LoadUnlockData(path).timestamps.at(42) == tick,
        "retain original unlock time");

  const auto blocked = directory / "blocked.json";
  std::filesystem::create_directory(blocked);
  Common::File sentinel;
  Check(sentinel.Create(blocked / "keep"),
        "create replacement failure fixture");
  sentinel.Close();
  Check(!Trophies::SaveUnlockData(blocked, loaded) &&
            std::filesystem::exists(blocked / "keep"),
        "failed replacement leaves destination intact");
  Check(!std::filesystem::exists(directory / "blocked.json.tmp"),
        "failed replacement removes temporary file");
}
} // namespace

int main() {
  TempDirectory directory;
  TestEventExtraction(directory.Path());
  TestProgress(directory.Path());
  TestUnlockPersistence(directory.Path());
  std::printf("TrophySystemTests: all cases passed\n");
  return 0;
}
