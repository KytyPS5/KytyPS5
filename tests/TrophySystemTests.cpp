#include "common/file.h"
#include "common/localization.h"
#include "common/trophies.h"
#include "libs/libTrophy.cpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Loader {
bool SystemContentParamSfoGetString(const char *, std::string *value) {
  *value = "PPSA00001";
  return true;
}
void SymbolDatabase::Add(const SymbolResolve &, uint64_t, const std::string &) {
}
namespace Timer {
double GetTimeMs() { return 0; }
} // namespace Timer
} // namespace Loader

namespace Libs::LibKernel::FileSystem {
std::filesystem::path GetRealFilename(const std::string &path) { return path; }
} // namespace Libs::LibKernel::FileSystem

namespace Libs::Graphics {
static int notifications = 0;
void NotifyTrophyUnlocked(std::string_view, int32_t,
                          std::span<const std::byte>) {
  ++notifications;
}
} // namespace Libs::Graphics

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

std::vector<int>
EvaluateEvent(const Trophies::Package &package, std::string_view event,
              const std::map<std::string, Trophies::UdsInteger> &properties) {
  Trophies::UnlockData unlocks;
  return Trophies::ApplyUdsEvent(package, unlocks, event, properties).trophies;
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
  Check(EvaluateEvent(package, "FIND_ALL_COINS", {{"counter", uint64_t{100}}})
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
			{"condition":{"eventName":"MULTI"},"action":{"input":"$.first","output":{"statId":10}}},
			{"condition":{"eventName":"MULTI"},"action":{"input":"$.last","output":{"statId":10}}},
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
  Check(package.event_rules.size() == 4,
        "skip unsupported extraction and aggregation");
  Check(EvaluateEvent(package, "MULTI",
                      {{"first", uint64_t{10}}, {"last", uint64_t{0}}}) ==
            std::vector<int>{1},
        "each latest action can satisfy an unlock condition");
  Check(
      EvaluateEvent(package, "COLLECT",
                    {{"counter", uint64_t{9}}, {"counterBase", uint64_t{100}}})
          .empty(),
      "do not add independent counters or treat gt as ge");
  Check(EvaluateEvent(package, "COLLECT",
                      {{"counter", uint64_t{10}},
                       {"counterBase", uint64_t{100}}}) == std::vector<int>{1},
        "each stat reads its configured property");
  Check(EvaluateEvent(package, "COLLECT",
                      {{"counter", uint64_t{0}},
                       {"counterBase", uint64_t{101}}}) == std::vector<int>{2},
        "strict comparison requires greater value");
  Check(EvaluateEvent(
            package, "COLLECT",
            {{"counter", uint64_t{10}}, {"counterBase", uint64_t{101}}}) ==
            std::vector<int>({1, 2}),
        "one event can unlock multiple mapped trophies");
  Check(EvaluateEvent(package, "TIME", {{"seconds", uint32_t{5}}}) ==
            std::vector<int>{3},
        "less-equal condition accepts its boundary");
  Check(EvaluateEvent(package, "TIME", {{"seconds", uint32_t{6}}}).empty(),
        "less-equal condition rejects larger input");
  Check(EvaluateEvent(package, "TIME", {{"counter", uint64_t{0}}}).empty(),
        "missing property is not zero");
  package.trophies.at(3).comparison = Trophies::Comparison::Less;
  Check(EvaluateEvent(package, "TIME", {{"seconds", uint32_t{5}}}).empty(),
        "strict less rejects its boundary");
  Check(EvaluateEvent(package, "TIME", {{"seconds", uint32_t{4}}}) ==
            std::vector<int>{3},
        "strict less accepts lower input");
  package.trophies.at(3).comparison = Trophies::Comparison::GreaterEqual;
  Check(EvaluateEvent(package, "TIME", {{"seconds", uint64_t{UINT32_MAX} + 1}})
            .empty(),
        "uint64 input cannot update a uint32 stat");
  Check(EvaluateEvent(package, "COLLECT", {{"counter", int32_t{100}}}).empty(),
        "signed inputs cannot update unsigned stats");
  Check(EvaluateEvent(package, "COLLECT", {{"counter", uint32_t{100}}}).empty(),
        "unsigned input width must match the stat");
  package.trophies.at(1).comparison = Trophies::Comparison::None;
  Check(EvaluateEvent(package, "COLLECT", {{"counter", uint64_t{100}}}).empty(),
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

Trophies::Package TestMaximumStats(const std::filesystem::path &directory) {
  auto package = MakePackage(directory);
  package.trophies.erase(3);
  package.trophies.erase(4);
  package.trophies.at(1).progressive = true;
  package.trophies.at(1).target = 5;
  package.trophies.at(1).platinum_id = 0;
  package.trophies.at(2).platinum_id = 0;
  const auto path = directory / "maximum.ucp";
  WritePackage(path, {{"stats_definition.json", R"({"statDefinitionArray":[
    {"statId":10,"dataType":"int64","aggregation":"max","minValue":"0","initialValue":"2"},
    {"statId":20,"dataType":"uint32","aggregation":"max"},
    {"statId":30,"dataType":"uint64","aggregation":"max"},
    {"statId":40,"dataType":"int64","aggregation":"max"},
    {"statId":50,"dataType":"int64","aggregation":"max","minValue":"-1"},
    {"statId":60,"dataType":"int64","aggregation":"max","minValue":"0","initialValue":"9223372036854775808"}
  ]})"},
                      {"stats_extraction.json", R"({"statsExtractionRuleArray":[
    {"condition":{"eventName":"MAX"},"action":{"input":"$.newValue","output":{"statId":10}}},
    {"condition":{"eventName":"MAX32"},"action":{"input":"$.newValue","output":{"statId":20}}},
    {"condition":{"eventName":"MAX64"},"action":{"input":"$.newValue","output":{"statId":30}}},
    {"condition":{"eventName":"BAD_MIN"},"action":{"input":"$.newValue","output":{"statId":40}}},
    {"condition":{"eventName":"NEGATIVE_MIN"},"action":{"input":"$.newValue","output":{"statId":50}}},
    {"condition":{"eventName":"BAD_INITIAL"},"action":{"input":"$.newValue","output":{"statId":60}}}
  ]})"}});
  package.event_rules = Trophies::LoadUdsRules(path);
  Check(package.event_rules.size() == 3,
        "only nonnegative representable maximum stats are supported");
  Trophies::UnlockData unlocks;
  const auto post = [&](std::string_view name, Trophies::UdsInteger value) {
    return Trophies::ApplyUdsEvent(package, unlocks, name,
                                   {{"newValue", value}});
  };
  for (const auto value :
       {Trophies::UdsInteger{INT64_MIN}, Trophies::UdsInteger{int32_t{5}},
        Trophies::UdsInteger{uint64_t{5}}}) {
    const auto result = post("MAX", value);
    Check(result.trophies.empty() && !result.stats_changed &&
              unlocks.max_stats.empty(),
          "invalid signed stat input never mutates progress");
  }
  Check(post("MAX", int64_t{1}).stats_changed && unlocks.max_stats.at(10) == 2,
        "maximum includes configured initial value");
  Check(post("MAX", int64_t{4}).trophies.empty() &&
            unlocks.max_stats.at(10) == 4,
        "subthreshold progress is retained");
  Check(!post("MAX", int64_t{2}).stats_changed &&
            !post("MAX", int64_t{4}).stats_changed &&
            unlocks.max_stats.at(10) == 4,
        "lower and repeated inputs do not change maximum");
  const auto saved = directory / "maximum.json";
  Check(Trophies::SaveUnlockData(saved, unlocks), "save subthreshold progress");
  unlocks = Trophies::LoadUnlockData(saved);
  Check(unlocks.max_stats.at(10) == 4 &&
            post("MAX", int64_t{5}).trophies == std::vector<int>{1},
        "reloaded progress reaches exact unlock threshold");
  unlocks.unlocked.insert(1);
  Check(post("MAX", int64_t{6}).trophies.empty(),
        "unlocked trophies are not reported again");
  Check(post("MAX", int64_t{INT64_MAX}).stats_changed &&
            unlocks.max_stats.at(10) == INT64_MAX,
        "retain full signed 64-bit precision");
  Check(post("MAX32", uint32_t{UINT32_MAX}).stats_changed &&
            post("MAX64", uint64_t{UINT64_MAX}).stats_changed &&
            unlocks.max_stats.at(20) == UINT32_MAX &&
            unlocks.max_stats.at(30) == UINT64_MAX,
        "retain unsigned maxima without signed overflow");
  Check(Trophies::SaveUnlockData(saved, unlocks) &&
            Trophies::LoadUnlockData(saved).max_stats == unlocks.max_stats,
        "persist full-width stat values");
  return package;
}

void TestMaximumEventHle(const std::filesystem::path &directory,
                         Trophies::Package package) {
  namespace Trophy = Libs::LibNpTrophy2;
  using namespace Libs::LibNpUniversalDataSystem;
  const auto previous = std::filesystem::current_path();
  std::filesystem::create_directory(directory / "hle");
  std::filesystem::current_path(directory / "hle");
  Config::Initialize();
  Trophy::g_packages.emplace(0, std::move(package));
  const int user = Config::GetUserId();
  int context = 0;
  Check(NpUniversalDataSystemCreateContext(&context, user, 0, 0) == 0 &&
            NpUniversalDataSystemRegisterContext(context, 1, 0) == 0,
        "register UDS context");
  auto *state = Trophy::LoadTrophyState({user, 0});
  Trophy::NpTrophy2Data data{};
  const auto progress = [&] {
    Trophy::FillTrophyInfo(*state, state->package->trophies.at(1), nullptr,
                           &data);
    return data.progress.value;
  };
  Check(progress() == 2 && !data.unlocked,
        "report configured initial progress");
  const auto post = [&](int target_context, int64_t value) {
    NpUniversalDataSystemEventPropertyObject properties;
    Check(NpUniversalDataSystemEventPropertyObjectSetInt64(
              &properties, "newValue", value) == 0,
          "set signed event value");
    NpUniversalDataSystemEvent *event = nullptr;
    Check(NpUniversalDataSystemCreateEvent("MAX", &properties, &event,
                                           nullptr) == 0,
          "create progress event");
    Check(NpUniversalDataSystemEventPropertyObjectSetString(
              &properties, "newValue", "replaced") == 0 &&
              properties.integers.empty(),
          "noninteger property replaces integer");
    Check(NpUniversalDataSystemPostEvent(target_context, 1, event, 0) == 0,
          "post copied signed event");
    Check(NpUniversalDataSystemDestroyEvent(event) == 0,
          "destroy progress event");
  };
  post(context, 4);
  Check(progress() == 4 && !data.unlocked && Libs::Graphics::notifications == 0,
        "signed event updates partial progress without unlocking");
  Trophy::g_states.clear();
  state = Trophy::LoadTrophyState({user, 0});
  post(context, 3);
  Check(progress() == 4 && !data.unlocked, "reloaded maximum cannot regress");
  NpUniversalDataSystemEvent direct{"_UnlockTrophy",
                                    {{{"_trophy_id", int32_t{2}}}}};
  Check(NpUniversalDataSystemPostEvent(context, 1, &direct, 0) == 0 &&
            state->unlocks.unlocked == std::set<int>{2},
        "ordinary trophy direct unlock still works");
  post(context, 5);
  Check(progress() == 5 && data.unlocked &&
            state->unlocks.unlocked == std::set<int>({0, 1, 2}) &&
            Libs::Graphics::notifications == 3,
        "threshold awards trophy and linked platinum once");
  const auto timestamps = state->unlocks.timestamps;
  post(context, 6);
  Check(state->unlocks.timestamps == timestamps &&
            Libs::Graphics::notifications == 3 &&
            Trophies::LoadUnlockData(state->path).max_stats.at(10) == 6,
        "later progress persists without duplicate unlocks");
  int guest_context = 0;
  const auto guest = Config::GetPlayerUserId(1);
  Check(NpUniversalDataSystemCreateContext(&guest_context, guest, 0, 0) == 0 &&
            NpUniversalDataSystemRegisterContext(guest_context, 1, 0) == 0,
        "register guest UDS context");
  post(guest_context, 5);
  const auto *guest_state = Trophy::LoadTrophyState({guest, 0});
  Check(guest_state->unlocks.max_stats.empty() &&
            guest_state->unlocks.unlocked.empty() &&
            guest_state->path.empty() && Libs::Graphics::notifications == 3,
        "guests do not earn or save progress");
  Check(NpUniversalDataSystemDestroyContext(context) == 0 &&
            NpUniversalDataSystemDestroyContext(guest_context) == 0,
        "destroy UDS contexts");
  Trophy::g_states.clear();
  Trophy::g_packages.clear();
  Config::Shutdown();
  std::filesystem::current_path(previous);
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

void TestReportedProgress(const std::filesystem::path &directory) {
  const auto trophy_path = directory / "trophy02.ucp";
  WritePackage(
      trophy_path,
      {{"tropconf.json", R"({"defaultLanguage":"en-US","trophies":[
			{"id":"0","grade":"P"},
			{"id":"9","grade":"B","unlockCondition":{"targetValue":"8","progressive":true}},
			{"id":"10","grade":"B"}
		]})"},
       {"tropmeta_en-US.json",
        R"({"metadata":{"titleMetadata":{"name":"Progress"},"trophyMetadata":[]}})"}});
  const auto package = Trophies::LoadPackage(trophy_path, 1);
  Check(package.trophies.size() == 3, "load progressive trophy without stat");

  Trophies::UnlockData unlocks;
  bool changed = false;
  const auto &trophy = package.trophies.at(9);
  Check(!Trophies::TrophyProgress(trophy, unlocks),
        "no progress before the first report");
  Check(!Trophies::UpdateTrophyProgress(package, 9, 1, unlocks, changed) && changed &&
            Trophies::TrophyProgress(trophy, unlocks) == 1,
        "first report records progress");
  Check(!Trophies::UpdateTrophyProgress(package, 9, 3, unlocks, changed) && changed &&
            Trophies::TrophyProgress(trophy, unlocks) == 3,
        "higher report raises progress");
  Check(!Trophies::UpdateTrophyProgress(package, 9, 2, unlocks, changed) && !changed &&
            Trophies::TrophyProgress(trophy, unlocks) == 3,
        "lower report is ignored");
  Check(!Trophies::UpdateTrophyProgress(package, 10, 5, unlocks, changed) &&
            !Trophies::UpdateTrophyProgress(package, 0, 5, unlocks, changed) &&
            !Trophies::UpdateTrophyProgress(package, 42, 5, unlocks, changed) && !changed &&
            unlocks.progress.size() == 1,
        "only progressive trophies take reported progress");

  const auto path = Trophies::UnlocksPath(directory, "PPSA09809", 1000, 0);
  Check(Trophies::SaveUnlockData(path, unlocks) &&
            Trophies::LoadUnlockData(path).progress == unlocks.progress,
        "persist reported progress");

  Check(Trophies::UpdateTrophyProgress(package, 9, 9, unlocks, changed) &&
            Trophies::TrophyProgress(trophy, unlocks) == 8,
        "reaching the target unlocks and caps progress");
  unlocks.unlocked.insert(9);
  Check(!Trophies::UpdateTrophyProgress(package, 9, 10, unlocks, changed) && !changed,
        "unlocked trophies stop taking progress");
}

void TestReportedEventHle(const std::filesystem::path &directory) {
  namespace Trophy = Libs::LibNpTrophy2;
  using namespace Libs::LibNpUniversalDataSystem;
  auto package = MakePackage(directory);
  package.trophies.erase(3);
  package.trophies.erase(4);
  package.trophies.at(2).progressive = true;
  package.trophies.at(2).target = 8;
  package.trophies.at(2).uds_stat_id = std::nullopt;
  package.trophies.at(1).platinum_id = 0;
  package.trophies.at(2).platinum_id = 0;
  const auto previous = std::filesystem::current_path();
  std::filesystem::create_directory(directory / "progress_hle");
  std::filesystem::current_path(directory / "progress_hle");
  Config::Initialize();
  Trophy::g_packages.emplace(0, std::move(package));
  const int user = Config::GetUserId();
  int context = 0;
  Check(NpUniversalDataSystemCreateContext(&context, user, 0, 0) == 0 &&
            NpUniversalDataSystemRegisterContext(context, 1, 0) == 0,
        "register UDS context for reported progress");
  auto *state = Trophy::LoadTrophyState({user, 0});
  Trophy::NpTrophy2Data data{};
  const auto progress = [&] {
    Trophy::FillTrophyInfo(*state, state->package->trophies.at(2), nullptr,
                           &data);
    return data.progress.value;
  };
  const int base = Libs::Graphics::notifications;
  Check(progress() == 0 && !data.unlocked, "reported progress starts empty");
  const auto post = [&](int target_context, int32_t trophy, int32_t value) {
    NpUniversalDataSystemEventPropertyObject properties;
    Check(NpUniversalDataSystemEventPropertyObjectSetInt32(
              &properties, "_trophy_id", trophy) == 0,
          "set reported trophy id");
    Check(NpUniversalDataSystemEventPropertyObjectSetInt32(
              &properties, "_trophy_progress", value) == 0,
          "set reported progress value");
    NpUniversalDataSystemEvent *event = nullptr;
    Check(NpUniversalDataSystemCreateEvent("_UpdateTrophyProgress", &properties,
                                           &event, nullptr) == 0,
          "create reported progress event");
    const int result = NpUniversalDataSystemPostEvent(target_context, 1, event, 0);
    Check(NpUniversalDataSystemDestroyEvent(event) == 0,
          "destroy reported progress event");
    return result;
  };
  Check(post(context, 1, 4) == 0 && state->unlocks.progress.empty() &&
            Libs::Graphics::notifications == base,
        "non-progressive trophies ignore reported progress");
  Check(post(context, 2, 0) == 0 && state->unlocks.progress.at(2) == 0,
        "zero is valid reported progress");
  const std::optional<Trophies::UdsInteger> invalid[] = {
      std::nullopt, int32_t{-1}, uint32_t{2}, int64_t{2}, uint64_t{2},
      uint64_t{UINT64_MAX}};
  for (const auto *key : {"_trophy_id", "_trophy_progress"}) {
    for (const auto &value : invalid) {
      NpUniversalDataSystemEvent event{
          "_UpdateTrophyProgress",
          {{{"_trophy_id", int32_t{2}}, {"_trophy_progress", int32_t{8}}}}};
      event.properties.SetInteger(key, value);
      Check(NpUniversalDataSystemPostEvent(context, 1, &event, 0) ==
                NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT,
            "progress events require two nonnegative Int32 properties");
    }
  }
  const auto saved = Trophies::LoadUnlockData(state->path);
  Check(state->unlocks.progress == (std::map<int, uint64_t>{{2, 0}}) &&
            saved.progress == state->unlocks.progress &&
            state->unlocks.unlocked.empty() && saved.unlocked.empty() &&
            state->unlocks.timestamps.empty() && saved.timestamps.empty() &&
            Libs::Graphics::notifications == base,
        "invalid progress events leave memory, disk and notifications unchanged");
  Check(post(context, 2, 3) == 0 && progress() == 3 && !data.unlocked &&
            Trophies::LoadUnlockData(state->path).progress.at(2) == 3,
        "partial report is saved as trophy progress");
  Trophy::g_states.clear();
  state = Trophy::LoadTrophyState({user, 0});
  Check(post(context, 2, 1) == 0 && progress() == 3,
        "reloaded reported progress cannot regress");
  Check(post(context, 2, 8) == 0 && progress() == 8 && data.unlocked &&
            state->unlocks.unlocked == std::set<int>{2} &&
            Libs::Graphics::notifications == base + 1,
        "reaching the target awards the trophy");
  Check(Trophies::LoadUnlockData(state->path).unlocked == std::set<int>{2},
        "reaching the target persists the trophy award");
  NpUniversalDataSystemEvent direct{"_UnlockTrophy",
                                    {{{"_trophy_id", int32_t{1}}}}};
  Check(NpUniversalDataSystemPostEvent(context, 1, &direct, 0) == 0 &&
            state->unlocks.unlocked == std::set<int>({0, 1, 2}) &&
            Libs::Graphics::notifications == base + 3,
        "remaining trophies award the linked platinum");
  int guest_context = 0;
  const auto guest = Config::GetPlayerUserId(1);
  Check(NpUniversalDataSystemCreateContext(&guest_context, guest, 0, 0) == 0 &&
            NpUniversalDataSystemRegisterContext(guest_context, 1, 0) == 0,
        "register guest UDS context for reported progress");
  post(guest_context, 2, 5);
  const auto *guest_state = Trophy::LoadTrophyState({guest, 0});
  Check(guest_state->unlocks.progress.empty() &&
            guest_state->path.empty() && Libs::Graphics::notifications == base + 3,
        "guests do not save reported progress");
  Check(NpUniversalDataSystemDestroyContext(context) == 0 &&
            NpUniversalDataSystemDestroyContext(guest_context) == 0,
        "destroy UDS contexts of reported progress");
  Trophy::g_states.clear();
  Trophy::g_packages.clear();
  Config::Shutdown();
  std::filesystem::current_path(previous);
}
} // namespace

int main() {
  TestLocalization();
  TempDirectory directory;
  TestEventExtraction(directory.Path());
  TestProgress(directory.Path());
  TestUnlockPersistence(directory.Path());
  TestMaximumEventHle(directory.Path(), TestMaximumStats(directory.Path()));
  TestReportedProgress(directory.Path());
  TestReportedEventHle(directory.Path());
  std::printf("TrophySystemTests: all cases passed\n");
  return 0;
}
