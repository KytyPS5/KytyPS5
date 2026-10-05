#include "common/dateTime.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/trophies.h"
#include "graphics/presentation/systemOverlay.h"
#include "kernel/fileSystem.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Libs {

namespace LibNpTrophy2 {

LIB_VERSION("NpTrophy2", 1, "NpTrophy2", 1, 1);

constexpr int NP_TROPHY2_ERROR_ICON_FILE_NOT_FOUND = -2141898479; /* 0x80553911 */

struct NpTrophy2Progress {
	int32_t  type;
	uint8_t  reserved[4];
	uint64_t value;
};

struct NpTrophy2GameDetails {
	uint32_t num_groups;
	uint32_t num_trophies;
	uint32_t num_platinum;
	uint32_t num_gold;
	uint32_t num_silver;
	uint32_t num_bronze;
	char     title[128];
};

struct NpTrophy2GameData {
	uint32_t unlocked_trophies;
	uint32_t unlocked_platinum;
	uint32_t unlocked_gold;
	uint32_t unlocked_silver;
	uint32_t unlocked_bronze;
	uint32_t progress_percentage;
};

struct NpTrophy2GroupDetails {
	int32_t  group_id;
	uint32_t num_trophies;
	uint32_t num_platinum;
	uint32_t num_gold;
	uint32_t num_silver;
	uint32_t num_bronze;
	char     title[128];
};

struct NpTrophy2GroupData {
	int32_t  group_id;
	uint32_t unlocked_trophies;
	uint32_t unlocked_platinum;
	uint32_t unlocked_gold;
	uint32_t unlocked_silver;
	uint32_t unlocked_bronze;
	uint32_t progress_percentage;
	uint8_t  reserved[4];
};

struct NpTrophy2Details {
	int32_t           trophy_id;
	int32_t           trophy_grade;
	int32_t           group_id;
	bool              hidden;
	bool              has_reward;
	uint8_t           reserved2[2];
	NpTrophy2Progress target;
	char              name[128];
	char              description[1024];
	char              reward[128];
};

struct NpTrophy2Data {
	int32_t           trophy_id;
	bool              unlocked;
	uint8_t           reserved[3];
	NpTrophy2Progress progress;
	uint64_t          timestamp_tick;
};

static_assert(sizeof(NpTrophy2GameDetails) == 152);
static_assert(sizeof(NpTrophy2GameData) == 24);
static_assert(sizeof(NpTrophy2GroupDetails) == 152);
static_assert(sizeof(NpTrophy2GroupData) == 32);
static_assert(sizeof(NpTrophy2Details) == 1312);
static_assert(sizeof(NpTrophy2Data) == 32);

namespace Trophies = Common::Trophies;
using TrophyKey    = std::pair<int, uint32_t>;
using PackageKey   = std::pair<std::string, uint32_t>;

struct TrophyState {
	const Trophies::Package* package = nullptr;
	Trophies::UnlockData     unlocks;
	std::filesystem::path    path;
};

struct TrophyContext {
	TrophyKey    key;
	TrophyState* state = nullptr;
};

static std::mutex                              g_trophy_mutex;
static std::map<PackageKey, Trophies::Package> g_packages;
static std::map<TrophyKey, TrophyState>        g_states;
static std::map<int, TrophyContext>            g_contexts;
static int                                     g_next_context = 1;

constexpr int NP_TROPHY2_ERROR_INVALID_ARGUMENT  = static_cast<int>(0x80553904);
constexpr int NP_TROPHY2_ERROR_INVALID_CONTEXT   = static_cast<int>(0x80553909);
constexpr int NP_TROPHY2_ERROR_INVALID_TROPHY_ID = static_cast<int>(0x8055390a);
constexpr int NP_TROPHY2_ERROR_INVALID_GROUP_ID  = static_cast<int>(0x8055390b);
constexpr int NP_TROPHY2_ERROR_NOT_REGISTERED    = static_cast<int>(0x80553920);

// Caller holds g_trophy_mutex. Both NpTrophy2 and UDS use the same user/service record.
static TrophyState* LoadTrophyState(TrophyKey key) {
	if (const auto it = g_states.find(key); it != g_states.end()) {
		return &it->second;
	}
	std::string title_id;
	Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id);
	auto& package = g_packages[{title_id, key.second}];
	if (package.trophies.empty()) {
		const auto filename = "/app0/" + Trophies::PackagePath(key.second).generic_string();
		package = Trophies::LoadPackage(LibKernel::FileSystem::GetRealFilename(filename),
		                                Config::GetConsoleLanguage());
		if (package.trophies.empty()) {
			LOGF("[Trophy] could not load trophy package %s for %s\n", filename.c_str(),
			     title_id.c_str());
			return nullptr;
		}
		LOGF("[Trophy] loaded %zu trophy definitions for %s (service %u)\n",
		     package.trophies.size(), title_id.c_str(), key.second);
		const auto uds_filename = "/app0/" + Trophies::UdsPackagePath(key.second).generic_string();
		package.event_stats     = Trophies::LoadUdsEventStats(
            LibKernel::FileSystem::GetRealFilename(uds_filename));
		if (package.event_stats.empty()) {
			LOGF("[Trophy] no usable UDS event mapping in %s; using name matching\n",
			     uds_filename.c_str());
		} else {
			LOGF("[Trophy] loaded %zu UDS event mappings from %s\n", package.event_stats.size(),
			     uds_filename.c_str());
		}
	}
	auto path     = Trophies::UnlocksPath({}, title_id, key.first, key.second);
	auto unlocks = Trophies::LoadUnlockData(path);
	return &g_states.emplace(key, TrophyState {&package, std::move(unlocks), std::move(path)})
	            .first->second;
}

static int GetTrophyState(int context, TrophyState** state) {
	const auto it = g_contexts.find(context);
	if (it == g_contexts.end()) {
		return NP_TROPHY2_ERROR_INVALID_CONTEXT;
	}
	*state = it->second.state;
	return *state == nullptr ? NP_TROPHY2_ERROR_NOT_REGISTERED : 0;
}

static void FillTrophyInfo(const TrophyState& state, const Trophies::Trophy& trophy,
                           NpTrophy2Details* details, NpTrophy2Data* data) {
	if (details != nullptr) {
		*details              = {};
		details->trophy_id    = trophy.id;
		details->trophy_grade = trophy.grade;
		details->group_id     = trophy.group_id;
		details->hidden       = trophy.hidden;
		details->has_reward   = trophy.has_reward;
		if (trophy.progressive && trophy.target) {
			details->target.type  = 1;
			details->target.value = *trophy.target;
		}
		std::snprintf(details->name, sizeof(details->name), "%s", trophy.name.c_str());
		std::snprintf(details->description, sizeof(details->description), "%s",
		              trophy.description.c_str());
		std::snprintf(details->reward, sizeof(details->reward), "%s", trophy.reward.c_str());
	}
	if (data != nullptr) {
		*data           = {};
		data->trophy_id = trophy.id;
		data->unlocked  = state.unlocks.unlocked.contains(trophy.id);
		if (trophy.progressive && trophy.target) {
			data->progress.type  = 1;
			data->progress.value = data->unlocked ? *trophy.target : 0;
		}
	}
}

// Game and group structures share the same count fields.
template <typename Details, typename Data>
static void FillTrophyCounts(const TrophyState& state, int group, Details* details, Data* data) {
	std::array<uint32_t, 5> total {}, unlocked {};
	for (const auto& [id, trophy]: state.package->trophies) {
		if (group != -2 && trophy.group_id != group) {
			continue;
		}
		++total[trophy.grade];
		if (state.unlocks.unlocked.contains(id)) {
			++unlocked[trophy.grade];
		}
	}
	if (details != nullptr) {
		*details              = {};
		details->num_trophies = total[1] + total[2] + total[3] + total[4];
		details->num_platinum = total[1];
		details->num_gold     = total[2];
		details->num_silver   = total[3];
		details->num_bronze   = total[4];
	}
	if (data != nullptr) {
		*data                     = {};
		data->unlocked_trophies   = unlocked[1] + unlocked[2] + unlocked[3] + unlocked[4];
		data->unlocked_platinum   = unlocked[1];
		data->unlocked_gold       = unlocked[2];
		data->unlocked_silver     = unlocked[3];
		data->unlocked_bronze     = unlocked[4];
		const auto points         = total[2] * 6 + total[3] * 2 + total[4];
		const auto earned         = unlocked[2] * 6 + unlocked[3] * 2 + unlocked[4];
		data->progress_percentage = points == 0 ? 0 : earned * 100 / points;
	}
}

static void FillGroupInfo(const TrophyState& state, int group_id, const std::string& title,
                          NpTrophy2GroupDetails* details, NpTrophy2GroupData* data) {
	FillTrophyCounts(state, group_id, details, data);
	if (details != nullptr) {
		details->group_id = group_id;
		std::snprintf(details->title, sizeof(details->title), "%s", title.c_str());
	}
	if (data != nullptr) {
		data->group_id = group_id;
	}
}

static void RecordTrophyUnlock(TrophyState& state, int trophy_id, bool from_counter = false) {
	const auto found = state.package->trophies.find(trophy_id);
	if (found == state.package->trophies.end()) {
		LOGF("[Trophy] ignored unknown trophy ID %d\n", trophy_id);
		return;
	}
	if ((!from_counter && (found->second.grade == 1 || found->second.progressive)) ||
	    !state.unlocks.unlocked.insert(trophy_id).second) {
		LOGF("[Trophy] trophy %d already unlocked or requires counter progress\n", trophy_id);
		return;
	}
	const auto date = Common::DateTime::FromSystem().ToString("YYYY-MM-DD HH24:MI:SS");
	if (!date.empty()) {
		state.unlocks.dates.emplace(trophy_id, date);
	}
	std::vector<int> earned {trophy_id};
	const auto       platinum_id = found->second.platinum_id;
	if (platinum_id >= 0 && state.package->trophies.contains(platinum_id) &&
	    std::all_of(state.package->trophies.begin(), state.package->trophies.end(),
	                [&](const auto& entry) {
		                return entry.second.platinum_id != platinum_id ||
		                       state.unlocks.unlocked.contains(entry.first);
	                }) &&
	    state.unlocks.unlocked.insert(platinum_id).second) {
		if (!date.empty()) {
			state.unlocks.dates.emplace(platinum_id, date);
		}
		earned.push_back(platinum_id);
	}
	if (!Trophies::SaveUnlockData(state.path, state.unlocks)) {
		LOGF("[Trophy] could not save unlocks to %s\n", state.path.string().c_str());
	} else {
		LOGF("[Trophy] recorded unlock %d: %s\n", trophy_id, found->second.name.c_str());
	}
	if (Config::TrophyEnabled()) {
		for (const auto id: earned) {
			const auto& trophy = state.package->trophies.at(id);
			Graphics::NotifyTrophyUnlocked(trophy.name, trophy.grade, trophy.icon_png);
		}
	} else {
		LOGF("[Trophy] notification disabled by configuration\n");
	}
}

static int KYTY_SYSV_ABI NpTrophy2CreateHandle(int* handle) {
	PRINT_NAME();
	if (handle == nullptr) {
		return NP_TROPHY2_ERROR_INVALID_ARGUMENT;
	}
	*handle = 1;
	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2CreateContext(int* context, int user_id, uint32_t service_label,
                                                uint64_t options) {
	PRINT_NAME();
	if (context == nullptr || options != 0 || user_id < 0) {
		return NP_TROPHY2_ERROR_INVALID_ARGUMENT;
	}
	std::scoped_lock lock(g_trophy_mutex);
	const TrophyKey  key {user_id, service_label};
	for (const auto& [id, value]: g_contexts) {
		if (value.key == key) {
			return static_cast<int>(0x80553910); // CONTEXT_ALREADY_EXISTS
		}
	}
	if (g_contexts.size() == 8) {
		return static_cast<int>(0x8055391b); // CONTEXT_EXCEEDS_MAX
	}
	*context = g_next_context++;
	g_contexts.emplace(*context, TrophyContext {key});
	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2RegisterContext(int context, int handle, uint64_t options) {
	PRINT_NAME();
	std::scoped_lock lock(g_trophy_mutex);
	const auto       it = g_contexts.find(context);
	if (it == g_contexts.end()) {
		return NP_TROPHY2_ERROR_INVALID_CONTEXT;
	}
	if (options != 0) {
		return NP_TROPHY2_ERROR_INVALID_ARGUMENT;
	}
	if (it->second.state != nullptr) {
		return static_cast<int>(0x80553921); // ALREADY_REGISTERED
	}
	it->second.state = LoadTrophyState(it->second.key);
	return it->second.state != nullptr ? 0
	                                   : static_cast<int>(0x8055391e); // TITLE_CONF_NOT_INSTALLED
}

static int KYTY_SYSV_ABI NpTrophy2GetGameInfo(int context, int handle,
                                              NpTrophy2GameDetails* details,
                                              NpTrophy2GameData*    data) {
	PRINT_NAME();
	if (details == nullptr && data == nullptr) {
		return NP_TROPHY2_ERROR_INVALID_ARGUMENT;
	}
	std::scoped_lock lock(g_trophy_mutex);
	TrophyState*     state = nullptr;
	if (const int error = GetTrophyState(context, &state); error != 0) {
		return error;
	}
	FillTrophyCounts(*state, -2, details, data);
	if (details != nullptr) {
		details->num_groups = state->package->groups.size();
		std::snprintf(details->title, sizeof(details->title), "%s", state->package->title.c_str());
	}
	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetGroupInfo(int context, int handle, int group_id,
                                               NpTrophy2GroupDetails* details,
                                               NpTrophy2GroupData*    data) {
	PRINT_NAME();
	if (details == nullptr && data == nullptr) {
		return NP_TROPHY2_ERROR_INVALID_ARGUMENT;
	}
	std::scoped_lock lock(g_trophy_mutex);
	TrophyState*     state = nullptr;
	if (const int error = GetTrophyState(context, &state); error != 0) {
		return error;
	}
	const auto group = state->package->groups.find(group_id);
	if (group == state->package->groups.end()) {
		return NP_TROPHY2_ERROR_INVALID_GROUP_ID;
	}
	FillGroupInfo(*state, group_id, group->second, details, data);
	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetGroupInfoArray(int context, int handle, uint32_t offset,
                                                    uint32_t               limit,
                                                    NpTrophy2GroupDetails* details_array,
                                                    NpTrophy2GroupData*    data_array,
                                                    uint32_t*              count) {
	PRINT_NAME();
	if (count == nullptr || (details_array == nullptr && data_array == nullptr)) {
		return NP_TROPHY2_ERROR_INVALID_ARGUMENT;
	}
	std::scoped_lock lock(g_trophy_mutex);
	TrophyState*     state = nullptr;
	if (const int error = GetTrophyState(context, &state); error != 0) {
		return error;
	}
	const auto& groups = state->package->groups;
	auto        it     = groups.begin();
	std::advance(it, std::min<size_t>(offset, groups.size()));
	*count = 0;
	for (; it != groups.end() && *count < limit; ++it, ++*count) {
		FillGroupInfo(*state, it->first, it->second,
		              details_array != nullptr ? &details_array[*count] : nullptr,
		              data_array != nullptr ? &data_array[*count] : nullptr);
	}
	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetTrophyInfo(int context, int handle, int trophy_id,
                                                NpTrophy2Details* details, NpTrophy2Data* data) {
	PRINT_NAME();
	if (details == nullptr && data == nullptr) {
		return NP_TROPHY2_ERROR_INVALID_ARGUMENT;
	}
	std::scoped_lock lock(g_trophy_mutex);
	TrophyState*     state = nullptr;
	if (const int error = GetTrophyState(context, &state); error != 0) {
		return error;
	}
	const auto trophy = state->package->trophies.find(trophy_id);
	if (trophy == state->package->trophies.end()) {
		return NP_TROPHY2_ERROR_INVALID_TROPHY_ID;
	}
	FillTrophyInfo(*state, trophy->second, details, data);
	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetTrophyInfoArray(int context, int handle, uint32_t offset,
                                                     uint32_t          limit,
                                                     NpTrophy2Details* details_array,
                                                     NpTrophy2Data* data_array, uint32_t* count) {
	PRINT_NAME();
	if (count == nullptr || (details_array == nullptr && data_array == nullptr)) {
		return NP_TROPHY2_ERROR_INVALID_ARGUMENT;
	}
	std::scoped_lock lock(g_trophy_mutex);
	TrophyState*     state = nullptr;
	if (const int error = GetTrophyState(context, &state); error != 0) {
		return error;
	}
	const auto& trophies = state->package->trophies;
	auto        it       = trophies.begin();
	std::advance(it, std::min<size_t>(offset, trophies.size()));
	*count = 0;
	for (; it != trophies.end() && *count < limit; ++it, ++*count) {
		FillTrophyInfo(*state, it->second,
		               details_array != nullptr ? &details_array[*count] : nullptr,
		               data_array != nullptr ? &data_array[*count] : nullptr);
	}
	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetGameIcon(int context, int handle, void* buffer, size_t* size) {
	PRINT_NAME();

	LOGF("\t context = %d\n"
	     "\t handle  = %d\n"
	     "\t buffer  = 0x%016" PRIx64 "\n"
	     "\t size    = 0x%016" PRIx64 "\n",
	     context, handle, reinterpret_cast<uint64_t>(buffer), reinterpret_cast<uint64_t>(size));

	if (size != nullptr) {
		*size = 0;
	}

	return NP_TROPHY2_ERROR_ICON_FILE_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpTrophy2GetGroupIcon(int context, int handle, int group_id, void* buffer,
                                               size_t* size) {
	PRINT_NAME();

	LOGF("\t context  = %d\n"
	     "\t handle   = %d\n"
	     "\t group_id = %d\n"
	     "\t buffer   = 0x%016" PRIx64 "\n"
	     "\t size     = 0x%016" PRIx64 "\n",
	     context, handle, group_id, reinterpret_cast<uint64_t>(buffer),
	     reinterpret_cast<uint64_t>(size));

	if (size != nullptr) {
		*size = 0;
	}

	return NP_TROPHY2_ERROR_ICON_FILE_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpTrophy2GetTrophyIcon(int context, int handle, int trophy_id,
                                                void* buffer, size_t* size) {
	PRINT_NAME();

	LOGF("\t context   = %d\n"
	     "\t handle    = %d\n"
	     "\t trophy_id = %d\n"
	     "\t buffer    = 0x%016" PRIx64 "\n"
	     "\t size      = 0x%016" PRIx64 "\n",
	     context, handle, trophy_id, reinterpret_cast<uint64_t>(buffer),
	     reinterpret_cast<uint64_t>(size));

	if (size != nullptr) {
		*size = 0;
	}

	return NP_TROPHY2_ERROR_ICON_FILE_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpTrophy2RegisterUnlockCallback(void* callback, void* userdata) {
	PRINT_NAME();

	LOGF("\t callback = 0x%016" PRIx64 "\n"
	     "\t userdata = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(callback), reinterpret_cast<uint64_t>(userdata));

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2AbortHandle(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2DestroyHandle(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2DestroyContext(int context) {
	PRINT_NAME();
	std::scoped_lock lock(g_trophy_mutex);
	return g_contexts.erase(context) != 0 ? 0 : NP_TROPHY2_ERROR_INVALID_CONTEXT;
}

LIB_DEFINE(InitNet_1_NpTrophy2) {
	LIB_FUNC("Bagshr7OQ6Q", LibNpTrophy2::NpTrophy2CreateContext);
	LIB_FUNC("Gz1rmUZpROM", LibNpTrophy2::NpTrophy2CreateHandle);
	LIB_FUNC("bIDov3wBu5Q", LibNpTrophy2::NpTrophy2RegisterContext);
	LIB_FUNC("4IzqhhUQ3nk", LibNpTrophy2::NpTrophy2GetGameInfo);
	LIB_FUNC("DoZWauG8mu0", LibNpTrophy2::NpTrophy2GetGroupInfo);
	LIB_FUNC("+PDSI6WgPRc", LibNpTrophy2::NpTrophy2GetGroupInfoArray);
	LIB_FUNC("EwNylPdWUTM", LibNpTrophy2::NpTrophy2GetTrophyInfo);
	LIB_FUNC("y3zHpdZO6ME", LibNpTrophy2::NpTrophy2GetTrophyInfoArray);
	LIB_FUNC("2QgUy+xJqS0", LibNpTrophy2::NpTrophy2GetGameIcon);
	LIB_FUNC("6IjXJUy6ZnA", LibNpTrophy2::NpTrophy2GetGroupIcon);
	LIB_FUNC("-9LLVU0uvs8", LibNpTrophy2::NpTrophy2GetTrophyIcon);
	LIB_FUNC("sUXGfNMalIo", LibNpTrophy2::NpTrophy2RegisterUnlockCallback);
	LIB_FUNC("fYapWA9xVmA", LibNpTrophy2::NpTrophy2AbortHandle);
	LIB_FUNC("d8P11CI40KE", LibNpTrophy2::NpTrophy2DestroyHandle);
	LIB_FUNC("sysY2FHYff4", LibNpTrophy2::NpTrophy2DestroyContext);
}

} // namespace LibNpTrophy2

namespace LibNpUniversalDataSystem {

LIB_VERSION("NpUniversalDataSystem", 1, "NpUniversalDataSystem", 1, 1);

constexpr int NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT = -2141900542; /* 0x80553102 */

struct NpUniversalDataSystemInitParam {
	size_t size;
	size_t pool_size;
};

struct NpUniversalDataSystemMemoryStat {
	size_t pool_size;
	size_t max_inuse_size;
	size_t current_inuse_size;
};

struct NpUniversalDataSystemEventPropertyObject {
	std::optional<int32_t>  trophy_id;
	std::optional<uint64_t> counter;
	std::optional<uint64_t> counter_base;

	void SetTrophyId(const char* key, std::optional<int32_t> value) {
		if (std::strcmp(key, "_trophy_id") == 0) {
			trophy_id = value;
		}
	}

	void SetCounter(const char* key, std::optional<uint64_t> value) {
		if (std::strcmp(key, "counter") == 0) {
			counter = value;
		} else if (std::strcmp(key, "counterBase") == 0) {
			counter_base = value;
		}
	}

	void SetInteger(const char* key, int64_t value) {
		SetTrophyId(key, value >= 0 && value <= INT32_MAX
		                     ? std::optional<int32_t>(static_cast<int32_t>(value))
		                     : std::nullopt);
		SetCounter(key, value >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(value))
		                           : std::nullopt);
	}

	void SetUnsigned(const char* key, uint64_t value) {
		SetTrophyId(key, value <= INT32_MAX ? std::optional<int32_t>(static_cast<int32_t>(value))
		                                    : std::nullopt);
		SetCounter(key, value);
	}

	void ClearInteger(const char* key) {
		SetTrophyId(key, std::nullopt);
		SetCounter(key, std::nullopt);
	}
};

struct NpUniversalDataSystemEvent {
	std::string                              name;
	NpUniversalDataSystemEventPropertyObject properties;
};

struct UdsContext {
	LibNpTrophy2::TrophyKey key;
	bool                    registered = false;
};
static std::map<int, UdsContext> g_contexts;
static int                       g_next_context = 1;

struct NpUniversalDataSystemEventPropertyArray {};

struct NpUniversalDataSystemStorageStat {
	size_t in_events;
	size_t out_events;
	size_t lost_events;
	size_t max_inuse_size;
	size_t current_events;
	size_t current_inuse_size;
	size_t current_free_size;
};

static int KYTY_SYSV_ABI
NpUniversalDataSystemInitialize(const NpUniversalDataSystemInitParam* param) {
	PRINT_NAME();

	if (param == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t size      = %" PRIu64 "\n"
	     "\t pool_size = %" PRIu64 "\n",
	     static_cast<uint64_t>(param->size), static_cast<uint64_t>(param->pool_size));

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemCreateContext(int* context, int user_id,
                                                            uint32_t service_label,
                                                            uint64_t options) {
	PRINT_NAME();
	if (context == nullptr || options != 0) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	if (user_id < 0) {
		return static_cast<int>(0x8055310a); // INVALID_USER
	}
	std::scoped_lock lock(LibNpTrophy2::g_trophy_mutex);
	if (g_contexts.size() == 8) {
		return static_cast<int>(0x80553105); // CONTEXT_EXCEEDS_MAX
	}
	*context = g_next_context++;
	g_contexts.emplace(*context, UdsContext {{user_id, service_label}});
	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemCreateHandle(int* handle) {
	PRINT_NAME();

	if (handle == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	if (handle != nullptr) {
		*handle = 1;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemDestroyHandle(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemAbortHandle(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemCreateEvent(
    const char* event_name, const NpUniversalDataSystemEventPropertyObject* prop,
    NpUniversalDataSystemEvent** new_event, NpUniversalDataSystemEventPropertyObject** prop_ptr) {
	PRINT_NAME();
	if (event_name == nullptr || new_event == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	*new_event = new NpUniversalDataSystemEvent {
	    event_name, prop != nullptr ? *prop : NpUniversalDataSystemEventPropertyObject {}};
	LOGF("[UDS] created event: %s\n", event_name);
	if (prop_ptr != nullptr) {
		*prop_ptr = &(*new_event)->properties;
	}
	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemPostEvent(int context, int handle, const void* event,
                                                        uint64_t options) {
	PRINT_NAME();
	if (event == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	std::scoped_lock lock(LibNpTrophy2::g_trophy_mutex);
	const auto       it = g_contexts.find(context);
	if (it == g_contexts.end()) {
		return static_cast<int>(0x80553104); // INVALID_CONTEXT
	}
	if (!it->second.registered) {
		return static_cast<int>(0x80553120); // NOT_REGISTERED
	}
	const auto& uds_event = *static_cast<const NpUniversalDataSystemEvent*>(event);
	LOGF("[UDS] posted event: %s (counter=%s%" PRIu64 ", counterBase=%s%" PRIu64 ")\n",
	     uds_event.name.c_str(), uds_event.properties.counter ? "" : "<unset>",
	     uds_event.properties.counter.value_or(0),
	     uds_event.properties.counter_base ? "" : "<unset>",
	     uds_event.properties.counter_base.value_or(0));
	if (uds_event.name == "_UnlockTrophy") {
		const auto trophy_id = uds_event.properties.trophy_id;
		if (!trophy_id || *trophy_id < 0) {
			return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
		}
		if (auto* state = LibNpTrophy2::LoadTrophyState(it->second.key); state != nullptr) {
			LibNpTrophy2::RecordTrophyUnlock(*state, *trophy_id);
		}
	} else if (uds_event.properties.counter) {
		if (auto* state = LibNpTrophy2::LoadTrophyState(it->second.key); state != nullptr) {
			const auto base = uds_event.properties.counter_base.value_or(0);
			const auto count =
			    *uds_event.properties.counter > UINT64_MAX - base
			        ? UINT64_MAX
			        : *uds_event.properties.counter + base;
			const auto ids =
			    LibNpTrophy2::Trophies::FindUdsTrophies(*state->package, uds_event.name, count);
			for (const auto trophy_id: ids) {
				LibNpTrophy2::RecordTrophyUnlock(*state, trophy_id, true);
			}
			if (ids.empty()) {
				LOGF("[Trophy] no trophy match for UDS event %s at counter %" PRIu64 "\n",
				     uds_event.name.c_str(), count);
			}
		}
	}
	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemEventEstimateSize(const NpUniversalDataSystemEvent* event, size_t* size) {
	PRINT_NAME();

	LOGF("\t event = 0x%016" PRIx64 "\n"
	     "\t size  = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(event), reinterpret_cast<uint64_t>(size));

	if (event == nullptr || size == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*size = 3;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventToString(const NpUniversalDataSystemEvent* event,
                                                            char* buf, size_t buf_size,
                                                            size_t* string_size) {
	PRINT_NAME();

	LOGF("\t event       = 0x%016" PRIx64 "\n"
	     "\t buf         = 0x%016" PRIx64 "\n"
	     "\t buf_size    = %" PRIu64 "\n"
	     "\t string_size = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(event), reinterpret_cast<uint64_t>(buf),
	     static_cast<uint64_t>(buf_size), reinterpret_cast<uint64_t>(string_size));

	if (event == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	const char* json = "{}";
	if (string_size != nullptr) {
		*string_size = std::strlen(json) + 1;
	}
	if (buf != nullptr && buf_size > 0) {
		std::snprintf(buf, buf_size, "%s", json);
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemDestroyEvent(NpUniversalDataSystemEvent* event) {
	PRINT_NAME();

	LOGF("\t event = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(event));

	delete event;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemRegisterContext(int context, int handle,
                                                              uint64_t options) {
	PRINT_NAME();
	std::scoped_lock lock(LibNpTrophy2::g_trophy_mutex);
	const auto       it = g_contexts.find(context);
	if (it == g_contexts.end()) {
		return static_cast<int>(0x80553104); // INVALID_CONTEXT
	}
	if (options != 0) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	if (it->second.registered) {
		return static_cast<int>(0x80553121); // ALREADY_REGISTERED
	}
	it->second.registered = true;
	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemDestroyContext(int context) {
	PRINT_NAME();
	std::scoped_lock lock(LibNpTrophy2::g_trophy_mutex);
	return g_contexts.erase(context) != 0 ? 0 : static_cast<int>(0x80553104);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemGetMemoryStat(NpUniversalDataSystemMemoryStat* stat) {
	PRINT_NAME();

	if (stat == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*stat = {};

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemCreateEventPropertyObject(
    NpUniversalDataSystemEventPropertyObject** new_object) {
	PRINT_NAME();

	if (new_object == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*new_object = new NpUniversalDataSystemEventPropertyObject;

	LOGF("\t new_object = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(*new_object));

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemDestroyEventPropertyObject(NpUniversalDataSystemEventPropertyObject* object) {
	PRINT_NAME();

	LOGF("\t object = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(object));

	delete object;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetString(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, const char* value) {
	PRINT_NAME();

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %s\n",
	     reinterpret_cast<uint64_t>(object), key != nullptr ? key : "<null>",
	     value != nullptr ? value : "<null>");

	if (object == nullptr || key == nullptr || value == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->ClearInteger(key);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetInt32(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, int32_t value) {
	PRINT_NAME();

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %" PRId32 "\n",
	     reinterpret_cast<uint64_t>(object), key != nullptr ? key : "<null>", value);

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->SetInteger(key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetUInt32(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, uint32_t value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->SetUnsigned(key, value);

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(object), key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetInt64(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, int64_t value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->SetInteger(key, value);

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %" PRId64 "\n",
	     reinterpret_cast<uint64_t>(object), key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetUInt64(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, uint64_t value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->SetUnsigned(key, value);

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(object), key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetFloat32(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, float value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->ClearInteger(key);

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %f\n",
	     reinterpret_cast<uint64_t>(object), key, static_cast<double>(value));

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetFloat64(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, double value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->ClearInteger(key);

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %f\n",
	     reinterpret_cast<uint64_t>(object), key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetBool(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, bool value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->ClearInteger(key);

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %d\n",
	     reinterpret_cast<uint64_t>(object), key, value ? 1 : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetBinary(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, const void* value,
    size_t value_size) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr || (value == nullptr && value_size != 0)) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->ClearInteger(key);

	LOGF("\t object     = 0x%016" PRIx64 "\n"
	     "\t key        = %s\n"
	     "\t value      = 0x%016" PRIx64 "\n"
	     "\t value_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(object), key, reinterpret_cast<uint64_t>(value),
	     static_cast<uint64_t>(value_size));

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetObject(
    NpUniversalDataSystemEventPropertyObject* object, const char* key,
    const NpUniversalDataSystemEventPropertyObject* value,
    NpUniversalDataSystemEventPropertyObject**      value_ptr) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->ClearInteger(key);

	if (value_ptr != nullptr) {
		*value_ptr =
		    (value != nullptr ? const_cast<NpUniversalDataSystemEventPropertyObject*>(value)
		                      : new NpUniversalDataSystemEventPropertyObject);
	}

	LOGF("\t object    = 0x%016" PRIx64 "\n"
	     "\t key       = %s\n"
	     "\t value     = 0x%016" PRIx64 "\n"
	     "\t value_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(object), key, reinterpret_cast<uint64_t>(value),
	     value_ptr != nullptr ? reinterpret_cast<uint64_t>(*value_ptr) : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetArray(
    NpUniversalDataSystemEventPropertyObject* object, const char* key,
    const NpUniversalDataSystemEventPropertyArray* value,
    NpUniversalDataSystemEventPropertyArray**      value_ptr) {
	PRINT_NAME();

	LOGF("\t object    = 0x%016" PRIx64 "\n"
	     "\t key       = %s\n"
	     "\t value     = 0x%016" PRIx64 "\n"
	     "\t value_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(object), key != nullptr ? key : "<null>",
	     reinterpret_cast<uint64_t>(value), reinterpret_cast<uint64_t>(value_ptr));

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->ClearInteger(key);

	if (value_ptr != nullptr) {
		*value_ptr = (value != nullptr ? const_cast<NpUniversalDataSystemEventPropertyArray*>(value)
		                               : new NpUniversalDataSystemEventPropertyArray);
	}

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemCreateEventPropertyArray(NpUniversalDataSystemEventPropertyArray** new_array) {
	PRINT_NAME();

	if (new_array == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*new_array = new NpUniversalDataSystemEventPropertyArray;

	LOGF("\t new_array = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(*new_array));

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemDestroyEventPropertyArray(NpUniversalDataSystemEventPropertyArray* array) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(array));

	delete array;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetString(
    NpUniversalDataSystemEventPropertyArray* array, const char* value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %s\n",
	     reinterpret_cast<uint64_t>(array), value != nullptr ? value : "<null>");

	if (array == nullptr || value == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetInt32(
    NpUniversalDataSystemEventPropertyArray* array, int32_t value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %" PRId32 "\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetUInt32(
    NpUniversalDataSystemEventPropertyArray* array, uint32_t value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetInt64(
    NpUniversalDataSystemEventPropertyArray* array, int64_t value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %" PRId64 "\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetUInt64(
    NpUniversalDataSystemEventPropertyArray* array, uint64_t value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetFloat32(
    NpUniversalDataSystemEventPropertyArray* array, float value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %f\n",
	     reinterpret_cast<uint64_t>(array), static_cast<double>(value));

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetFloat64(
    NpUniversalDataSystemEventPropertyArray* array, double value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %f\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetBool(
    NpUniversalDataSystemEventPropertyArray* array, bool value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %d\n",
	     reinterpret_cast<uint64_t>(array), value ? 1 : 0);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetBinary(
    NpUniversalDataSystemEventPropertyArray* array, const void* value, size_t value_size) {
	PRINT_NAME();

	LOGF("\t array      = 0x%016" PRIx64 "\n"
	     "\t value      = 0x%016" PRIx64 "\n"
	     "\t value_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(array), reinterpret_cast<uint64_t>(value),
	     static_cast<uint64_t>(value_size));

	if (array == nullptr || (value == nullptr && value_size != 0)) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetObject(
    NpUniversalDataSystemEventPropertyArray*        array,
    const NpUniversalDataSystemEventPropertyObject* value,
    NpUniversalDataSystemEventPropertyObject**      value_ptr) {
	PRINT_NAME();

	if (array == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	if (value_ptr != nullptr) {
		*value_ptr =
		    (value != nullptr ? const_cast<NpUniversalDataSystemEventPropertyObject*>(value)
		                      : new NpUniversalDataSystemEventPropertyObject);
	}

	LOGF("\t array     = 0x%016" PRIx64 "\n"
	     "\t value     = 0x%016" PRIx64 "\n"
	     "\t value_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(array), reinterpret_cast<uint64_t>(value),
	     value_ptr != nullptr ? reinterpret_cast<uint64_t>(*value_ptr) : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetArray(
    NpUniversalDataSystemEventPropertyArray*       array,
    const NpUniversalDataSystemEventPropertyArray* value,
    NpUniversalDataSystemEventPropertyArray**      value_ptr) {
	PRINT_NAME();

	if (array == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	if (value_ptr != nullptr) {
		*value_ptr = (value != nullptr ? const_cast<NpUniversalDataSystemEventPropertyArray*>(value)
		                               : new NpUniversalDataSystemEventPropertyArray);
	}

	LOGF("\t array     = 0x%016" PRIx64 "\n"
	     "\t value     = 0x%016" PRIx64 "\n"
	     "\t value_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(array), reinterpret_cast<uint64_t>(value),
	     value_ptr != nullptr ? reinterpret_cast<uint64_t>(*value_ptr) : 0);

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemGetStorageStat(int context, NpUniversalDataSystemStorageStat* stat) {
	PRINT_NAME();

	LOGF("\t context = %d\n"
	     "\t stat    = 0x%016" PRIx64 "\n",
	     context, reinterpret_cast<uint64_t>(stat));

	if (stat == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*stat = {};

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemTerminate() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitNet_1_NpUniversalDataSystem) {
	LIB_FUNC("sjaobBgqeB4", LibNpUniversalDataSystem::NpUniversalDataSystemInitialize);
	LIB_FUNC("5zBnau1uIEo", LibNpUniversalDataSystem::NpUniversalDataSystemCreateContext);
	LIB_FUNC("hT0IAEvN+M0", LibNpUniversalDataSystem::NpUniversalDataSystemCreateHandle);
	LIB_FUNC("p+GcLqwpL9M", LibNpUniversalDataSystem::NpUniversalDataSystemCreateEvent);
	LIB_FUNC("CzkKf7ahIyU", LibNpUniversalDataSystem::NpUniversalDataSystemPostEvent);
	LIB_FUNC("AUIHb7jUX3I", LibNpUniversalDataSystem::NpUniversalDataSystemDestroyHandle);
	LIB_FUNC("jZCqWFgMehE", LibNpUniversalDataSystem::NpUniversalDataSystemAbortHandle);
	LIB_FUNC("wB7IWzGp2v0", LibNpUniversalDataSystem::NpUniversalDataSystemDestroyContext);
	LIB_FUNC("su7jW3VDDb4", LibNpUniversalDataSystem::NpUniversalDataSystemGetMemoryStat);
	LIB_FUNC("+s14jq-KGYw", LibNpUniversalDataSystem::NpUniversalDataSystemEventEstimateSize);
	LIB_FUNC("vj6CQGWtEBg", LibNpUniversalDataSystem::NpUniversalDataSystemEventToString);
	LIB_FUNC("wG+84pnNIuo", LibNpUniversalDataSystem::NpUniversalDataSystemDestroyEvent);
	LIB_FUNC("tpFJ8LIKvPw", LibNpUniversalDataSystem::NpUniversalDataSystemRegisterContext);
	LIB_FUNC("s6W4Zl4Slgk",
	         LibNpUniversalDataSystem::NpUniversalDataSystemCreateEventPropertyObject);
	LIB_FUNC("kKUH0Viib3c",
	         LibNpUniversalDataSystem::NpUniversalDataSystemDestroyEventPropertyObject);
	LIB_FUNC("MfDb+4Nln64",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetString);
	LIB_FUNC("YE4dbtbz6OE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetInt32);
	LIB_FUNC("AzD4irAcKE4",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetUInt32);
	LIB_FUNC("56QLTqx911s",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetInt64);
	LIB_FUNC("xvsP5Yz6FmY",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetUInt64);
	LIB_FUNC("lbPlT4+QVcE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetFloat32);
	LIB_FUNC("4Fu8tHW+u-k",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetFloat64);
	LIB_FUNC("Fidd8vWgyVE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetBool);
	LIB_FUNC("wAcxBDLHj1M",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetBinary);
	LIB_FUNC("74ASEqxSnkM",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetObject);
	LIB_FUNC("Wxbg5x3pTXA",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetArray);
	LIB_FUNC("Hm7qubT3b70",
	         LibNpUniversalDataSystem::NpUniversalDataSystemCreateEventPropertyArray);
	LIB_FUNC("W-0xwY0ZMjw",
	         LibNpUniversalDataSystem::NpUniversalDataSystemDestroyEventPropertyArray);
	LIB_FUNC("4llLk7YJRTE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetString);
	LIB_FUNC("BypQuF113-k",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetInt32);
	LIB_FUNC("yMi0xAOpmXM",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetUInt32);
	LIB_FUNC("viVXAwmmYrY",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetInt64);
	LIB_FUNC("Qo9qR7v5zO4",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetUInt64);
	LIB_FUNC("JmgwKm96Lq4",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetFloat32);
	LIB_FUNC("sbSYZLR5AiE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetFloat64);
	LIB_FUNC("0+l4QSWCM4E",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetBool);
	LIB_FUNC("IEdUCV9j2Cw",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetBinary);
	LIB_FUNC("XY14n3jNIpE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetObject);
	LIB_FUNC("rdi9BAfDLq8",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetArray);
	LIB_FUNC("KmN62tT4U8A", LibNpUniversalDataSystem::NpUniversalDataSystemGetStorageStat);
	LIB_FUNC("47UAEuQl+iI", LibNpUniversalDataSystem::NpUniversalDataSystemTerminate);
}

} // namespace LibNpUniversalDataSystem

} // namespace Libs
