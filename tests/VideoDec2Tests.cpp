#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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
	size_t   this_size              = sizeof(DecoderMemory);
	size_t   cpu_memory_size        = 0;
	void*    cpu_memory             = nullptr;
	size_t   gpu_memory_size        = 0;
	void*    gpu_memory             = nullptr;
	size_t   cpu_gpu_memory_size    = 0;
	void*    cpu_gpu_memory         = nullptr;
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

struct InputData {
	size_t      this_size = sizeof(InputData);
	const void* au_data;
	size_t      au_size;
	uint64_t    pts_data      = 0;
	uint64_t    dts_data      = 0;
	uint64_t    attached_data = 0;
};

struct FrameBuffer {
	size_t this_size = sizeof(FrameBuffer);
	void*  frame_buffer;
	size_t frame_buffer_size;
	bool   is_accepted = false;
};

struct OutputInfo {
	size_t   this_size            = sizeof(OutputInfo);
	bool     is_valid             = false;
	bool     is_error_frame       = false;
	uint8_t  picture_count        = 0;
	bool     is_discarded_frame   = false;
	uint32_t codec_type           = 0;
	uint32_t frame_width          = 0;
	uint32_t frame_pitch          = 0;
	uint32_t frame_height         = 0;
	void*    frame_buffer         = nullptr;
	size_t   frame_buffer_size    = 0;
	uint32_t frame_format         = 0;
	uint32_t frame_pitch_in_bytes = 0;
};

struct ComputeMemory {
	size_t this_size           = sizeof(ComputeMemory);
	size_t cpu_gpu_memory_size = 0;
	void*  cpu_gpu_memory      = nullptr;
};

struct ComputeConfig {
	size_t   this_size             = sizeof(ComputeConfig);
	uint16_t compute_pipe_id       = 0;
	uint16_t compute_queue_id      = 0;
	uint8_t  flags_and_reserved[4] = {};
};

constexpr uint32_t AVC  = 1;
constexpr uint32_t HEVC = 974921;
constexpr uint32_t VP9  = 2382845;

template <typename Function>
Function Export(const char* nid) {
	Loader::SymbolDatabase symbols;
	Libs::VideoDec2::InitVideoDec2_1(&symbols);
	const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
	EXPECT_NE(symbol, nullptr);
	return reinterpret_cast<Function>(symbol->vaddr);
}

using Query = int32_t(KYTY_SYSV_ABI*)(const DecoderConfig*, DecoderMemory*);

size_t FrameBufferSize(uint32_t codec, uint32_t profile, int32_t height, const void* extra = nullptr) {
	const DecoderConfig config {.codec_type        = codec,
	                            .profile           = profile,
	                            .max_frame_width   = 1920,
	                            .max_frame_height  = height,
	                            .extra_config_info = extra};
	DecoderMemory       memory;
	EXPECT_EQ(Export<Query>("qqMCwlULR+E")(&config, &memory), 0);
	return memory.max_frame_buffer_size;
}

TEST(VideoDec2, FrameBufferSizeFollowsCodecAndBitDepth) {
	const HevcExtraConfig hevc_8bit {.max_bit_depth_luma = 8, .max_bit_depth_chroma = 8};
	const Vp9ExtraConfig  vp9_8bit {.max_bit_depth = 8};

	EXPECT_EQ(FrameBufferSize(AVC, 100, 1080), 3346432u);
	EXPECT_EQ(FrameBufferSize(HEVC, 1, 1080), 3346432u);
	EXPECT_EQ(FrameBufferSize(HEVC, 2, 1080), 6270976u);
	EXPECT_EQ(FrameBufferSize(HEVC, 2, 1080, &hevc_8bit), 3346432u);
	EXPECT_EQ(FrameBufferSize(VP9, 0, 1090), 3547136u);
	EXPECT_EQ(FrameBufferSize(VP9, 2, 1088), 6275072u);
	EXPECT_EQ(FrameBufferSize(VP9, 2, 1088, &vp9_8bit), 3350528u);
}

TEST(VideoDec2, ComputeQueueTakesExactlyTheQueriedMemory) {
	using QueryCompute  = int32_t(KYTY_SYSV_ABI*)(ComputeMemory*);
	using AllocateQueue = int32_t(KYTY_SYSV_ABI*)(const ComputeConfig*, const ComputeMemory*, void**);

	ComputeMemory memory;
	ASSERT_EQ(Export<QueryCompute>("RnDibcGCPKw")(&memory), 0);
	EXPECT_EQ(memory.cpu_gpu_memory_size, 0x495300u);

	std::vector<uint8_t> backing(memory.cpu_gpu_memory_size);
	memory.cpu_gpu_memory = backing.data();
	const ComputeConfig config;
	void*               queue    = nullptr;
	const auto          allocate = Export<AllocateQueue>("eD+X2SmxUt4");
	EXPECT_EQ(allocate(&config, &memory, &queue), 0);
	memory.cpu_gpu_memory_size--;
	EXPECT_EQ(allocate(&config, &memory, &queue), static_cast<int32_t>(0x811d0104));
}

// A 16x16 lossless HEVC Main10 frame with luma 0x123, Cb 0x234 and Cr 0x345.
constexpr std::array<uint8_t, 197> TEN_BIT_FRAME {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00,
    0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0xff, 0x92, 0x80, 0x90, 0x00, 0x00, 0x00, 0x01,
    0x42, 0x01, 0x01, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0xff, 0xa0, 0x88, 0x44, 0xd9, 0x64, 0xaa, 0xbc, 0xaf, 0x01, 0x68, 0x08, 0x00, 0x00, 0x03,
    0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0xc8, 0x40, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x71,
    0xa9, 0x12, 0x00, 0x00, 0x01, 0x28, 0x01, 0xaf, 0x05, 0xb8, 0x24, 0x98, 0x00, 0x67, 0x7f, 0xff,
    0xf7, 0xf7, 0xcb, 0xf5, 0xbb, 0xdd, 0x17, 0x57, 0x9a, 0x4a, 0x69, 0x29, 0xa4, 0xa6, 0x92, 0x9a,
    0x7b, 0xa9, 0xee, 0xa7, 0xba, 0x9e, 0xea, 0x7b, 0xa9, 0xee, 0xa7, 0xba, 0x9f, 0x0b, 0xc5, 0x7f,
    0xff, 0xc8, 0x9a, 0xb8, 0x02, 0xf5, 0xc5, 0xed, 0xcf, 0xc5, 0x22, 0xa4, 0x64, 0x64, 0x64, 0x65,
    0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x22, 0x9f, 0x3f, 0xfd, 0x8f, 0x74, 0x00, 0xff, 0xe4, 0x0f,
    0xfc, 0x7f, 0xff, 0x1d, 0xff, 0x8c, 0xff, 0x88, 0xff, 0x88, 0xff, 0x88, 0xff, 0x88, 0xff, 0x89,
    0x3f, 0x89, 0x3f, 0x89, 0x3f, 0x89, 0x3f, 0x89, 0x3f, 0x89, 0x3f, 0x89, 0x3f, 0x89, 0x1c, 0xed,
    0x9b, 0x51, 0x55, 0x53, 0xf8};

// A 16x16 decoder created through the exported functions, decoding into one frame buffer.
class TestDecoder {
public:
	TestDecoder(uint32_t codec, uint32_t profile) {
		DecoderConfig config {.codec_type = codec, .profile = profile, .max_frame_width = 16, .max_frame_height = 16};
		EXPECT_EQ(Export<Query>("qqMCwlULR+E")(&config, &m_memory), 0);
		m_cpu.resize(m_memory.cpu_memory_size);
		m_gpu.resize(m_memory.gpu_memory_size);
		m_shared.resize(m_memory.cpu_gpu_memory_size);
		m_memory.cpu_memory     = m_cpu.data();
		m_memory.gpu_memory     = m_gpu.data();
		m_memory.cpu_gpu_memory = m_shared.data();
		config.compute_queue    = m_shared.data();
		using Create = int32_t(KYTY_SYSV_ABI*)(const DecoderConfig*, const DecoderMemory*, void**);
		EXPECT_EQ(Export<Create>("CNNRoRYd8XI")(&config, &m_memory, &m_handle), 0);
		frame.resize(m_memory.max_frame_buffer_size / 2);
		m_buffer = {.frame_buffer = frame.data(), .frame_buffer_size = m_memory.max_frame_buffer_size};
	}

	~TestDecoder() { EXPECT_EQ(Export<int32_t(KYTY_SYSV_ABI*)(void*)>("jwImxXRGSKA")(m_handle), 0); }

	OutputInfo Decode(std::span<const uint8_t> au) {
		using Function = int32_t(KYTY_SYSV_ABI*)(void*, const InputData*, FrameBuffer*, OutputInfo*);
		const InputData input {.au_data = au.data(), .au_size = au.size()};
		OutputInfo      output;
		EXPECT_EQ(Export<Function>("852F5+q6+iM")(m_handle, &input, &m_buffer, &output), 0);
		return output;
	}

	OutputInfo Flush() {
		using Function = int32_t(KYTY_SYSV_ABI*)(void*, FrameBuffer*, OutputInfo*);
		OutputInfo output;
		EXPECT_EQ(Export<Function>("l1hXwscLuCY")(m_handle, &m_buffer, &output), 0);
		return output;
	}

	std::vector<uint16_t> frame;

private:
	DecoderMemory        m_memory;
	std::vector<uint8_t> m_cpu;
	std::vector<uint8_t> m_gpu;
	std::vector<uint8_t> m_shared;
	FrameBuffer          m_buffer {};
	void*                m_handle = nullptr;
};

TEST(VideoDec2, TenBitFramesKeepTheirSamplesInTheLowBits) {
	TestDecoder decoder(HEVC, 2);
	auto        output = decoder.Decode(TEN_BIT_FRAME);
	if (!output.is_valid) {
		output = decoder.Flush();
	}
	ASSERT_TRUE(output.is_valid);
	EXPECT_EQ((std::array {output.frame_pitch, output.frame_pitch_in_bytes}), (std::array {128u, 256u}));
	const auto& frame = decoder.frame;
	EXPECT_EQ((std::array {frame[0], frame[128 * 16], frame[128 * 16 + 1]}),
	          (std::array<uint16_t, 3> {0x123, 0x234, 0x345}));

	// The HEVC picture information holds bit_depth_luma_minus8 and bit_depth_chroma_minus8 at 48.
	alignas(8) std::array<uint8_t, 184> info {};
	info[0] = info.size();
	using Picture = int32_t(KYTY_SYSV_ABI*)(const OutputInfo*, void*, void*);
	ASSERT_EQ(Export<Picture>("NtXRa3dRzU0")(&output, info.data(), nullptr), 0);
	EXPECT_EQ((std::array {info[48], info[49]}), (std::array<uint8_t, 2> {2, 2}));
}

// A 16x16 AVC Main stream of five gray frames, decoded as I P B B P with one frame of reorder delay.
constexpr std::array<uint8_t, 81> B_FRAME_STREAM {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x0a, 0xe9, 0xbd, 0x80, 0x88, 0x00, 0x00, 0x03, 0x00,
    0x08, 0x00, 0x00, 0x03, 0x01, 0x90, 0x78, 0x91, 0x29, 0xc0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce,
    0x03, 0x9c, 0x80, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x01, 0xd1, 0x37, 0x80, 0x00, 0x00, 0x00,
    0x01, 0x41, 0x9a, 0x26, 0x14, 0xa0, 0x00, 0x00, 0x00, 0x01, 0x01, 0x9e, 0x42, 0x82, 0x25, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x9e, 0x44, 0x82, 0x25, 0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x48, 0x14,
    0xa0};
constexpr std::array<size_t, 5> B_FRAME_AU_SIZES {45, 9, 9, 9, 9};

TEST(VideoDec2, FlushOutputsHeldFramesAndDecodingResumesAfterTheLast) {
	TestDecoder decoder(AVC, 77);
	for (int pass = 0; pass < 2; pass++) {
		int    decoded = 0;
		size_t offset  = 0;
		for (const auto size: B_FRAME_AU_SIZES) {
			decoded += decoder.Decode(std::span(B_FRAME_STREAM).subspan(offset, size)).is_valid ? 1 : 0;
			offset += size;
		}
		// No flush past the last frame: the next pass decodes right after it.
		ASSERT_LT(decoded, 5);
		for (; decoded < 5; decoded++) {
			ASSERT_TRUE(decoder.Flush().is_valid);
		}
	}
	EXPECT_FALSE(decoder.Flush().is_valid);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
