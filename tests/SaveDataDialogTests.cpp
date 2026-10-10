#include "libs/dialog.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace Loader::Timer {
double GetTimeMs() {
	return 0.0;
}
} // namespace Loader::Timer

namespace {
namespace Save = Libs::Dialog::SaveDataDialog;

struct BaseParam {
	uint64_t size;
	uint8_t  reserved[36];
	uint32_t magic;
};

struct Items {
	int32_t     user_id;
	uint32_t    pad0;
	const void* title_id;
	const void* dir_names;
	uint32_t    dir_names_num;
	uint32_t    pad1;
	const void* new_item;
	int32_t     focus_pos;
	uint32_t    pad2;
	const void* focus_dir_name;
	int32_t     item_style;
	uint8_t     reserved[36];
};

struct UserMessage {
	int32_t     button_type;
	int32_t     message_type;
	const char* message;
	uint8_t     reserved[32];
};

struct Param {
	BaseParam    base;
	int32_t      size;
	int32_t      mode;
	int32_t      display_type;
	uint32_t     pad0;
	void*        animation;
	Items*       items;
	UserMessage* user_message;
	void*        system_message;
	void*        error_code;
	void*        progress_bar;
	void*        user_data;
	void*        option;
	void*        wizard;
	uint8_t      reserved[16];
};

static_assert(sizeof(BaseParam) == 48);
static_assert(sizeof(Items) == 96);
static_assert(sizeof(Param) == 152);
static_assert(offsetof(Param, items) == 72);
static_assert(offsetof(Param, user_data) == 112);

bool CheckLatestStatus(const char* stage) {
	const int updated = Save::SaveDataDialogUpdateStatus();
	const int current = Save::SaveDataDialogGetStatus();
	if (updated != current) {
		std::fprintf(stderr,
		             "SaveDataDialogTests: %s: UpdateStatus returned %d but the current "
		             "status is %d\n",
		             stage, updated, current);
		return false;
	}
	return true;
}

bool CheckStatus(int actual, int expected, const char* stage) {
	if (actual != expected) {
		std::fprintf(stderr, "SaveDataDialogTests: %s: expected %d, got %d\n", stage, expected,
		             actual);
		return false;
	}
	return true;
}

bool TestRunningThenFinished(Param& param) {
	bool passed = CheckStatus(Save::SaveDataDialogOpen(&param), 0, "Open");
	for (int poll = 0; poll < 3; ++poll) {
		passed =
		    CheckStatus(Save::SaveDataDialogGetStatus(), 2, "peek before first update") && passed;
	}
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 2, "first update") && passed;
	for (int poll = 0; poll < 3; ++poll) {
		passed =
		    CheckStatus(Save::SaveDataDialogGetStatus(), 2, "peek after first update") && passed;
	}
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 3, "second update") && passed;
	passed = CheckStatus(Save::SaveDataDialogGetStatus(), 3, "finished peek") && passed;
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 3, "finished update") && passed;
	return passed;
}

bool TestCloseAndReset(Param& param) {
	bool passed = CheckStatus(Save::SaveDataDialogOpen(&param), 0, "Open before Close");
	passed =
	    CheckStatus(Save::SaveDataDialogClose(nullptr), 0, "Close before first poll") && passed;
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 3, "poll after Close") && passed;
	passed = CheckStatus(Save::SaveDataDialogGetStatus(), 3, "peek after Close") && passed;
	passed = TestRunningThenFinished(param) && passed;

	passed = CheckStatus(Save::SaveDataDialogOpen(&param), 0, "Open before Terminate") && passed;
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 2, "poll before Terminate") && passed;
	passed = CheckStatus(Save::SaveDataDialogTerminate(), 0, "Terminate while running") && passed;
	passed = CheckStatus(Save::SaveDataDialogGetStatus(), 0, "peek after Terminate") && passed;
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 0, "poll after Terminate") && passed;
	passed = CheckStatus(Save::SaveDataDialogInitialize(), 0, "reinitialize") && passed;
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 1, "initialized update") && passed;
	passed = TestRunningThenFinished(param) && passed;
	return passed;
}

struct DirName {
	char data[32];
};

struct NewItem {
	const char* title;
	void*       icon;
	size_t      icon_size;
	uint8_t     reserved[32];
};

struct Result {
	int32_t  mode;
	int32_t  result;
	int32_t  button_id;
	uint32_t pad0;
	void*    dir_name;
	void*    param;
	void*    user_data;
	uint8_t  reserved[32];
};

void NoVisibilityCallback() {}

bool ProvideSaveInfo(int32_t, const char*, const char* dir_name,
                     Libs::Dialog::SystemDialog::SaveListEntry* entry, void* param) {
	if (std::strcmp(dir_name, "MISSING") == 0) {
		return false;
	}
	entry->title = std::string("Title ") + dir_name;
	std::memset(param, 0, Save::SAVE_DATA_PARAM_SIZE);
	std::memcpy(param, dir_name, std::strlen(dir_name));
	return true;
}

bool Fail(const char* stage) {
	std::fprintf(stderr, "SaveDataDialogTests: %s\n", stage);
	return false;
}

// Opens a list dialog and returns the overlay snapshot of it.
bool OpenList(Param& param, Libs::Dialog::SystemDialog::SaveListSnapshot* snapshot) {
	return Save::SaveDataDialogOpen(&param) == 0 &&
	       Libs::Dialog::SystemDialog::GetSaveListSnapshot(snapshot);
}

bool TestSaveList(Param& param) {
	namespace System = Libs::Dialog::SystemDialog;
	const DirName dirs[] = {{"SLOT1"}, {"MISSING"}, {"SLOT2"}};
	NewItem       new_item {};
	new_item.title = "New Saved Data";
	Items items {};
	items.user_id       = 1000;
	items.dir_names     = dirs;
	items.dir_names_num = 3;
	items.new_item      = &new_item;
	Param list          = param;
	list.mode           = 1; // LIST
	list.items          = &items;
	bool passed         = true;

	// Without the overlay the list finishes on its own.
	System::SetVisibilityCallback(nullptr);
	System::SaveListSnapshot snapshot;
	if (OpenList(list, &snapshot)) {
		passed = Fail("list opened without an overlay");
	}
	Save::SaveDataDialogUpdateStatus();
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 3, "list without overlay") && passed;

	System::SetVisibilityCallback(NoVisibilityCallback);
	Save::SetSaveInfoProvider(ProvideSaveInfo);
	if (!OpenList(list, &snapshot)) {
		return Fail("list did not reach the overlay");
	}
	if (snapshot.entries.size() != 2 || snapshot.entries[1].dir_name != "SLOT2" ||
	    snapshot.entries[1].title != "Title SLOT2" || !snapshot.has_new_item ||
	    snapshot.new_item_title != "New Saved Data") {
		passed = Fail("list snapshot has the wrong rows");
	}
	for (int poll = 0; poll < 4; ++poll) {
		passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 2, "list waits") && passed;
	}
	passed = CheckStatus(System::HostSelectSave(snapshot.generation, 1), 1, "select") && passed;
	passed = CheckStatus(Save::SaveDataDialogUpdateStatus(), 3, "list selected") && passed;
	DirName  chosen {"stale"};
	uint8_t  chosen_param[Save::SAVE_DATA_PARAM_SIZE] {};
	Result   result {};
	result.dir_name = &chosen;
	result.param    = chosen_param;
	Save::SaveDataDialogGetResult(&result);
	if (result.result != 0 || std::strcmp(chosen.data, "SLOT2") != 0 ||
	    std::memcmp(chosen_param, "SLOT2", 5) != 0) {
		passed = Fail("selected save not returned");
	}

	if (!OpenList(list, &snapshot)) {
		return Fail("second list did not reach the overlay");
	}
	passed = CheckStatus(System::HostSelectSave(snapshot.generation, -1), 1, "new item") && passed;
	std::strcpy(chosen.data, "stale");
	Save::SaveDataDialogGetResult(&result);
	if (result.result != 0 || chosen.data[0] != '\0') {
		passed = Fail("new item not returned as an empty dir name");
	}

	if (!OpenList(list, &snapshot)) {
		return Fail("third list did not reach the overlay");
	}
	passed = CheckStatus(System::HostClose(snapshot.generation), 1, "cancel") && passed;
	passed = CheckStatus(Save::SaveDataDialogGetStatus(), 3, "list cancelled") && passed;
	Save::SaveDataDialogGetResult(&result);
	passed = CheckStatus(result.result, 1, "cancel result") && passed;

	System::SetVisibilityCallback(nullptr);
	Save::SetSaveInfoProvider(nullptr);
	return passed;
}
} // namespace

int main(int argc, char** argv) {
	const bool consistency_only = argc == 2 && std::strcmp(argv[1], "--consistency-only") == 0;
	if (argc != 1 && !consistency_only) {
		std::fprintf(stderr, "usage: SaveDataDialogTests [--consistency-only]\n");
		return 2;
	}
	Items items {};
	items.user_id       = 1000;
	items.dir_names_num = 1; // No save-data information is displayed with a null dir_names.
	UserMessage message {};
	message.message = "Status polling";
	Param param {};
	param.base.size = sizeof(BaseParam);
	param.base.magic =
	    static_cast<uint32_t>(0xc0d1a109u + reinterpret_cast<uintptr_t>(&param.base));
	param.size         = sizeof(Param);
	param.mode         = 2; // USER_MSG
	param.display_type = 1; // SAVE
	param.items        = &items;
	param.user_message = &message;

	if (Libs::Dialog::CommonDialog::CommonDialogInitialize() != 0 ||
	    Save::SaveDataDialogInitialize() != 0) {
		return 2;
	}
	bool passed = CheckLatestStatus("initialized");
	if (Save::SaveDataDialogOpen(&param) != 0) {
		return 2;
	}
	passed = CheckLatestStatus("first poll after Open") && passed;
	passed = CheckLatestStatus("second poll after Open") && passed;
	if (!consistency_only) {
		passed = TestRunningThenFinished(param) && passed;
		passed = TestCloseAndReset(param) && passed;
		passed = TestSaveList(param) && passed;
	}
	Save::SaveDataDialogTerminate();
	passed = CheckLatestStatus("terminated") && passed;
	return passed ? 0 : 1;
}
