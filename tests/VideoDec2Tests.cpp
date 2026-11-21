#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace Libs::VideoDec2 {
void InitVideoDec2_1(Loader::SymbolDatabase*);
}

namespace {

// The decoder configuration and memory information as titles lay them out.
struct DecoderConfig {
	size_t      this_size     = sizeof(DecoderConfig);
	uint32_t    resource_type = 1;
	uint32_t    codec_type    = 0;
	uint32_t    profile       = 0;
	uint32_t    max_level     = 0;
	int32_t     max_frame_width;
	int32_t     max_frame_height;
	int32_t     max_dpb_frame_count      = -1;
	uint32_t    decode_input_queue_depth = 1;
	void*       compute_queue            = nullptr;
	uint64_t    cpu_affinity_mask        = 0;
	int32_t     cpu_thread_priority      = 0;
	uint8_t     flags_and_reserved[4]    = {};
	const void* extra_config_info        = nullptr;
};

struct DecoderMemory {
	size_t   this_size = sizeof(DecoderMemory);
	size_t   sizes_and_memory[6] {};
	size_t   max_frame_buffer_size  = 0;
	uint32_t frame_buffer_alignment = 0;
	uint32_t reserved0              = 0;
};

struct HevcExtraConfig {
	size_t   this_size = sizeof(HevcExtraConfig);
	int32_t  tier_type = 0;
	int8_t   max_bit_depth_luma;
	int8_t   max_bit_depth_chroma;
	uint16_t reserved0 = 0;
	int32_t  max_temporal_id_to_decode = -1;
	uint32_t hdr_frame_format          = 0;
	uint32_t gpu_load_level            = 1;
};

struct Vp9ExtraConfig {
	size_t   this_size = sizeof(Vp9ExtraConfig);
	int8_t   max_bit_depth;
	uint32_t hdr_frame_format = 0;
};

size_t FrameBufferSize(uint32_t codec, uint32_t profile, int32_t height, const void* extra = nullptr) {
	Loader::SymbolDatabase symbols;
	Libs::VideoDec2::InitVideoDec2_1(&symbols);
	const auto* symbol = symbols.FindByNid("qqMCwlULR+E", Loader::SymbolType::Func);
	EXPECT_NE(symbol, nullptr);
	const auto query = reinterpret_cast<int32_t(KYTY_SYSV_ABI*)(const DecoderConfig*, DecoderMemory*)>(symbol->vaddr);
	const DecoderConfig config {.codec_type        = codec,
	                            .profile           = profile,
	                            .max_frame_width   = 1920,
	                            .max_frame_height  = height,
	                            .extra_config_info = extra};
	DecoderMemory       memory;
	EXPECT_EQ(query(&config, &memory), 0);
	return memory.max_frame_buffer_size;
}

TEST(VideoDec2, FrameBufferSizeFollowsCodecAndBitDepth) {
	constexpr uint32_t avc  = 1;
	constexpr uint32_t hevc = 974921;
	constexpr uint32_t vp9  = 2382845;
	const HevcExtraConfig hevc_8bit {.max_bit_depth_luma = 8, .max_bit_depth_chroma = 8};
	const Vp9ExtraConfig  vp9_8bit {.max_bit_depth = 8};

	EXPECT_EQ(FrameBufferSize(avc, 100, 1080), 3346432u);
	EXPECT_EQ(FrameBufferSize(hevc, 1, 1080), 3346432u);
	EXPECT_EQ(FrameBufferSize(hevc, 2, 1080), 6270976u);
	EXPECT_EQ(FrameBufferSize(hevc, 2, 1080, &hevc_8bit), 3346432u);
	EXPECT_EQ(FrameBufferSize(vp9, 0, 1090), 3547136u);
	EXPECT_EQ(FrameBufferSize(vp9, 2, 1088), 6275072u);
	EXPECT_EQ(FrameBufferSize(vp9, 2, 1088, &vp9_8bit), 3350528u);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
