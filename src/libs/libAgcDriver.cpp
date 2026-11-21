#include "common/abi.h"
#include "libs/agc.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

namespace Graphics::Agc {

LIB_NAME("Agc", "Agc");

LIB_DEFINE(InitAgcDriver_1) {
	LIB_FUNC("23LRUSvYu1M", AgcInit);
	LIB_FUNC("2JtWUUiYBXs", AgcGetRegisterDefaults2);
	LIB_FUNC("wRbq6ZjNop4", AgcGetRegisterDefaults2Internal);
	LIB_FUNC("f3dg2CSgRKY", AgcCreateShader);
	LIB_FUNC("dolOmWH+huQ", AgcGetFusedShaderSize);
	LIB_FUNC("fd5Bp5tGTgo", AgcFuseShaderHalves_0200);
	LIB_FUNC("nApJjpKNBl4", AgcFuseShaderHalves);
	LIB_FUNC("vcmNN+AAXnY", AgcSetCxRegIndirectPatchSetAddress);
	LIB_FUNC("Qrj4c+61z4A", AgcSetShRegIndirectPatchSetAddress);
	LIB_FUNC("6lNcCp+fxi4", AgcSetUcRegIndirectPatchSetAddress);
	LIB_FUNC("whb1RL7K4Ss", AgcSetCxRegIndirectPatchSetNumRegisters);
	LIB_FUNC("nCUgItdN2ms", AgcSetShRegIndirectPatchSetNumRegisters);
	LIB_FUNC("fRG-JOH5+sI", AgcSetUcRegIndirectPatchSetNumRegisters);
	LIB_FUNC("d-6uF9sZDIU", AgcSetCxRegIndirectPatchAddRegisters);
	LIB_FUNC("z2duB-hHQSM", AgcSetShRegIndirectPatchAddRegisters);
	LIB_FUNC("vRoArM9zaIk", AgcSetUcRegIndirectPatchAddRegisters);
	LIB_FUNC("D9sr1xGUriE", AgcCreatePrimState);
	LIB_FUNC("Y3ymLfZ1384", AgcUpdatePrimState);
	LIB_FUNC("NKIzURsgV7I", AgcGetGsOversubscription);
	LIB_FUNC("HV4j+E0MBHE", AgcCreateInterpolantMapping);
	LIB_FUNC("V++UgBtQhn0", AgcGetDataPacketPayloadAddress);
	LIB_FUNC("s+VGAMDQ0AQ", AgcGetDataPacketPayloadRange);
	LIB_FUNC("fPSCdQxgpSw", AgcWriteDataPatchSetAddressOrOffset);
	LIB_FUNC("2BS4EtAaF28", AgcJumpPatchSetTarget);
	LIB_FUNC("h9z6+0hEydk", AgcSuspendPoint);
	LIB_FUNC("qj7QZpgr9Uw", AgcDcbContextStateOp);
	LIB_FUNC("H6vHS5cidSA", AgcDcbContextStateOpGetSize);
	LIB_FUNC("BfBDZGbti7A", AgcGetIsTrinityMode);
	LIB_FUNC("dbOlWdppb4o", AgcCreateInterpolantMapping2);

	LIB_FUNC("LtTouSCZjHM", AgcCbNop);
	LIB_FUNC("t7PlZ9nt5Lc", AgcCbNopGetSize);
	LIB_FUNC("k3GhuSNmBLU", AgcCbDispatch);
	LIB_FUNC("Abendgtz+3o", AgcCbDispatchGetSize);
	LIB_FUNC("w1KFAHVqpaU", AgcCbBranch);
	LIB_FUNC("uZW-mqsxkrM", AgcCbBranchGetSize);
	LIB_FUNC("7Wa3aeJgeVU", AgcBranchPatchSetThenTarget);
	LIB_FUNC("n2fD4A+pb+g", AgcCbSetShRegisterRangeDirect);
	LIB_FUNC("bxGoVxpdSPQ", AgcCbSetShRegisterRangeDirectGetSize);
	LIB_FUNC("UZbQjYAwwXM", AgcCbSetShRegistersDirect);
	LIB_FUNC("03RZmELWWzw", AgcCbSetUcRegistersDirect);
	LIB_FUNC("wr23dPKyWc0", AgcCbReleaseMem);
	LIB_FUNC("hL7C0IRpWZI", AgcCbQueueEndOfPipeActionGetSize);
	LIB_FUNC("T6xuVw0KUJo", AgcDebugRaiseException);
	LIB_FUNC("JrtiDtKeS38", AgcAcbResetQueue);
	LIB_FUNC("cFazmnXpJOE", AgcAcbEventWrite);
	LIB_FUNC("KT-hTp-Ch14", AgcAcbAcquireMem);
	LIB_FUNC("ewobAQeMo5k", AgcAcbAcquireMemGetSize);
	LIB_FUNC("qyM2bxYFPAk", AgcAcbCondExec);
	LIB_FUNC("ozKzBP4aki4", AgcAcbCondExecGetSize);
	LIB_FUNC("e1DFTg+Sd8U", AgcAcbJump);
	LIB_FUNC("b-oySn+G2tE", AgcAcbJumpGetSize);
	LIB_FUNC("DwICrVxerkY", AgcAcbRewind);
	LIB_FUNC("htn36gPnBk4", AgcAcbWaitRegMem);
	LIB_FUNC("q4VuU-QsLOE", AgcAcbMemSemaphore);
	LIB_FUNC("idlaArvdXEs", AgcAcbWaitOnAddressGetSize);
	LIB_FUNC("-RnpfpxIhec", AgcAcbDmaData);
	LIB_FUNC("qzMN2XKGA4k", AgcAcbCopyData);
	LIB_FUNC("CbQh3DKMSno", AgcAcbCopyDataGetSize);
	LIB_FUNC("j3EtxFkSIhQ", AgcAcbDispatchIndirect);
	LIB_FUNC("eZ4+17OQz4Q", AgcAcbWriteData);
	LIB_FUNC("xAeBOa0A3kk", AgcAcbSetMarker);
	LIB_FUNC("cpCILPya5Zk", AgcAcbPushMarker);
	LIB_FUNC("6mFxkVqdmbQ", AgcAcbPopMarker);
	LIB_FUNC("TRO721eVt4g", AgcDcbResetQueue);
	LIB_FUNC("MWiElSNE8j8", AgcDcbWaitUntilSafeForRendering);
	LIB_FUNC("LFSPFmGc9Hg", AgcDcbSetWorkloadsActive);
	LIB_FUNC("hEK26Wdny6s", AgcDcbSetWorkloadComplete);
	LIB_FUNC("QhCbS4X9Rl8", AgcDcbSetMarker);
	LIB_FUNC("pFLArOT53+w", AgcDcbSetShRegisterDirect);
	LIB_FUNC("LHFXRrlTPD8", AgcDcbSetCxRegisterDirect);
	LIB_FUNC("1DeUNpRIDDA", AgcDcbSetCxRegisterDirectGetSize);
	LIB_FUNC("w4-d0n60hdo", AgcDcbSetUcRegisterDirect);
	LIB_FUNC("ZvwO9euwYzc", AgcDcbSetCxRegistersIndirect);
	LIB_FUNC("GBCh3zCihoU", AgcDcbSetCxRegistersIndirectGetSize);
	LIB_FUNC("-HOOCn0JY48", AgcDcbSetShRegistersIndirect);
	LIB_FUNC("nNlUtdDDvZ0", AgcDcbSetShRegistersIndirectGetSize);
	LIB_FUNC("hvUfkUIQcOE", AgcDcbSetUcRegistersIndirect);
	LIB_FUNC("GIIW2J37e70", AgcDcbSetIndexSize);
	LIB_FUNC("l4fM9K-Lyks", AgcDcbSetIndexBuffer);
	LIB_FUNC("8N2tmT3jmC8", AgcDcbSetIndexCount);
	LIB_FUNC("tSBxhAPyytQ", AgcDcbSetNumInstances);
	LIB_FUNC("6DFuRKT4C9w", AgcDcbSetNumInstancesGetSize);
	LIB_FUNC("q88lQ+GP5Yk", AgcDcbDrawIndex);
	LIB_FUNC("6ee9Hd3EWXQ", AgcDcbDrawIndexGetSize);
	LIB_FUNC("Ikfdt-rIqCE", AgcJumpPatchSetTarget_0300);
	LIB_FUNC("Rlx+bykm0r0", AgcDcbDrawIndexMultiInstanced);
	LIB_FUNC("mR9j7+SfM34", AgcDcbDrawIndexMultiInstancedGetSize);
	LIB_FUNC("Yw0jKSqop+E", AgcDcbDrawIndexAuto);
	LIB_FUNC("WrdP9Zxx3lQ", AgcDcbDrawIndexAutoGetSize);
	LIB_FUNC("B+aG9DUnTKA", AgcDcbDrawIndexOffset);
	LIB_FUNC("qMlfB1ZhMDc", AgcDcbDrawIndexOffsetGetSize);
	LIB_FUNC("RmaJwLtc8rY", AgcDcbSetBaseIndirectArgs);
	LIB_FUNC("1q1titRBL6o", AgcDcbDrawIndirect);
	LIB_FUNC("cxPZ4Wgvdj8", AgcDcbDrawIndirectGetSize);
	LIB_FUNC("kUlvghKs-mA", AgcDcbDrawIndirectMulti);
	LIB_FUNC("t1vNu082-jM", AgcDcbDrawIndexIndirect);
	LIB_FUNC("ypVBz4uPKcQ", AgcDcbDrawIndexIndirectMulti);
	LIB_FUNC("CtB+A9-VxO0", AgcDcbDispatchIndirect);
	LIB_FUNC("w8HVkEeXPv8", AgcDcbDispatchIndirectGetSize);
	LIB_FUNC("aJf+j5yntiU", AgcDcbEventWrite);
	LIB_FUNC("C4l9fB17t8w", AgcDcbEventWriteGetSize);
	LIB_FUNC("57labkp+rSQ", AgcDcbAcquireMem);
	LIB_FUNC("-vnlTPPXPrw", AgcDcbAcquireMemGetSize);
	LIB_FUNC("1rZSWUv1IRc", AgcDcbCopyData);
	LIB_FUNC("b5u0Jzm8TF8", AgcDcbCopyDataGetSize);
	LIB_FUNC("WmAc2MEj6Io", AgcDcbDmaData);
	LIB_FUNC("xSAR0LTcRKM", AgcDcbJump);
	LIB_FUNC("VEGu4dixjUg", AgcDcbJumpGetSize);
	LIB_FUNC("QIXCsbipds0", AgcDcbRewindGetSize);
	LIB_FUNC("zfcxg-ewMK8", AgcDcbRewind);
	LIB_FUNC("BIPexNBSGog", AgcDcbCondExec);
	LIB_FUNC("ou16V5hh5sg", AgcDcbCondExecGetSize);
	LIB_FUNC("bbFueFP+J4k", AgcDcbSetPredication);
	LIB_FUNC("-KRzWekV120", AgcDcbSetIndexSize_0600);
	LIB_FUNC("IxYiarKlXxM", AgcDmaDataPatchSetDstAddressOrOffset);
	LIB_FUNC("cdDRpqcFGbU", AgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate);
	LIB_FUNC("Lkf86B98qPc", AgcGetPacketSize);
	LIB_FUNC("w6Dj1VJt5qY", AgcSetPacketPredication);
	LIB_FUNC("n8vgpaQg6dA", AgcSetRangePredication);
	LIB_FUNC("K2mciNVxUCE", AgcSetNop);
	LIB_FUNC("i1jyy49AjXU", AgcDcbWriteData);
	LIB_FUNC("p9tI+yTvx68", AgcDcbWriteDataGetSize);
	LIB_FUNC("vuSXe69VILM", AgcDcbGetLodStats);
	LIB_FUNC("VmW0Tdpy420", AgcDcbWaitRegMem);
	LIB_FUNC("43WJ08sSugE", AgcDcbWaitOnAddressGetSize);
	LIB_FUNC("+kSrjIVxKFE", AgcDcbPushMarker);
	LIB_FUNC("H7uZqCoNuWk", AgcDcbPopMarker);
	LIB_FUNC("u2T2DiA5hRI", AgcDcbStallCommandBufferParser);
	LIB_FUNC("3KDcnM3lrcU", AgcWaitRegMemPatchAddress);
	LIB_FUNC("7nOoijNPvEU", AgcWaitRegMemPatchReference);
	LIB_FUNC("n485EBnIWmk", AgcWaitRegMemPatchCompareFunction);
	LIB_FUNC("0fWWK5uG9rQ", AgcQueueEndOfPipeActionPatchAddress);
	LIB_FUNC("MlEw1feXcjg", AgcQueueEndOfPipeActionPatchData);
	LIB_FUNC("ORWsxIbk4TE", AgcCondExecPatchSetEnd);
	LIB_FUNC("YWTKOju587o", AgcCondExecPatchSetCommandAddress);
	LIB_FUNC("k-JpyR2dYAM", AgcCondExecPatchSetEnd); // sceAgcAsyncCondExecPatchSetEnd
	LIB_FUNC("3ZWa3AoyWZQ",
	         AgcCondExecPatchSetCommandAddress); // sceAgcAsyncCondExecPatchSetCommandAddress
	LIB_FUNC("ziVA3whp3p4", AgcRewindPatchSetRewindState);
	LIB_FUNC("eWaWyFegzgQ", AgcRewindPatchSetRewindState); // sceAgcAsyncRewindPatchSetRewindState
	LIB_FUNC("YUeqkyT7mEQ", AgcDcbSetFlip);
	LIB_FUNC("ebixW91gpPw", AgcAcbSetFlip);
}

} // namespace Graphics::Agc

namespace Graphics::AgcDriver {

LIB_NAME("AgcDriver", "AgcDriver");

LIB_DEFINE(InitAgcDriver_1) {
	LIB_FUNC("UglJIZjGssM", AgcDriverSubmitDcb);
	LIB_FUNC("AhGvpITrf4M", AgcDriverAgrSubmit); // sceAgcDriverAgrSubmitDcb
	LIB_FUNC("6UzEidRZwkg", AgcDriverSubmitMultiDcbs);
	LIB_FUNC("+T8Xo6LtFJI", AgcDriverAgrSubmit); // sceAgcDriverAgrSubmitMultiDcbs
	LIB_FUNC("b4fpgH5ZXxQ", AgcDriverSubmitCommandBuffer);
	LIB_FUNC("Fj7r9EHzF38", AgcDriverSubmitMultiCommandBuffers);
	LIB_FUNC("gSRnr79F8tQ", AgcDriverSubmitAcb);
	LIB_FUNC("HF3YllT3mXU", AgcDriverSubmitMultiAcbs);
	LIB_FUNC("w2rJhmD+dsE", AgcDriverAddEqEvent);
	LIB_FUNC("DL2RXaXOy88", AgcDriverDeleteEqEvent);
	LIB_FUNC("5CdQTZIQPxM", AgcDriverGetEqEventType);
	LIB_FUNC("Zw7uUVPulbw", AgcDriverGetEqContextId);
	LIB_FUNC("XlNp7jzGiPo", AgcDriverSetTFRing);
	LIB_FUNC("MM4IZSEYytQ", AgcDriverSetHsOffchipParam);
	LIB_FUNC("Ddwk4gLT5j0", AgcDriverIsCaptureInProgress);
	LIB_FUNC("U9ueyEhSkF4", AgcDriverRegisterDefaultOwner);
	LIB_FUNC("F0ZXt5q0ZTA", Agc::AgcDriverGetDefaultOwner);
	LIB_FUNC("F0Y42t-3e18", Agc::AgcDriverInitResourceRegistration);
	LIB_FUNC("AOLcoIkQDgM", Agc::AgcDriverQueryResourceRegistrationUserMemoryRequirements);
	LIB_FUNC("uJziRsODk1c", Agc::AgcDriverGetResourceRegistrationMaxNameLength);
	LIB_FUNC("X-Nm5KLREeg", Agc::AgcDriverRegisterOwner);
	LIB_FUNC("W5z4eZrjEas", Agc::AgcDriverRegisterResource);
	LIB_FUNC("ZLJk9r2+2Aw", Agc::AgcDriverUnregisterOwnerAndResources);
	LIB_FUNC("pWLG7WOpVcw", Agc::AgcDriverUnregisterResource);
	LIB_FUNC("3AyTaWcF-H8", Agc::AgcDriverRegisterWorkloadStream);
}

} // namespace Graphics::AgcDriver

LIB_DEFINE(InitAgcDriver_1) {
	Graphics::Agc::InitAgcDriver_1(s);
	Graphics::AgcDriver::InitAgcDriver_1(s);
}

} // namespace Libs
