#include "common/abi.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstring>

namespace Libs {

namespace LibGen5 {

LIB_VERSION("VideoOut", 1, "VideoOut", 1, 1);

LIB_DEFINE(InitVideoOut_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("Up36PTk687E", VideoOut::VideoOutOpen);
	LIB_FUNC("uquVH4-Du78", VideoOut::VideoOutClose);
	LIB_FUNC("WkYtyOg30do", VideoOut::VideoOutSetFlipMaster);
	LIB_FUNC("MBlv8YWoyOk", VideoOut::VideoOutAddBufferAttributeOption);
	LIB_FUNC("PjS5uASwcV8", VideoOut::VideoOutSetBufferAttribute2);
	LIB_FUNC("rKBUtgRrtbk", VideoOut::VideoOutRegisterBuffers2);
	LIB_FUNC("HuViW4HnrOw", VideoOut::VideoOutSubmitChangeBufferAttribute2);
	LIB_FUNC("N5KDtkIjjJ4", VideoOut::VideoOutUnregisterBuffers);
	LIB_FUNC("CBiu4mCE1DA", VideoOut::VideoOutSetFlipRate);
	LIB_FUNC("HXzjK9yI30k", VideoOut::VideoOutAddFlipEvent);
	LIB_FUNC("Xru92wHJRmg", VideoOut::VideoOutAddVblankEvent);
	LIB_FUNC("keipklF0pMY", VideoOut::VideoOutAddPreVblankStartEvent);
	LIB_FUNC("-Ozn0F1AFRg", VideoOut::VideoOutDeleteFlipEvent);
	LIB_FUNC("oNOQn3knW6s", VideoOut::VideoOutDeleteVblankEvent);
	LIB_FUNC("elWQ9vERF-Q", VideoOut::VideoOutDeletePreVblankStartEvent);
	LIB_FUNC("U46NwOiJpys", VideoOut::VideoOutSubmitFlip);
	LIB_FUNC("SbU3dwp80lQ", VideoOut::VideoOutGetFlipStatus);
	LIB_FUNC("zgXifHT9ErY", VideoOut::VideoOutIsFlipPending);
	LIB_FUNC("1FZBKy8HeNU", VideoOut::VideoOutGetVblankStatus);
	LIB_FUNC("MTxxrOCeSig", VideoOut::VideoOutSetWindowModeMargins);
	LIB_FUNC("kmSe30JTs+E", VideoOut::VideoOutAddOutputModeEvent);
	LIB_FUNC("U2JJtSqNKZI", VideoOut::VideoOutGetEventId);
	LIB_FUNC("rWUTcKdkUzQ", VideoOut::VideoOutGetEventData);
	LIB_FUNC("Mt4QHHkxkOc", VideoOut::VideoOutGetEventCount);
	LIB_FUNC("j6RaAUlaLv0", VideoOut::VideoOutWaitVblank);
	LIB_FUNC("utPrVdxio-8", VideoOut::VideoOutGetOutputStatus);
	LIB_FUNC("5tRaBjtdTzY", VideoOut::VideoOutVrrPegToFixedRate);
	LIB_FUNC("T4ucGB8CsnM", VideoOut::VideoOutVrrUnpegFromFixedRate);
	LIB_FUNC("+I4K03i3EL0", VideoOut::VideoOutInitializeOutputOptions);
	LIB_FUNC("Nv8c-Kb+DUM", VideoOut::VideoOutIsOutputSupported);
	LIB_FUNC("w0hLuNarQxY", VideoOut::VideoOutConfigureOutput);
	LIB_FUNC("eb-gvTYQcoY", VideoOut::VideoOutLatencyControlWaitBeforeInput);
	LIB_FUNC("MCJ8SkzsQxY", VideoOut::VideoOutLatencyMeasureSetStartPoint);
	LIB_FUNC("DYhhWbJSeRg", VideoOut::VideoOutColorSettingsSetGamma);
	LIB_FUNC("pv9CI5VC+R0", VideoOut::VideoOutAdjustColor);
	LIB_FUNC("w7Ipp9Xl7hg", VideoOut::VideoOutAllowOutputResolutionWqhdDetection);
}

} // namespace LibGen5

namespace LibGen5::VrrStatus {

LIB_VERSION("VideoOutVrrStatus", 1, "VideoOut", 1, 1);

static KYTY_SYSV_ABI int VideoOutVrrStatus_kP2L8t3j_aM() {
	// The observed guest call passes no arguments.
	// Return success for Kyty's fixed-refresh path.
	return OK;
}

static KYTY_SYSV_ABI int VideoOutVrrStatus_LibwuIonIBw(LibKernel::EventQueue::KernelEqueue eq,
                                                       int handle, void* udata) {
	PRINT_NAME();

	LOGF("\t eq     = 0x%016" PRIx64 "\n", static_cast<uint64_t>(eq));
	LOGF("\t handle = %d\n", handle);
	LOGF("\t udata  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(udata));

	return OK;
}

static KYTY_SYSV_ABI int VideoOutVrrStatus__q_vbO859Tw(LibKernel::EventQueue::KernelEqueue eq,
                                                       int                                 handle) {
	PRINT_NAME();

	LOGF("\t eq     = 0x%016" PRIx64 "\n", static_cast<uint64_t>(eq));
	LOGF("\t handle = %d\n", handle);

	return OK;
}

static KYTY_SYSV_ABI int VideoOutVrrStatus_gWT7X8H0bYs(int handle, void* status) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);
	LOGF("\t status = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(status));

	if (status == nullptr) {
		return VideoOut::VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	// Flags bit 4 stays clear (VRR off), so the guest skips the VRR range at +0x30.
	memset(status, 0, 0x38);

	return VideoOut::VideoOutGetOutputStatus(handle,
	                                         static_cast<VideoOut::VideoOutOutputStatus*>(status));
}

LIB_DEFINE(InitVideoOutVrrStatus_1) {
	LIB_FUNC("kP2L8t3j-aM", VideoOutVrrStatus_kP2L8t3j_aM);
	LIB_FUNC("LibwuIonIBw", VideoOutVrrStatus_LibwuIonIBw);
	LIB_FUNC("-q-vbO859Tw", VideoOutVrrStatus__q_vbO859Tw);
	LIB_FUNC("gWT7X8H0bYs", VideoOutVrrStatus_gWT7X8H0bYs);
}

} // namespace LibGen5::VrrStatus

} // namespace Libs
