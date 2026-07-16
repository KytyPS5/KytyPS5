#ifndef EMULATOR_INCLUDE_EMULATOR_DIALOG_H_
#define EMULATOR_INCLUDE_EMULATOR_DIALOG_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Dialog {

namespace CommonDialog {

int KYTY_SYSV_ABI CommonDialogInitialize();

} // namespace CommonDialog

namespace ImeDialog {

int KYTY_SYSV_ABI ImeDialogGetStatus();

} // namespace ImeDialog

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
