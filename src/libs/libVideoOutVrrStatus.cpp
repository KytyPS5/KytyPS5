#include "common/abi.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstring>

namespace Libs::LibGen5::VrrStatus {

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

	// Fixed-refresh output has no VRR status changes to signal.
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

	const int result = VideoOut::VideoOutGetOutputStatus(
	    handle, static_cast<VideoOut::VideoOutOutputStatus*>(status));
	if (result != OK) {
		return result;
	}

	// The guest's private status layout extends the 0x30-byte output status
	// with an 8-byte VRR range. GetOutputStatus clears flags, including bit 4 (VRR off).
	std::memset(static_cast<uint8_t*>(status) + 0x30, 0, 0x08);
	return OK;
}

LIB_DEFINE(InitVideoOutVrrStatus_1) {
	LIB_FUNC("kP2L8t3j-aM", VideoOutVrrStatus_kP2L8t3j_aM);
	LIB_FUNC("LibwuIonIBw", VideoOutVrrStatus_LibwuIonIBw);
	LIB_FUNC("-q-vbO859Tw", VideoOutVrrStatus__q_vbO859Tw);
	LIB_FUNC("gWT7X8H0bYs", VideoOutVrrStatus_gWT7X8H0bYs);
}

} // namespace Libs::LibGen5::VrrStatus
