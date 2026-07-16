#include "common/abi.h"
#include "libs/agc.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstring>

namespace Libs {

namespace LibGen5 {

LIB_VERSION("Graphics5", 1, "Graphics5", 1, 1);

namespace Gen5 = Graphics::Gen5;

template <class T>
static T ReadUnaligned(const uint8_t* ptr, uint32_t offset) {
	T ret {};
	std::memcpy(&ret, ptr + offset, sizeof(T));
	return ret;
}

static uint32_t BitExtract(uint32_t value, uint32_t start, uint32_t len) {
	return (value >> start) & ((1u << len) - 1u);
}

static void Graphics5UnknownDbWriteDefault(uint64_t* out, uint32_t first) {
	constexpr uint64_t default_entry = 0x10000000ull;

	for (uint32_t i = first; i < 32u; i++) {
		const auto low  = static_cast<uint32_t>(default_entry) + i;
		const auto high = static_cast<uint32_t>(default_entry >> 37u) * 32u + i;
		out[i]          = (static_cast<uint64_t>(high) << 32u) | low;
	}
}

static uint32_t Graphics5UnknownDbApplyTwoBitField(uint32_t value, uint32_t field, uint32_t shift) {
	const auto mask = 0x3u << shift;

	switch (field & 0x3u) {
		case 0: return value & ~mask;
		case 1: return (value & ~mask) | (0x1u << shift);
		case 2: return (value & ~mask) | (0x2u << shift);
		default: return value | mask;
	}
}

static uint32_t Graphics5UnknownDbApplyFinalMask(uint32_t flags, uint32_t src,
                                                 uint32_t mask_value) {
	flags &= 0xffffffe0u;
	flags |= BitExtract(mask_value, 8u, 5u);
	flags &= 0xfffffbffu;
	flags |= ((src & 0x400000u) != 0 ? 0x400u : ((src >> 14u) & 0x400u));
	return flags;
}

static int KYTY_SYSV_ABI Graphics5UnknownDb(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                            uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	(void)arg3;
	(void)arg4;
	(void)arg5;

	auto* out = reinterpret_cast<uint64_t*>(arg0);

	if (arg2 == 0) {
		Graphics5UnknownDbWriteDefault(out, 0);
		return 0;
	}

	const auto* desc = reinterpret_cast<const uint8_t*>(arg1);
	const auto* src  = reinterpret_cast<const uint8_t*>(arg2);

	const auto count = ReadUnaligned<uint32_t>(src, 0x50u);

	if (count == 0) {
		Graphics5UnknownDbWriteDefault(out, 0);
		return 0;
	}

	const auto  mask_count = ReadUnaligned<uint16_t>(desc, 0x56u);
	const auto* masks = reinterpret_cast<const uint32_t*>(ReadUnaligned<uint64_t>(desc, 0x38u));
	const auto* src_entries =
	    reinterpret_cast<const uint32_t*>(ReadUnaligned<uint64_t>(src, 0x30u));

	for (uint32_t i = 0; i < count; i++) {
		auto src_value  = src_entries[i];
		auto mask_index = static_cast<uint32_t>(mask_count);

		for (uint32_t j = 0; j < mask_count; j++) {
			if ((static_cast<uint8_t>(masks[j]) ^ static_cast<uint8_t>(src_value)) == 0) {
				mask_index = j;
				break;
			}
		}

		const bool has_mask = (mask_index < mask_count);
		const auto mode     = (src_value >> 20u) & 0x3u;
		auto       flags    = 0u;

		if (mode == 0) {
			flags = (((src_value >> 24u) & 0x1u) | (has_mask ? 0u : 1u)) << 5u;
			flags = Graphics5UnknownDbApplyTwoBitField(flags, (src_value >> 28u) & 0x3u, 8u);
		} else {
			const auto shifted_mode = (src_value << 4u) & 0x03000000u;
			flags                   = shifted_mode + 0x80000u;

			if (mode == 2) {
				flags &= 0xffefffdfu;
				if (has_mask) {
					flags |= ((~(masks[mask_index] & src_value) >> 16u) & 0x20u);
				} else {
					flags |= 0x20u;
				}
				flags = Graphics5UnknownDbApplyTwoBitField(flags, (src_value >> 30u) & 0x3u, 8u);
				flags = Graphics5UnknownDbApplyTwoBitField(flags, (src_value >> 30u) & 0x3u, 21u);
			} else {
				if (has_mask) {
					const auto masked = masks[mask_index] & src_value;
					flags &= 0xffffffdfu;
					flags |= (masked >> 15u) & 0x20u;
					flags ^= 0x20u;
					flags &= 0xffefffffu;
					flags |= ((~masked >> 1u) & 0x100000u);
					flags =
					    Graphics5UnknownDbApplyTwoBitField(flags, (src_value >> 30u) & 0x3u, 8u);
				} else {
					flags |= 0x100020u;
					flags =
					    Graphics5UnknownDbApplyTwoBitField(flags, (src_value >> 28u) & 0x3u, 8u);
				}
				flags = Graphics5UnknownDbApplyTwoBitField(flags, (src_value >> 30u) & 0x3u, 21u);
			}
		}

		if (has_mask) {
			flags = Graphics5UnknownDbApplyFinalMask(flags, src_value, masks[mask_index]);
		} else {
			flags &= 0xfffffbe0u;
		}

		out[i] = (static_cast<uint64_t>(flags) << 32u) | (0x10000000u + i);
	}

	if (count < 32u) {
		Graphics5UnknownDbWriteDefault(out, count);
	}

	return 0;
}

LIB_DEFINE(InitGraphicsDriver_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("23LRUSvYu1M", Gen5::GraphicsInit);
	LIB_FUNC("2JtWUUiYBXs", Gen5::GraphicsGetRegisterDefaults2);
	LIB_FUNC("wRbq6ZjNop4", Gen5::GraphicsGetRegisterDefaults2Internal);
	LIB_FUNC("f3dg2CSgRKY", Gen5::GraphicsCreateShader);
	LIB_FUNC("dolOmWH+huQ", Gen5::GraphicsUnknownGetFusedShaderSize);
	LIB_FUNC("fd5Bp5tGTgo", Gen5::GraphicsUnknownFuseShaderHalves);
	LIB_FUNC("vcmNN+AAXnY", Gen5::GraphicsSetCxRegIndirectPatchSetAddress);
	LIB_FUNC("Qrj4c+61z4A", Gen5::GraphicsSetShRegIndirectPatchSetAddress);
	LIB_FUNC("6lNcCp+fxi4", Gen5::GraphicsSetUcRegIndirectPatchSetAddress);
	LIB_FUNC("whb1RL7K4Ss", Gen5::GraphicsSetCxRegIndirectPatchSetNumRegisters);
	LIB_FUNC("nCUgItdN2ms", Gen5::GraphicsSetShRegIndirectPatchSetNumRegisters);
	LIB_FUNC("fRG-JOH5+sI", Gen5::GraphicsSetUcRegIndirectPatchSetNumRegisters);
	LIB_FUNC("d-6uF9sZDIU", Gen5::GraphicsSetCxRegIndirectPatchAddRegisters);
	LIB_FUNC("z2duB-hHQSM", Gen5::GraphicsSetShRegIndirectPatchAddRegisters);
	LIB_FUNC("vRoArM9zaIk", Gen5::GraphicsSetUcRegIndirectPatchAddRegisters);
	LIB_FUNC("D9sr1xGUriE", Gen5::GraphicsCreatePrimState);
	LIB_FUNC("Y3ymLfZ1384", Gen5::GraphicsUpdatePrimState);
	LIB_FUNC("HV4j+E0MBHE", Gen5::GraphicsCreateInterpolantMapping);
	LIB_FUNC("V++UgBtQhn0", Gen5::GraphicsGetDataPacketPayloadAddress);
	LIB_FUNC("s+VGAMDQ0AQ", Gen5::GraphicsGetDataPacketPayloadRange);
	LIB_FUNC("fPSCdQxgpSw", Gen5::GraphicsWriteDataPatchSetAddressOrOffset);
	LIB_FUNC("bxca0BK4FNg", Gen5::GraphicsUnknownJumpPatchSetTarget);
	LIB_FUNC("2BS4EtAaF28", Gen5::GraphicsJumpPatchSetTarget);
	LIB_FUNC("h9z6+0hEydk", Gen5::GraphicsSuspendPoint);
	LIB_FUNC("qj7QZpgr9Uw", Gen5::GraphicsUnknownQj7QZpgr9Uw);
	LIB_FUNC("BfBDZGbti7A", Gen5::GraphicsGetIsTrinityMode);
	LIB_FUNC("dbOlWdppb4o", LibGen5::Graphics5UnknownDb);

	LIB_FUNC("F0ZXt5q0ZTA", Gen5::GraphicsDriverGetDefaultOwner);
	LIB_FUNC("F0Y42t-3e18", Gen5::GraphicsDriverInitResourceRegistration);
	LIB_FUNC("AOLcoIkQDgM", Gen5::GraphicsDriverQueryResourceRegistrationUserMemoryRequirements);
	LIB_FUNC("uJziRsODk1c", Gen5::GraphicsDriverGetResourceRegistrationMaxNameLength);
	LIB_FUNC("X-Nm5KLREeg", Gen5::GraphicsDriverRegisterOwner);
	LIB_FUNC("W5z4eZrjEas", Gen5::GraphicsDriverRegisterResource);
	LIB_FUNC("pWLG7WOpVcw", Gen5::GraphicsDriverUnregisterResource);
	LIB_FUNC("3AyTaWcF-H8", Gen5::GraphicsDriverRegisterWorkloadStream);

	LIB_FUNC("LtTouSCZjHM", Gen5::GraphicsCbNop);
	LIB_FUNC("t7PlZ9nt5Lc", Gen5::GraphicsCbNopGetSize);
	LIB_FUNC("k3GhuSNmBLU", Gen5::GraphicsCbDispatch);
	LIB_FUNC("Abendgtz+3o", Gen5::GraphicsCbDispatchGetSize);
	LIB_FUNC("w1KFAHVqpaU", Gen5::GraphicsCbBranch);
	LIB_FUNC("n2fD4A+pb+g", Gen5::GraphicsCbSetShRegisterRangeDirect);
	LIB_FUNC("bxGoVxpdSPQ", Gen5::GraphicsCbSetShRegisterRangeDirectGetSize);
	LIB_FUNC("UZbQjYAwwXM", Gen5::GraphicsCbSetShRegistersDirect);
	LIB_FUNC("wr23dPKyWc0", Gen5::GraphicsCbReleaseMem);
	LIB_FUNC("hL7C0IRpWZI", Gen5::GraphicsCbQueueEndOfPipeActionGetSize);
	LIB_FUNC("T6xuVw0KUJo", Gen5::GraphicsDebugRaiseException);
	LIB_FUNC("JrtiDtKeS38", Gen5::GraphicsAcbResetQueue);
	LIB_FUNC("cFazmnXpJOE", Gen5::GraphicsAcbEventWrite);
	LIB_FUNC("KT-hTp-Ch14", Gen5::GraphicsAcbAcquireMem);
	LIB_FUNC("ewobAQeMo5k", Gen5::GraphicsAcbAcquireMemGetSize);
	LIB_FUNC("qyM2bxYFPAk", Gen5::GraphicsAcbCondExec);
	LIB_FUNC("ozKzBP4aki4", Gen5::GraphicsAcbCondExecGetSize);
	LIB_FUNC("htn36gPnBk4", Gen5::GraphicsAcbWaitRegMem);
	LIB_FUNC("-RnpfpxIhec", Gen5::GraphicsAcbDmaData);
	LIB_FUNC("qzMN2XKGA4k", Gen5::GraphicsAcbCopyData);
	LIB_FUNC("j3EtxFkSIhQ", Gen5::GraphicsAcbDispatchIndirect);
	LIB_FUNC("eZ4+17OQz4Q", Gen5::GraphicsAcbWriteData);
	LIB_FUNC("xAeBOa0A3kk", Gen5::GraphicsAcbSetMarker);
	LIB_FUNC("cpCILPya5Zk", Gen5::GraphicsAcbPushMarker);
	LIB_FUNC("6mFxkVqdmbQ", Gen5::GraphicsAcbPopMarker);
	LIB_FUNC("TRO721eVt4g", Gen5::GraphicsDcbResetQueue);
	LIB_FUNC("MWiElSNE8j8", Gen5::GraphicsDcbWaitUntilSafeForRendering);
	LIB_FUNC("LFSPFmGc9Hg", Gen5::GraphicsDcbSetWorkloadsActive);
	LIB_FUNC("hEK26Wdny6s", Gen5::GraphicsDcbSetWorkloadComplete);
	LIB_FUNC("QhCbS4X9Rl8", Gen5::GraphicsDcbSetMarker);
	LIB_FUNC("pFLArOT53+w", Gen5::GraphicsDcbSetShRegisterDirect);
	LIB_FUNC("LHFXRrlTPD8", Gen5::GraphicsDcbSetCxRegisterDirect);
	LIB_FUNC("1DeUNpRIDDA", Gen5::GraphicsDcbSetCxRegisterDirectGetSize);
	LIB_FUNC("w4-d0n60hdo", Gen5::GraphicsDcbSetUcRegisterDirect);
	LIB_FUNC("ZvwO9euwYzc", Gen5::GraphicsDcbSetCxRegistersIndirect);
	LIB_FUNC("-HOOCn0JY48", Gen5::GraphicsDcbSetShRegistersIndirect);
	LIB_FUNC("hvUfkUIQcOE", Gen5::GraphicsDcbSetUcRegistersIndirect);
	LIB_FUNC("GIIW2J37e70", Gen5::GraphicsDcbSetIndexSize);
	LIB_FUNC("l4fM9K-Lyks", Gen5::GraphicsDcbSetIndexBuffer);
	LIB_FUNC("8N2tmT3jmC8", Gen5::GraphicsDcbSetIndexCount);
	LIB_FUNC("tSBxhAPyytQ", Gen5::GraphicsDcbSetNumInstances);
	LIB_FUNC("6DFuRKT4C9w", Gen5::GraphicsDcbSetNumInstancesGetSize);
	LIB_FUNC("q88lQ+GP5Yk", Gen5::GraphicsDcbDrawIndex);
	LIB_FUNC("6ee9Hd3EWXQ", Gen5::GraphicsDcbDrawIndexGetSize);
	LIB_FUNC("Ikfdt-rIqCE", Gen5::GraphicsUnknownIkfdtRIqCE);
	LIB_FUNC("Rlx+bykm0r0", Gen5::GraphicsDcbDrawIndexMultiInstanced);
	LIB_FUNC("mR9j7+SfM34", Gen5::GraphicsDcbDrawIndexMultiInstancedGetSize);
	LIB_FUNC("Yw0jKSqop+E", Gen5::GraphicsDcbDrawIndexAuto);
	LIB_FUNC("WrdP9Zxx3lQ", Gen5::GraphicsDcbDrawIndexAutoGetSize);
	LIB_FUNC("B+aG9DUnTKA", Gen5::GraphicsDcbDrawIndexOffset);
	LIB_FUNC("qMlfB1ZhMDc", Gen5::GraphicsDcbDrawIndexOffsetGetSize);
	LIB_FUNC("RmaJwLtc8rY", Gen5::GraphicsDcbSetBaseIndirectArgs);
	LIB_FUNC("1q1titRBL6o", Gen5::GraphicsDcbDrawIndirect);
	LIB_FUNC("cxPZ4Wgvdj8", Gen5::GraphicsDcbDrawIndirectGetSize);
	LIB_FUNC("t1vNu082-jM", Gen5::GraphicsDcbDrawIndexIndirect);
	LIB_FUNC("ypVBz4uPKcQ", Gen5::GraphicsDcbDrawIndexIndirectMulti);
	LIB_FUNC("CtB+A9-VxO0", Gen5::GraphicsDcbDispatchIndirect);
	LIB_FUNC("w8HVkEeXPv8", Gen5::GraphicsDcbDispatchIndirectGetSize);
	LIB_FUNC("aJf+j5yntiU", Gen5::GraphicsDcbEventWrite);
	LIB_FUNC("57labkp+rSQ", Gen5::GraphicsDcbAcquireMem);
	LIB_FUNC("-vnlTPPXPrw", Gen5::GraphicsDcbAcquireMemGetSize);
	LIB_FUNC("1rZSWUv1IRc", Gen5::GraphicsDcbCopyData);
	LIB_FUNC("WmAc2MEj6Io", Gen5::GraphicsDcbDmaData);
	LIB_FUNC("xSAR0LTcRKM", Gen5::GraphicsDcbJump);
	LIB_FUNC("VEGu4dixjUg", Gen5::GraphicsDcbJumpGetSize);
	LIB_FUNC("QIXCsbipds0", Gen5::GraphicsDcbRewindGetSize);
	LIB_FUNC("zfcxg-ewMK8", Gen5::GraphicsDcbRewind);
	LIB_FUNC("BIPexNBSGog", Gen5::GraphicsDcbCondExec);
	LIB_FUNC("ou16V5hh5sg", Gen5::GraphicsDcbCondExecGetSize);
	LIB_FUNC("bbFueFP+J4k", Gen5::GraphicsDcbSetPredication);
	LIB_FUNC("-KRzWekV120", Gen5::GraphicsUnknownKRzWekV120);
	LIB_FUNC("IxYiarKlXxM", Gen5::GraphicsDmaDataPatchSetDstAddressOrOffset);
	LIB_FUNC("cdDRpqcFGbU", Gen5::GraphicsDmaDataPatchSetSrcAddressOrOffsetOrImmediate);
	LIB_FUNC("Lkf86B98qPc", Gen5::GraphicsGetPacketSize);
	LIB_FUNC("w6Dj1VJt5qY", Gen5::GraphicsSetPacketPredication);
	LIB_FUNC("n8vgpaQg6dA", Gen5::GraphicsSetRangePredication);
	LIB_FUNC("i1jyy49AjXU", Gen5::GraphicsDcbWriteData);
	LIB_FUNC("p9tI+yTvx68", Gen5::GraphicsDcbWriteDataGetSize);
	LIB_FUNC("vuSXe69VILM", Gen5::GraphicsDcbGetLodStats);
	LIB_FUNC("VmW0Tdpy420", Gen5::GraphicsDcbWaitRegMem);
	LIB_FUNC("43WJ08sSugE", Gen5::GraphicsDcbWaitOnAddressGetSize);
	LIB_FUNC("+kSrjIVxKFE", Gen5::GraphicsDcbPushMarker);
	LIB_FUNC("H7uZqCoNuWk", Gen5::GraphicsDcbPopMarker);
	LIB_FUNC("u2T2DiA5hRI", Gen5::GraphicsDcbStallCommandBufferParser);
	LIB_FUNC("3KDcnM3lrcU", Gen5::GraphicsWaitRegMemPatchAddress);
	LIB_FUNC("7nOoijNPvEU", Gen5::GraphicsWaitRegMemPatchReference);
	LIB_FUNC("0fWWK5uG9rQ", Gen5::GraphicsQueueEndOfPipeActionPatchAddress);
	LIB_FUNC("MlEw1feXcjg", Gen5::GraphicsQueueEndOfPipeActionPatchData);
	LIB_FUNC("ORWsxIbk4TE", Gen5::GraphicsCondExecPatchSetEnd);
	LIB_FUNC("YWTKOju587o", Gen5::GraphicsCondExecPatchSetCommandAddress);
	LIB_FUNC("k-JpyR2dYAM", Gen5::GraphicsCondExecPatchSetEnd);
	LIB_FUNC("3ZWa3AoyWZQ", Gen5::GraphicsCondExecPatchSetCommandAddress);
	LIB_FUNC("YUeqkyT7mEQ", Gen5::GraphicsDcbSetFlip);
}

} // namespace LibGen5

namespace LibGen5Driver {

LIB_VERSION("Graphics5Driver", 1, "Graphics5Driver", 1, 1);

namespace Gen5Driver = Graphics::Gen5Driver;

LIB_DEFINE(InitGraphicsDriver_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("UglJIZjGssM", Gen5Driver::GraphicsDriverSubmitDcb);
	LIB_FUNC("AhGvpITrf4M", Gen5Driver::GraphicsDriverSubmitDcb);
	LIB_FUNC("6UzEidRZwkg", Gen5Driver::GraphicsDriverSubmitMultiDcbs);
	LIB_FUNC("+T8Xo6LtFJI", Gen5Driver::GraphicsDriverSubmitMultiDcbs);
	LIB_FUNC("b4fpgH5ZXxQ", Gen5Driver::GraphicsDriverSubmitCommandBuffer);
	LIB_FUNC("Fj7r9EHzF38", Gen5Driver::GraphicsDriverSubmitMultiCommandBuffers);
	LIB_FUNC("gSRnr79F8tQ", Gen5Driver::GraphicsDriverSubmitAcb);
	LIB_FUNC("HF3YllT3mXU", Gen5Driver::GraphicsDriverSubmitMultiAcbs);
	LIB_FUNC("w2rJhmD+dsE", Gen5Driver::GraphicsDriverAddEqEvent);
	LIB_FUNC("DL2RXaXOy88", Gen5Driver::GraphicsDriverDeleteEqEvent);
	LIB_FUNC("5CdQTZIQPxM", Gen5Driver::GraphicsDriverGetEqEventType);
	LIB_FUNC("Zw7uUVPulbw", Gen5Driver::GraphicsDriverGetEqContextId);
	LIB_FUNC("XlNp7jzGiPo", Gen5Driver::GraphicsDriverSetTFRing);
	LIB_FUNC("MM4IZSEYytQ", Gen5Driver::GraphicsDriverSetHsOffchipParam);
	LIB_FUNC("Ddwk4gLT5j0", Gen5Driver::GraphicsDriverIsCaptureInProgress);
	LIB_FUNC("U9ueyEhSkF4", Gen5Driver::GraphicsDriverUnknownU9ueyEhSkF4);
}

} // namespace LibGen5Driver

LIB_DEFINE(InitGraphicsDriver_1) {
	LibGen5::InitGraphicsDriver_1(s);
	LibGen5Driver::InitGraphicsDriver_1(s);
}

} // namespace Libs
