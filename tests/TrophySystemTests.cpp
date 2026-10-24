#include "common/file.h"
#include "common/localization.h"
#include "common/trophies.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace Trophies = Common::Trophies;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "TrophySystemTests: failed: %s\n", text);
    std::abort();
  }
}

void TestLocalization() {
  struct Translation {
    std::string_view locale;
    std::string_view earned;
  };
  static constexpr Translation translations[] = {
      {"ja-JP", "トロフィーを獲得しました！"},
      {"en-US", "Trophy earned!"},
      {"fr-FR", "Trophée obtenu !"},
      {"es-ES", "¡Ganaste un trofeo!"},
      {"de-DE", "Trophäe erhalten!"},
      {"it-IT", "Trofeo ottenuto!"},
      {"nl-NL", "Trofee behaald!"},
      {"pt-PT", "Troféu conquistado!"},
      {"ru-RU", "Трофей получен!"},
      {"ko-KR", "트로피를 획득했습니다!"},
      {"zh-Hant", "已獲得獎盃！"},
      {"zh-Hans", "已获得奖杯！"},
      {"fi-FI", "Pokaali ansaittu!"},
      {"sv-SE", "Trofé intjänad!"},
      {"da-DK", "Trofæ optjent!"},
      {"no-NO", "Trofé opptjent!"},
      {"pl-PL", "Zdobyto trofeum!"},
      {"pt-BR", "Troféu conquistado!"},
      {"en-GB", "Trophy earned!"},
      {"tr-TR", "Kupa kazanıldı!"},
      {"es-419", "¡Ganaste un trofeo!"},
      {"ar-AE", "تم كسب الكأس!"},
      {"fr-CA", "Trophée obtenu !"},
      {"cs-CZ", "Trofej získána!"},
      {"hu-HU", "Trófea megszerezve!"},
      {"el-GR", "Κερδίσατε ένα τρόπαιο!"},
      {"ro-RO", "Trofeu câștigat!"},
      {"th-TH", "ได้รับถ้วยรางวัลแล้ว!"},
      {"vi-VN", "Đã nhận cúp!"},
      {"id-ID", "Trofi diperoleh!"},
  };
  constexpr std::string_view key = "trophy.toast.earned";
  const auto japanese = Common::Localization::Get(0, key);
  int language = 0;
  for (const auto &translation : translations) {
    Check(Common::Localization::LocaleName(language) == translation.locale,
          "console language selects its locale");
    Check(Common::Localization::Get(language, key) == translation.earned,
          "embedded notification preserves translated UTF-8 bytes");
    ++language;
  }
  Check(japanese == translations[0].earned,
        "translation view survives other locale lookups");
  for (const int invalid : {-1, 30, 1000}) {
    Check(Common::Localization::LocaleName(invalid) == "en-US" &&
              Common::Localization::Get(invalid, key) == "Trophy earned!",
          "invalid console language falls back to English");
  }
  Check(Common::Localization::Get(0, "missing.translation") ==
            "missing.translation",
        "unknown translation returns its key");
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

using Properties = std::map<std::string, Trophies::UdsInteger>;

// Evaluates an event against fresh stats, as if nothing was posted before.
std::vector<int> FindUdsTrophies(const Trophies::Package &package,
                                 std::string_view event,
                                 const Properties &properties) {
  Trophies::UnlockData unlocks;
  bool changed = false;
  return Trophies::ApplyUdsEvent(package, event, properties, unlocks, changed);
}

// Posts an event into persistent stats and records the unlocks it earns.
std::vector<int> Post(const Trophies::Package &package,
                      Trophies::UnlockData &unlocks, std::string_view event,
                      const Properties &properties, bool *changed = nullptr) {
  bool stats_changed = false;
  const auto ids = Trophies::ApplyUdsEvent(package, event, properties, unlocks,
                                           stats_changed);
  unlocks.unlocked.insert(ids.begin(), ids.end());
  if (changed != nullptr)
    *changed = stats_changed;
  return ids;
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
  Check(FindUdsTrophies(package, "FIND_ALL_COINS",
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
  Check(package.event_rules.size() == 5 &&
            !package.event_rules.contains("FILTERED") &&
            !package.event_rules.contains("NESTED") &&
            !package.event_rules.contains("BOUNDED"),
        "skip filtered, nested and bounded extraction");
  Check(FindUdsTrophies(
            package, "COLLECT",
            {{"counter", uint64_t{9}}, {"counterBase", uint64_t{100}}})
            .empty(),
        "do not add independent counters or treat gt as ge");
  Check(FindUdsTrophies(
            package, "COLLECT",
            {{"counter", uint64_t{10}}, {"counterBase", uint64_t{100}}}) ==
            std::vector<int>{1},
        "each stat reads its configured property");
  Check(FindUdsTrophies(
            package, "COLLECT",
            {{"counter", uint64_t{0}}, {"counterBase", uint64_t{101}}}) ==
            std::vector<int>{2},
        "strict comparison requires greater value");
  Check(FindUdsTrophies(
            package, "COLLECT",
            {{"counter", uint64_t{10}}, {"counterBase", uint64_t{101}}}) ==
            std::vector<int>({1, 2}),
        "one event can unlock multiple mapped trophies");
  Check(FindUdsTrophies(
            package, "TIME", {{"seconds", uint32_t{5}}}) == std::vector<int>{3},
        "less-equal condition accepts its boundary");
  Check(FindUdsTrophies(package, "TIME", {{"seconds", uint32_t{6}}})
            .empty(),
        "less-equal condition rejects larger input");
  Check(FindUdsTrophies(package, "TIME", {{"counter", uint64_t{0}}})
            .empty(),
        "missing property is not zero");
  package.trophies.at(3).comparison = Trophies::Comparison::Less;
  Check(FindUdsTrophies(package, "TIME", {{"seconds", uint32_t{5}}})
            .empty(),
        "strict less rejects its boundary");
  Check(FindUdsTrophies(
            package, "TIME", {{"seconds", uint32_t{4}}}) == std::vector<int>{3},
        "strict less accepts lower input");
  package.trophies.at(3).comparison = Trophies::Comparison::GreaterEqual;
  Check(FindUdsTrophies(package, "TIME",
                                  {{"seconds", uint64_t{UINT32_MAX} + 1}})
            .empty(),
        "uint64 input cannot update a uint32 stat");
  Check(
      FindUdsTrophies(package, "COLLECT", {{"counter", int32_t{100}}})
          .empty(),
      "signed inputs cannot update unsigned stats");
  Check(FindUdsTrophies(package, "COLLECT",
                                  {{"counter", uint32_t{100}}})
            .empty(),
        "unsigned input width must match the stat");
  package.trophies.at(1).comparison = Trophies::Comparison::None;
  Check(FindUdsTrophies(package, "COLLECT",
                                  {{"counter", uint64_t{100}}})
            .empty(),
        "unknown comparator cannot unlock");
  WritePackage(path, {{"stats_extraction.json", "{}"}});
  Check(Trophies::LoadUdsRules(path).empty(),
        "missing stat definitions cannot infer aggregation");
  Check(Trophies::LoadUdsRules(directory / "missing.ucp").empty(),
        "missing UDS package has no rules");
}

// Prince of Persia: The Lost Crown (PPSA10067) reports kill counters as int64
// "newValue" properties feeding max-aggregated stats (trophy 0030 Deadly
// Trap: UpdateNbSpikesKills -> stat 113, ge 5, progressive).
void TestAggregatedStats(const std::filesystem::path &directory) {
  const auto trophy_path = directory / "trophy01.ucp";
  WritePackage(
      trophy_path,
      {{"tropconf.json", R"({"defaultLanguage":"en-US","trophies":[
			{"id":"0","grade":"P"},
			{"id":"1","grade":"B","unlockCondition":{"udsStatId":"113","comparator":"ge","targetValue":"5","progressive":true}},
			{"id":"2","grade":"B","unlockCondition":{"udsStatId":"200","comparator":"ge","targetValue":"3","progressive":false}},
			{"id":"3","grade":"B","unlockCondition":{"udsStatId":"300","comparator":"gt","targetValue":"10","progressive":false}},
			{"id":"4","grade":"B","unlockCondition":{"udsStatId":"400","comparator":"le","targetValue":"10","progressive":false}}
		]})"},
       {"tropmeta_en-US.json",
        R"({"metadata":{"titleMetadata":{"name":"Stats"},"trophyMetadata":[]}})"}});
  auto package = Trophies::LoadPackage(trophy_path, 1);
  const auto uds_path = directory / "uds01.ucp";
  WritePackage(uds_path, {{"stats_definition.json", R"({"statDefinitionArray":[
			{"statId":113,"dataType":"int64","aggregation":"max"},
			{"statId":200,"dataType":"uint64","aggregation":"count"},
			{"statId":300,"dataType":"int32","aggregation":"sum","minValue":-2147483648},
			{"statId":400,"dataType":"uint32","aggregation":"min","maxValue":4294967295}
		]})"},
                          {"stats_extraction.json", R"({"statsExtractionRuleArray":[
			{"condition":{"eventName":"UpdateNbSpikesKills"},"action":{"input":"$.newValue","output":{"statId":113}}},
			{"condition":{"eventName":"KILL"},"action":{"output":{"statId":200}}},
			{"condition":{"eventName":"SCORE"},"action":{"input":"$.delta","output":{"statId":300}}},
			{"condition":{"eventName":"LAP"},"action":{"input":"$.seconds","output":{"statId":400}}}
		]})"}});
  package.event_rules = Trophies::LoadUdsRules(uds_path);
  Check(package.trophies.size() == 5 && package.event_rules.size() == 4,
        "load signed and aggregated stats");

  Trophies::UnlockData unlocks;
  bool changed = false;
  const auto &deadly_trap = package.trophies.at(1);
  Check(Post(package, unlocks, "UpdateNbSpikesKills",
             {{"newValue", int64_t{3}}})
            .empty(),
        "int64 stat below target does not unlock");
  Check(Trophies::StatProgress(deadly_trap, unlocks.stats) == 3,
        "progressive trophy reports its stat value");
  Check(Post(package, unlocks, "UpdateNbSpikesKills",
             {{"newValue", int64_t{2}}}, &changed)
                .empty() &&
            !changed,
        "max aggregation keeps the larger value");
  Check(Post(package, unlocks, "UpdateNbSpikesKills",
             {{"newValue", uint64_t{9}}})
            .empty(),
        "input type must match the int64 stat");
  Check(Post(package, unlocks, "UpdateNbSpikesKills",
             {{"newValue", int64_t{5}}}) == std::vector<int>{1},
        "int64 max stat unlocks at its target");
  Check(Post(package, unlocks, "UpdateNbSpikesKills",
             {{"newValue", int64_t{7}}}, &changed)
                .empty() &&
            !changed && std::get<int64_t>(unlocks.stats.at(113)) == 5,
        "stats of unlocked trophies stop updating");

  Check(Post(package, unlocks, "KILL", {}).empty() &&
            Post(package, unlocks, "KILL", {{"unrelated", int32_t{1}}}).empty(),
        "count stat tallies events without input");
  Check(Post(package, unlocks, "KILL", {}) == std::vector<int>{2},
        "count stat unlocks on the third event");

  Check(Post(package, unlocks, "SCORE", {{"delta", int32_t{6}}}).empty() &&
            Post(package, unlocks, "SCORE", {{"delta", int32_t{-3}}}).empty() &&
            Post(package, unlocks, "SCORE", {{"delta", int32_t{7}}}).empty(),
        "signed sum accumulates negative input");
  Check(std::get<int64_t>(unlocks.stats.at(300)) == 10,
        "strict comparison waits past the boundary");
  Check(Post(package, unlocks, "SCORE", {{"delta", int32_t{1}}}) ==
            std::vector<int>{3},
        "signed sum unlocks above target");

  Trophies::UnlockData saturated;
  saturated.stats.emplace(300, int64_t{INT32_MAX - 1});
  Check(Post(package, saturated, "SCORE", {{"delta", int32_t{5}}}) ==
                std::vector<int>{3} &&
            std::get<int64_t>(saturated.stats.at(300)) == INT32_MAX,
        "int32 sum saturates at the type range");

  Check(Post(package, unlocks, "LAP", {{"seconds", uint32_t{30}}}).empty() &&
            Post(package, unlocks, "LAP", {{"seconds", uint32_t{12}}}).empty(),
        "min stat above le target does not unlock");
  Check(Post(package, unlocks, "LAP", {{"seconds", uint32_t{40}}}, &changed)
                .empty() &&
            !changed,
        "min aggregation keeps the smaller value");
  Check(!Trophies::StatProgress(package.trophies.at(4), unlocks.stats),
        "le trophies report no progress value");
  Check(Post(package, unlocks, "LAP", {{"seconds", uint32_t{10}}}) ==
            std::vector<int>{4},
        "min stat unlocks at le boundary");

  unlocks.stats.emplace(999, int64_t{-7});
  const auto path = Trophies::UnlocksPath(directory, "PPSA10067", 1000, 1);
  Check(Trophies::SaveUnlockData(path, unlocks), "save stats");
  const auto loaded = Trophies::LoadUnlockData(path);
  Check(loaded.unlocked == unlocks.unlocked &&
            std::get<int64_t>(loaded.stats.at(999)) == -7 &&
            std::get<uint64_t>(loaded.stats.at(200)) == 3 &&
            loaded.stats.size() == unlocks.stats.size(),
        "persist signed and unsigned stat values");

  // JSON cannot tell int64 from uint64 for non-negative values.
  Trophies::UnlockData resumed;
  resumed.stats = {{113, uint64_t{4}}};
  Check(Post(package, resumed, "UpdateNbSpikesKills",
             {{"newValue", int64_t{1}}})
            .empty(),
        "reloaded stat keeps its saved maximum");
  Check(Post(package, resumed, "UpdateNbSpikesKills",
             {{"newValue", int64_t{5}}}) == std::vector<int>{1},
        "reloaded stat continues aggregation");
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
  TestLocalization();
  TempDirectory directory;
  TestEventExtraction(directory.Path());
  TestAggregatedStats(directory.Path());
  TestProgress(directory.Path());
  TestUnlockPersistence(directory.Path());
  std::printf("TrophySystemTests: all cases passed\n");
  return 0;
}
