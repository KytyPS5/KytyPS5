// AjmAt9Decoder is intentionally internal to ajm.cpp. This focused target
// amalgamates that implementation so it can exercise the real decoder path.
#include "libs/ajm.cpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace Libs::Audio::Ajm;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AjmAtrac9StreamingTests: failed: %s\n", message);
		std::abort();
	}
}

template <size_t Size>
void WriteFourCc(std::array<uint8_t, Size>& buffer, size_t offset, const char* value) {
	std::memcpy(buffer.data() + offset, value, 4);
}

template <size_t Size>
void WriteLe16(std::array<uint8_t, Size>& buffer, size_t offset, uint16_t value) {
	buffer[offset]     = static_cast<uint8_t>(value);
	buffer[offset + 1] = static_cast<uint8_t>(value >> 8u);
}

template <size_t Size>
void WriteLe32(std::array<uint8_t, Size>& buffer, size_t offset, uint32_t value) {
	for (uint32_t index = 0; index < 4; index++) {
		buffer[offset + index] = static_cast<uint8_t>(value >> (index * 8u));
	}
}

std::array<uint8_t, 76> MakeStreamingHeader() {
	std::array<uint8_t, 76> header {};
	WriteFourCc(header, 0, "RIFF");
	WriteLe32(header, 4, 0x00010044u);
	WriteFourCc(header, 8, "WAVE");

	WriteFourCc(header, 12, "fmt ");
	WriteLe32(header, 16, 48);
	WriteLe16(header, 20, 0xfffe);
	WriteLe16(header, 22, 2);
	WriteLe32(header, 24, 48000);
	constexpr std::array<uint8_t, 16> atrac9_guid = {
	    0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	    0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c,
	};
	std::memcpy(header.data() + 44, atrac9_guid.data(), atrac9_guid.size());
	constexpr std::array<uint8_t, 4> atrac9_config = {0xfe, 0x74, 0x0b, 0xe0};
	std::memcpy(header.data() + 64, atrac9_config.data(), atrac9_config.size());

	WriteFourCc(header, 68, "data");
	WriteLe32(header, 72, 0x00010000u);
	return header;
}

void TestStreamingDataChunkDoesNotRequireDeclaredPayload() {
	const auto    header = MakeStreamingHeader();
	AjmAt9Decoder decoder(2, 48000, AjmSampleEncoding::S16,
	                      AJM_INSTANCE_FLAG_DEC_AT9_PARSE_RIFF_HEADER);
	const auto    result = decoder.Decode(header.data(), header.size(), nullptr, 0, false, nullptr);

	Check(result.result == AJM_RESULT_PARTIAL_INPUT,
	      "header-only input did not request the first audio packet");
	Check(result.input_consumed == header.size(), "valid streaming RIFF header was not consumed");
	Check(result.format.channel_num == 2 && result.format.sampling_frequency == 48000,
	      "streaming header did not initialize the ATRAC9 format");
}

void TestTruncatedMetadataStillRequiresItsPayload() {
	auto header = MakeStreamingHeader();
	WriteLe32(header, 16, 0x00010000u);

	AjmAt9Decoder decoder(2, 48000, AjmSampleEncoding::S16,
	                      AJM_INSTANCE_FLAG_DEC_AT9_PARSE_RIFF_HEADER);
	const auto    result = decoder.Decode(header.data(), header.size(), nullptr, 0, false, nullptr);
	Check(result.result == AJM_RESULT_PARTIAL_INPUT && result.input_consumed == 0,
	      "truncated metadata chunk was accepted as streaming audio");
}

} // namespace

int main() {
	TestStreamingDataChunkDoesNotRequireDeclaredPayload();
	TestTruncatedMetadataStillRequiresItsPayload();
	return 0;
}
