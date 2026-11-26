#include "gpu_blit_shaders/gpu_mesh_indirect_spv.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp>

#include <cstdint>
#include <iterator>

namespace {

TEST(ImageMetadata, ColorRangeMayHaveUnknownSizeButMustHaveAValidAddress) {
	using namespace Libs::Graphics;
	ImageMetadataInfo metadata {};
	EXPECT_FALSE(metadata.ValidColorRange());
	metadata.range = {LOWER_ADDRESS_SIZE - 1, 0};
	EXPECT_TRUE(metadata.ValidColorRange());
	metadata.range.size = 1;
	EXPECT_TRUE(metadata.ValidColorRange());
	metadata.range.size = 2;
	EXPECT_FALSE(metadata.ValidColorRange());
	metadata.range = {LOWER_ADDRESS_SIZE, 0};
	EXPECT_FALSE(metadata.ValidColorRange());
	metadata.range = {UINT64_MAX, 2};
	EXPECT_FALSE(metadata.ValidColorRange());
}

TEST(VideoOutFormat, SourceFlagsAndRouting) {
	struct Case {
		uint64_t format;
		uint32_t flags;
		bool     draw_on_sdr;
		bool     draw_on_hdr;
	};
	// sRGB, linear, PQ and linear-nit PQ formats in R and B order; drawn sources go through the
	// overlay shader, the others are blitted.
	for (const auto& c: {Case {0x8000000022000000ull, 0, false, false},
	                     Case {0x8000000000000000ull, 0, false, false},
	                     Case {0x8100000022000000ull, 0, false, false},
	                     Case {0x8100000000000000ull, 0, false, false},
	                     Case {0x8100000622000000ull, 8, true, true},
	                     Case {0x8100000600000000ull, 8, true, true},
	                     Case {0xc001000622000000ull, 8, true, true},
	                     Case {0xc001000600000000ull, 8, true, true},
	                     Case {0x8100070422000000ull, 4, true, false},
	                     Case {0x8100070400000000ull, 4, true, false},
	                     Case {0xc001070722000000ull, 12, true, true},
	                     Case {0xc001070700000000ull, 12, true, true}}) {
		SCOPED_TRACE(testing::Message() << std::hex << c.format);
		const uint32_t flags = Libs::Graphics::VideoOutSourceFlags(c.format);
		EXPECT_EQ(flags, c.flags);
		EXPECT_EQ(Libs::Graphics::VideoOutSourceNeedsDraw(flags, false), c.draw_on_sdr);
		EXPECT_EQ(Libs::Graphics::VideoOutSourceNeedsDraw(flags, true), c.draw_on_hdr);
	}
}

TEST(VideoOutFormat, DccClearColorNeedsCompressedBuffers) {
	// PPSA28824 sets its R8G8B8A8 register clear color on a DCC 256/64/64 display buffer.
	EXPECT_EQ(Libs::Graphics::ClassifyVideoOutCompression(true, 0x215468000ull, 0x208, 0x311308),
	          Libs::Graphics::VideoOutCompression::Dcc256_64_64);
	EXPECT_EQ(Libs::Graphics::ClassifyVideoOutCompression(false, 0, 0, 0x311308),
	          Libs::Graphics::VideoOutCompression::Unsupported);
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
