#include "common/abi.h"
#include "libs/dialog.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

namespace LibCommonDialog {

LIB_VERSION("CommonDialog", 1, "CommonDialog", 1, 1);

namespace CommonDialog = Dialog::CommonDialog;

LIB_DEFINE(InitDialog_1_CommonDialog) {
	LIB_FUNC("uoUpLGNkygk", CommonDialog::CommonDialogInitialize);
}

} // namespace LibCommonDialog

namespace LibImeDialog {

LIB_VERSION("ImeDialog", 1, "ImeDialog", 1, 1);

namespace ImeDialog = Dialog::ImeDialog;

LIB_DEFINE(InitDialog_1_ImeDialog) {
	LIB_FUNC("IADmD4tScBY", ImeDialog::ImeDialogGetStatus);
}

} // namespace LibImeDialog

namespace LibLoginDialog {

LIB_VERSION("LoginDialog", 1, "LoginDialog", 1, 1);

namespace LoginDialog = Dialog::LoginDialog;

LIB_DEFINE(InitDialog_1_LoginDialog) {
	LIB_FUNC("qP-EvQRl2Hc", LoginDialog::LoginDialogInitialize);
	LIB_FUNC("vMQJRUKsf3U", LoginDialog::LoginDialogTerminate);
	LIB_FUNC("S56ra1+Tymg", LoginDialog::LoginDialogOpen);
	LIB_FUNC("F0XIzrG5yvw", LoginDialog::LoginDialogClose);
	LIB_FUNC("2rc+egSfb5A", LoginDialog::LoginDialogUpdateStatus);
	LIB_FUNC("HAiWUEwEfGo", LoginDialog::LoginDialogGetStatus);
	LIB_FUNC("Btkx21f1M8k", LoginDialog::LoginDialogGetResult);
	LIB_FUNC("3NPobi5lNmk", LoginDialog::LoginDialogParamInitialize);
}

} // namespace LibLoginDialog

namespace LibSaveDataDialog {

LIB_VERSION("SaveDataDialog", 1, "SaveDataDialog", 1, 1);

namespace SaveDataDialog = Dialog::SaveDataDialog;

LIB_DEFINE(InitDialog_1_SaveDataDialog) {
	LIB_FUNC("ERKzksauAJA", SaveDataDialog::SaveDataDialogGetStatus);
	LIB_FUNC("KK3Bdg1RWK0", SaveDataDialog::SaveDataDialogUpdateStatus);
	LIB_FUNC("YuH2FA7azqQ", SaveDataDialog::SaveDataDialogTerminate);
	LIB_FUNC("V-uEeFKARJU", SaveDataDialog::SaveDataDialogProgressBarInc);
	LIB_FUNC("hay1CfTmLyA", SaveDataDialog::SaveDataDialogProgressBarSetValue);
}

} // namespace LibSaveDataDialog

namespace LibSaveDataDialogNative {

LIB_VERSION("SaveDataDialog.native", 1, "SaveDataDialog", 1, 1);

namespace SaveDataDialog = Dialog::SaveDataDialog;

LIB_DEFINE(InitDialog_1_SaveDataDialogNative) {
	LIB_FUNC("fH46Lag88XY", SaveDataDialog::SaveDataDialogClose);
	LIB_FUNC("yEiJ-qqr6Cg", SaveDataDialog::SaveDataDialogGetResult);
	LIB_FUNC("ERKzksauAJA", SaveDataDialog::SaveDataDialogGetStatus);
	LIB_FUNC("s9e3+YpRnzw", SaveDataDialog::SaveDataDialogInitialize);
	LIB_FUNC("en7gNVnh878", SaveDataDialog::SaveDataDialogIsReadyToDisplay);
	LIB_FUNC("4tPhsP6FpDI", SaveDataDialog::SaveDataDialogOpen);
	LIB_FUNC("V-uEeFKARJU", SaveDataDialog::SaveDataDialogProgressBarInc);
	LIB_FUNC("hay1CfTmLyA", SaveDataDialog::SaveDataDialogProgressBarSetValue);
	LIB_FUNC("YuH2FA7azqQ", SaveDataDialog::SaveDataDialogTerminate);
	LIB_FUNC("KK3Bdg1RWK0", SaveDataDialog::SaveDataDialogUpdateStatus);
}

} // namespace LibSaveDataDialogNative

namespace LibMsgDialog {

LIB_VERSION("MsgDialog.native", 1, "MsgDialog", 1, 1);

namespace MsgDialog = Dialog::MsgDialog;

LIB_DEFINE(InitDialog_1_MsgDialog) {
	LIB_FUNC("HTrcDKlFKuM", MsgDialog::MsgDialogClose);
	LIB_FUNC("Lr8ovHH9l6A", MsgDialog::MsgDialogGetResult);
	LIB_FUNC("CWVW78Qc3fI", MsgDialog::MsgDialogGetStatus);
	LIB_FUNC("lDqxaY1UbEo", MsgDialog::MsgDialogInitialize);
	LIB_FUNC("b06Hh0DPEaE", MsgDialog::MsgDialogOpen);
	LIB_FUNC("Gc5k1qcK4fs", MsgDialog::MsgDialogProgressBarInc);
	LIB_FUNC("6H-71OdrpXM", MsgDialog::MsgDialogProgressBarSetMsg);
	LIB_FUNC("wTpfglkmv34", MsgDialog::MsgDialogProgressBarSetValue);
	LIB_FUNC("ePw-kqZmelo", MsgDialog::MsgDialogTerminate);
	LIB_FUNC("6fIC3XKt2k0", MsgDialog::MsgDialogUpdateStatus);
}

} // namespace LibMsgDialog

namespace LibErrorDialog {

LIB_VERSION("ErrorDialog", 1, "ErrorDialog", 1, 1);

namespace ErrorDialog = Dialog::ErrorDialog;

LIB_DEFINE(InitDialog_1_ErrorDialog) {
	LIB_FUNC("ekXHb1kDBl0", ErrorDialog::ErrorDialogClose);
	LIB_FUNC("t2FvHRXzgqk", ErrorDialog::ErrorDialogGetStatus);
	LIB_FUNC("I88KChlynSs", ErrorDialog::ErrorDialogInitialize);
	LIB_FUNC("M2ZF-ClLhgY", ErrorDialog::ErrorDialogOpen);
	LIB_FUNC("9XAxK2PMwk8", ErrorDialog::ErrorDialogTerminate);
	LIB_FUNC("WWiGuh9XfgQ", ErrorDialog::ErrorDialogUpdateStatus);
}

} // namespace LibErrorDialog

LIB_DEFINE(InitDialog_1) {
	LibCommonDialog::InitDialog_1_CommonDialog(s);
	LibImeDialog::InitDialog_1_ImeDialog(s);
	LibLoginDialog::InitDialog_1_LoginDialog(s);
	LibSaveDataDialog::InitDialog_1_SaveDataDialog(s);
	LibSaveDataDialogNative::InitDialog_1_SaveDataDialogNative(s);
	LibMsgDialog::InitDialog_1_MsgDialog(s);
	LibErrorDialog::InitDialog_1_ErrorDialog(s);
}

} // namespace Libs
