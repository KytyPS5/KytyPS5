#include "libs/agc.h"

#include <gtest/gtest.h>

#include <array>

namespace {

namespace Gen5 = Libs::Graphics::Gen5;

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

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
