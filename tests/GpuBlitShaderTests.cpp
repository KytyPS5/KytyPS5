#include "gpu_blit_shaders/gpu_mesh_indirect_spv.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp>

#include <cstdint>
#include <iterator>

namespace {

using namespace Libs;

TEST(VideoOutFormat, DetectsHdrPixelFormats) {
	EXPECT_TRUE(Graphics::IsHdrVideoOutPixelFormat(Graphics::VIDEO_OUT_PIXEL_FORMAT_R10_G10_B10_A2_BT2100_PQ));
	EXPECT_TRUE(Graphics::IsHdrVideoOutPixelFormat(Graphics::VIDEO_OUT_PIXEL_FORMAT_B10_G10_R10_A2_BT2100_PQ));
	EXPECT_TRUE(Graphics::IsHdrVideoOutPixelFormat(Graphics::VIDEO_OUT_PIXEL_FORMAT_R16_G16_B16_A16_FLOAT_FOR_BT2100_PQ));
	EXPECT_TRUE(Graphics::IsHdrVideoOutPixelFormat(Graphics::VIDEO_OUT_PIXEL_FORMAT_B16_G16_R16_A16_FLOAT_FOR_BT2100_PQ));
	EXPECT_FALSE(Graphics::IsHdrVideoOutPixelFormat(0x8000000022000000ull));
	EXPECT_FALSE(Graphics::IsHdrVideoOutPixelFormat(0x8100000022000000ull));
}

TEST(VideoOutFormat, DistinguishesLinearFromPqFormats) {
	EXPECT_TRUE(Graphics::IsLinearVideoOutPixelFormat(0x8100000622000000ull));
	EXPECT_TRUE(Graphics::IsLinearVideoOutPixelFormat(0x8100000600000000ull));
	EXPECT_TRUE(Graphics::IsLinearVideoOutPixelFormat(0xc001000622000000ull));
	EXPECT_TRUE(Graphics::IsLinearVideoOutPixelFormat(0xc001000600000000ull));
	EXPECT_TRUE(Graphics::IsLinearVideoOutPixelFormat(Graphics::VIDEO_OUT_PIXEL_FORMAT_R16_G16_B16_A16_FLOAT_FOR_BT2100_PQ));
	EXPECT_TRUE(Graphics::IsLinearVideoOutPixelFormat(Graphics::VIDEO_OUT_PIXEL_FORMAT_B16_G16_R16_A16_FLOAT_FOR_BT2100_PQ));
	EXPECT_FALSE(Graphics::IsLinearVideoOutPixelFormat(Graphics::VIDEO_OUT_PIXEL_FORMAT_R10_G10_B10_A2_BT2100_PQ));
	EXPECT_FALSE(Graphics::IsLinearVideoOutPixelFormat(Graphics::VIDEO_OUT_PIXEL_FORMAT_B10_G10_R10_A2_BT2100_PQ));
	EXPECT_FALSE(Graphics::IsLinearVideoOutPixelFormat(0x8000000022000000ull));
	EXPECT_FALSE(Graphics::IsLinearVideoOutPixelFormat(0x8100000022000000ull));
}

TEST(GpuBlitShaders, MeshIndirectTaskDeclaresLocalSize) {
	bool local_size    = false;
	bool local_size_id = false;
	for (size_t i = 5; i < std::size(GPU_MESH_INDIRECT_SPV);) {
		const uint32_t word  = GPU_MESH_INDIRECT_SPV[i];
		const uint32_t count = word >> spv::WordCountShift;
		ASSERT_NE(count, 0u);
		ASSERT_LE(i + count, std::size(GPU_MESH_INDIRECT_SPV));
		const auto op = static_cast<spv::Op>(word & spv::OpCodeMask);
		if (count > 2 && (op == spv::OpExecutionMode || op == spv::OpExecutionModeId)) {
			const auto mode = static_cast<spv::ExecutionMode>(GPU_MESH_INDIRECT_SPV[i + 2]);
			local_size |= mode == spv::ExecutionModeLocalSize;
			local_size_id |= mode == spv::ExecutionModeLocalSizeId;
		}
		i += count;
	}
	EXPECT_TRUE(local_size);
	EXPECT_FALSE(local_size_id);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
