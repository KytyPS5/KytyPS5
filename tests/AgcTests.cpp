#include "libs/agc.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <array>

namespace Libs {
void InitAgcDriver_1(Loader::SymbolDatabase*);
}

namespace {

using namespace Libs::Graphics;

// A command buffer over a fixed packet array, laid out as titles pass it.
template <size_t N>
struct Packet {
	struct Layout {
		uint32_t* bottom;
		uint32_t* top;
		uint32_t* cursor_up;
		uint32_t* cursor_down;
		void*     callback;
		void*     user_data;
		uint32_t  reserved_dw;
	};

	Agc::CommandBuffer* Buf() { return reinterpret_cast<Agc::CommandBuffer*>(&layout); }

	std::array<uint32_t, N> dw {};
	Layout layout {dw.data(), dw.data() + N, dw.data(), dw.data() + N};
};

TEST(Agc, AcquireMemWritesBackInvalidatedGl2) {
	for (const auto [gcr, expected]: {std::pair {0x4123u, 0xc123u}, std::pair {0x0123u, 0x0123u}}) {
		Packet<8> dcb;
		Agc::AgcDcbAcquireMem(dcb.Buf(), 0, 0, gcr, nullptr, 0, 0);
		EXPECT_EQ(dcb.dw[7], expected);
	}
}

TEST(Agc, AcquireMemDbFlushIsGlobal) {
	const auto* base = reinterpret_cast<const volatile void*>(0x12345600);
	for (const auto [gcr, expected]: {std::pair {0x910au, 0x8101u}, std::pair {0x500bu, 0xc001u},
	                                  std::pair {0x1108u, 0x0100u}}) {
		Packet<8> dcb;
		Agc::AgcDcbAcquireMem(dcb.Buf(), 1, 0x04000040u, gcr, base, 0x100, 400);
		EXPECT_EQ(dcb.dw, (std::array<uint32_t, 8> {0xc0065800u, 0x84004040u, 0xfffffffeu, 0xffu, 1u,
		                                            0u, 0x19u, expected}));
	}
}

TEST(Agc, SetIndexCountWritesZeroAsOne) {
	for (const auto [count, expected]: {std::pair {0u, 1u}, std::pair {5u, 5u}}) {
		Packet<2> dcb;
		Agc::AgcDcbSetIndexCount(dcb.Buf(), count);
		EXPECT_EQ(dcb.dw, (std::array<uint32_t, 2> {0xc0001300u, expected}));
	}
}

TEST(Agc, CondExecWritesAnyArguments) {
	Packet<5> null_large;
	EXPECT_EQ(Agc::AgcDcbCondExec(null_large.Buf(), nullptr, 0x4005u), null_large.dw.data());
	EXPECT_EQ(null_large.dw, (std::array<uint32_t, 5> {0xc0032200u, 0u, 0u, 0u, 5u}));
	Packet<5> unaligned;
	Agc::AgcDcbCondExec(unaligned.Buf(), reinterpret_cast<const volatile uint32_t*>(0x1234561002ull), 7u);
	EXPECT_EQ(unaligned.dw, (std::array<uint32_t, 5> {0xc0032200u, 0x34561000u, 0x12u, 0u, 7u}));
}

TEST(Agc, CondExecPatchesWrapWithoutArgumentChecks) {
	std::array<uint32_t, 5> cmd {0xc0032200u, ~0u, ~0u, 0u, 0xffffc000u};
	const auto at = [&cmd](uintptr_t dwords) {
		return reinterpret_cast<const volatile uint32_t*>(reinterpret_cast<uintptr_t>(cmd.data()) + dwords * 4u);
	};
	const auto*    unaligned = reinterpret_cast<const volatile uint32_t*>(0x1234561002ull);
	constexpr auto invalid   = static_cast<int>(0x8a6c000cu);
	EXPECT_EQ(Agc::AgcCondExecPatchSetEnd(cmd.data(), at(3)), 0);
	EXPECT_EQ(cmd[4], 0xffffc000u | 0x3ffeu);
	EXPECT_EQ(Agc::AgcCondExecPatchSetEnd(cmd.data(), at(5 + 0x4001)), 0);
	EXPECT_EQ(cmd[4], 0xffffc000u | 1u);
	EXPECT_EQ(Agc::AgcCondExecPatchSetCommandAddress(cmd.data(), nullptr), 0);
	EXPECT_EQ((std::array {cmd[1], cmd[2]}), (std::array {3u, 0u}));
	EXPECT_EQ(Agc::AgcCondExecPatchSetCommandAddress(cmd.data(), unaligned), 0);
	EXPECT_EQ((std::array {cmd[1], cmd[2]}), (std::array {0x34561003u, 0x12u}));

	cmd[0] = 0xc0031000u;
	EXPECT_EQ(Agc::AgcCondExecPatchSetEnd(cmd.data(), at(9)), invalid);
	EXPECT_EQ(Agc::AgcCondExecPatchSetCommandAddress(cmd.data(), at(9)), invalid);
	EXPECT_EQ(cmd, (std::array<uint32_t, 5> {0xc0031000u, 0x34561003u, 0x12u, 0u, 0xffffc001u}));
}

TEST(Agc, EqAccessorsDecodeEventData) {
	const Libs::LibKernel::EventQueue::KernelEvent ev {
	    .filter = Libs::LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS, .data = 0x7654321ll << 16 | 0x21};
	EXPECT_EQ(AgcDriver::AgcDriverGetEqEventType(&ev), 0x43210021);
	EXPECT_EQ(AgcDriver::AgcDriverGetEqContextId(&ev), 0x7654321u);
}

TEST(Agc, BranchLeavesNullTargetSlotsZero) {
	const auto* then_target = reinterpret_cast<const volatile uint32_t*>(0x1234567898ull);
	Packet<14> dcb;
	Agc::AgcCbBranch(dcb.Buf(), 1, 3, nullptr, 0, 0, 1, then_target, 0x10u, 2, nullptr, 0x20u);
	EXPECT_EQ(dcb.dw, (std::array<uint32_t, 14> {0xc00c3f00u, 0x301u, 0u, 0u, 0u, 0u, 0u, 0u,
	                                             0x34567898u, 0x12u, 0x10000010u, 0u, 0u, 0u}));
}

TEST(Agc, DebugRaiseExceptionFails) {
	EXPECT_EQ(Agc::AgcDebugRaiseException(0), static_cast<int>(0x8a6d0003u));
}

TEST(Agc, AgrSubmitFailsWithoutSubmitting) {
	EXPECT_EQ(AgcDriver::AgcDriverAgrSubmit(), static_cast<int>(0x8a6d0003u));
	Loader::SymbolDatabase symbols;
	Libs::InitAgcDriver_1(&symbols);
	for (const char* nid: {"AhGvpITrf4M", "+T8Xo6LtFJI"}) {
		const auto* func = symbols.FindByNid(nid, Loader::SymbolType::Func);
		ASSERT_NE(func, nullptr);
		EXPECT_EQ(func->vaddr, reinterpret_cast<uint64_t>(&AgcDriver::AgcDriverAgrSubmit));
	}
}

TEST(Agc, EmptyMultiSubmitsFail) {
	constexpr auto empty = static_cast<int>(0x8a6d0109u);
	EXPECT_EQ(AgcDriver::AgcDriverSubmitMultiDcbs(nullptr, nullptr, 0), empty);
	EXPECT_EQ(AgcDriver::AgcDriverSubmitMultiCommandBuffers(nullptr, nullptr, nullptr, 0), empty);
	EXPECT_EQ(AgcDriver::AgcDriverSubmitMultiAcbs(0, nullptr, nullptr, 0), empty);
}

TEST(Agc, RegisterWorkloadStreamChecksRegistrationFirst) {
	EXPECT_EQ(Agc::AgcDriverRegisterWorkloadStream(5, "x"), 0);
	EXPECT_EQ(Agc::AgcDriverRegisterWorkloadStream(5, nullptr), static_cast<int>(0x8a6c0033u));
	EXPECT_EQ(Agc::AgcDriverRegisterWorkloadStream(6, nullptr), static_cast<int>(0x8a6c0035u));
}

TEST(Agc, IndexedMultiDrawsAreTagged) {
	Packet<15> instanced;
	EXPECT_EQ(Agc::AgcDcbDrawIndexMultiInstanced(instanced.Buf(), 2, reinterpret_cast<const volatile void*>(0x1000),
	                                              reinterpret_cast<const volatile void*>(0x2000), 0, 0),
	          instanced.dw.data());
	EXPECT_EQ(instanced.dw, (std::array<uint32_t, 15> {0xc0017904u, 0x342u, 0xc6000006u, 0xc0073a00u, 2u, 0x1000u, 0u,
	                                                   1u, 0x2000u, 0u, 0u, 0x80u, 0xc0017904u, 0x342u, 0xc6000000u}));
	EXPECT_EQ(Agc::AgcDcbDrawIndexMultiInstancedGetSize(), 60u);
	EXPECT_TRUE(Agc::AgcIsInternalDataPacket(instanced.dw[0], &instanced.dw[1]));

	Packet<16> indirect;
	EXPECT_EQ(Agc::AgcDcbDrawIndexIndirectMulti(indirect.Buf(), 0x10u, 1, 2, nullptr, 0x20u, 0), indirect.dw.data());
	const auto& dw = indirect.dw;
	EXPECT_EQ((std::array {dw[0], dw[1], dw[2], dw[3], dw[13], dw[14], dw[15]}),
	          (std::array<uint32_t, 7> {0xc0017904u, 0x342u, 0xc6000005u, 0xc0083800u, 0xc0017904u, 0x342u, 0xc6000000u}));
	EXPECT_TRUE(Agc::AgcIsInternalDataPacket(dw[0], &dw[1]));
}

TEST(Agc, JumpPatchSetTargetKeepsLowBitsAndWritesHighDword) {
	std::array<uint32_t, 4> jump {0xc0023f00u, 3u, 0xffff0000u, 0x12300000u};
	EXPECT_EQ(Agc::AgcJumpPatchSetTarget(jump.data(), reinterpret_cast<const volatile uint32_t*>(0x123456789abcull),
	                                      0x10u),
	          0);
	EXPECT_EQ(jump, (std::array<uint32_t, 4> {0xc0023f00u, 0x56789abfu, 0x1234u, 0x12300010u}));
}

TEST(Agc, AcbUserDataHasNoR) {
	uint64_t   label = 0;
	Packet<16> dcb;
	Packet<16> acb;
	Agc::AgcDcbWaitRegMem(dcb.Buf(), 1, 3, 0, 0, &label, 1, ~0ull, 400);
	Agc::AgcAcbWaitRegMem(acb.Buf(), 1, 3, 0, &label, 1, ~0ull, 400);
	EXPECT_EQ((std::array {dcb.dw[0], dcb.dw[13], acb.dw[0], acb.dw[13]}),
	          (std::array {0xc0027904u, 0xc0017904u, 0xc0027900u, 0xc0017900u}));
	dcb.dw[0] = acb.dw[0];
	dcb.dw[13] = acb.dw[13];
	EXPECT_EQ(acb.dw, dcb.dw);
	EXPECT_EQ(Agc::AgcWaitRegMemPatchReference(acb.dw.data(), 2), 0);

	Packet<11> semaphore;
	Agc::AgcAcbMemSemaphore(semaphore.Buf(), &label, 0, 0, 7);
	EXPECT_EQ((std::array {semaphore.dw[0], semaphore.dw[8]}), (std::array {0xc0027900u, 0xc0017900u}));
}

TEST(AgcDeathTest, DrawIndirectMultiNeedsABuffer) {
	EXPECT_DEATH(Agc::AgcDcbDrawIndirectMulti(nullptr, 0, 0, 0, nullptr, 0, 0), "");
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
