#ifndef EMULATOR_INCLUDE_EMULATOR_DIALOG_H_
#define EMULATOR_INCLUDE_EMULATOR_DIALOG_H_

#include "common/abi.h"
#include "common/common.h"

#include <string>
#include <vector>

namespace Libs::Dialog {

namespace CommonDialog {

int KYTY_SYSV_ABI  CommonDialogInitialize();
bool KYTY_SYSV_ABI CommonDialogIsUsed();

} // namespace CommonDialog

namespace LoginDialog {

int KYTY_SYSV_ABI  LoginDialogInitialize();
int KYTY_SYSV_ABI  LoginDialogTerminate();
int KYTY_SYSV_ABI  LoginDialogOpen(const void* param);
int KYTY_SYSV_ABI  LoginDialogClose();
int KYTY_SYSV_ABI  LoginDialogUpdateStatus();
int KYTY_SYSV_ABI  LoginDialogGetStatus();
int KYTY_SYSV_ABI  LoginDialogGetResult(void* result);
void KYTY_SYSV_ABI LoginDialogParamInitialize(void* param);

} // namespace LoginDialog

namespace SigninDialog {

int KYTY_SYSV_ABI SigninDialogInitialize();
int KYTY_SYSV_ABI SigninDialogTerminate();
int KYTY_SYSV_ABI SigninDialogOpen(const void* param);
int KYTY_SYSV_ABI SigninDialogClose();
int KYTY_SYSV_ABI SigninDialogUpdateStatus();
int KYTY_SYSV_ABI SigninDialogGetStatus();
int KYTY_SYSV_ABI SigninDialogGetResult(void* result);

} // namespace SigninDialog

namespace SaveDataDialog {

int KYTY_SYSV_ABI SaveDataDialogInitialize();
int KYTY_SYSV_ABI SaveDataDialogGetStatus();
int KYTY_SYSV_ABI SaveDataDialogUpdateStatus();
int KYTY_SYSV_ABI SaveDataDialogGetResult(void* result);
int KYTY_SYSV_ABI SaveDataDialogOpen(const void* param);
int KYTY_SYSV_ABI SaveDataDialogClose(const void* close_param);
int KYTY_SYSV_ABI SaveDataDialogIsReadyToDisplay();
int KYTY_SYSV_ABI SaveDataDialogTerminate();
int KYTY_SYSV_ABI SaveDataDialogProgressBarInc(int target, uint32_t delta);
int KYTY_SYSV_ABI SaveDataDialogProgressBarSetValue(int target, uint32_t rate);

} // namespace SaveDataDialog

namespace MsgDialog {

int KYTY_SYSV_ABI MsgDialogInitialize();
int KYTY_SYSV_ABI MsgDialogOpen(const void* param);
int KYTY_SYSV_ABI MsgDialogUpdateStatus();
int KYTY_SYSV_ABI MsgDialogGetStatus();
int KYTY_SYSV_ABI MsgDialogGetResult(void* result);
int KYTY_SYSV_ABI MsgDialogTerminate();
int KYTY_SYSV_ABI MsgDialogClose();
int KYTY_SYSV_ABI MsgDialogProgressBarInc(int target, uint32_t delta);
int KYTY_SYSV_ABI MsgDialogProgressBarSetValue(int target, uint32_t rate);
int KYTY_SYSV_ABI MsgDialogProgressBarSetMsg(int target, const char* msg);

} // namespace MsgDialog

namespace SystemDialog {

enum class Kind { Error, Signin, SaveList };

struct HostSnapshot {
	Kind     kind;
	uint64_t generation;
	int32_t  error_code;
};

struct VisualState {
	bool     active;
	bool     background;
	uint64_t revision;
};

struct SaveListEntry {
	std::string dir_name;
	std::string title;
	std::string sub_title;
	std::string detail;
	int64_t     mtime = 0;
};

struct SaveListSnapshot {
	uint64_t                   generation   = 0;
	int32_t                    display_type = 0;
	bool                       has_new_item = false;
	std::string                new_item_title;
	std::vector<SaveListEntry> entries;
};

bool        GetHostSnapshot(HostSnapshot* snapshot);
bool        GetSaveListSnapshot(SaveListSnapshot* snapshot);
VisualState GetVisualState() noexcept;
void        SetVisibilityCallback(void (*callback)());
bool        HostClose(uint64_t generation);
// entry < 0 selects the new-save item.
bool HostSelectSave(uint64_t generation, int32_t entry);

} // namespace SystemDialog

namespace SaveDataDialog {

constexpr size_t SAVE_DATA_PARAM_SIZE = 1328;

// Fills the entry and the guest SceSaveDataParam bytes; false when the save does not exist.
using SaveInfoProvider = bool (*)(int32_t user_id, const char* title_id, const char* dir_name,
                                  SystemDialog::SaveListEntry* entry, void* param);
void SetSaveInfoProvider(SaveInfoProvider provider);

} // namespace SaveDataDialog

namespace ErrorDialog {

int KYTY_SYSV_ABI ErrorDialogInitialize();
int KYTY_SYSV_ABI ErrorDialogOpen(const void* param);
int KYTY_SYSV_ABI ErrorDialogClose();
int KYTY_SYSV_ABI ErrorDialogTerminate();
int KYTY_SYSV_ABI ErrorDialogUpdateStatus();
int KYTY_SYSV_ABI ErrorDialogGetStatus();

} // namespace ErrorDialog

} // namespace Libs::Dialog

#endif /* EMULATOR_INCLUDE_EMULATOR_DIALOG_H_ */
