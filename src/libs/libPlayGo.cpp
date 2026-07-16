#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <algorithm>

namespace Libs {

LIB_VERSION("PlayGo", 1, "PlayGo", 1, 0);

namespace PlayGo {

static constexpr int      PLAYGO_HANDLE                 = 1;
static constexpr int8_t   PLAYGO_LOCUS_NOT_DOWNLOADED   = 0;
static constexpr int8_t   PLAYGO_LOCUS_LOCAL_SLOW       = 2;
static constexpr int8_t   PLAYGO_LOCUS_LOCAL_FAST       = 3;
static constexpr int32_t  PLAYGO_INSTALL_SPEED_FULL     = 2;
static constexpr int32_t  PLAYGO_OPTIONAL_TYPE_LANGUAGE = 0;
static constexpr int32_t  PLAYGO_OPTIONAL_TYPE_SCENARIO = 1;
static constexpr uint64_t PLAYGO_LANGUAGE_MASK_ALL      = 0xffffffffffffffffull;
static constexpr uint64_t PLAYGO_SCENARIO_MASK_ALL      = 0x1full;

static uint32_t g_chunks_num = 0;

struct PlayGoInitParams {
	const void* buf_addr;
	uint32_t    buf_size;
	uint32_t    reserved;
};

struct PlayGoToDo {
	uint16_t chunk_id;
	int8_t   locus;
	int8_t   reserved;
};

struct PlayGoProgress {
	uint64_t progress_size;
	uint64_t total_size;
};

union PlayGoOptionalChunk {
	uint64_t bitmask;
	uint64_t languages;
	uint64_t scenarios;
};

static bool ensure_chunks_loaded() {
	if (g_chunks_num != 0) {
		return true;
	}
	if (!Loader::SystemContentGetChunksNum(&g_chunks_num)) {
		LOGF("Warning: assume that chunks count is 1\n");
		g_chunks_num = 1;
	}
	return g_chunks_num != 0;
}

static bool is_valid_chunk(uint16_t chunk_id) {
	return ensure_chunks_loaded() && chunk_id < g_chunks_num;
}

static bool is_valid_locus(int8_t locus) {
	return locus == PLAYGO_LOCUS_NOT_DOWNLOADED || locus == PLAYGO_LOCUS_LOCAL_SLOW ||
	       locus == PLAYGO_LOCUS_LOCAL_FAST;
}

static int validate_handle(int handle) {
	return (handle == PLAYGO_HANDLE ? OK : PLAYGO_ERROR_BAD_HANDLE);
}

static int validate_optional_type(int32_t type) {
	return (type == PLAYGO_OPTIONAL_TYPE_LANGUAGE || type == PLAYGO_OPTIONAL_TYPE_SCENARIO
	            ? OK
	            : PLAYGO_ERROR_BAD_OPTIONAL_TYPE);
}

static void set_optional_chunk(int32_t type, PlayGoOptionalChunk* option) {
	EXIT_IF(option == nullptr);

	option->bitmask = (type == PLAYGO_OPTIONAL_TYPE_LANGUAGE ? PLAYGO_LANGUAGE_MASK_ALL
	                                                         : PLAYGO_SCENARIO_MASK_ALL);
}

int KYTY_SYSV_ABI PlayGoInitialize(const PlayGoInitParams* init) {
	PRINT_NAME();

	if (init == nullptr || init->buf_addr == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (init->buf_size < 2u * 1024u * 1024u) {
		return PLAYGO_ERROR_BAD_SIZE;
	}

	LOGF("\t buf_addr = %016" PRIx64 "\n"
	     "\t buf_size = %" PRIu32 "\n"
	     "\t reserved = %" PRId32 "\n",
	     reinterpret_cast<uint64_t>(init->buf_addr), init->buf_size, init->reserved);

	return OK;
}

int KYTY_SYSV_ABI PlayGoTerminate() {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI PlayGoOpen(int* out_handle, const void* param) {
	PRINT_NAME();

	if (out_handle == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (param != nullptr) {
		return PLAYGO_ERROR_INVALID_ARGUMENT;
	}

	*out_handle = PLAYGO_HANDLE;

	ensure_chunks_loaded();

	return OK;
}

int KYTY_SYSV_ABI PlayGoClose(int handle) {
	PRINT_NAME();

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetLocus(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries,
                                 int8_t* out_loci) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (chunk_ids == nullptr || out_loci == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries == 0) {
		return PLAYGO_ERROR_BAD_SIZE;
	}
	ensure_chunks_loaded();

	for (uint32_t i = 0; i < number_of_entries; i++) {
		LOGF("\t chunk_ids[%u] = %" PRIu16 "\n", i, chunk_ids[i]);

		if (is_valid_chunk(chunk_ids[i])) {
			out_loci[i] = PLAYGO_LOCUS_LOCAL_FAST;
		} else {
			return PLAYGO_ERROR_BAD_CHUNK_ID;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetInstallSpeed(int handle, int32_t* out_speed) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (out_speed == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}

	*out_speed = PLAYGO_INSTALL_SPEED_FULL;

	return OK;
}

int KYTY_SYSV_ABI PlayGoSetInstallSpeed(int handle, int32_t speed) {
	PRINT_NAME();

	LOGF("\t handle = %d\n"
	     "\t speed  = %" PRId32 "\n",
	     handle, speed);

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (speed < 0 || speed > 2) {
		return PLAYGO_ERROR_BAD_SPEED;
	}

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetChunkId(int handle, uint16_t* out_chunk_id_list,
                                   uint32_t number_of_entries, uint32_t* out_entries) {
	PRINT_NAME();

	LOGF("\t handle            = %d\n"
	     "\t out_chunk_id_list = 0x%016" PRIx64 "\n"
	     "\t number_of_entries = %" PRIu32 "\n"
	     "\t out_entries       = 0x%016" PRIx64 "\n",
	     handle, reinterpret_cast<uint64_t>(out_chunk_id_list), number_of_entries,
	     reinterpret_cast<uint64_t>(out_entries));

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (out_entries == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries != 0 && out_chunk_id_list == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	ensure_chunks_loaded();

	const uint32_t entries = std::min(number_of_entries, g_chunks_num);
	for (uint32_t i = 0; i < entries; i++) {
		out_chunk_id_list[i] = static_cast<uint16_t>(i);
	}
	*out_entries = entries;

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetToDoList(int handle, PlayGoToDo* out_todo_list,
                                    uint32_t number_of_entries, uint32_t* out_entries) {
	PRINT_NAME();

	LOGF("\t handle            = %d\n"
	     "\t out_todo_list     = 0x%016" PRIx64 "\n"
	     "\t number_of_entries = %" PRIu32 "\n"
	     "\t out_entries       = 0x%016" PRIx64 "\n",
	     handle, reinterpret_cast<uint64_t>(out_todo_list), number_of_entries,
	     reinterpret_cast<uint64_t>(out_entries));

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (out_entries == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries != 0 && out_todo_list == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	*out_entries = 0;

	return OK;
}

int KYTY_SYSV_ABI PlayGoSetToDoList(int handle, const PlayGoToDo* todo_list,
                                    uint32_t number_of_entries) {
	PRINT_NAME();

	LOGF("\t handle            = %d\n"
	     "\t todo_list         = 0x%016" PRIx64 "\n"
	     "\t number_of_entries = %" PRIu32 "\n",
	     handle, reinterpret_cast<uint64_t>(todo_list), number_of_entries);

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (todo_list == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries == 0) {
		return PLAYGO_ERROR_BAD_SIZE;
	}
	for (uint32_t i = 0; i < number_of_entries; i++) {
		if (!is_valid_chunk(todo_list[i].chunk_id)) {
			return PLAYGO_ERROR_BAD_CHUNK_ID;
		}
		if (!is_valid_locus(todo_list[i].locus)) {
			return PLAYGO_ERROR_BAD_LOCUS;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI PlayGoPrefetch(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries,
                                 int8_t minimum_locus) {
	PRINT_NAME();

	LOGF("\t handle            = %d\n"
	     "\t chunk_ids         = 0x%016" PRIx64 "\n"
	     "\t number_of_entries = %" PRIu32 "\n"
	     "\t minimum_locus     = %" PRId8 "\n",
	     handle, reinterpret_cast<uint64_t>(chunk_ids), number_of_entries, minimum_locus);

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (chunk_ids == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries == 0) {
		return PLAYGO_ERROR_BAD_SIZE;
	}
	if (!is_valid_locus(minimum_locus)) {
		return PLAYGO_ERROR_BAD_LOCUS;
	}
	for (uint32_t i = 0; i < number_of_entries; i++) {
		if (!is_valid_chunk(chunk_ids[i])) {
			return PLAYGO_ERROR_BAD_CHUNK_ID;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetEta(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries,
                               int64_t* out_eta) {
	PRINT_NAME();

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (chunk_ids == nullptr || out_eta == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries == 0) {
		return PLAYGO_ERROR_BAD_SIZE;
	}
	for (uint32_t i = 0; i < number_of_entries; i++) {
		if (!is_valid_chunk(chunk_ids[i])) {
			return PLAYGO_ERROR_BAD_CHUNK_ID;
		}
	}
	*out_eta = 0;

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetLanguageMask(int handle, uint64_t* out_language_mask) {
	PRINT_NAME();

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (out_language_mask == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}

	*out_language_mask = PLAYGO_LANGUAGE_MASK_ALL;

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetProgress(int handle, const uint16_t* chunk_ids,
                                    uint32_t number_of_entries, PlayGoProgress* out_progress) {
	PRINT_NAME();

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (chunk_ids == nullptr || out_progress == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (number_of_entries == 0) {
		return PLAYGO_ERROR_BAD_SIZE;
	}
	for (uint32_t i = 0; i < number_of_entries; i++) {
		if (!is_valid_chunk(chunk_ids[i])) {
			return PLAYGO_ERROR_BAD_CHUNK_ID;
		}
	}

	out_progress->total_size    = number_of_entries;
	out_progress->progress_size = number_of_entries;

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetOptionalChunk(int handle, int32_t type, PlayGoOptionalChunk* option) {
	PRINT_NAME();

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (option == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (auto ret = validate_optional_type(type); ret != OK) {
		return ret;
	}

	set_optional_chunk(type, option);

	return OK;
}

int KYTY_SYSV_ABI PlayGoPrefetchOptionalChunk(int handle, int32_t type,
                                              const PlayGoOptionalChunk* option) {
	PRINT_NAME();

	if (auto ret = validate_handle(handle); ret != OK) {
		return ret;
	}
	if (option == nullptr) {
		return PLAYGO_ERROR_BAD_POINTER;
	}
	if (auto ret = validate_optional_type(type); ret != OK) {
		return ret;
	}

	return OK;
}

int KYTY_SYSV_ABI PlayGoGetInstallChunkId(int handle, uint16_t* out_chunk_id_list,
                                          uint32_t number_of_entries, uint32_t* out_entries) {
	PRINT_NAME();

	return PlayGoGetChunkId(handle, out_chunk_id_list, number_of_entries, out_entries);
}

int KYTY_SYSV_ABI PlayGoGetSupportedOptionalChunk(int handle, int32_t type,
                                                  PlayGoOptionalChunk* option) {
	PRINT_NAME();

	return PlayGoGetOptionalChunk(handle, type, option);
}

} // namespace PlayGo

LIB_DEFINE(InitPlayGo_1) {
	LIB_FUNC("ts6GlZOKRrE", PlayGo::PlayGoInitialize);
	LIB_FUNC("MPe0EeBGM-E", PlayGo::PlayGoTerminate);
	LIB_FUNC("M1Gma1ocrGE", PlayGo::PlayGoOpen);
	LIB_FUNC("Uco1I0dlDi8", PlayGo::PlayGoClose);
	LIB_FUNC("uWIYLFkkwqk", PlayGo::PlayGoGetLocus);
	LIB_FUNC("rvBSfTimejE", PlayGo::PlayGoGetInstallSpeed);
	LIB_FUNC("4AAcTU9R3XM", PlayGo::PlayGoSetInstallSpeed);
	LIB_FUNC("73fF1MFU8hA", PlayGo::PlayGoGetChunkId);
	LIB_FUNC("Nn7zKwnA5q0", PlayGo::PlayGoGetToDoList);
	LIB_FUNC("gUPGiOQ1tmQ", PlayGo::PlayGoSetToDoList);
	LIB_FUNC("-Q1-u1a7p0g", PlayGo::PlayGoPrefetch);
	LIB_FUNC("v6EZ-YWRdMs", PlayGo::PlayGoGetEta);
	LIB_FUNC("3OMbYZBaa50", PlayGo::PlayGoGetLanguageMask);
	LIB_FUNC("-RJWNMK3fC8", PlayGo::PlayGoGetProgress);
	LIB_FUNC("g4AZyxpSAlA", PlayGo::PlayGoGetOptionalChunk);
	LIB_FUNC("HVAa744ecdw", PlayGo::PlayGoPrefetchOptionalChunk);
	LIB_FUNC("8-e7E989rCU", PlayGo::PlayGoGetInstallChunkId);
	LIB_FUNC("IfiN+-oeVWI", PlayGo::PlayGoGetSupportedOptionalChunk);
}

} // namespace Libs
