#include "libs/agc.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <array>

namespace Libs {
void InitAgcDriver_1(Loader::SymbolDatabase*);
}

namespace {

namespace Gen5       = Libs::Graphics::Gen5;
namespace Gen5Driver = Libs::Graphics::Gen5Driver;

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

	Gen5::CommandBuffer* Buf() { return reinterpret_cast<Gen5::CommandBuffer*>(&layout); }

	std::array<uint32_t, N> dw {};
	Layout layout {dw.data(), dw.data() + N, dw.data(), dw.data() + N};
};

TEST(Agc, AcquireMemWritesBackInvalidatedGl2) {
	for (const auto [gcr, expected]: {std::pair {0x4123u, 0xc123u}, std::pair {0x0123u, 0x0123u}}) {
		Packet<8> dcb;
		Gen5::AgcDcbAcquireMem(dcb.Buf(), 0, 0, gcr, nullptr, 0, 0);
		EXPECT_EQ(dcb.dw[7], expected);
	}
}

TEST(Agc, AcquireMemDbFlushIsGlobal) {
	const auto* base = reinterpret_cast<const volatile void*>(0x12345600);
	for (const auto [gcr, expected]: {std::pair {0x910au, 0x8101u}, std::pair {0x500bu, 0xc001u},
	                                  std::pair {0x1108u, 0x0100u}}) {
		Packet<8> dcb;
		Gen5::AgcDcbAcquireMem(dcb.Buf(), 1, 0x04000040u, gcr, base, 0x100, 400);
		EXPECT_EQ(dcb.dw, (std::array<uint32_t, 8> {0xc0065800u, 0x84004040u, 0xfffffffeu, 0xffu, 1u,
		                                            0u, 0x19u, expected}));
	}
}

TEST(Agc, SetIndexCountWritesZeroAsOne) {
	for (const auto [count, expected]: {std::pair {0u, 1u}, std::pair {5u, 5u}}) {
		Packet<2> dcb;
		Gen5::AgcDcbSetIndexCount(dcb.Buf(), count);
		EXPECT_EQ(dcb.dw, (std::array<uint32_t, 2> {0xc0001300u, expected}));
	}
}

TEST(Agc, CondExecWritesAnyArguments) {
	Packet<5> null_large;
	EXPECT_EQ(Gen5::AgcDcbCondExec(null_large.Buf(), nullptr, 0x4005u), null_large.dw.data());
	EXPECT_EQ(null_large.dw, (std::array<uint32_t, 5> {0xc0032200u, 0u, 0u, 0u, 5u}));
	Packet<5> unaligned;
	Gen5::AgcDcbCondExec(unaligned.Buf(), reinterpret_cast<const volatile uint32_t*>(0x1234561002ull), 7u);
	EXPECT_EQ(unaligned.dw, (std::array<uint32_t, 5> {0xc0032200u, 0x34561000u, 0x12u, 0u, 7u}));
}

TEST(Agc, BranchLeavesNullTargetSlotsZero) {
	const auto* then_target = reinterpret_cast<const volatile uint32_t*>(0x1234567898ull);
	Packet<14> dcb;
	Gen5::AgcCbBranch(dcb.Buf(), 1, 3, nullptr, 0, 0, 1, then_target, 0x10u, 2, nullptr, 0x20u);
	EXPECT_EQ(dcb.dw, (std::array<uint32_t, 14> {0xc00c3f00u, 0x301u, 0u, 0u, 0u, 0u, 0u, 0u,
	                                             0x34567898u, 0x12u, 0x10000010u, 0u, 0u, 0u}));
}

TEST(Agc, DebugRaiseExceptionFails) {
	EXPECT_EQ(Gen5::AgcDebugRaiseException(0), static_cast<int>(0x8a6d0003u));
}

TEST(Agc, AgrSubmitFailsWithoutSubmitting) {
	EXPECT_EQ(Gen5Driver::AgcDriverAgrSubmit(), static_cast<int>(0x8a6d0003u));
	Loader::SymbolDatabase symbols;
	Libs::InitAgcDriver_1(&symbols);
	for (const char* nid: {"AhGvpITrf4M", "+T8Xo6LtFJI"}) {
		const auto* func = symbols.FindByNid(nid, Loader::SymbolType::Func);
		ASSERT_NE(func, nullptr);
		EXPECT_EQ(func->vaddr, reinterpret_cast<uint64_t>(&Gen5Driver::AgcDriverAgrSubmit));
	}
}

TEST(Agc, EmptyMultiSubmitsFail) {
	constexpr auto empty = static_cast<int>(0x8a6d0109u);
	EXPECT_EQ(Gen5Driver::AgcDriverSubmitMultiDcbs(nullptr, nullptr, 0), empty);
	EXPECT_EQ(Gen5Driver::AgcDriverSubmitMultiCommandBuffers(nullptr, nullptr, nullptr, 0), empty);
	EXPECT_EQ(Gen5Driver::AgcDriverSubmitMultiAcbs(0, nullptr, nullptr, 0), empty);
}

TEST(Agc, RegisterWorkloadStreamChecksRegistrationFirst) {
	EXPECT_EQ(Gen5::AgcDriverRegisterWorkloadStream(5, "x"), 0);
	EXPECT_EQ(Gen5::AgcDriverRegisterWorkloadStream(5, nullptr), static_cast<int>(0x8a6c0033u));
	EXPECT_EQ(Gen5::AgcDriverRegisterWorkloadStream(6, nullptr), static_cast<int>(0x8a6c0035u));
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
