#include "common/abi.h"
#include "libs/agc.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

namespace LibGen5 {

LIB_VERSION("Graphics5", 1, "Graphics5", 1, 1);

namespace Gen5 = Graphics::Gen5;

LIB_DEFINE(InitAgcDriver_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("23LRUSvYu1M", Gen5::AgcInit);
	LIB_FUNC("2JtWUUiYBXs", Gen5::AgcGetRegisterDefaults2);
	LIB_FUNC("wRbq6ZjNop4", Gen5::AgcGetRegisterDefaults2Internal);
	LIB_FUNC("f3dg2CSgRKY", Gen5::AgcCreateShader);
	LIB_FUNC("dolOmWH+huQ", Gen5::AgcUnknownGetFusedShaderSize);
	LIB_FUNC("fd5Bp5tGTgo", Gen5::AgcUnknownFuseShaderHalves);
	LIB_FUNC("nApJjpKNBl4", Gen5::AgcUnknownNApJjpKNBl4);
	LIB_FUNC("vcmNN+AAXnY", Gen5::AgcSetCxRegIndirectPatchSetAddress);
	LIB_FUNC("Qrj4c+61z4A", Gen5::AgcSetShRegIndirectPatchSetAddress);
	LIB_FUNC("6lNcCp+fxi4", Gen5::AgcSetUcRegIndirectPatchSetAddress);
	LIB_FUNC("whb1RL7K4Ss", Gen5::AgcSetCxRegIndirectPatchSetNumRegisters);
	LIB_FUNC("nCUgItdN2ms", Gen5::AgcSetShRegIndirectPatchSetNumRegisters);
	LIB_FUNC("fRG-JOH5+sI", Gen5::AgcSetUcRegIndirectPatchSetNumRegisters);
	LIB_FUNC("d-6uF9sZDIU", Gen5::AgcSetCxRegIndirectPatchAddRegisters);
	LIB_FUNC("z2duB-hHQSM", Gen5::AgcSetShRegIndirectPatchAddRegisters);
	LIB_FUNC("vRoArM9zaIk", Gen5::AgcSetUcRegIndirectPatchAddRegisters);
	LIB_FUNC("D9sr1xGUriE", Gen5::AgcCreatePrimState);
	LIB_FUNC("Y3ymLfZ1384", Gen5::AgcUpdatePrimState);
	LIB_FUNC("NKIzURsgV7I", Gen5::AgcGetGsOversubscription);
	LIB_FUNC("HV4j+E0MBHE", Gen5::AgcCreateInterpolantMapping);
	LIB_FUNC("V++UgBtQhn0", Gen5::AgcGetDataPacketPayloadAddress);
	LIB_FUNC("s+VGAMDQ0AQ", Gen5::AgcGetDataPacketPayloadRange);
	LIB_FUNC("fPSCdQxgpSw", Gen5::AgcWriteDataPatchSetAddressOrOffset);
	LIB_FUNC("bxca0BK4FNg", Gen5::AgcUnknownJumpPatchSetTarget);
	LIB_FUNC("2BS4EtAaF28", Gen5::AgcJumpPatchSetTarget);
	LIB_FUNC("h9z6+0hEydk", Gen5::AgcSuspendPoint);
	LIB_FUNC("qj7QZpgr9Uw", Gen5::AgcDcbContextStateOp);
	LIB_FUNC("H6vHS5cidSA", Gen5::AgcDcbContextStateOpGetSize);
	LIB_FUNC("BfBDZGbti7A", Gen5::AgcGetIsTrinityMode);
	LIB_FUNC("dbOlWdppb4o", Gen5::AgcCreateInterpolantMapping2);

	LIB_FUNC("F0ZXt5q0ZTA", Gen5::AgcDriverGetDefaultOwner);
	LIB_FUNC("F0Y42t-3e18", Gen5::AgcDriverInitResourceRegistration);
	LIB_FUNC("AOLcoIkQDgM", Gen5::AgcDriverQueryResourceRegistrationUserMemoryRequirements);
	LIB_FUNC("uJziRsODk1c", Gen5::AgcDriverGetResourceRegistrationMaxNameLength);
	LIB_FUNC("X-Nm5KLREeg", Gen5::AgcDriverRegisterOwner);
	LIB_FUNC("W5z4eZrjEas", Gen5::AgcDriverRegisterResource);
	LIB_FUNC("ZLJk9r2+2Aw", Gen5::AgcDriverUnregisterOwnerAndResources);
	LIB_FUNC("pWLG7WOpVcw", Gen5::AgcDriverUnregisterResource);
	LIB_FUNC("3AyTaWcF-H8", Gen5::AgcDriverRegisterWorkloadStream);

	LIB_FUNC("LtTouSCZjHM", Gen5::AgcCbNop);
	LIB_FUNC("t7PlZ9nt5Lc", Gen5::AgcCbNopGetSize);
	LIB_FUNC("k3GhuSNmBLU", Gen5::AgcCbDispatch);
	LIB_FUNC("Abendgtz+3o", Gen5::AgcCbDispatchGetSize);
	LIB_FUNC("w1KFAHVqpaU", Gen5::AgcCbBranch);
	LIB_FUNC("n2fD4A+pb+g", Gen5::AgcCbSetShRegisterRangeDirect);
	LIB_FUNC("bxGoVxpdSPQ", Gen5::AgcCbSetShRegisterRangeDirectGetSize);
	LIB_FUNC("UZbQjYAwwXM", Gen5::AgcCbSetShRegistersDirect);
	LIB_FUNC("03RZmELWWzw", Gen5::AgcCbSetUcRegistersDirect);
	LIB_FUNC("wr23dPKyWc0", Gen5::AgcCbReleaseMem);
	LIB_FUNC("hL7C0IRpWZI", Gen5::AgcCbQueueEndOfPipeActionGetSize);
	LIB_FUNC("T6xuVw0KUJo", Gen5::AgcDebugRaiseException);
	LIB_FUNC("JrtiDtKeS38", Gen5::AgcAcbResetQueue);
	LIB_FUNC("cFazmnXpJOE", Gen5::AgcAcbEventWrite);
	LIB_FUNC("KT-hTp-Ch14", Gen5::AgcAcbAcquireMem);
	LIB_FUNC("ewobAQeMo5k", Gen5::AgcAcbAcquireMemGetSize);
	LIB_FUNC("qyM2bxYFPAk", Gen5::AgcAcbCondExec);
	LIB_FUNC("ozKzBP4aki4", Gen5::AgcAcbCondExecGetSize);
	LIB_FUNC("b-oySn+G2tE", Gen5::AgcAcbJumpGetSize);
	LIB_FUNC("htn36gPnBk4", Gen5::AgcAcbWaitRegMem);
	LIB_FUNC("-RnpfpxIhec", Gen5::AgcAcbDmaData);
	LIB_FUNC("qzMN2XKGA4k", Gen5::AgcAcbCopyData);
	LIB_FUNC("j3EtxFkSIhQ", Gen5::AgcAcbDispatchIndirect);
	LIB_FUNC("eZ4+17OQz4Q", Gen5::AgcAcbWriteData);
	LIB_FUNC("xAeBOa0A3kk", Gen5::AgcAcbSetMarker);
	LIB_FUNC("cpCILPya5Zk", Gen5::AgcAcbPushMarker);
	LIB_FUNC("6mFxkVqdmbQ", Gen5::AgcAcbPopMarker);
	LIB_FUNC("TRO721eVt4g", Gen5::AgcDcbResetQueue);
	LIB_FUNC("MWiElSNE8j8", Gen5::AgcDcbWaitUntilSafeForRendering);
	LIB_FUNC("LFSPFmGc9Hg", Gen5::AgcDcbSetWorkloadsActive);
	LIB_FUNC("hEK26Wdny6s", Gen5::AgcDcbSetWorkloadComplete);
	LIB_FUNC("QhCbS4X9Rl8", Gen5::AgcDcbSetMarker);
	LIB_FUNC("pFLArOT53+w", Gen5::AgcDcbSetShRegisterDirect);
	LIB_FUNC("LHFXRrlTPD8", Gen5::AgcDcbSetCxRegisterDirect);
	LIB_FUNC("1DeUNpRIDDA", Gen5::AgcDcbSetCxRegisterDirectGetSize);
	LIB_FUNC("w4-d0n60hdo", Gen5::AgcDcbSetUcRegisterDirect);
	LIB_FUNC("ZvwO9euwYzc", Gen5::AgcDcbSetCxRegistersIndirect);
	LIB_FUNC("-HOOCn0JY48", Gen5::AgcDcbSetShRegistersIndirect);
	LIB_FUNC("hvUfkUIQcOE", Gen5::AgcDcbSetUcRegistersIndirect);
	LIB_FUNC("GIIW2J37e70", Gen5::AgcDcbSetIndexSize);
	LIB_FUNC("l4fM9K-Lyks", Gen5::AgcDcbSetIndexBuffer);
	LIB_FUNC("8N2tmT3jmC8", Gen5::AgcDcbSetIndexCount);
	LIB_FUNC("tSBxhAPyytQ", Gen5::AgcDcbSetNumInstances);
	LIB_FUNC("6DFuRKT4C9w", Gen5::AgcDcbSetNumInstancesGetSize);
	LIB_FUNC("q88lQ+GP5Yk", Gen5::AgcDcbDrawIndex);
	LIB_FUNC("6ee9Hd3EWXQ", Gen5::AgcDcbDrawIndexGetSize);
	LIB_FUNC("Ikfdt-rIqCE", Gen5::AgcUnknownIkfdtRIqCE);
	LIB_FUNC("Rlx+bykm0r0", Gen5::AgcDcbDrawIndexMultiInstanced);
	LIB_FUNC("mR9j7+SfM34", Gen5::AgcDcbDrawIndexMultiInstancedGetSize);
	LIB_FUNC("Yw0jKSqop+E", Gen5::AgcDcbDrawIndexAuto);
	LIB_FUNC("WrdP9Zxx3lQ", Gen5::AgcDcbDrawIndexAutoGetSize);
	LIB_FUNC("B+aG9DUnTKA", Gen5::AgcDcbDrawIndexOffset);
	LIB_FUNC("qMlfB1ZhMDc", Gen5::AgcDcbDrawIndexOffsetGetSize);
	LIB_FUNC("RmaJwLtc8rY", Gen5::AgcDcbSetBaseIndirectArgs);
	LIB_FUNC("1q1titRBL6o", Gen5::AgcDcbDrawIndirect);
	LIB_FUNC("cxPZ4Wgvdj8", Gen5::AgcDcbDrawIndirectGetSize);
	LIB_FUNC("kUlvghKs-mA", Gen5::AgcDcbDrawIndirectMulti);
	LIB_FUNC("t1vNu082-jM", Gen5::AgcDcbDrawIndexIndirect);
	LIB_FUNC("ypVBz4uPKcQ", Gen5::AgcDcbDrawIndexIndirectMulti);
	LIB_FUNC("CtB+A9-VxO0", Gen5::AgcDcbDispatchIndirect);
	LIB_FUNC("w8HVkEeXPv8", Gen5::AgcDcbDispatchIndirectGetSize);
	LIB_FUNC("aJf+j5yntiU", Gen5::AgcDcbEventWrite);
	LIB_FUNC("C4l9fB17t8w", Gen5::AgcDcbEventWriteGetSize);
	LIB_FUNC("57labkp+rSQ", Gen5::AgcDcbAcquireMem);
	LIB_FUNC("-vnlTPPXPrw", Gen5::AgcDcbAcquireMemGetSize);
	LIB_FUNC("1rZSWUv1IRc", Gen5::AgcDcbCopyData);
	LIB_FUNC("WmAc2MEj6Io", Gen5::AgcDcbDmaData);
	LIB_FUNC("xSAR0LTcRKM", Gen5::AgcDcbJump);
	LIB_FUNC("VEGu4dixjUg", Gen5::AgcDcbJumpGetSize);
	LIB_FUNC("QIXCsbipds0", Gen5::AgcDcbRewindGetSize);
	LIB_FUNC("zfcxg-ewMK8", Gen5::AgcDcbRewind);
	LIB_FUNC("BIPexNBSGog", Gen5::AgcDcbCondExec);
	LIB_FUNC("ou16V5hh5sg", Gen5::AgcDcbCondExecGetSize);
	LIB_FUNC("bbFueFP+J4k", Gen5::AgcDcbSetPredication);
	LIB_FUNC("-KRzWekV120", Gen5::AgcUnknownKRzWekV120);
	LIB_FUNC("IxYiarKlXxM", Gen5::AgcDmaDataPatchSetDstAddressOrOffset);
	LIB_FUNC("cdDRpqcFGbU", Gen5::AgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate);
	LIB_FUNC("Lkf86B98qPc", Gen5::AgcGetPacketSize);
	LIB_FUNC("w6Dj1VJt5qY", Gen5::AgcSetPacketPredication);
	LIB_FUNC("n8vgpaQg6dA", Gen5::AgcSetRangePredication);
	LIB_FUNC("i1jyy49AjXU", Gen5::AgcDcbWriteData);
	LIB_FUNC("p9tI+yTvx68", Gen5::AgcDcbWriteDataGetSize);
	LIB_FUNC("vuSXe69VILM", Gen5::AgcDcbGetLodStats);
	LIB_FUNC("VmW0Tdpy420", Gen5::AgcDcbWaitRegMem);
	LIB_FUNC("43WJ08sSugE", Gen5::AgcDcbWaitOnAddressGetSize);
	LIB_FUNC("+kSrjIVxKFE", Gen5::AgcDcbPushMarker);
	LIB_FUNC("H7uZqCoNuWk", Gen5::AgcDcbPopMarker);
	LIB_FUNC("u2T2DiA5hRI", Gen5::AgcDcbStallCommandBufferParser);
	LIB_FUNC("3KDcnM3lrcU", Gen5::AgcWaitRegMemPatchAddress);
	LIB_FUNC("7nOoijNPvEU", Gen5::AgcWaitRegMemPatchReference);
	LIB_FUNC("0fWWK5uG9rQ", Gen5::AgcQueueEndOfPipeActionPatchAddress);
	LIB_FUNC("MlEw1feXcjg", Gen5::AgcQueueEndOfPipeActionPatchData);
	LIB_FUNC("ORWsxIbk4TE", Gen5::AgcCondExecPatchSetEnd);
	LIB_FUNC("YWTKOju587o", Gen5::AgcCondExecPatchSetCommandAddress);
	LIB_FUNC("k-JpyR2dYAM", Gen5::AgcCondExecPatchSetEnd);
	LIB_FUNC("3ZWa3AoyWZQ", Gen5::AgcCondExecPatchSetCommandAddress);
	LIB_FUNC("ziVA3whp3p4", Gen5::AgcRewindPatchSetRewindState);
	LIB_FUNC("YUeqkyT7mEQ", Gen5::AgcDcbSetFlip);
}

} // namespace LibGen5

namespace LibGen5Driver {

LIB_VERSION("Graphics5Driver", 1, "Graphics5Driver", 1, 1);

namespace Gen5Driver = Graphics::Gen5Driver;

LIB_DEFINE(InitAgcDriver_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("UglJIZjGssM", Gen5Driver::AgcDriverSubmitDcb);
	LIB_FUNC("AhGvpITrf4M", Gen5Driver::AgcDriverSubmitDcb);
	LIB_FUNC("6UzEidRZwkg", Gen5Driver::AgcDriverSubmitMultiDcbs);
	LIB_FUNC("+T8Xo6LtFJI", Gen5Driver::AgcDriverSubmitMultiDcbs);
	LIB_FUNC("b4fpgH5ZXxQ", Gen5Driver::AgcDriverSubmitCommandBuffer);
	LIB_FUNC("Fj7r9EHzF38", Gen5Driver::AgcDriverSubmitMultiCommandBuffers);
	LIB_FUNC("gSRnr79F8tQ", Gen5Driver::AgcDriverSubmitAcb);
	LIB_FUNC("HF3YllT3mXU", Gen5Driver::AgcDriverSubmitMultiAcbs);
	LIB_FUNC("w2rJhmD+dsE", Gen5Driver::AgcDriverAddEqEvent);
	LIB_FUNC("DL2RXaXOy88", Gen5Driver::AgcDriverDeleteEqEvent);
	LIB_FUNC("5CdQTZIQPxM", Gen5Driver::AgcDriverGetEqEventType);
	LIB_FUNC("Zw7uUVPulbw", Gen5Driver::AgcDriverGetEqContextId);
	LIB_FUNC("XlNp7jzGiPo", Gen5Driver::AgcDriverSetTFRing);
	LIB_FUNC("MM4IZSEYytQ", Gen5Driver::AgcDriverSetHsOffchipParam);
	LIB_FUNC("Ddwk4gLT5j0", Gen5Driver::AgcDriverIsCaptureInProgress);
	LIB_FUNC("U9ueyEhSkF4", Gen5Driver::AgcDriverUnknownU9ueyEhSkF4);
}

} // namespace LibGen5Driver

LIB_DEFINE(InitAgcDriver_1) {
	LibGen5::InitAgcDriver_1(s);
	LibGen5Driver::InitAgcDriver_1(s);
}

} // namespace Libs
