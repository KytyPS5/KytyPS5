#include "libs/ajm.cpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <opus.h>

using namespace Libs::Audio::Ajm;

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "AjmResampleTests: %s\n", message);
		std::abort();
	}
}

struct DecodeResult {
	AjmSidebandResult result;
	AjmSidebandStream stream;
	AjmSidebandMFrame frames;
};

struct Fixture {
	uint32_t                  instance = 0;
	uint32_t                  channels;
	std::array<uint8_t, 4096> batch_data {};
	AjmBatchInfo              batch {};

	explicit Fixture(uint32_t channels = 1, AjmSampleEncoding encoding = AjmSampleEncoding::S16,
	                 uint64_t flags = 0x1000)
	    : channels(channels) {
		Check(AjmInstanceCreate(1, 24, flags | channels | (uint64_t(encoding) << 7), &instance) ==
		          OK,
		      "create Opus instance");
		Initialize();
	}
	~Fixture() { AjmInstanceDestroy(1, instance); }

	AjmBatchInfo* Batch() {
		batch = {batch_data.data(), 0, batch_data.size(), nullptr, nullptr};
		return &batch;
	}

	void Initialize() {
		AjmDecOpusInitializeParameters params {channels, 48000, 0};
		AjmSidebandResult              result {};
		AjmBatchJobInitialize(Batch(), instance, &params, sizeof(params), &result);
		Check(result.result == OK, "initialize Opus instance");
	}

	void Parameters(float ratio, uint32_t flags, float change = 0, bool extended = false) {
		AjmSidebandResult result {};
		if (extended) {
			AjmBatchJobSetResampleParametersEx(Batch(), instance, ratio, change, flags, &result);
		} else {
			AjmBatchJobSetResampleParameters(Batch(), instance, ratio, flags, &result);
		}
		Check(result.result == OK, "set resampling parameters");
	}

	AjmSidebandResampleInfo Info() {
		struct {
			AjmSidebandResult       result;
			AjmSidebandResampleInfo info;
		} result {};
		AjmBatchJobGetResampleInfo(Batch(), instance, &result);
		Check(result.result.result == OK, "get resampling info");
		return result.info;
	}

	void Gapless(uint32_t samples, uint16_t skip) {
		AjmSidebandGaplessDecode params {samples, skip, 0};
		AjmSidebandResult        result {};
		AjmBatchJobSetGaplessDecode(Batch(), instance, &params, 1, &result);
		Check(result.result == OK, "set gapless parameters");
	}

	DecodeResult Decode(const void* input, size_t size, void* output, size_t capacity) {
		DecodeResult result {};
		AjmBatchJobDecode(Batch(), instance, input, size, output, capacity, &result);
		Check((result.result.result & ~(AJM_RESULT_NOT_ENOUGH_ROOM | AJM_RESULT_PARTIAL_INPUT)) ==
		          0,
		      "decode returned unexpected error");
		return result;
	}
};

std::vector<uint8_t> Encode(uint32_t channels, int packets) {
	int   error   = 0;
	auto* encoder = opus_encoder_create(48000, channels, OPUS_APPLICATION_AUDIO, &error);
	Check(encoder != nullptr && error == OPUS_OK, "create test encoder");
	std::vector<uint8_t>      stream;
	std::vector<int16_t>      pcm(960 * channels);
	std::array<uint8_t, 4096> packet;
	for (int p = 0; p < packets; ++p) {
		for (size_t i = 0; i < pcm.size(); ++i) {
			pcm[i] = static_cast<int16_t>(
			    12000 * std::sin((i / channels + p * 960) * 0.041 + (i % channels)));
		}
		const int size = opus_encode(encoder, pcm.data(), 960, packet.data(), packet.size());
		Check(size > 0, "encode test packet");
		stream.push_back(static_cast<uint8_t>(size));
		stream.push_back(static_cast<uint8_t>(size >> 8));
		stream.insert(stream.end(), packet.begin(), packet.begin() + size);
	}
	opus_encoder_destroy(encoder);
	return stream;
}

void SmallOutputs(const std::vector<uint8_t>& packet, uint16_t skip = 0) {
	Fixture f;
	f.Gapless(960 - skip, skip);
	f.Parameters(1, 3);
	std::array<uint8_t, 1030> output;
	output.fill(0x5a);
	auto first = f.Decode(packet.data(), packet.size(), output.data(), 1024);
	Check(first.stream.size_consumed == static_cast<int>(packet.size()),
	      "small output must consume packet");
	Check(first.stream.size_produced == 1024, "small output must produce 1024 bytes");
	Check(first.frames.num_frames == 1 && first.stream.total_decoded_samples == 960 - skip,
	      "decoded frame counted once before resampling");
	Check(f.Info().num_samples == 448 - skip, "report retained source samples");
	Check(std::all_of(output.begin() + 1024, output.end(), [](auto v) { return v == 0x5a; }),
	      "do not write past output capacity");
	const auto last = f.Decode(nullptr, 0, output.data(), 1024);
	Check(last.result.result == OK && last.stream.size_consumed == 0 &&
	          last.stream.size_produced == 896 - skip * 2 && last.frames.num_frames == 0 &&
	          last.stream.total_decoded_samples == first.stream.total_decoded_samples,
	      "END drains retained PCM without decoding or reporting partial input");
	Check(f.Decode(nullptr, 0, output.data(), 1024).stream.size_produced == 0,
	      "END drains only once");
}

void UnityMatchesWholeDecode(uint32_t channels, AjmSampleEncoding encoding) {
	const auto stream       = Encode(channels, 6);
	const auto sample_bytes = channels * AjmBytesPerSample(encoding);
	Fixture    reference(channels, encoding, 0);
	reference.Gapless(5328, 312);
	std::vector<uint8_t> expected(5760 * sample_bytes);
	const auto           full =
	    reference.Decode(stream.data(), stream.size(), expected.data(), expected.size());
	expected.resize(full.stream.size_produced);
	Check(expected.size() == 5328 * sample_bytes, "reference gapless length");

	Fixture f(channels, encoding);
	f.Gapless(5328, 312);
	f.Parameters(1, 3);
	std::vector<uint8_t>      actual;
	std::array<uint8_t, 1025> output;
	size_t                    offset = 0;
	uint32_t                  frames = 0;
	for (int i = 0; i < 100; ++i) {
		const auto decoded = f.Decode(offset < stream.size() ? stream.data() + offset : nullptr,
		                              stream.size() - offset, output.data(), output.size());
		offset += decoded.stream.size_consumed;
		frames += decoded.frames.num_frames;
		actual.insert(actual.end(), output.begin(), output.begin() + decoded.stream.size_produced);
		if (decoded.stream.size_produced == 0) {
			break;
		}
	}
	Check(actual == expected && frames == 6 && offset == stream.size(),
	      "chunked PCM must exactly match whole decode, without loss or duplication");
}

void ChunkedRates(const std::vector<uint8_t>& stream, uint32_t samples, uint16_t skip) {
	for (float ratio: {0.125f, 0.749153f, 1.334840f}) {
		Fixture reference;
		reference.Gapless(samples, skip);
		reference.Parameters(ratio, 3);
		std::vector<uint8_t> expected(
		    (static_cast<size_t>(std::ceil(samples / double(ratio))) + 1) * 2);
		const auto full =
		    reference.Decode(stream.data(), stream.size(), expected.data(), expected.size());
		expected.resize(full.stream.size_produced);
		Check(expected.size() == static_cast<size_t>(std::ceil(samples / double(ratio))) * 2,
		      "fractional resampling rounds up final output sample count");

		Fixture f;
		f.Gapless(samples, skip);
		f.Parameters(ratio, 1);
		std::vector<uint8_t>      actual;
		std::array<uint8_t, 1024> output;
		size_t                    offset = 0;
		for (int i = 0; i < 1000; ++i) {
			if (offset == stream.size()) {
				// The caller can signal END only after submitting the last packet.
				f.Parameters(0, 2 | 8);
			}
			const auto decoded = f.Decode(offset < stream.size() ? stream.data() + offset : nullptr,
			                              stream.size() - offset, output.data(), output.size());
			offset += decoded.stream.size_consumed;
			actual.insert(actual.end(), output.begin(),
			              output.begin() + decoded.stream.size_produced);
			if (decoded.stream.size_produced == 0) {
				break;
			}
		}
		Check(actual == expected && offset == stream.size(),
		      "non-unity PCM is independent of output chunk boundaries and deferred END");
	}
}

void LoopBoundaries() {
	const auto loop   = Encode(1, 2);
	auto       stream = loop;
	stream.insert(stream.end(), loop.begin(), loop.end());
	for (uint64_t flags: {0x1400u, 0x3400u}) {
		Fixture f(1, AjmSampleEncoding::S16, flags);
		f.Gapless(1500, 100);
		f.Parameters(1, 1);
		std::array<uint8_t, 8192> pcm;
		const auto                first = f.Decode(stream.data(), stream.size(), pcm.data(), 1024);
		Check(first.stream.size_produced == 1024, "loop first partial output");
		const auto second =
		    f.Decode(stream.data() + first.stream.size_consumed,
			         stream.size() - first.stream.size_consumed, pcm.data(), pcm.size());
		const bool seamless = flags == 0x3400;
		Check(second.stream.size_produced == (seamless ? 6000 - 1024 : 3000 - 1024),
		      "non-seamless loop stops job at stream boundary");
		const auto consumed = first.stream.size_consumed + second.stream.size_consumed;
		Check(consumed == static_cast<int>(seamless ? stream.size() : loop.size()),
		      "loop boundary input consumption");
		if (!seamless) {
			Check(
			    f.Decode(stream.data() + consumed, stream.size() - consumed, pcm.data(), pcm.size())
			            .stream.size_produced == 3000,
			    "next job resumes gapless loop");
		}
	}
}

void RatesAndControls(const std::vector<uint8_t>& packet) {
	for (float ratio: {0.125f, 0.5f, 1.0f, 1.25f, 2.0f, 4.0f}) {
		Fixture f;
		f.Parameters(ratio, 3);
		std::vector<int16_t> pcm(8000);
		const auto result = f.Decode(packet.data(), packet.size(), pcm.data(), pcm.size() * 2);
		Check(result.stream.size_produced == static_cast<int>(std::ceil(960.0 / ratio)) * 2,
		      "resampling ratio controls output duration");
		Check(result.stream.total_decoded_samples == 960,
		      "ratio does not change decoded sample count");
	}
	Fixture f;
	f.Parameters(1, 1);
	std::array<int16_t, 512> pcm;
	f.Decode(packet.data(), packet.size(), pcm.data(), sizeof(pcm));
	f.Initialize();
	Check(f.Info().num_samples == 448, "codec initialize must preserve resampler PCM");
	f.Parameters(0, 0);
	const auto frozen = f.Decode(nullptr, 0, pcm.data(), sizeof(pcm));
	Check(
	    frozen.stream.size_produced == sizeof(pcm) && f.Info().num_samples == 448 &&
	        std::all_of(pcm.begin(), pcm.end(), [&](auto sample) { return sample == pcm.front(); }),
	    "zero ratio repeats one sample without advancing input");
	f.Parameters(1, 0, 0.25f, true);
	f.Parameters(std::numeric_limits<float>::quiet_NaN(), 8);
	f.Decode(nullptr, 0, pcm.data(), 4 * sizeof(int16_t));
	Check(f.Info().ratio == 2.0f, "ignore preserves ratio ramp");
	f.Parameters(1, 1);
	Check(f.Info().num_samples == 0, "resample RESET discards retained PCM");
	Check(f.Decode(nullptr, 0, pcm.data(), sizeof(pcm)).stream.size_produced == 0,
	      "RESET leaves no stale audio");
}

void UnbufferedAndSplit(const std::vector<uint8_t>& packet) {
	Fixture                   raw(1, AjmSampleEncoding::S16, 0);
	std::array<uint8_t, 1024> pcm;
	const auto too_small = raw.Decode(packet.data(), packet.size(), pcm.data(), pcm.size());
	Check(too_small.result.result == AJM_RESULT_NOT_ENOUGH_ROOM &&
	          too_small.stream.size_consumed == 0 && too_small.stream.size_produced == 0,
	      "ordinary decoding still requires room for a whole packet");
	Fixture f;
	f.Parameters(1, 3);
	AjmBuffer    input[] {{const_cast<uint8_t*>(packet.data()), 1},
	                      {const_cast<uint8_t*>(packet.data()) + 1, packet.size() - 1}};
	AjmBuffer    output[] {{pcm.data(), 257}, {pcm.data() + 257, 767}};
	DecodeResult result {};
	AjmBatchJobDecodeSplit(f.Batch(), f.instance, input, 2, output, 2, &result);
	Check(result.stream.size_produced == 1024 &&
	          result.stream.size_consumed == static_cast<int>(packet.size()),
	      "split buffers retain PCM across jobs");
}

void FormatChangeRequiresReset() {
	Fixture f(2);
	f.Parameters(1, 1);
	const auto                stereo = Encode(2, 1);
	std::array<uint8_t, 4096> pcm;
	f.Decode(stereo.data(), stereo.size(), pcm.data(), 1024);
	f.channels = 1;
	f.Initialize();
	DecodeResult result {};
	AjmBatchJobDecode(f.Batch(), f.instance, nullptr, 0, pcm.data(), pcm.size(), &result);
	Check(result.result.result == AJM_RESULT_INVALID_PARAMETER && result.stream.size_produced == 0,
	      "format change cannot reinterpret retained PCM with a different channel stride");
	f.Parameters(1, 3);
	const auto mono = Encode(1, 1);
	Check(f.Decode(mono.data(), mono.size(), pcm.data(), pcm.size()).stream.size_produced == 1920,
	      "resampler RESET permits a new stream format");
}

void EndPersistsUntilReset(const std::vector<uint8_t>& packet) {
	Fixture f;
	f.Parameters(0.5f, 1);
	std::array<int16_t, 64> pcm;
	auto   decoded  = f.Decode(packet.data(), packet.size(), pcm.data(), sizeof(pcm));
	size_t produced = decoded.stream.size_produced;
	f.Parameters(0.5f, 2);
	for (int i = 0; i < 40; ++i) {
		f.Parameters(0.5f, 0);
		decoded = f.Decode(nullptr, 0, pcm.data(), sizeof(pcm));
		produced += decoded.stream.size_produced;
		Check((decoded.result.result & AJM_RESULT_PARTIAL_INPUT) == 0,
		      "END remains active across subsequent ratio controls");
		if (f.Info().num_samples < 0) {
			break;
		}
	}
	Check(produced == 3840 && f.Info().num_samples < 0,
	      "fully drained END reports a negative retained sample count");
	f.Parameters(1, 1);
	Check(f.Info().num_samples == 0, "RESET clears end-of-stream state");
}

} // namespace

int main(int argc, char** argv) {
	const auto packet = Encode(1, 1);
	SmallOutputs(packet);
	SmallOutputs(packet, 312);
	for (uint32_t channels: {1u, 2u}) {
		for (auto encoding:
		     {AjmSampleEncoding::S16, AjmSampleEncoding::S32, AjmSampleEncoding::Float}) {
			UnityMatchesWholeDecode(channels, encoding);
		}
	}
	RatesAndControls(packet);
	UnbufferedAndSplit(packet);
	ChunkedRates(Encode(1, 6), 5328, 312);
	LoopBoundaries();
	FormatChangeRequiresReset();
	EndPersistsUntilReset(packet);
	if (argc >= 2) {
		std::ifstream        file(argv[1], std::ios::binary);
		std::vector<uint8_t> real_packet((std::istreambuf_iterator<char>(file)), {});
		Check(real_packet.size() == 209, "expected real 209-byte packet fixture");
		SmallOutputs(real_packet, 312);
	}
	if (argc >= 3) {
		std::ifstream        file(argv[2], std::ios::binary);
		std::vector<uint8_t> real_stream((std::istreambuf_iterator<char>(file)), {});
		Check(real_stream.size() == 4048, "expected complete real stream fixture");
		ChunkedRates(real_stream, 25920, 312);
	}
	std::puts("AjmResampleTests: passed");
}
