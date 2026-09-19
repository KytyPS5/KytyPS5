#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMEMORY_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Libs::SaveData {

// Option bits of sceSaveDataSetupSaveDataMemory2(). SET_PARAM declares that init_param and
// init_icon are filled in; without it those pointers are not read at all, because a title that
// did not ask for them is free to leave them uninitialized.
static constexpr uint32_t SAVE_DATA_MEMORY_SET_PARAM     = 1u;
static constexpr uint32_t SAVE_DATA_MEMORY_DOUBLE_BUFFER = 2u;

static constexpr uint32_t SAVE_DATA_MEMORY_SLOT_COUNT   = 4;
static constexpr uint32_t SAVE_DATA_MEMORY_DATA_NUM_MAX = 5;

struct SaveDataParam {
	char title[128];
	char sub_title[128];
	char detail[1024];
	uint32_t user_param;
	int pad;
	int64_t mtime;
	uint8_t reserved[32];
};

struct SaveDataIcon {
	void* buf;
	size_t buf_size;
	size_t data_size;
	uint8_t reserved[32];
};

struct SaveDataMemoryData {
	void* buf;
	size_t buf_size;
	int64_t offset;
	uint8_t reserved[40];
};

struct SaveDataMemoryGet2 {
	int32_t user_id;
	uint8_t padding[4];
	SaveDataMemoryData* data;
	SaveDataParam* param;
	SaveDataIcon* icon;
	uint32_t slot_id;
	uint8_t reserved[28];
};

struct SaveDataMemorySetup2 {
	uint32_t option;
	int32_t user_id;
	size_t memory_size;
	size_t icon_memory_size;
	const SaveDataParam* init_param;
	const SaveDataIcon* init_icon;
	uint32_t slot_id;
	uint8_t reserved[20];
};

struct SaveDataMemorySetupResult {
	size_t existed_memory_size;
	uint8_t reserved[16];
};

struct SaveDataMemorySet2 {
	int32_t user_id;
	uint8_t padding[4];
	const SaveDataMemoryData* data;
	const SaveDataParam* param;
	const SaveDataIcon* icon;
	uint32_t data_num;
	uint32_t slot_id;
	uint8_t reserved[24];
};

struct SaveDataMemorySync {
	int32_t user_id;
	uint32_t slot_id;
	uint32_t option;
	uint8_t reserved[28];
};

static_assert(sizeof(SaveDataParam) == 1328);
static_assert(sizeof(SaveDataMemorySetup2) == 64);
static_assert(sizeof(SaveDataMemoryGet2) == 64);
static_assert(sizeof(SaveDataMemorySet2) == 64);
static_assert(sizeof(SaveDataMemorySync) == 40);

// Directory a slot's save data memory lives in, below "<root>/<title id>/". PS4 names the first
// one sce_sdmemory and suffixes the rest, and the same name identifies the slot in the event
// queue, so both users of it share this one definition.
std::string MemoryDirectoryName(uint32_t slot_id);

class SaveDataMemoryStore {
public:
	explicit SaveDataMemoryStore(std::filesystem::path root);
	~SaveDataMemoryStore();
	SaveDataMemoryStore(const SaveDataMemoryStore&) = delete;
	SaveDataMemoryStore& operator=(const SaveDataMemoryStore&) = delete;

	int Setup(std::string_view title, const SaveDataMemorySetup2* request,
	          SaveDataMemorySetupResult* result);
	int Get(std::string_view title, SaveDataMemoryGet2* request);
	int Set(std::string_view title, const SaveDataMemorySet2* request);
	int Sync(std::string_view title, const SaveDataMemorySync* request);
	void Clear();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

#ifdef KYTY_SAVE_DATA_TESTS
namespace MemoryTesting {
enum class Stage { Read, Create, Write, Flush, Replace, DirectoryFlush };
using FaultHook = bool (*)(Stage);
void SetFaultHook(FaultHook hook);
}
#endif

} // namespace Libs::SaveData

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_SAVEDATAMEMORY_H_ */
