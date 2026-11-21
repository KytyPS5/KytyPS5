// Exercise sampler controls and rendering without a host audio device.
#include "libs/ngs2.cpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <system_error>

using namespace Libs::Audio::Ngs2;

// Guest-path mount used by the waveform file tests: /app0/ maps to a
// per-test directory under the host temp path.
static std::filesystem::path g_ngs2GuestRoot;

namespace Libs::LibKernel::FileSystem {
std::filesystem::path GetRealFilename(const std::string& mounted) {
	if (mounted.starts_with("/app0/")) {
		return g_ngs2GuestRoot / mounted.substr(6);
	}
	return {};
}
} // namespace Libs::LibKernel::FileSystem

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "Ngs2SamplerTests: %s\n", message);
		std::fflush(stderr);
		std::abort();
	}
}

struct Fixture {
	Ngs2Internal      system;
	Ngs2RackInternal  rack {};
	Ngs2VoiceInternal voice;

	Fixture(uint32_t rate, uint32_t channels = 1, Ngs2RackType type = Ngs2RackType::Sampler) {
		system.option.sample_rate = 48000;
		system.option.num_grain_samples = 256;
		rack.ngs                  = &system;
		rack.type                 = type;
		voice.rack                = &rack;
		SetCapacity(4);
		struct Setup {
			Ngs2VoiceParamHeader header {40, 0, 0x10000000};
			Ngs2WaveformFormat   format;
			uint32_t             flags = 0, reserved = 0;
		} setup;
		setup.header.id = type == Ngs2RackType::Sampler ? 0x10000000 : 0x40010000;
		setup.format    = {0x12, channels, rate};
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
		      "sampler setup failed");
		voice.state = Ngs2VoicePlayState::Playing;
	}

	void SetCapacity(uint32_t capacity) {
		if (rack.type == Ngs2RackType::Sampler) {
			rack.option.sampler.max_waveform_blocks = capacity;
		} else {
			rack.option.custom_sampler.max_waveform_blocks = capacity;
			rack.option.custom_sampler.custom_rack_option.state_size = sizeof(Ngs2CustomSamplerVoiceState);
			rack.option.custom_sampler.custom_rack_option.num_buffers = 1;
		}
	}

	Ngs2SamplerVoiceState State() {
		Ngs2SamplerVoiceState state {};
		if (rack.type == Ngs2RackType::CustomSampler) {
			Ngs2CustomSamplerVoiceState custom {};
			Check(Ngs2VoiceGetState(reinterpret_cast<uintptr_t>(&voice), &custom.voice_state,
			                       sizeof(custom)) == OK, "custom sampler state failed");
			state.voice_state = custom.voice_state;
			state.num_decoded_samples = custom.num_decoded_samples;
			state.decoded_data_size = custom.decoded_data_size;
			state.waveform_data = custom.waveform_data;
			state.user_data = custom.user_data;
		} else {
			Check(Ngs2VoiceGetState(reinterpret_cast<uintptr_t>(&voice), &state.voice_state,
			                       sizeof(state)) == OK, "sampler state failed");
		}
		return state;
	}

	int QueueBlocks(const void* data, const Ngs2WaveformBlock* blocks, uint32_t count,
	                uint32_t flags) {
		struct Blocks {
			Ngs2VoiceParamHeader     header {32, 0, 0x10000001};
			const void*              data;
			uint32_t                 flags, count;
			const Ngs2WaveformBlock* blocks;
		} param {{32, 0, 0x10000001}, data, flags, count, blocks};
		param.header.id = rack.type == Ngs2RackType::Sampler ? 0x10000001 : 0x40010001;
		return Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &param.header);
	}

	void Queue(const int16_t* data, uint32_t frames, uint32_t flags, uint32_t repeats = 0,
	           uint32_t skip = 0) {
		const Ngs2WaveformBlock block {
		    0, size_t(frames + skip) * voice.channels * sizeof(int16_t), repeats, skip, frames, 0,
		    123};
		Check(QueueBlocks(data, &block, 1, flags) == OK, "sampler queue failed");
	}

	void Render(uint32_t frames) {
		voice.rendered = false;
		Ngs2RenderVoice(voice, {}, frames);
	}
};

void TestStereoPan() {
	Ngs2PanWork work;
	Check(Ngs2PanInit(&work, nullptr, 360.0f, 2) == OK, "pan init failed");
	Check(work.speaker_angles[0] == 270.0f && work.speaker_angles[1] == 90.0f,
	      "pan init did not install the default speaker positions");
	auto pan = [&](float angle, float distance = 1.0f) {
		const Ngs2PanParam   param {angle, distance, 1.0f, 1.0f};
		std::array<float, 2> matrix {};
		Check(Ngs2PanGetVolumeMatrix(&work, &param, 1, 2, matrix.data()) == OK,
		      "pan matrix failed");
		return matrix;
	};
	const auto left  = pan(270.0f);
	const auto right = pan(90.0f);
	Check(std::abs(left[0] - 1.0f) < 1e-6f && std::abs(left[1]) < 1e-6f,
	      "270 degrees should be hard left");
	Check(std::abs(right[1] - 1.0f) < 1e-6f && std::abs(right[0]) < 1e-6f,
	      "90 degrees should be hard right");
	const auto centre = pan(0.0f);
	Check(std::abs(centre[0] - centre[1]) < 1e-6f &&
	          std::abs(centre[0] - std::numbers::sqrt2_v<float> / 2.0f) < 1e-6f,
	      "0 degrees should be centred");
	const auto opposite = pan(90.0f, -1.0f);
	Check(std::abs(opposite[0] - left[0]) < 1e-6f && std::abs(opposite[1] - left[1]) < 1e-6f,
	      "negative pan distance did not reverse direction");

	// L is across zero from R; positions need not use the default 180-degree separation.
	const float speakers[] = {320.0f, 40.0f};
	Check(Ngs2PanInit(&work, speakers, 360.0f, 2) == OK, "custom pan init failed");
	const auto custom_left  = pan(320.0f);
	const auto custom_right = pan(40.0f);
	Check(std::abs(custom_left[0] - 1.0f) < 1e-6f && std::abs(custom_left[1]) < 1e-6f &&
	          std::abs(custom_right[1] - 1.0f) < 1e-6f && std::abs(custom_right[0]) < 1e-6f,
	      "custom speaker positions were ignored");
	const auto front = pan(0.0f);
	const auto rear  = pan(180.0f);
	Check(std::abs(front[0] - front[1]) < 1e-6f && std::abs(rear[0] - rear[1]) < 1e-6f,
	      "custom speaker arcs did not meet at their midpoints");
	for (float angle: {-40.0f, 320.0f, 680.0f}) {
		const auto wrapped = pan(angle);
		Check(std::abs(wrapped[0] - custom_left[0]) < 1e-6f &&
		          std::abs(wrapped[1] - custom_left[1]) < 1e-6f,
		      "source pan angle did not wrap across zero");
	}
	const float wrapped_speakers[] = {-40.0f, 400.0f};
	Check(Ngs2PanInit(&work, wrapped_speakers, 360.0f, 2) == OK, "wrapped speaker pan init failed");
	const auto wrapped_front = pan(0.0f);
	Check(std::abs(wrapped_front[0] - front[0]) < 1e-6f &&
	          std::abs(wrapped_front[1] - front[1]) < 1e-6f,
	      "speaker angles did not wrap across zero");

	// All gains for one destination must be adjacent, even for a non-square input matrix.
	const Ngs2PanParam params[] = {
	    {320.0f, 1.0f, 0.25f, 0.0f}, {40.0f, 1.0f, 0.75f, 0.0f}, {320.0f, 1.0f, 0.5f, 0.0f}};
	float matrix[6] {};
	Check(Ngs2PanGetVolumeMatrix(&work, params, 3, 2, matrix) == OK,
	      "multi-input pan matrix failed");
	const float expected[] = {0.25f, 0.0f, 0.5f, 0.0f, 0.75f, 0.0f};
	for (size_t i = 0; i < std::size(matrix); i++) {
		Check(std::abs(matrix[i] - expected[i]) < 1e-6f,
		      "pan matrix channel order or FBW level is incorrect");
	}

	const auto      degrees           = pan(17.0f, 0.4f);
	constexpr float radians           = 2.0f * std::numbers::pi_v<float>;
	const float     radian_speakers[] = {-40.0f / 360.0f * radians, 400.0f / 360.0f * radians};
	Check(Ngs2PanInit(&work, radian_speakers, radians, 2) == OK, "radian pan init failed");
	const auto in_radians = pan(17.0f / 360.0f * radians, 0.4f);
	Check(std::abs(degrees[0] - in_radians[0]) < 1e-6f &&
	          std::abs(degrees[1] - in_radians[1]) < 1e-6f,
	      "degree and radian panning disagree");
	Check(Ngs2PanInit(&work, nullptr, radians, 2) == OK &&
	          std::abs(work.speaker_angles[0] - 0.75f * radians) < 1e-6f &&
	          std::abs(work.speaker_angles[1] - 0.25f * radians) < 1e-6f,
	      "default speaker angles did not use the requested units");

	const float narrow_and_large_layouts[][2] = {
	    {100.0f, std::nextafter(100.0f, 101.0f)}, {0.0f, -1e-20f}, {1e20f, 40.0f}};
	for (const auto& speakers: narrow_and_large_layouts) {
		Check(Ngs2PanInit(&work, speakers, 360.0f, 2) == OK,
		      "distinct finite speaker positions were rejected");
		const auto endpoint_left  = pan(speakers[0]);
		const auto endpoint_right = pan(speakers[1]);
		Check(std::abs(endpoint_left[0] - 1.0f) < 1e-6f && std::abs(endpoint_left[1]) < 1e-6f &&
		          std::abs(endpoint_right[1] - 1.0f) < 1e-6f && std::abs(endpoint_right[0]) < 1e-6f,
		      "speaker angle normalization lost distinct endpoints");
	}

	const float duplicate_speakers[] = {0.0f, 360.0f};
	Check(Ngs2PanInit(&work, duplicate_speakers, 360.0f, 2) == static_cast<int32_t>(0x804a8451u),
	      "coincident speaker positions were accepted");
}

void TestWaveformQueueCapacity() {
	constexpr int32_t full = static_cast<int32_t>(0x804a8054u);
	constexpr int32_t invalid_count = static_cast<int32_t>(0x804a8053u);
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		for (uint32_t capacity: {1u, 4u, 16u}) {
			Fixture f(24000, 1, rack);
			f.SetCapacity(capacity);
			f.system.option.num_grain_samples = 1;
			const int16_t pcm[] = {8192, 16384};
			const int16_t replacement[] = {24576, 4096};
			std::vector<Ngs2WaveformBlock> blocks(capacity + 1);
			for (uint32_t i = 0; i < blocks.size(); ++i) {
				blocks[i] = {0, sizeof(pcm), 0, 0, 2, 0, i + 1};
			}
			Check(f.QueueBlocks(pcm, blocks.data(), capacity, 1) == OK,
			      "configured waveform capacity was not available");
			f.Render(1);
			const auto phase = f.voice.sample_phase;
			Check(f.QueueBlocks(replacement, blocks.data(), 1, 0) == full,
			      "full waveform queue accepted another guest buffer");
			Check(f.QueueBlocks(replacement, blocks.data(), capacity + 1, 4) == invalid_count,
			      "oversized waveform request did not reject before CLEAR");
			Check(f.voice.blocks.size() == capacity && f.voice.accepts_blocks &&
			          f.voice.sample_phase == phase && f.voice.decoded_samples == 1 &&
			          f.voice.decoded_bytes == sizeof(int16_t),
			      "rejected waveform request changed playback or stream continuity");
			for (uint32_t i = 0; i < capacity; ++i) {
				Check(f.voice.blocks[i].data == reinterpret_cast<const uint8_t*>(pcm) &&
				          f.voice.blocks[i].info.user_data == i + 1 &&
				          f.voice.blocks[i].data_cursor == (i == 0 ? sizeof(int16_t) : 0u),
				      "rejected waveform request replaced an outstanding block");
			}
			f.Render(3);
			Check(f.voice.samples == std::vector<float> {0.375f, 0.5f,
			                                            capacity == 1 ? 0.5f : 0.375f} &&
			          f.voice.blocks.size() == capacity - 1,
			      "queue rejection changed partially played source audio");
			Check(f.QueueBlocks(replacement, blocks.data(), 1, 1) == OK &&
			          f.voice.blocks.size() == capacity,
			      "completed waveform block did not release its queue slot");
			const auto progress = f.State();
			Check(f.QueueBlocks(replacement, blocks.data(), 1, 5) == OK &&
			          f.voice.blocks.size() == 1 && f.voice.sample_phase == 0 &&
			          f.voice.decoded_samples == progress.num_decoded_samples &&
			          f.voice.decoded_bytes == progress.decoded_data_size,
			      "CLEAR failed to replace a full queue while preserving progress");
			f.Render(2);
			Check(f.voice.samples == std::vector<float> {0.75f, 0.4375f},
			      "CLEAR retained an old source or fractional playback position");
		}
	}
}

struct PcmRing {
	static constexpr uint32_t block_frames = 7, block_count = 64;
	Fixture& fixture;
	std::array<std::array<int16_t, block_frames * 2>, 4> buffers {};
	uint32_t next_block = 0, completed = 0;

	void QueueNext(uint32_t slot) {
		auto& buffer = buffers[slot];
		for (uint32_t i = 0; i < block_frames; ++i) {
			buffer[2 * i] = static_cast<int16_t>(100 + (next_block * block_frames + i) * 16);
			buffer[2 * i + 1] = -buffer[2 * i];
		}
		const Ngs2WaveformBlock block {0, sizeof(buffer), 0, 0, block_frames, 0, slot};
		++next_block;
		Check(fixture.QueueBlocks(buffer.data(), &block, 1, next_block == block_count ? 0 : 1) == OK,
		      "released ring buffer could not be queued again");
	}
};

void KYTY_SYSV_ABI RefillPcmRing(const Ngs2VoiceCallbackInfo* info) {
	auto& ring = *reinterpret_cast<PcmRing*>(info->data);
	const auto slot = ring.completed++ % ring.buffers.size();
	Check(info->flag == 1 && info->attributes == 0 && info->user == slot &&
	          info->block_data == ring.buffers[slot].data() &&
	          info->size == sizeof(ring.buffers[slot]),
	      "ring completion callback released the wrong source buffer");
	for (const auto& block: ring.fixture.voice.blocks) {
		Check(block.data != reinterpret_cast<const uint8_t*>(ring.buffers[slot].data()),
		      "completion callback retained the buffer about to be overwritten");
	}
	if (ring.next_block < PcmRing::block_count) {
		ring.QueueNext(static_cast<uint32_t>(slot));
	}
}

void TestWaveformRingReuse() {
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		Fixture f(48000, 2, rack);
		PcmRing ring {f};
		f.voice.callback = reinterpret_cast<uintptr_t>(&RefillPcmRing);
		f.voice.callback_data = reinterpret_cast<uintptr_t>(&ring);
		f.voice.callback_flags = 1;
		for (uint32_t slot = 0; slot < ring.buffers.size(); ++slot) {
			ring.QueueNext(slot);
		}
		constexpr std::array<uint32_t, 6> grains {1, 16, 3, 29, 2, 11};
		constexpr auto total_frames = PcmRing::block_frames * PcmRing::block_count;
		uint32_t rendered = 0, grain_index = 0;
		while (rendered < total_frames) {
			const auto grain = std::min(grains[grain_index++ % grains.size()], total_frames - rendered);
			f.Render(grain);
			for (uint32_t i = 0; i < grain; ++i) {
				const float expected = (100 + (rendered + i) * 16) / 32768.0f;
				Check(f.voice.samples[i] == expected && f.voice.samples[grain + i] == -expected,
				      "ring buffer reuse replayed overwritten data or mixed stereo channels");
			}
			rendered += grain;
			const auto decoded = f.State().num_decoded_samples;
			Check(f.voice.blocks.size() <= ring.buffers.size() &&
			          decoded >= rendered && decoded - rendered < 256 &&
			          f.voice.decoded_bytes == decoded * 4 &&
			          ring.completed == decoded / PcmRing::block_frames,
			      "ring playback lost queue capacity or decoded progress");
			if (decoded < total_frames) {
				const auto slot = (decoded / PcmRing::block_frames) % ring.buffers.size();
				Check(f.voice.WaveformData() == reinterpret_cast<const uint8_t*>(
				          ring.buffers[slot].data() + (decoded % PcmRing::block_frames) * 2),
				      "ring read address did not track the active source buffer");
			}
		}
		Check(f.voice.blocks.empty() && f.voice.state == Ngs2VoicePlayState::Empty &&
		          ring.completed == PcmRing::block_count,
		      "finite ring stream did not release all buffers and finish");
	}
}

void TestProgressDrivenWaveformRing() {
	const auto source_sample = [](uint64_t frame) {
		return static_cast<int16_t>(static_cast<int32_t>((frame * 43) % 32768) - 16384);
	};
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		Fixture f(44100, 2, rack);
		std::array<std::array<int16_t, 1024 * 2>, 4> buffers {};
		uint64_t submitted = 0, decoded_bytes = 0, rendered = 0;
		uint32_t slot = 0;
		const auto refill = [&](uint32_t frames) {
			auto& buffer = buffers[slot];
			for (const auto& block: f.voice.blocks) {
				Check(block.data != reinterpret_cast<const uint8_t*>(buffer.data()),
				      "decoded progress allowed the guest to overwrite an outstanding ring buffer");
			}
			for (uint32_t i = 0; i < frames; ++i) {
				buffer[i * 2] = source_sample(submitted + i);
				buffer[i * 2 + 1] = -buffer[i * 2];
			}
			const Ngs2WaveformBlock block {0, frames * 4u, 0, 0, frames, 0, slot};
			Check(f.QueueBlocks(buffer.data(), &block, 1, 1) == OK,
			      "progress-driven ring exhausted its waveform slots");
			submitted += frames;
			slot = (slot + 1) % buffers.size();
		};
		refill(1024);
		for (uint32_t grain = 0; grain < 128; ++grain) {
			f.Render(256);
			for (uint32_t i = 0; i < 256; ++i) {
				const uint64_t phase = (rendered + i) * 44100;
				const auto frame = phase / 48000;
				const float fraction = static_cast<float>(phase % 48000) / 48000.0f;
				const float expected = std::lerp(static_cast<float>(source_sample(frame)),
				                                 static_cast<float>(source_sample(frame + 1)), fraction) /
				                       32768.0f;
				Check(std::abs(f.voice.samples[i] - expected) < 0.00001f &&
				          std::abs(f.voice.samples[256 + i] + expected) < 0.00001f,
				      "progress-driven ring replayed overwritten music samples");
			}
			rendered += 256;
			const auto state = f.State();
			const auto delta = state.decoded_data_size - decoded_bytes;
			Check(state.decoded_data_size >= decoded_bytes && delta <= sizeof(buffers[0]) &&
			          delta % 4 == 0 && state.num_decoded_samples * 4 == state.decoded_data_size &&
			          state.num_decoded_samples <= submitted,
			      "published sampler progress does not describe decoded source data");
			decoded_bytes = state.decoded_data_size;
			if (delta != 0) {
				refill(static_cast<uint32_t>(delta / 4));
			}
		}
		Check(decoded_bytes > 28 * sizeof(buffers[0]), "ring test did not exercise repeated buffer reuse");
	}
}

void TestStreamingResample() {
	Fixture              f(44100, 2);
	std::vector<int16_t> pcm(441 * 2);
	for (int i = 0; i < 441; ++i) {
		pcm[i * 2]     = static_cast<int16_t>(i * 32);
		pcm[i * 2 + 1] = static_cast<int16_t>(-i * 32);
	}
	f.Queue(pcm.data(), 200, 1);
	f.Queue(pcm.data() + 400, 241, 0);
	for (uint32_t grain = 0; grain < 3; ++grain) {
		f.Render(160);
		for (uint32_t i = 0; i < 160; ++i) {
			const float expected = std::min((grain * 160 + i) * 44100.0 / 48000.0, 440.0) / 1024.0;
			Check(std::abs(f.voice.samples[i] - expected) < 0.00001f,
			      "resampling discontinuity across grain or block boundary");
			Check(std::abs(f.voice.samples[160 + i] + expected) < 0.00001f,
			      "stereo channels were mixed");
		}
	}
	Check(f.voice.blocks.empty(), "final streaming block did not finish");
	Check(f.voice.state == Ngs2VoicePlayState::Empty, "finished voice remained active");
	Ngs2SamplerVoiceState state {};
	Ngs2VoiceGetState(reinterpret_cast<uintptr_t>(&f.voice), &state.voice_state, sizeof(state));
	Check(state.num_decoded_samples == 441 && state.decoded_data_size == pcm.size() * 2,
	      "streaming progress does not match consumed source data");
}

// One encoded mono block with one nonzero spectral coefficient.
constexpr std::array<uint8_t, 36> ATRAC9_MONO_BLOCK {0, 0, 0x04, 0x20, 0x04, 0xc0, 0, 0, 0x1f, 0xc0};

void TestAtrac9Vibration() {
	using namespace Libs::Audio::Ajm;
	for (uint8_t index: {7, 6}) {
		const int channels = index - 5;
		std::array<uint8_t, 4> config {0xfe, static_cast<uint8_t>(0x70 | (index << 1)), 0x03, 0xf0};
		const auto original = config;
		AjmAt9Decoder decoder(channels, 48000, AjmSampleEncoding::Float, 0);
		const auto initialized = decoder.Initialize(config.data(), config.size());
		AjmSidebandDecAt9CodecInfo info {};
		decoder.WriteCodecInfo(&info, sizeof(info), initialized);
		Check(initialized.result == OK && initialized.format.channel_num == channels &&
		          initialized.format.sampling_frequency == 48000 && info.frame_samples == 256 &&
		          info.frames_in_super_frame == 4 && info.super_frame_size == 128 &&
		          config == original,
		      "ATRAC9 vibration config returned incorrect geometry or changed input");
		Atrac9CodecInfo parsed {};
		Check(Ngs2GetAtrac9CodecInfo(config, parsed) && parsed.channels == channels &&
		          parsed.samplingRate == 48000 && parsed.frameSamples == 256 &&
		          parsed.framesInSuperframe == 4 && parsed.superframeSize == 128,
		      "NGS2 vibration metadata differs from decoder metadata");

		// Enlarge the superframe for the synthetic blocks and compare ordinary mono/dual mono.
		config[2] = 0x0b;
		Check(decoder.Initialize(config.data(), config.size()).result == OK,
		      "ATRAC9 fixture initialization failed");
		config[1] -= 12;
		auto reference = std::unique_ptr<void, decltype(&Atrac9ReleaseHandle)>(
		    Atrac9GetHandle(), Atrac9ReleaseHandle);
		Check(reference != nullptr && Atrac9InitDecoder(reference.get(), config.data()) == 0,
		      "ATRAC9 reference initialization failed");
		std::array<uint8_t, 384> encoded {};
		for (int frame = 0; frame < 4; ++frame) {
			for (int channel = 0; channel < channels; ++channel) {
				auto block = encoded.begin() + (frame * channels + channel) * ATRAC9_MONO_BLOCK.size();
				std::copy(ATRAC9_MONO_BLOCK.begin(), ATRAC9_MONO_BLOCK.end(), block);
				if (frame != 0) *block |= 0x80;
			}
		}
		std::vector<float> actual(4 * 256 * channels), expected(actual.size());
		size_t offset = 0;
		for (int frame = 0; frame < 4; ++frame) {
			int used = 0;
			Check(Atrac9DecodeF32(reference.get(), encoded.data() + offset,
			                       expected.data() + frame * 256 * channels, &used, 0) == 0 &&
			          used == channels * ATRAC9_MONO_BLOCK.size(),
			      "ATRAC9 reference decoding failed");
			offset += used;
		}
		for (int pass = 0; pass < 2; ++pass) {
			if (pass != 0) decoder.Reset();
			const auto decoded = decoder.Decode(encoded.data(), encoded.size(), actual.data(),
			                                      actual.size() * sizeof(float), true, nullptr);
			Check(decoded.result == OK && decoded.frames == 4 &&
			          decoded.input_consumed == encoded.size() &&
			          decoded.output_written == actual.size() * sizeof(float) &&
			          actual == expected && std::ranges::any_of(actual, [](float sample) {
				          return std::abs(sample) > 0.00001f;
			          }),
			      "ATRAC9 vibration decoding or reset differs from mono/dual mono");
		}
		{
			auto invalid = original;
			invalid[0] = 0;
			Check(decoder.Initialize(invalid.data(), invalid.size()).result != OK &&
			          !Ngs2GetAtrac9CodecInfo(invalid, parsed),
			      "ATRAC9 vibration accepted an invalid header");
			invalid = original;
			invalid[1] |= 1;
			Check(decoder.Initialize(invalid.data(), invalid.size()).result != OK &&
			          !Ngs2GetAtrac9CodecInfo(invalid, parsed),
			      "ATRAC9 vibration accepted an unsupported extension");
		}
	}
}

void TestAtrac9Discrete() {
	using namespace Libs::Audio::Ajm;
	constexpr std::array<uint8_t, 4> captured {0x30, 0x72, 0xc0, 0xfe};
	struct Geometry { uint32_t channels, rate_index, rate, samples, frames, bytes; };
	for (const auto [channels, rate_index, rate, samples, frames, bytes]:
	     {Geometry {12, 7, 48000, 256, 4, 64}, Geometry {16, 4, 24000, 128, 1, 96},
	      Geometry {64, 1, 12000, 64, 4, 96}}) {
		const uint32_t word = 0x30000000u | (rate_index << 20u) | ((channels - 1u) << 14u) |
		                      ((bytes - 1u) << 2u) | (frames == 4 ? 2u : 0u);
		std::array<uint8_t, 4> config {uint8_t(word >> 24u), uint8_t(word >> 16u),
		                               uint8_t(word >> 8u), uint8_t(word)};
		const auto original = config;
		AjmAt9Decoder decoder(channels, rate, AjmSampleEncoding::S16, 0);
		const auto initialized = decoder.Initialize(config.data(), config.size());
		AjmSidebandDecAt9CodecInfo info {};
		decoder.WriteCodecInfo(&info, sizeof(info), initialized);
		Atrac9CodecInfo parsed {};
		Check(initialized.result == OK && initialized.format.channel_num == channels &&
		          initialized.format.sampling_frequency == rate && info.frame_samples == samples &&
		          info.frames_in_super_frame == frames && info.super_frame_size == bytes * frames * channels &&
		          Ngs2GetAtrac9CodecInfo(config, parsed) && parsed.channels == channels &&
		          parsed.samplingRate == rate && parsed.frameSamples == samples &&
		          parsed.framesInSuperframe == frames && parsed.superframeSize == bytes * frames * channels &&
		          config == original && (channels != 12 || config == captured),
		      "ATRAC9 discrete metadata or channel limit is incorrect");
	}

	// The existing mono fixture's constant gradient/scalefactors give 24 nine-bit
	// coefficients. Ignored extension bits vary frame lengths independently of PCM.
	std::array<std::array<std::vector<uint8_t>, 4>, 12> blocks;
	std::array<size_t, 12> channel_bytes {};
	std::array<size_t, 4> frame_ends {};
	std::vector<uint8_t> encoded;
	for (unsigned frame = 0; frame < 4; ++frame) {
		for (unsigned channel = 0; channel < 12; ++channel) {
			auto& block = blocks[channel][frame];
			unsigned position = 0;
			auto put = [&](unsigned value, unsigned bits) {
				for (unsigned bit = bits; bit != 0; --bit) {
					if (position % 8 == 0) block.push_back(0);
					block.back() |= ((value >> (bit - 1)) & 1u) << (7u - position++ % 8u);
				}
			};
			put(frame != 0, 1); // First frame, fresh band parameters, three bands, no BEX.
			put(0, 6);
			put(0, 14); // Gradient mode/start/end, then constant value 16.
			put(16, 5);
			put(16, 5);
			put(0, 4);
			put(1, 1); // Optional ignored extension payload of 0/8/16/24 bits.
			put(0, 2);
			put((channel % 4) * 8, 5);
			put(0, (channel % 4) * 8);
			put(1, 2); // CLC scale factors: two bits each, base 24.
			put(0, 2);
			put(24, 5);
			for (unsigned unit = 0; unit < 10; ++unit) put(0, 2);
			const int coefficient = int((channel + 1) * (frame + 1) * 4);
			put(unsigned(channel % 2 ? -coefficient : coefficient) & 0x1ffu, 9);
			for (unsigned coefficient = 1; coefficient < 24; ++coefficient) put(0, 9);
			encoded.insert(encoded.end(), block.begin(), block.end());
			channel_bytes[channel] += block.size();
			if (frame == 3) {
				Check(channel_bytes[channel] < 256, "ATRAC9 test channel has no padding");
				encoded.insert(encoded.end(), 256 - channel_bytes[channel], 0x01);
			}
		}
		frame_ends[frame] = encoded.size();
	}
	Check(encoded.size() == 3072 && blocks[0][0].size() != blocks[3][0].size(),
	      "ATRAC9 discrete fixture did not exercise distinct channel padding");

	for (auto encoding: {AjmSampleEncoding::S16, AjmSampleEncoding::S32,
	                     AjmSampleEncoding::Float}) {
		const size_t sample_bytes = AjmBytesPerSample(encoding);
		const size_t row_bytes = 12 * sample_bytes;
		const size_t frame_bytes = 256 * row_bytes;
		std::vector<uint8_t> expected(4 * frame_bytes), mono(256 * sample_bytes);
		for (unsigned channel = 0; channel < 12; ++channel) {
			auto reference = std::unique_ptr<void, decltype(&Atrac9ReleaseHandle)>(
			    Atrac9GetHandle(), Atrac9ReleaseHandle);
			std::array<uint8_t, 4> mono_config {0xfe, 0x70, 0x07, 0xf0};
			Check(reference && Atrac9InitDecoder(reference.get(), mono_config.data()) == 0,
			      "ATRAC9 discrete mono reference initialization failed");
			for (unsigned frame = 0; frame < 4; ++frame) {
				int used = 0, result = -1;
				std::array<uint8_t, 64> input {}; // LibAtrac9's bit reader peeks past the final byte.
				std::copy(blocks[channel][frame].begin(), blocks[channel][frame].end(), input.begin());
				switch (encoding) {
					case AjmSampleEncoding::S16:
						result = Atrac9Decode(reference.get(), input.data(),
						                      reinterpret_cast<int16_t*>(mono.data()), &used, 0);
						break;
					case AjmSampleEncoding::S32:
						result = Atrac9DecodeS32(reference.get(), input.data(),
						                         reinterpret_cast<int32_t*>(mono.data()), &used, 0);
						break;
					case AjmSampleEncoding::Float:
						result = Atrac9DecodeF32(reference.get(), input.data(),
						                         reinterpret_cast<float*>(mono.data()), &used, 0);
						break;
				}
				Check(result == 0 && used == blocks[channel][frame].size(),
				      "ATRAC9 discrete mono reference did not consume its exact frame");
				for (unsigned sample = 0; sample < 256; ++sample) {
					std::memcpy(expected.data() + (frame * 256 + sample) * row_bytes +
					                channel * sample_bytes,
					            mono.data() + sample * sample_bytes, sample_bytes);
				}
			}
		}
		Check(std::ranges::any_of(expected, [](uint8_t byte) { return byte != 0; }),
		      "ATRAC9 discrete reference unexpectedly contains only silence");
		AjmAt9Decoder decoder(12, 48000, encoding, 0);
		Check(decoder.Initialize(captured.data(), captured.size()).result == OK,
		      "ATRAC9 discrete decoder initialization failed");
		std::vector<uint8_t> actual(expected.size() + 32, 0xa5);
		auto decode = [&](size_t input_size, size_t output_size) {
			return decoder.Decode(encoded.data(), input_size, actual.data() + 16,
			                      output_size, true, nullptr);
		};
		const auto partial = decode(encoded.size() - 1, expected.size());
		const auto short_output = decode(encoded.size(), frame_bytes - 1);
		Check(partial.result == AJM_RESULT_PARTIAL_INPUT && partial.frames == 0 &&
		          partial.input_consumed == 0 && partial.output_written == 0 &&
		          short_output.result == AJM_RESULT_NOT_ENOUGH_ROOM && short_output.frames == 0 &&
		          short_output.input_consumed == 0 && short_output.output_written == 0 &&
		          std::ranges::all_of(actual, [](uint8_t byte) { return byte == 0xa5; }),
		      "ATRAC9 discrete partial input/output consumed data or wrote PCM");
		for (unsigned pass = 0; pass < 2; ++pass) {
			if (pass != 0) decoder.Reset();
			const auto result = decode(encoded.size(), expected.size());
			Check(result.result == OK && result.frames == 4 &&
			          result.input_consumed == encoded.size() &&
			          result.output_written == expected.size() && result.total_decoded_samples == 1024 &&
			          std::equal(expected.begin(), expected.end(), actual.begin() + 16) &&
			          std::all_of(actual.begin(), actual.begin() + 16, [](auto b) { return b == 0xa5; }) &&
			          std::all_of(actual.end() - 16, actual.end(), [](auto b) { return b == 0xa5; }),
			      "ATRAC9 discrete interleave, per-channel padding, reset or PCM bounds differ");
		}
		decoder.Reset();
		size_t offset = 0;
		for (unsigned frame = 0; frame < 4; ++frame) {
			const auto result = decoder.Decode(encoded.data() + offset, encoded.size() - offset,
			                                   actual.data() + 16, frame_bytes, false, nullptr);
			Check(result.result == OK && result.frames == 1 &&
			          result.input_consumed == frame_ends[frame] - offset &&
			          result.output_written == frame_bytes &&
			          std::equal(expected.begin() + frame * frame_bytes,
			                     expected.begin() + (frame + 1) * frame_bytes, actual.begin() + 16),
			      "ATRAC9 discrete split calls lost channel histories or padding position");
			offset = frame_ends[frame];
		}
		decoder.Reset();
		AjmGaplessState gapless;
		gapless.Set({700, 17, 0}, true);
		const auto trimmed = decoder.Decode(encoded.data(), encoded.size(), actual.data() + 16,
		                                    700 * row_bytes, true, &gapless);
		Check(trimmed.result == OK && trimmed.frames == 4 &&
		          trimmed.input_consumed == encoded.size() && trimmed.output_written == 700 * row_bytes &&
		          trimmed.total_decoded_samples == 700 && gapless.IsEnd() &&
		          std::equal(expected.begin() + 17 * row_bytes,
		                     expected.begin() + 717 * row_bytes, actual.begin() + 16),
		      "ATRAC9 discrete gapless trim or superframe drain changed channel order");
	}
	for (auto invalid: {std::array<uint8_t, 4> {0x31, 0x72, 0xc0, 0xfe},
	                   std::array<uint8_t, 4> {0x30, 0xf2, 0xc0, 0xfe},
	                   std::array<uint8_t, 4> {0x30, 0x62, 0xc0, 0xfe},
	                   std::array<uint8_t, 4> {0x30, 0x72, 0xe0, 0xfe},
	                   std::array<uint8_t, 4> {0x30, 0x72, 0xc1, 0x82},
	                   std::array<uint8_t, 4> {0x30, 0x72, 0xc0, 0xfd},
	                   std::array<uint8_t, 4> {0x30, 0x72, 0xc0, 0xff}}) {
		Atrac9CodecInfo info {};
		AjmAt9Decoder decoder(12, 48000, AjmSampleEncoding::S16, 0);
		Check(!Ngs2GetAtrac9CodecInfo(invalid, info) &&
		          decoder.Initialize(invalid.data(), invalid.size()).result != OK,
		      "ATRAC9 discrete accepted unsupported configuration fields");
	}
}

void TestAtrac9ResampleAcrossFrames() {
	const auto make_voice = [] {
		auto f = std::make_unique<Fixture>(24000);
		f->voice.SetupSampler({NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0});
		f->voice.state = Ngs2VoicePlayState::Playing;
		f->voice.decoded_frame[0] = 0.0f;
		f->voice.decoded_frame[1] = 0.5f;
		f->voice.frame_samples = 2;
		f->voice.next_decoded_frame.resize(f->voice.decoded_frame.size());
		f->voice.next_decoded_frame[0] = 1.0f;
		f->voice.next_decoded_frame[1] = 1.5f;
		f->voice.next_frame_samples = 2;
		f->voice.decoded_samples = 4;
		f->voice.accepts_blocks = false;
		return f;
	};
	auto whole = make_voice();
	whole->Render(8);
	const std::vector<float> expected {0.0f, 0.25f, 0.5f, 0.75f,
	                                   1.0f, 1.25f, 1.5f, 1.5f};
	Check(whole->voice.samples == expected && whole->voice.decoded_samples == 4 &&
	          whole->voice.state == Ngs2VoicePlayState::Empty,
	      "24 kHz ATRAC9 interpolation or source progress failed at frame boundary");
	auto split = make_voice();
	split->Render(3);
	const auto first = split->voice.samples;
	split->Render(5);
	std::vector<float> joined = first;
	joined.insert(joined.end(), split->voice.samples.begin(), split->voice.samples.end());
	Check(joined == expected && split->voice.decoded_samples == 4,
	      "ATRAC9 resampling changed across render grains");
	auto pitched = make_voice();
	struct Pitch {
		Ngs2VoiceParamHeader header {16, 0, 0x10000005};
		float                ratio    = 2.0f;
		uint32_t             reserved = 0;
	} pitch;
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&pitched->voice), &pitch.header) == OK,
	      "ATRAC9 pitch control failed");
	pitched->Render(4);
	Check(pitched->voice.samples == std::vector<float> {0.0f, 0.5f, 1.0f, 1.5f} &&
	          pitched->voice.decoded_samples == 4,
	      "ATRAC9 pitch did not use source-rate phase advancement");
}

void TestAtrac9ResampleAfterStarvation() {
	Fixture f(24000);
	f.voice.SetupSampler({NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0});
	f.voice.state            = Ngs2VoicePlayState::Playing;
	f.voice.accepts_blocks   = true;
	f.voice.decoded_frame[0] = 0.0f;
	f.voice.decoded_frame[1] = 0.5f;
	f.voice.frame_samples    = 2;
	f.voice.decoded_samples  = 2;
	f.Render(4);
	Check(f.voice.samples == std::vector<float> {0.0f, 0.25f, 0.5f, 0.5f} &&
	          f.voice.decoded_samples == 2,
	      "ATRAC9 resampling lost the final sample before starvation");
	f.Render(2);
	Check(!f.voice.has_samples && f.voice.state == Ngs2VoicePlayState::Playing &&
	          f.voice.decoded_samples == 2,
	      "starved ATRAC9 stream consumed source samples");
	f.voice.next_decoded_frame.resize(f.voice.decoded_frame.size());
	f.voice.next_decoded_frame[0] = 1.0f;
	f.voice.next_decoded_frame[1] = 1.5f;
	f.voice.next_frame_samples    = 2;
	f.voice.decoded_samples       += 2;
	f.Render(4);
	Check(f.voice.samples == std::vector<float> {1.0f, 1.25f, 1.5f, 1.5f} &&
	          f.voice.decoded_samples == 4,
	      "ATRAC9 refill lost source-rate phase");
}

void TestWaveformBlockGeometry() {
	const Ngs2WaveformFormat format {NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0};
	Ngs2WaveformBlock block {};
	Check(Ngs2CalcWaveformBlock(&format, 0, 34534, &block) == OK &&
	          block.data_offset == 0 && block.data_size == 13056 &&
	          block.num_skip_samples == 0 && block.num_samples == 34534,
	      "ATRAC9 block calculation omitted encoded superframes");
	Check(Ngs2CalcWaveformBlock(&format, 513, 1, &block) == OK &&
	          block.data_offset == 192 && block.data_size == 192 &&
	          block.num_skip_samples == 1 && block.num_samples == 1,
	      "ATRAC9 block calculation lost the source offset");
	Check(Ngs2CalcWaveformBlock(&format, 0, 0, &block) == OK && block.data_size == 0 &&
	          block.num_samples == 0,
	      "empty ATRAC9 block acquired encoded data");
	const Ngs2WaveformFormat pcm {0x12, 2, 48000};
	Check(Ngs2CalcWaveformBlock(&pcm, 4096, 4096, &block) == OK &&
	          block.data_offset == 16384 && block.data_size == 16384 &&
	          block.num_skip_samples == 0 && block.num_samples == 4096,
	      "PCM block calculation omitted the sample frames");
	Fixture f(24000);
	const int16_t silence = 0;
	f.Queue(&silence, 0, 0, UINT32_MAX);
	f.Render(1);
	Check(f.voice.blocks.empty() && f.voice.state == Ngs2VoicePlayState::Empty,
	      "empty repeated block kept the sampler active");
}

void TestSubmixerSetupAndRouting() {
	Fixture f(48000, 2);
	Ngs2RackInternal submixer_rack {};
	submixer_rack.ngs                         = &f.system;
	submixer_rack.type                        = Ngs2RackType::Submixer;
	submixer_rack.option.submixer.max_channels = 8;
	Ngs2VoiceInternal submixer;
	submixer.rack = &submixer_rack;
	submixer.ports.resize(1);
	submixer.matrices.resize(1);
	Ngs2RackInternal mastering_rack {};
	mastering_rack.ngs  = &f.system;
	mastering_rack.type = Ngs2RackType::Mastering;
	Ngs2VoiceInternal mastering;
	mastering.rack     = &mastering_rack;
	mastering.channels = 2;
	mastering.state    = Ngs2VoicePlayState::Playing;
	submixer.state         = Ngs2VoicePlayState::Playing;
	submixer.ports[0].dest = &mastering;
	submixer.matrices[0]  = {1.0f};
	f.voice.ports.push_back({&submixer, 0, 1.0f, -1});
	const int16_t pcm[] = {8192, -8192, 16384, -16384};
	f.Queue(pcm, 2, 0);
	const std::vector<Ngs2VoiceInternal*> voices {&f.voice, &submixer, &mastering};
	Ngs2RenderVoice(mastering, voices, 1);
	Check(!submixer.has_samples && !mastering.has_samples && f.voice.decoded_samples == 0,
	      "unconfigured zero-channel submixer claimed to produce samples");
	Ngs2SubmixerVoiceSetupParam setup {{16, 0, 0x20000000}, 2, 0};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&submixer), &setup.header) == OK &&
	          submixer.channels == 2 && submixer.state == Ngs2VoicePlayState::Empty &&
	          submixer.ports[0].dest == nullptr && submixer.matrices[0].empty(),
	      "submixer setup did not establish channels and clear old routing");
	submixer.state = Ngs2VoicePlayState::Playing;
	submixer.ports[0] = {&mastering, 0, 1.0f, 0};
	submixer.matrices[0] = {1.0f, 0.0f, 0.0f, 1.0f};
	for (float expected: {0.25f, 0.5f}) {
		for (auto* voice: voices) {
			voice->rendered = false;
		}
		Ngs2RenderVoice(mastering, voices, 1);
		Check(mastering.has_samples && mastering.samples[0] == expected &&
		          mastering.samples[1] == -expected,
		      "submixer lost routed samples across render grains");
	}
}

uint32_t g_side_chain_flags = 0;

void KYTY_SYSV_ABI MultiplyBySideChain(Ngs2UserFxProcessContext* context) {
	Check(context->num_channels == 1 && context->grain_samples == 1 &&
	          context->user_data[0] == 7,
	      "user FX context did not describe the custom voice");
	g_side_chain_flags = context->flags;
	context->channels[0][0] *= context->channels[1][0];
}

void TestCustomSideChainUserFx() {
	Fixture          main(48000), side(48000);
	Ngs2RackInternal custom_rack {};
	auto&            custom = custom_rack.option.custom_submixer.custom_rack_option;
	custom_rack.ngs         = &main.system;
	custom_rack.type        = Ngs2RackType::CustomSubmixer;
	custom.num_buffers      = 2;
	custom.num_modules      = 1;
	custom.module[0]        = {nullptr, 0x19, 0, 1, 0};
	Ngs2VoiceInternal voice;
	voice.rack = &custom_rack;
	voice.ports.resize(1);
	voice.modules.resize(1);
	struct {
		Ngs2VoiceParamHeader header {24, 0, 0x40020000};
		uint32_t             inputs = 1, outputs = 1, flags = 0, reserved = 0;
	} setup;
	struct {
		Ngs2VoiceParamHeader header;
		void                 KYTY_SYSV_ABI (*handler)(Ngs2UserFxProcessContext*);
		uintptr_t            user_data[3];
	} fx {{40, 0, 0x40001900}, MultiplyBySideChain, {7, 0, 0}};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK &&
	          Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &fx.header) == OK,
	      "custom submixer user FX setup failed");
	voice.state = Ngs2VoicePlayState::Playing;
	main.voice.ports.push_back({&voice, 0, 1.0f, -1});
	side.voice.ports.push_back({&voice, 1, 1.0f, -1});
	const int16_t main_pcm = 8192, side_pcm = 16384;
	main.Queue(&main_pcm, 1, 0);
	side.Queue(&side_pcm, 1, 0);
	Ngs2RenderVoice(voice, {&main.voice, &side.voice, &voice}, 1);
	Check(voice.has_samples && voice.samples[0] == 0.125f && voice.samples[1] == 0.5f &&
	          g_side_chain_flags == 1,
	      "user FX did not receive the side-chain buffer after the source channels");
}

struct RenderRack {
	std::unique_ptr<std::byte[]> storage;
	uintptr_t handle = 0;

	RenderRack(Ngs2Internal& system, uint32_t id, const Ngs2RackOption& option) {
		Ngs2ContextBufferInfo buffer {};
		Check(Ngs2RackQueryBufferSize(id, &option, &buffer) == OK, "rack size query failed");
		storage = std::make_unique<std::byte[]>(buffer.host_buffer_size);
		buffer.host_buffer = storage.get();
		Check(Ngs2RackCreate(reinterpret_cast<uintptr_t>(&system), id, &option, &buffer,
		                     &handle) == OK, "render rack creation failed");
	}

	~RenderRack() { Check(Ngs2RackDestroy(handle, nullptr) == OK, "render rack cleanup failed"); }

	Ngs2VoiceInternal& Voice() {
		return *reinterpret_cast<Ngs2VoiceInternal*>(reinterpret_cast<Ngs2RackInternal*>(handle) + 1);
	}
};

void TestPcm16RenderOutput() {
	Ngs2Internal system;
	system.option                   = Ngs2DefaultSystemOption();
	system.option.num_grain_samples = 4;
	Ngs2RackOptionUnion sampler_option {}, mastering_option {};
	Ngs2FillDefaultRackOption(0x1000, &sampler_option);
	Ngs2FillDefaultRackOption(0x3000, &mastering_option);
	sampler_option.common.max_voices = 2;
	sampler_option.common.max_ports  = 1;
	RenderRack sampler(system, 0x1000, sampler_option.common);
	RenderRack master(system, 0x3000, mastering_option.common);
	const auto system_handle = reinterpret_cast<uintptr_t>(&system);
	auto* samplers = &sampler.Voice();
	auto& mastering = master.Voice();
	mastering.channels = 1;
	mastering.state    = Ngs2VoicePlayState::Playing;
	const int16_t source[2][4] = {{16384, -16384, 32767, -32768},
	                              {8192, -8192, 32767, -32768}};
	for (uint32_t i = 0; i < 2; ++i) {
		samplers[i].SetupSampler({0x12, 1, 48000});
		samplers[i].blocks.push_back(
		    {reinterpret_cast<const uint8_t*>(source[i]), {0, sizeof(source[i]), 0, 0, 4}});
		samplers[i].ports[0].dest = &mastering;
		samplers[i].SetEvent(1);
	}
	int16_t output_samples[4] = {};
	Ngs2RenderBufferInfo output {output_samples, sizeof(output_samples), 0x12, 1};
	Check(Ngs2SystemRender(system_handle, &output, 1) == OK &&
	          output_samples[0] == 24576 && output_samples[1] == -24576 &&
	          output_samples[2] == 32767 && output_samples[3] == -32768,
	      "PCM16 output failed to mix or saturate mastering voices");
	Check(Ngs2SystemRender(system_handle, &output, 1) == OK &&
	          std::ranges::all_of(output_samples, [](int16_t sample) { return sample == 0; }),
	      "PCM16 output retained samples from the previous render");
}

void TestPatchToRackHandle() {
	Ngs2Internal system;
	Ngs2RackOptionUnion sampler_option {}, mastering_option {};
	Ngs2FillDefaultRackOption(0x1000, &sampler_option);
	Ngs2FillDefaultRackOption(0x3000, &mastering_option);
	sampler_option.common.max_voices = 1;
	RenderRack sampler(system, 0x1000, sampler_option.common);
	RenderRack master(system, 0x3000, mastering_option.common);
	auto& source = sampler.Voice();
	Ngs2VoicePatchParam patch {{24, 0, 5}, 0, 0, master.handle};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&source), &patch.header) == OK &&
	          source.ports[0].dest == &master.Voice(),
	      "patch to a rack handle did not connect to its voice #0");
}

void TestMasteringGain() {
	Ngs2Internal system;
	system.option                   = Ngs2DefaultSystemOption();
	system.option.num_grain_samples = 1;
	Ngs2RackOptionUnion sampler_option {}, mastering_option {};
	Ngs2FillDefaultRackOption(0x1000, &sampler_option);
	Ngs2FillDefaultRackOption(0x3000, &mastering_option);
	sampler_option.common.max_voices = 1;
	RenderRack sampler(system, 0x1000, sampler_option.common);
	RenderRack master(system, 0x3000, mastering_option.common);
	auto& source = sampler.Voice();
	auto& mastering = master.Voice();
	mastering.channels = 8;
	mastering.state = Ngs2VoicePlayState::Playing;
	const int16_t frame[8] = {8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192};
	source.SetupSampler({0x12, 8, 48000});
	source.blocks.push_back({reinterpret_cast<const uint8_t*>(frame), {0, sizeof(frame), 0, 0, 1}});
	source.ports[0].dest = &mastering;
	source.SetEvent(1);
	struct {
		Ngs2VoiceParamHeader header {16, 0, 0x30000004};
		float fbw = 5.0f, lfe = 0.5f;
	} gain;
	float pcm[8] = {};
	Ngs2RenderBufferInfo output {pcm, sizeof(pcm), 0x18, 8};
	const float expected[8] = {1.0f, 1.0f, 1.0f, 0.125f, 1.0f, 1.0f, 1.0f, 1.0f};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&mastering), &gain.header) == OK &&
	          Ngs2SystemRender(reinterpret_cast<uintptr_t>(&system), &output, 1) == OK &&
	          std::ranges::equal(pcm, expected),
	      "mastering gain was not clamped or applied per full band and LFE channel");
}

void TestMasteringUnknownControl() {
	Ngs2Internal system;
	system.option = Ngs2DefaultSystemOption();
	Ngs2RackOptionUnion mastering_option {};
	Ngs2FillDefaultRackOption(0x3000, &mastering_option);
	mastering_option.common.max_voices = 1;
	RenderRack master(system, 0x3000, mastering_option.common);
	auto& mastering = master.Voice();

	// Test 0x30000002: LFE Filter
	Ngs2MasteringVoiceEffectParam lfeParam {};
	lfeParam.header      = {sizeof(Ngs2MasteringVoiceEffectParam), 0, SCE_NGS2_MASTERING_VOICE_PARAM_LFE};
	lfeParam.enable_flag = 1;
	lfeParam.value       = 80; // 80 Hz cutoff
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&mastering), &lfeParam.header) == OK,
	      "mastering LFE control was not accepted");
	Check(mastering.mastering_lfe.enabled && mastering.mastering_lfe.cutoff == 80,
	      "mastering LFE not configured correctly");

	// Test 0x30000003: Limiter
	Ngs2MasteringVoiceEffectParam limiterParam {};
	limiterParam.header      = {sizeof(Ngs2MasteringVoiceEffectParam), 0, SCE_NGS2_MASTERING_VOICE_PARAM_LIMITER};
	limiterParam.enable_flag = 1;
	float threshold = 0.75f;
	std::memcpy(&limiterParam.value, &threshold, sizeof(threshold));
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&mastering), &limiterParam.header) == OK,
	      "mastering Limiter control was not accepted");
	Check(mastering.mastering_limiter.enabled && std::abs(mastering.mastering_limiter.threshold - 0.75f) < 1e-4f,
	      "mastering Limiter not configured correctly");

	// Test 0x30000006: Peak Meter
	Ngs2MasteringVoiceEffectParam peakParam {};
	peakParam.header      = {sizeof(Ngs2MasteringVoiceEffectParam), 0, SCE_NGS2_MASTERING_VOICE_PARAM_PEAK_METER};
	peakParam.enable_flag = 1; // Peak mode
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&mastering), &peakParam.header) == OK,
	      "mastering Peak Meter control was not accepted");
	Check(mastering.peak_meter_mode == 1, "mastering Peak Meter mode not configured correctly");
}

void TestPcmWaveformFormats() {
	Ngs2Internal system;
	system.option = Ngs2DefaultSystemOption();
	Ngs2RackOptionUnion sampler_option {};
	Ngs2FillDefaultRackOption(0x1000, &sampler_option);
	sampler_option.common.max_voices = 1;
	RenderRack sampler(system, 0x1000, sampler_option.common);
	auto& voice = sampler.Voice();

	// PCM format tests: 0x11 (8-bit), 0x12 (16-bit), 0x14 (24-bit), 0x16 (32-bit integer), 0x18 (32-bit floating-point), 0x1a (64-bit double)
	const uint32_t formats[] = {0x11, 0x12, 0x14, 0x16, 0x18, 0x1a};
	for (uint32_t fmt : formats) {
		struct {
			Ngs2VoiceParamHeader header {40, 0, 0x10000000};
			Ngs2WaveformFormat   format {};
			uint32_t             flags = 0, reserved = 0;
		} setup;
		setup.format.waveform_type = fmt;
		setup.format.num_channels  = 2;
		setup.format.sample_rate   = 48000;
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
		      "PCM waveform_type setup was not accepted");

		Ngs2WaveformBlock block {};
		Check(Ngs2CalcWaveformBlock(&setup.format, 100, 200, &block) == OK,
		      "PCM CalcWaveformBlock failed");
		Check(block.num_samples == 200, "PCM CalcWaveformBlock wrong num_samples");
	}
}

void TestMasteringDownmix() {
	Ngs2Internal system;
	system.option                   = Ngs2DefaultSystemOption();
	system.option.num_grain_samples = 1;
	Ngs2RackOptionUnion sampler_option {}, mastering_option {};
	Ngs2FillDefaultRackOption(0x1000, &sampler_option);
	Ngs2FillDefaultRackOption(0x3000, &mastering_option);
	sampler_option.common.max_voices = 1;
	RenderRack sampler(system, 0x1000, sampler_option.common);
	RenderRack master(system, 0x3000, mastering_option.common);
	auto& source = sampler.Voice();
	auto& mastering = master.Voice();
	mastering.channels = 2;
	mastering.state = Ngs2VoicePlayState::Playing;
	const int16_t frame[2] = {8192, 16384};
	source.SetupSampler({0x12, 2, 48000});
	source.blocks.push_back({reinterpret_cast<const uint8_t*>(frame), {0, sizeof(frame), 0, 0, 1}});
	source.ports[0].dest = &mastering;
	source.SetEvent(1);
	// The stored 2.0ch -> 2.0ch matrix applies to neither a mono output nor a mono voice.
	mastering.output_matrix.assign(4, 0.0f);
	float pcm = 0.0f;
	Ngs2RenderBufferInfo output {&pcm, sizeof(pcm), 0x18, 1};
	Check(Ngs2SystemRender(reinterpret_cast<uintptr_t>(&system), &output, 1) == OK &&
	          pcm == 0.75f * std::numbers::sqrt2_v<float> / 2,
	      "stereo mastering voice did not downmix into a mono output");
	mastering.channels = 1;
	source.blocks.push_back({reinterpret_cast<const uint8_t*>(frame), {0, sizeof(frame), 0, 0, 1}});
	float stereo[2] = {};
	output = {stereo, sizeof(stereo), 0x18, 2};
	Check(Ngs2SystemRender(reinterpret_cast<uintptr_t>(&system), &output, 1) == OK &&
	          stereo[0] == pcm * (std::numbers::sqrt2_v<float> / 2) && stereo[1] == stereo[0],
	      "mono mastering voice did not upmix into a stereo output");
}

struct ReverbControl {
	Ngs2VoiceParamHeader header {96, 0, 0x20010001};
	float                wet = 1.0f, dry = 0.0f;
	int32_t              room = 0, room_hf = 0;
	uint32_t             reflection_pattern = 0;
	float                decay_time = 1.0f, hf_ratio = 1.0f;
	int32_t              reflections       = 0;
	float                reflections_delay = 0.0f;
	int32_t              reverb            = -10000;
	float    reverb_delay = 0.0f, diffusion = 100.0f, density = 100.0f, hf_reference = 5000.0f;
	uint32_t channel_mask = 0, reserved[7] {};
};
static_assert(sizeof(ReverbControl) == 96);

struct ReverbFixture {
	Fixture           source;
	Ngs2RackInternal  rack {};
	Ngs2VoiceInternal voice;

	explicit ReverbFixture(uint32_t channels = 1): source(48000, channels) {
		rack.ngs   = &source.system;
		rack.type  = Ngs2RackType::Reverb;
		voice.rack = &rack;
		Setup();
		source.voice.ports.push_back({&voice, 0, 1.0f, -1});
	}

	void Setup() {
		struct {
			Ngs2VoiceParamHeader header {24, 0, 0x20010000};
			uint32_t             input, output, flags = 0, reserved = 0;
		} setup {{24, 0, 0x20010000}, source.voice.channels, source.voice.channels};
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
		      "reverb setup failed");
		Event(1);
	}

	void Control(const ReverbControl& param) {
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &param.header) == OK,
		      "reverb I3DL2 control failed");
	}

	void Event(uint32_t event) {
		Ngs2VoiceEventParam param {{sizeof(Ngs2VoiceEventParam), 0, 6}, event};
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &param.header) == OK,
		      "reverb event failed");
	}

	const std::vector<float>& Render(uint32_t grain) {
		source.voice.rendered = voice.rendered = false;
		Ngs2RenderVoice(voice, {&source.voice, &voice}, grain);
		return voice.samples;
	}
};

void TestReverbMaskAndLevels() {
	ReverbFixture f(6);
	ReverbControl param;
	param.wet               = 2.0f;
	param.dry               = 3.0f;
	param.channel_mask      = 0x8; // The stock presets exclude the LFE channel.
	param.reflections_delay = 0.001f;
	f.Control(param);
	const int16_t impulse[6] = {8192, 0, 0, 8192, 0, 0};
	f.source.Queue(impulse, 1, 0);
	const auto& output = f.Render(128);
	Check(output[0] == 0.75f && output[3 * 128] == 0.75f && output[48] == 0.5f &&
	          output[3 * 128 + 48] == 0.0f,
	      "reverb lost gain above one, dry mixing or the preset LFE mask");

	ReverbFixture clipped;
	param.channel_mask = 0;
	param.wet = param.dry = 64.0f;
	clipped.Control(param);
	clipped.source.Queue(impulse, 1, 0);
	const auto& limited = clipped.Render(128);
	Check(limited[0] == 8.0f && limited[48] == 8.0f,
	      "reverb wet/dry gains did not clamp to the PS5 maximum of 32");

	ReverbFixture toggled;
	param                   = {};
	param.reflections_delay = 0.02f;
	toggled.Control(param);
	toggled.source.Queue(impulse, 1, 0);
	toggled.Render(64);
	param.channel_mask = 1;
	toggled.Control(param);
	param.channel_mask = 0;
	toggled.Control(param);
	const auto& cleared = toggled.Render(2048);
	Check(std::ranges::all_of(cleared, [](float v) { return v == 0.0f; }),
	      "unmasking a channel replayed its old reflection");
}

void TestReverbRoomFilter() {
	std::vector<int16_t> sine(48000);
	for (size_t i = 0; i < sine.size(); ++i) {
		sine[i] =
		    static_cast<int16_t>(8192.0 * std::sin(2.0 * std::numbers::pi * 5000 * i / 48000));
	}
	auto energy = [&](int32_t room_hf, float reference) {
		ReverbFixture f;
		ReverbControl param;
		param.room_hf      = room_hf;
		param.hf_reference = reference;
		f.Control(param);
		f.source.Queue(sine.data(), sine.size(), 0);
		const auto& output = f.Render(sine.size());
		double      energy = 0.0;
		for (size_t i = sine.size() / 2; i < sine.size(); ++i) {
			energy += double(output[i]) * output[i];
		}
		return energy;
	};
	const double bright = energy(0, 5000.0f);
	const double dark   = energy(-6000, 5000.0f);
	Check(std::abs(std::sqrt(dark / bright) - 0.001) < 0.00003,
	      "roomHF did not attenuate early reflections at HFReference");
	Check(energy(-2000, 1000.0f) < energy(-2000, 5000.0f) * 0.1,
	      "HFReference did not move the room filter response");

	ReverbFixture extreme;
	ReverbControl param;
	param.room_hf      = -10000;
	param.hf_reference = 20.0f;
	extreme.Control(param);
	const int16_t impulse = 8192;
	extreme.source.Queue(&impulse, 1, 0);
	const auto& output = extreme.Render(32);
	Check(output[0] > 0.0f && std::ranges::all_of(output, [](float v) { return std::isfinite(v); }),
	      "extreme room filtering rounded its input coefficient to zero");
}

void TestReverbLifecycleAndDecayRange() {
	const int16_t impulse = 8192;
	for (uint32_t event: {2u, 4u, 8u}) {
		ReverbFixture f;
		ReverbControl param;
		param.reflections_delay = 0.02f;
		f.Control(param);
		f.source.Queue(&impulse, 1, 0);
		f.Render(64);
		f.Event(event);
		f.Render(64); // Include a stopped render before restarting the effect.
		f.Event(1);
		const auto& output = f.Render(2048);
		Check(std::ranges::all_of(output, [](float v) { return v == 0.0f; }),
		      "restarted reverb replayed reflections from a terminated voice");
	}
	ReverbFixture reset;
	ReverbControl param;
	param.wet = 2.0f;
	param.dry = 3.0f;
	reset.Control(param);
	reset.Setup();
	reset.source.Queue(&impulse, 1, 0);
	const auto& defaults = reset.Render(4096);
	Check(std::ranges::all_of(defaults, [](float v) { return v == 0.0f; }),
	      "reverb setup did not restore the silent default preset");

	auto tail = [&](float ratio) {
		ReverbFixture f;
		ReverbControl param;
		param.reflections = -10000;
		param.reverb      = 0;
		param.hf_ratio    = ratio;
		f.Control(param);
		f.source.Queue(&impulse, 1, 0);
		return f.Render(8192);
	};
	const auto damped = tail(0.01f), brighter = tail(0.1f);
	float      difference = 0.0f;
	for (size_t i = 0; i < damped.size(); ++i) {
		Check(std::isfinite(damped[i]), "minimum HF decay ratio produced nonfinite audio");
		difference = std::max(difference, std::abs(damped[i] - brighter[i]));
	}
	Check(difference > 0.001f, "valid PS5 HF decay ratios below 0.1 were clamped away");
}

void TestReverbDryLevel() {
	Fixture          f(48000);
	Ngs2RackInternal reverb_rack {};
	reverb_rack.ngs  = &f.system;
	reverb_rack.type = Ngs2RackType::Reverb;
	Ngs2VoiceInternal reverb;
	reverb.rack = &reverb_rack;
	f.voice.ports.push_back({&reverb, 0, 1.0f, -1});
	const int16_t pcm = 8192;
	f.Queue(&pcm, 1, 0);
	struct {
		Ngs2VoiceParamHeader header {24, 24, 0x20010000};
		uint32_t             channels[4] = {1, 1, 0, 0};
		Ngs2VoiceParamHeader levels_header {96, 0, 0x20010001};
		float                levels[22] = {0.0f, 0.4f};
	} setup;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&reverb), &setup.header);
	reverb.SetEvent(1);
	Ngs2RenderVoice(reverb, {&f.voice, &reverb}, 1);
	Check(reverb.has_samples && reverb.samples[0] == 0.25f * 0.4f,
	      "reverb did not pass its dry signal");
}

// Geometry flags: source cone=1, Doppler=2, volume matrix=4, Audio3d attributes=8.
void TestGeomStereoPlacement() {
	Ngs2GeomListenerParam listener;
	Ngs2GeomListenerWork  work;
	Ngs2GeomResetListenerParam(&listener);
	Check(listener.orient_front.z == -1.0f && listener.sound_speed == 340.29f,
	      "geometry listener defaults are incorrect");

	auto place = [&](float x, float z, uint32_t format = 2) {
		Ngs2GeomSourceParam source;
		Ngs2GeomResetSourceParam(&source);
		source.matrix_format = format;
		source.position      = {x, 0.0f, z};
		Ngs2GeomAttribute attrib;
		Check(Ngs2GeomApply(&work, &source, &attrib, 4) == OK, "geom apply failed");
		for (uint32_t i = format; i < 64; ++i) {
			Check(attrib.level[i] == 0.0f, "geometry emitted more than one input row");
		}
		return attrib;
	};
	for (uint32_t flags: {0u, 1u}) {
		// No flag means right-handed; bit 0 explicitly selects left-handed coordinates.
		listener.orient_front.z = flags == 0 ? -1.0f : 1.0f;
		Check(Ngs2GeomCalcListener(&listener, &work, flags) == OK, "geom listener failed");
		auto attrib = place(0.0f, listener.orient_front.z);
		Check(std::abs(attrib.level[0] - attrib.level[1]) < 0.001f && attrib.level[0] > 0.5f,
		      "source ahead should be centred");
		attrib = place(1.0f, 0.0f);
		Check(attrib.level[1] > 0.99f && std::abs(attrib.level[0]) < 0.001f,
		      "source on the right should pan right in either coordinate system");
		attrib = place(-1.0f, 0.0f);
		Check(attrib.level[0] > 0.99f && std::abs(attrib.level[1]) < 0.001f,
		      "source on the left should pan left in either coordinate system");
		Check(place(1.0f, 0.0f, 1).level[0] == 1.0f && place(-1.0f, 0.0f, 1).level[0] == 1.0f,
		      "mono geometry output depends on stereo pan direction");
		const auto near = place(2.0f, 0.0f);
		Check(place(20.0f, 0.0f).level[1] < near.level[1], "a distant source should be quieter");
	}
	listener.position     = {10.0f, 0.0f, 0.0f};
	listener.orient_front = {1.0f, 0.0f, 0.0f};
	Check(Ngs2GeomCalcListener(&listener, &work, 0) == OK, "rotated listener failed");
	Check(place(10.0f, 1.0f).level[1] > 0.99f, "listener position or rotation was ignored");
}

void TestGeomRolloffAndFlags() {
	Ngs2GeomListenerParam listener;
	Ngs2GeomListenerWork  work;
	Ngs2GeomResetListenerParam(&listener);
	Check(Ngs2GeomCalcListener(&listener, &work, 0) == OK, "geom listener failed");
	Ngs2GeomSourceParam source;
	Ngs2GeomResetSourceParam(&source);
	Ngs2GeomAttribute attrib;
	// The six models are inverse, linear, exponential, then their clamped variants.
	const float at_five[3]   = {1.0f / 3.0f, 7.0f / 9.0f, std::sqrt(0.2f)};
	const float at_twenty[6] = {2.0f / 21.0f, 0.0f, std::sqrt(0.05f),
	                            2.0f / 11.0f, 0.5f, std::sqrt(0.1f)};
	for (uint32_t model = 0; model < 6; ++model) {
		source.rolloff  = {model, 10.0f, 0.5f, 1.0f};
		source.position = {0.0f, 0.0f, -5.0f};
		Check(Ngs2GeomApply(&work, &source, &attrib, 8) == OK &&
		          std::abs(attrib.a3d_attrib.volume - at_five[model % 3]) < 1e-6f,
		      "geometry selected the wrong rolloff equation");
		source.position.z = -20.0f;
		Check(Ngs2GeomApply(&work, &source, &attrib, 8) == OK &&
		          std::abs(attrib.a3d_attrib.volume - at_twenty[model]) < 1e-6f,
		      "geometry confused clamped and unclamped rolloff models");
	}

	source.rolloff.rolloff_factor = 0.0f;
	source.position               = {0.0f, 0.0f, -1.0f};
	source.direction              = {0.0f, 0.0f, -1.0f};
	source.cone                   = {1.0f, 30.0f, 0.0f, 60.0f};
	source.velocity               = {0.0f, 0.0f, 5.0f};
	Check(Ngs2GeomApply(&work, &source, &attrib, 4) == OK && attrib.pitch_ratio == 1.0f &&
	          attrib.a3d_attrib.volume == 0.0f && attrib.a3d_attrib.position.z == 0.0f &&
	          attrib.level[0] > 0.5f && attrib.level[1] > 0.5f,
	      "matrix-only geometry applied disabled cone, Doppler, or Audio3d flags");
	Check(Ngs2GeomApply(&work, &source, &attrib, 8) == OK && attrib.a3d_attrib.volume == 1.0f,
	      "disabled source cone changed Audio3d volume");
	for (float value: attrib.level) {
		Check(value == 0.0f, "Audio3d-only geometry calculated a volume matrix");
	}
	Check(Ngs2GeomApply(&work, &source, &attrib, 1 | 8) == OK && attrib.a3d_attrib.volume == 0.0f,
	      "enabled source cone did not attenuate a listener behind the source");
	source.cone.outer_angle = 360.0f;
	// acos and the conversion to degrees can round just below the outer cone boundary.
	Check(Ngs2GeomApply(&work, &source, &attrib, 1 | 8) == OK &&
	          std::abs(attrib.a3d_attrib.volume) < 1e-6f,
	      "360-degree outer cone bypassed source directionality");
	source.cone      = {2.0f, 360.0f, 2.0f, 360.0f};
	source.max_level = 0.75f;
	Check(Ngs2GeomApply(&work, &source, &attrib, 1 | 8) == OK && attrib.a3d_attrib.volume == 0.75f,
	      "source cone amplification exceeded the maximum level");
	source.cone      = {-1.0f, 0.0f, 1.0f, 360.0f};
	source.direction = {1.0f, 0.0f, 0.0f};
	source.max_level = 1.0f;
	Check(Ngs2GeomApply(&work, &source, &attrib, 1 | 8) == OK &&
	          std::abs(attrib.a3d_attrib.volume - 0.5f) < 1e-6f,
	      "cone levels were not clamped before interpolation");
	source.position  = {};
	source.rolloff   = {0, 100.0f, 1.0f, 1.0f};
	source.min_level = source.max_level = 0.0f;
	Check(Ngs2GeomApply(&work, &source, &attrib, 8) == OK && attrib.a3d_attrib.volume == 0.0f,
	      "a muted colocated source produced a non-finite volume");
}

void TestGeomDoppler() {
	Ngs2GeomListenerParam listener;
	Ngs2GeomListenerWork  work;
	Ngs2GeomResetListenerParam(&listener);
	Check(Ngs2GeomCalcListener(&listener, &work, 0) == OK, "geom listener failed");

	auto pitch = [&](float source_vz, float listener_vz, uint32_t flags = 2,
	                 float doppler_factor = 1.0f) {
		Ngs2GeomListenerWork moving = work;
		moving.velocity             = {0.0f, 0.0f, listener_vz};
		Ngs2GeomSourceParam source;
		Ngs2GeomResetSourceParam(&source);
		source.position       = {0.0f, 0.0f, 10.0f};
		source.velocity       = {0.0f, 0.0f, source_vz};
		source.doppler_factor = doppler_factor;
		Ngs2GeomAttribute attrib;
		Check(Ngs2GeomApply(&moving, &source, &attrib, flags) == OK, "geom apply failed");
		return attrib.pitch_ratio;
	};
	Check(pitch(-5.0f, 0.0f) > 1.0f, "approaching source should raise pitch");
	Check(pitch(5.0f, 0.0f) < 1.0f, "receding source should lower pitch");
	Check(pitch(0.0f, 5.0f) > 1.0f, "approaching listener should raise pitch");
	Check(pitch(0.0f, -5.0f) < 1.0f, "receding listener should lower pitch");
	Check(pitch(-5.0f, 5.0f, 0) == 1.0f, "disabled Doppler changed pitch");
	Check(pitch(-1000.0f, 0.0f) == 4.0f, "Doppler exceeded the pitch ratio limit");
	Check(std::abs(pitch(-0.5f, 0.0f, 2, work.sound_speed) - 2.0f) < 1e-6f,
	      "a large Doppler factor incorrectly capped subsonic velocity");
}

void TestReverbTail() {
	Ngs2ReverbState reverb;
	reverb.params = {0.6f, 0.4f, -1000, -602, -302, 2.91f, 1.3f, 0.015f, 0.022f};
	reverb.Prepare(2, 48000);
	const uint32_t     grain = 256;
	std::vector<float> block(2 * grain, 0.0f);
	Check(!reverb.Process(block, grain, false), "idle reverb produced output");

	block[0]     = 1.0f;
	block[grain] = 1.0f;
	Check(reverb.Process(block, grain, true), "reverb dropped its input");
	Check(std::abs(block[0] - 0.4f) < 1e-6f, "reverb changed the dry level");

	float early = 0.0f, late = 0.0f;
	for (int g = 1; g < 1400; ++g) {
		std::ranges::fill(block, 0.0f);
		const bool out  = reverb.Process(block, grain, false);
		float      peak = 0.0f;
		for (float v: block) {
			Check(std::isfinite(v) && std::abs(v) < 1.0f, "reverb tail is unstable");
			peak = std::max(peak, std::abs(v));
		}
		if (g == 20) {
			early = peak;
		}
		if (g == 400) {
			late = peak;
		}
		if (!out) {
			Check(g > 600, "reverb tail ended too early");
			Check(late > 0.0f && late < early, "reverb tail did not decay");
			return;
		}
	}
	Check(false, "reverb tail never ended");
}

void TestCustomMasteringChannelMixer() {
	Ngs2Internal system;
	system.option = Ngs2DefaultSystemOption();
	system.option.num_grain_samples = 6;
	Ngs2RackOptionUnion sampler_option {};
	Ngs2FillDefaultRackOption(0x1000, &sampler_option);
	sampler_option.common.max_voices = 1;
	sampler_option.common.max_ports = 1;
	Ngs2CustomMasteringRackOption master_option {};
	auto& custom = master_option.custom_rack_option;
	custom.rack_option.size = sizeof(master_option);
	custom.rack_option.max_voices = 1;
	custom.rack_option.max_voice_channels = 6;
	custom.num_buffers = 1;
	custom.num_modules = 1;
	custom.module[0].module_id = 0x17;
	RenderRack sampler(system, 0x1000, sampler_option.common);
	RenderRack master(system, 0x4003, custom.rack_option);
	auto& source = sampler.Voice();
	auto& output = master.Voice();
	output.channels = 6;
	output.SetEvent(1);
	struct {
		Ngs2VoiceParamHeader header {5484, 0, 0x40001700};
		float levels[37][37] {};
	} matrix;
	constexpr float center = 0.70710677f;
	matrix.levels[0][0] = matrix.levels[1][1] = 1.0f;
	matrix.levels[0][2] = matrix.levels[1][2] = center;
	matrix.levels[0][4] = matrix.levels[1][5] = center;
	int16_t impulses[6][6] {};
	for (uint32_t c = 0; c < 6; ++c) impulses[c][c] = 8192;
	for (unsigned pass = 0; pass < 2; ++pass) {
		if (pass == 1) {
			std::memset(matrix.levels, 0, sizeof(matrix.levels));
			matrix.levels[0][1] = 64.0f;
			matrix.levels[1][0] = -64.0f;
		}
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&output), &matrix.header) == OK,
		      "custom channel mixer control failed");
		source.SetupSampler({0x12, 6, 48000});
		source.blocks.push_back({reinterpret_cast<const uint8_t*>(impulses),
		                         {0, sizeof(impulses), 0, 0, 6}});
		source.ports[0].dest = &output;
		source.SetEvent(1);
		std::array<float, 14> pcm;
		pcm.fill(99.0f);
		Ngs2RenderBufferInfo buffer {pcm.data() + 1, 12 * sizeof(float), 0x18, 2};
		Check(Ngs2SystemRender(reinterpret_cast<uintptr_t>(&system), &buffer, 1) == OK,
		      "custom mastering rejected stereo output");
		const std::array<float, 12> expected = pass == 0
		    ? std::array<float, 12> {0.25f, 0, 0, 0.25f, center / 4, center / 4,
		                             0, 0, center / 4, 0, 0, center / 4}
		    : std::array<float, 12> {0, -8, 8, 0};
		Check(pcm.front() == 99 && pcm.back() == 99 &&
		          std::equal(expected.begin(), expected.end(), pcm.begin() + 1),
		      "custom mixer lost channel routing, in-place input, clamping or output bounds");
	}
}

void TestCustomVoiceStateCapacity() {
	for (auto type: {Ngs2RackType::CustomSampler, Ngs2RackType::CustomMastering}) {
		Fixture f(48000, 1, Ngs2RackType::CustomSampler);
		f.rack.type = type;
		f.rack.option.custom_sampler.custom_rack_option.state_size = 212;
		f.voice.state_flags = 3;
		f.voice.decoded_samples = 37;
		f.voice.decoded_bytes = 128;
		alignas(Ngs2CustomSamplerVoiceState) std::array<uint8_t, 384> storage;
		auto* state = reinterpret_cast<Ngs2VoiceState*>(storage.data() + 8);
		const auto handle = reinterpret_cast<uintptr_t>(&f.voice);
		for (size_t capacity: {212u, 368u}) {
			storage.fill(0xa5);
			Check(Ngs2VoiceGetState(handle, state, capacity) == OK && state->state_flags == 3 &&
			          state->error_code == 0 &&
			          std::all_of(storage.begin(), storage.begin() + 8,
			                      [](uint8_t v) { return v == 0xa5; }) &&
			          std::all_of(storage.begin() + 220, storage.end(),
			                      [](uint8_t v) { return v == 0xa5; }),
			      "oversized custom state buffer was rejected or overwritten past configured state");
			if (type == Ngs2RackType::CustomSampler) {
				const auto* sampler = reinterpret_cast<const Ngs2CustomSamplerVoiceState*>(state);
				Check(sampler->num_decoded_samples == 37 && sampler->decoded_data_size == 128,
				      "custom sampler state omitted playback progress");
			}
			const size_t base_size = type == Ngs2RackType::CustomSampler
			    ? sizeof(Ngs2CustomSamplerVoiceState) : sizeof(Ngs2CustomMasteringVoiceState);
			Check(std::all_of(storage.begin() + 8 + base_size, storage.begin() + 220,
			                  [](uint8_t v) { return v == 0; }),
			      "configured custom state extension was not initialized");
		}
		storage.fill(0xa5);
		Check(Ngs2VoiceGetState(handle, state, 211) == static_cast<int32_t>(0x804a8011u) &&
		          std::ranges::all_of(storage, [](uint8_t v) { return v == 0xa5; }),
		      "undersized custom state buffer was accepted or modified");
	}
}

void TestPitchLoopAndSkip() {
	Fixture       f(24000);
	const int16_t pcm[] = {-30000, 8192, 16384};
	f.Queue(pcm, 2, 0, 1, 1);
	struct Pitch {
		Ngs2VoiceParamHeader header {16, 0, 0x10000005};
		float                ratio    = 2.0f;
		uint32_t             reserved = 0;
	} pitch;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &pitch.header);
	f.Render(5);
	const float expected[] = {0.25f, 0.5f, 0.25f, 0.5f, 0.0f};
	for (size_t i = 0; i < 5; ++i) {
		Check(std::abs(f.voice.samples[i] - expected[i]) < 0.00001f,
		      "pitch, repeat count, or skipped samples incorrect");
	}
}

void TestPitchClamps() {
	Fixture       f(48000);
	const int16_t pcm[] = {8192, 0, 0, 0, -8192};
	f.Queue(pcm, 5, 0);
	struct Pitch {
		Ngs2VoiceParamHeader header {16, 0, 0x10000005};
		float                ratio    = 5.0f;
		uint32_t             reserved = 0;
	} pitch;
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &pitch.header) == OK,
	      "out-of-range pitch was rejected");
	f.Render(2);
	Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == -0.25f,
	      "pitch was not clamped to four times playback speed");
}

void TestStarvationAndPause() {
	Fixture       f(48000);
	const int16_t pcm[] = {16384, 8192};
	f.Queue(pcm, 2, 1);
	f.Render(4);
	Check(f.voice.state == Ngs2VoicePlayState::Playing, "stream starvation stopped voice");
	Check(f.voice.samples[2] == 0.0f, "starved output was not silent");
	f.Queue(pcm, 2, 0);
	f.voice.SetEvent(16);
	f.Render(2);
	Check(!f.voice.has_samples && f.voice.blocks.front().data_cursor == 0,
	      "paused voice consumed data");
	f.voice.SetEvent(32);
	f.Render(2);
	Check(f.voice.samples[0] == 0.5f && f.voice.samples[1] == 0.25f,
	      "stream failed to resume after starvation/pause");
}

void TestPcmByteUnitStreaming() {
	Fixture                 f(48000);
	const int16_t           pcm[] = {0, 4096, 8192, 12288, 16384, 20480, 24576, 28672};
	const Ngs2WaveformBlock first {0, 4, 0, 1, 5, 0, 0}, rest {4, 12, 0, 0, 99, 0, 0};
	Check(f.QueueBlocks(pcm, &first, 1, 1) == OK && f.QueueBlocks(pcm, &rest, 1, 0x12) == OK &&
	          f.voice.WaveformData() == reinterpret_cast<const uint8_t*>(pcm + 1),
	      "byte-unit stream was rejected or its read address ignored the skipped frame");
	f.Render(8);
	Check(f.voice.samples == std::vector<float> {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0, 0, 0} &&
	          f.voice.blocks.empty() && f.voice.decoded_samples == 5,
	      "byte-unit stream did not play exactly its duration across data-only blocks");
}

void TestPcmSkipAcrossBlocks() {
	Fixture                 f(48000);
	const int16_t           pcm[] = {1, 2, 3, 8192, 16384};
	const Ngs2WaveformBlock first {0, 4, 0, 3, 2, 0, 0}, rest {4, 6, 0, 0, 0, 0, 0};
	Check(f.QueueBlocks(pcm, &first, 1, 1) == OK, "skipping stream block was rejected");
	f.Render(1);
	Check(!f.voice.has_samples && f.voice.blocks.empty(), "skip-only block produced samples");
	Check(f.QueueBlocks(pcm, &rest, 1, 2) == OK &&
	          f.voice.WaveformData() == reinterpret_cast<const uint8_t*>(pcm + 3),
	      "read address ignored the skip remaining from the previous block");
	f.Render(2);
	Check(f.voice.samples == std::vector<float> {0.25f, 0.5f} && f.voice.decoded_samples == 2 &&
	          f.voice.decoded_bytes == sizeof(pcm),
	      "skip spanning blocks played skipped frames or miscounted decoded data");
}

void TestPcmBlocksEndWithTheirData() {
	// The Last Oricru queues blocks holding fewer frames than they declare, even none.
	Fixture                 f(48000, 2);
	const int16_t           pcm[] = {4096, -4096, 8192, -8192, 12288, -12288};
	const Ngs2WaveformBlock blocks[] = {
	    {0, 0, 0, 0, 16385, 0, 0}, {0, 8, 0, 0, 4, 0, 0}, {8, 4, 0, 0, 3, 0, 0}};
	Check(f.QueueBlocks(pcm, blocks, 3, 0) == OK, "partially filled blocks were rejected");
	f.Render(5);
	Check(f.voice.samples == std::vector<float> {0.125f, 0.25f, 0.375f, 0, 0, -0.125f, -0.25f,
	                                             -0.375f, 0, 0} &&
	          f.voice.blocks.empty() && f.voice.decoded_samples == 3,
	      "partially filled blocks did not play their data back to back");
}

void TestMonoRateAndRouting() {
	Fixture              f(22050);
	std::vector<int16_t> pcm(441, 16384);
	f.Queue(pcm.data(), 441, 0);
	Ngs2RackInternal mastering_rack {};
	mastering_rack.ngs  = &f.system;
	mastering_rack.type = Ngs2RackType::Mastering;
	Ngs2VoiceInternal mastering;
	mastering.rack     = &mastering_rack;
	mastering.channels = 2;
	mastering.state    = Ngs2VoicePlayState::Playing;
	f.voice.matrices   = {{0.5f, 0.25f}};
	f.voice.ports.push_back({&mastering, 0, 1.0f, 0});
	for (int i = 0; i < 3; ++i) {
		mastering.rendered = false;
		f.voice.rendered   = false;
		Ngs2RenderVoice(mastering, {&f.voice, &mastering}, 320);
		Check(mastering.has_samples, "standard sampler did not reach mastering voice");
		for (uint32_t j = 0; j < 320; ++j) {
			Check(mastering.samples[j] == 0.25f && mastering.samples[320 + j] == 0.125f,
			      "mono PCM routing matrix was not applied");
		}
	}
	Check(f.voice.decoded_samples == 441 && f.voice.blocks.empty(),
	      "22.05 kHz effect duration was incorrect");
}

void TestDefaultMatrix() {
	struct Case {
		uint32_t           src_channels;
		int32_t            matrix;
		uint32_t           dst_channels;
		std::vector<float> expected;
	};
	constexpr float minus_3db = std::numbers::sqrt2_v<float> / 2;
	const int16_t   pcm[]     = {8192, -16384};
	for (const auto& c: {Case {1, -1, 2, {0.25f * minus_3db, 0.25f * minus_3db}},
	                     Case {1, 0, 2, {0.25f * minus_3db, 0.25f * minus_3db}},
	                     Case {2, -1, 1, {0.25f * minus_3db + -0.5f * minus_3db}},
	                     Case {1, -1, 6, {0, 0, 0.25f, 0, 0, 0}}}) {
		Fixture          f(48000, c.src_channels);
		Ngs2RackInternal rack {};
		rack.ngs  = &f.system;
		rack.type = Ngs2RackType::Mastering;
		Ngs2VoiceInternal dest;
		dest.rack     = &rack;
		dest.channels = c.dst_channels;
		dest.state    = Ngs2VoicePlayState::Playing;
		f.voice.matrices.resize(1);
		f.voice.ports.push_back({&dest, 0, 1.0f, c.matrix});
		f.Queue(pcm, 1, 0);
		Ngs2RenderVoice(dest, {&f.voice, &dest}, 1);
		Check(dest.samples == c.expected, "default port mix produced the wrong channel levels");
	}
}

void TestMatrixOutputChannels() {
	Fixture f(48000, 2);
	Ngs2RackInternal mastering_rack {};
	mastering_rack.ngs  = &f.system;
	mastering_rack.type = Ngs2RackType::Mastering;
	Ngs2VoiceInternal mastering;
	mastering.rack     = &mastering_rack;
	mastering.channels = 8;
	mastering.state    = Ngs2VoicePlayState::Playing;
	f.voice.matrices   = {{1.0f, 0.0f, 0.0f, 1.0f}};
	f.voice.ports.push_back({&mastering, 0, 1.0f, 0});
	const int16_t pcm[] = {8192, -8192};
	f.Queue(pcm, 1, 0);
	Ngs2RenderVoice(mastering, {&f.voice, &mastering}, 1);
	Check(mastering.samples[0] == 0.25f && mastering.samples[1] == -0.25f,
	      "stereo matrix did not route its output channels");
	for (size_t channel = 2; channel < mastering.channels; ++channel) {
		Check(mastering.samples[channel] == 0.0f,
		      "matrix routed audio to an unspecified output channel");
	}
}

void TestCustomPcmStillPlays() {
	Fixture       f(48000, 1, Ngs2RackType::CustomSampler);
	const int16_t pcm[] = {8192, -16384};
	f.Queue(pcm, 2, 0);
	f.Render(4);
	Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == -0.5f &&
	          f.voice.samples[2] == 0.0f && f.voice.blocks.empty(),
	      "custom PCM sampler regressed");
}

void TestSamplerReuseResetsRouting() {
	for (auto type: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		Fixture          f(48000, 1, type);
		Ngs2RackInternal mastering_rack {};
		mastering_rack.ngs  = &f.system;
		mastering_rack.type = Ngs2RackType::Mastering;
		Ngs2VoiceInternal mastering;
		mastering.rack     = &mastering_rack;
		mastering.channels = 2;
		mastering.state    = Ngs2VoicePlayState::Playing;
		f.voice.ports.push_back({&mastering, 1, 0.25f, 0});
		f.voice.matrices = {{0.5f, 0.25f}};
		f.voice.SetupSampler({0x12, 2, 48000});
		Check(f.voice.ports.size() == 1 && f.voice.ports[0].dest == nullptr &&
		          f.voice.ports[0].input == 0 && f.voice.ports[0].volume == 1.0f &&
		          f.voice.ports[0].matrix == -1 && f.voice.matrices.size() == 1 &&
		          f.voice.matrices[0].empty(),
		      "sampler setup retained routing from the previous channel layout");
		const int16_t pcm[] = {8192, 16384, -8192, -16384};
		f.Queue(pcm, 2, 0);
		f.Render(1);
		Check(!f.voice.has_samples && f.voice.blocks.front().data_cursor == 0,
		      "sampler setup started the new waveform without a play event");
		f.voice.ports[0].dest = &mastering;
		f.voice.SetEvent(1);
		f.voice.rendered = false;
		Ngs2RenderVoice(mastering, {&f.voice, &mastering}, 2);
		Check(mastering.samples == std::vector<float> {0.25f, -0.25f, 0.5f, -0.5f},
		      "reused stereo sampler did not restore default channel routing");
	}
}

void TestDeferredSamplerSetup() {
	Fixture f(48000);
	struct Setup {
		Ngs2VoiceParamHeader header {40, 0, 0x10000000};
		Ngs2WaveformFormat   format;
		uint32_t             flags = 0, reserved = 0;
	} setup;
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &setup.header) == OK,
	      "deferred stream setup failed");
	Check(f.voice.state == Ngs2VoicePlayState::Empty && f.voice.channels == 0 &&
	          f.voice.sample_rate == 0 && f.voice.decoder == nullptr,
	      "deferred stream setup did not clear the sampler");
	setup.format = {0x12, 1, 48000};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &setup.header) == OK,
	      "sampler setup after deferred stream failed");
	const int16_t pcm = 8192;
	f.Queue(&pcm, 1, 0);
	f.voice.SetEvent(1);
	f.Render(1);
	Check(f.voice.samples[0] == 0.25f,
	      "sampler did not play after deferred stream setup");
}

void TestClearReopensClosedList() {
	Fixture       f(48000);
	const int16_t first = 8192, second = -16384;
	f.Queue(&first, 1, 0);
	f.Queue(&second, 1, 4);
	f.Render(1);
	Check(f.voice.samples[0] == -0.5f, "CLEAR did not reopen a closed waveform list");
}

std::vector<uint32_t> g_block_attributes;

void KYTY_SYSV_ABI RecordBlockAttributes(const Ngs2VoiceCallbackInfo* info) {
	g_block_attributes.push_back(info->attributes);
}

void TestCancelSkipsBlocksNotStarted() {
	Fixture f(48000);
	f.system.option.num_grain_samples = 1;
	f.voice.callback       = reinterpret_cast<uintptr_t>(&RecordBlockAttributes);
	f.voice.callback_flags = 1;
	const int16_t pcm[] = {8192, 16384, -8192, -16384};
	f.Queue(pcm, 2, 1, 1);
	f.Queue(pcm + 2, 1, 1);
	f.Render(1);
	// Cancel during the first pass of the looping block.
	f.Queue(pcm + 3, 1, 9);
	f.Render(4);
	Check(f.voice.samples == std::vector<float> {0.5f, 0.25f, 0.5f, -0.5f},
	      "CANCEL did not keep the playing block and skip the queued one");
	Check(g_block_attributes == std::vector<uint32_t> {1, 1, 0},
	      "block callbacks did not report the canceled blocks");
}

void TestSetupQuery() {
	Fixture  f(48000, 2);
	uint32_t info[2] = {99, 99};
	Ngs2VoiceQueryInfo(reinterpret_cast<uintptr_t>(&f.voice), 0x4001, info, sizeof(info));
	Check(info[0] == 2 && info[1] == 0, "setup query did not report the voice channels");
}

void TestPortInfo() {
	Fixture           f(48000);
	Ngs2VoiceInternal dest;
	f.voice.ports.push_back({&dest, 1, 0.5f, 0});
	struct {
		int32_t   matrix;
		float     volume;
		uint32_t  delay_samples, input;
		uintptr_t dest;
	} info {99, 99, 99, 99, 99};
	Ngs2VoiceGetPortInfo(reinterpret_cast<uintptr_t>(&f.voice), 0, &info, sizeof(info));
	Check(info.matrix == 0 && info.volume == 0.5f && info.delay_samples == 0 && info.input == 1 &&
	          info.dest == reinterpret_cast<uintptr_t>(&dest),
	      "port info did not report the port routing and volume");
}

void TestPlayStateBeforeRender() {
	Fixture       f(48000);
	const int16_t pcm[] = {8192, -16384};
	f.voice.state       = Ngs2VoicePlayState::Empty;
	f.Queue(pcm, 2, 0);
	Ngs2VoiceEventParam event {{sizeof(Ngs2VoiceEventParam), 0, 6}, 1};
	const auto handle = reinterpret_cast<uintptr_t>(&f.voice);
	Ngs2VoiceControl(handle, &event.header);
	uint32_t flags = 0;
	Ngs2VoiceGetStateFlags(handle, &flags);
	Check(flags == 1, "play must reserve the voice before rendering starts playback");
	Ngs2SamplerVoiceState state {};
	Ngs2VoiceGetState(handle, &state.voice_state, sizeof(state));
	Check(state.voice_state.state_flags == flags && state.num_decoded_samples == 0,
	      "state queries disagreed or play consumed samples before render");
	f.Render(4);
	Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == -0.5f,
	      "one-shot effect was dropped before its first render");
}

void TestStatePublication() {
	Ngs2Internal system;
	system.option = Ngs2DefaultSystemOption();
	Ngs2RackOptionUnion option {};
	Ngs2FillDefaultRackOption(0x1000, &option);
	option.common.max_voices = 1;
	alignas(Ngs2RackInternal) alignas(Ngs2VoiceInternal)
	    std::byte         storage[sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal)];
	Ngs2ContextBufferInfo buffer {storage, sizeof(storage)};
	uintptr_t             rack_handle   = 0;
	const auto            system_handle = reinterpret_cast<uintptr_t>(&system);
	Check(Ngs2RackCreate(system_handle, 0x1000, &option.common, &buffer, &rack_handle) == OK,
	      "state publication rack creation failed");
	auto& voice =
	    *reinterpret_cast<Ngs2VoiceInternal*>(reinterpret_cast<Ngs2RackInternal*>(rack_handle) + 1);
	voice.SetupSampler({0x12, 1, 48000});
	const auto handle      = reinterpret_cast<uintptr_t>(&voice);
	auto       check_flags = [&](uint32_t expected) {
		uint32_t              flags = 0;
		Ngs2SamplerVoiceState state {};
		Ngs2VoiceGetStateFlags(handle, &flags);
		Ngs2VoiceGetState(handle, &state.voice_state, sizeof(state));
		Check(flags == expected && state.voice_state.state_flags == expected,
		      "voice state was published at the wrong render boundary");
	};
	struct Transition {
		uint32_t event, before_render, after_render;
	};
	const Transition     transitions[] = {{1, 1, 3}, {16, 3, 5}, {32, 5, 3}, {2, 3, 0},
	                                      {1, 1, 3}, {16, 3, 5}, {2, 5, 0},  {1, 1, 3},
	                                      {4, 3, 0}, {1, 1, 3},  {8, 3, 0}};
	Ngs2RenderBufferInfo output {};
	for (const auto& transition: transitions) {
		Ngs2VoiceEventParam event {{sizeof(Ngs2VoiceEventParam), 0, 6}, transition.event};
		Ngs2VoiceControl(handle, &event.header);
		check_flags(transition.before_render);
		Ngs2SystemRender(system_handle, &output, 1);
		check_flags(transition.after_render);
	}
	Check(Ngs2RackDestroy(rack_handle, nullptr) == OK, "state publication rack cleanup failed");
}

void TestStopThenPlayBeforeRender() {
	Fixture       f(48000);
	const int16_t pcm[] = {16384, -8192};
	f.Queue(pcm, 2, 0);
	f.voice.SetEvent(2);
	f.voice.SetEvent(1);
	Check(Ngs2GetStateFlags(&f.voice) == 1, "stopped voice could not restart");
	f.voice.SetEvent(8);
	f.voice.SetEvent(1);
	Check(Ngs2GetStateFlags(&f.voice) == 1, "kill/play commands lost their ordering");
	f.Render(2);
	Check(f.voice.samples[0] == 0.5f && f.voice.samples[1] == -0.25f,
	      "restarted voice produced no audio");
}

void SetLowPass(Fixture& f, float frequency = 1000.0f, uint64_t mask = 0) {
	struct Param {
		Ngs2VoiceParamHeader   header {56, 0, 0x1000000a};
		Ngs2SamplerFilterParam filter {0, 1, 1, 0, 1000.0f, 0.70710678f, 1.0f, {}};
	} param;
	param.filter.frequency            = frequency;
	param.filter.channel_mask         = mask;
	f.rack.option.sampler.max_filters = 8;
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &param.header) == OK,
	      "filter ABI command failed");
}

void TestLowPassResponse() {
	// A second-order Butterworth LPF has unity DC gain and -3 dB at cutoff.
	for (double frequency: {100.0, 1000.0, 10000.0}) {
		Fixture f(48000);
		SetLowPass(f);
		std::vector<int16_t> pcm(48000);
		for (size_t i = 0; i < pcm.size(); ++i) {
			pcm[i] =
			    static_cast<int16_t>(8192 * std::sin((2.0 * std::numbers::pi) * frequency * i / 48000));
		}
		f.Queue(pcm.data(), static_cast<uint32_t>(pcm.size()), 0);
		f.Render(static_cast<uint32_t>(pcm.size()));
		double input = 0, output = 0;
		for (size_t i = 24000; i < pcm.size(); ++i) {
			input += std::pow(pcm[i] / 32768.0, 2);
			output += std::pow(f.voice.samples[i], 2);
		}
		const double ratio = std::sqrt(output / input);
		if (frequency == 100) {
			Check(std::abs(ratio - 1.0) < 0.002, "low-pass lost bass");
		}
		if (frequency == 1000) {
			Check(std::abs(ratio - std::sqrt(0.5)) < 0.002, "cutoff gain incorrect");
		}
		if (frequency == 10000) {
			Check(ratio < 0.01, "low-pass did not reject high frequencies");
		}
	}
}

void TestFilterHistoryAndChannels() {
	Fixture whole(48000, 2), split(48000, 2);
	SetLowPass(whole, 1000, 1);
	SetLowPass(split, 1000, 1);
	std::vector<int16_t> pcm(256 * 2, 0);
	pcm[0] = pcm[1] = 16384;
	whole.Queue(pcm.data(), 256, 0);
	split.Queue(pcm.data(), 32, 1);
	split.Queue(pcm.data() + 64, 224, 0);
	whole.Render(256);
	for (uint32_t g = 0; g < 8; ++g) {
		// Games resend unchanged parameters; this must not reset the delay line.
		SetLowPass(split, 1000, 1);
		split.Render(32);
		for (uint32_t c = 0; c < 2; ++c) {
			for (uint32_t i = 0; i < 32; ++i) {
				Check(std::abs(split.voice.samples[c * 32 + i] -
				               whole.voice.samples[c * 256 + g * 32 + i]) < 1e-7,
				      "filter history changed at a grain/block/control boundary");
			}
		}
	}
	Check(whole.voice.samples[0] == 0.5f && whole.voice.samples[1] == 0.0f &&
	          whole.voice.samples[256] > 0.0f && whole.voice.samples[256] < 0.5f,
	      "channel mask must bypass selected channels and filter the others");
	Fixture stereo(48000, 2);
	SetLowPass(stereo);
	pcm[1] = 0;
	stereo.Queue(pcm.data(), 256, 0);
	stereo.Render(256);
	for (size_t i = 256; i < 512; ++i) {
		Check(stereo.voice.samples[i] == 0.0f, "filter leaked history between channels");
	}
	stereo.voice.SetupSampler({0x12, 2, 48000});
	Check(stereo.voice.filters.empty(), "reused voice retained old filters");
}

void TestFilterUsesOutputRate() {
	Fixture f(22050);
	SetLowPass(f, 6833.0f);
	const auto& filter = f.voice.filters[0];
	// Rear filtering operates at the 48 kHz system rate, not the PCM source rate.
	const double w        = (2.0 * std::numbers::pi) * 6833 / 48000;
	const double expected = (1 - std::cos(w)) / (2 * (1 + std::sin(w) / (2 * double(0.70710678f))));
	Check(std::abs(filter.b0 - expected) < 1e-10, "rear filter used source sample rate");
	SetLowPass(f, 48000);
	const int16_t pcm[] = {16384, 16384};
	f.Queue(pcm, 2, 0);
	f.Render(2);
	Check(f.voice.samples[0] == 0.5f && f.voice.samples[1] == 0.5f,
	      "above-Nyquist cutoff was unstable");
}

void TestFilterControlLayout() {
	Fixture f(48000, 2);
	f.rack.option.sampler.max_filters = 8;
	// A 56-byte control command with nonzero trailing padding.
	alignas(8) uint32_t words[] = {56, 0x1000000a, 0, 1, 1, 0, 0,
	                               0,  0x45d58800, 0x3fd55555, 0x3f800000, 0, 0, 0x13f};
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice),
	                 reinterpret_cast<const Ngs2VoiceParamHeader*>(words));
	Check(f.voice.filters.size() == 1 && f.voice.filters[0].enabled,
	      "FCQ filter command was not recognized");
	const double omega    = (2.0 * std::numbers::pi) * 6833.0 / 48000.0;
	const double q        = static_cast<double>(std::bit_cast<float>(0x3fd55555u));
	const double expected = (1 - std::cos(omega)) / (2 * (1 + std::sin(omega) / (2 * q)));
	Check(std::abs(f.voice.filters[0].b0 - expected) < 1e-10,
	      "FCQ fields were read at the wrong offsets");
	words[6] = 1; // Bypass channel 0 in the low half of the 64-bit mask.
	words[7] = 1; // Bypass channel 32 in the high half.
	Fixture wide(48000, 33);
	wide.rack.option.sampler.max_filters = 8;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&wide.voice),
	                 reinterpret_cast<const Ngs2VoiceParamHeader*>(words));
	const std::vector<int16_t> pcm(33, 16384);
	wide.Queue(pcm.data(), 1, 1);
	wide.Render(1);
	Check(wide.voice.samples[0] == 0.5f && wide.voice.samples[32] == 0.5f &&
	          std::abs(wide.voice.samples[1] - expected * 0.5) < 1e-7,
	      "channel mask was not decoded as a 64-bit bypass mask");
	words[4] = 0; // OFF ignores unused FCQ values and clears the old delay line.
	words[8] = 0x7fc00000;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&wide.voice),
	                 reinterpret_cast<const Ngs2VoiceParamHeader*>(words));
	Check(!wide.voice.filters[0].enabled && wide.voice.filters[0].history.empty() &&
	          !wide.voice.filters[0].warned,
	      "filter OFF was treated as an unsupported FCQ configuration");
	wide.Queue(pcm.data(), 1, 1);
	wide.Render(1);
	Check(wide.voice.samples[1] == 0.5f, "filter OFF did not bypass audio");
	SetLowPass(wide, 0);
	wide.Queue(pcm.data(), 1, 1);
	wide.Render(1);
	Check(wide.voice.samples[1] == 0.0f, "zero-Hz low-pass leaked source audio");
}

struct LoopCallbacks {
	std::vector<std::pair<uint32_t, uint32_t>> events;
	bool                                       pause = false;
};

void KYTY_SYSV_ABI RecordLoopCallback(const Ngs2VoiceCallbackInfo* info) {
	auto& log = *reinterpret_cast<LoopCallbacks*>(info->data);
	log.events.emplace_back(info->flag, info->repeats);
	Check(info->user == 123 && info->attributes == 0, "loop callback metadata changed");
	if (log.pause && info->flag == 2) {
		reinterpret_cast<Ngs2VoiceInternal*>(info->voice)->SetEvent(16);
	}
}

void TestLoopCallbacksAndExit(Ngs2RackType type) {
	const int16_t pcm[] = {8192, 16384, 24576};
	Fixture       f(48000, 1, type);
	LoopCallbacks log;
	f.voice.callback       = reinterpret_cast<uintptr_t>(&RecordLoopCallback);
	f.voice.callback_data  = reinterpret_cast<uintptr_t>(&log);
	f.voice.callback_flags = 3;
	f.Queue(pcm, 2, 0, 2);
	f.Render(6);
	Check(log.events == std::vector<std::pair<uint32_t, uint32_t>> {{2, 1}, {2, 2}, {1, 2}},
	      "loop and completion callbacks lost the completed repeat count");
	Check(f.voice.blocks.empty(), "finite loop did not complete");

	Fixture active(48000, 1, type);
	log.events.clear();
	log.pause                   = true;
	active.voice.callback       = reinterpret_cast<uintptr_t>(&RecordLoopCallback);
	active.voice.callback_data  = reinterpret_cast<uintptr_t>(&log);
	active.voice.callback_flags = 2;
	active.Queue(pcm, 2, 1, UINT32_MAX);
	active.Queue(pcm + 2, 1, 0);
	active.Render(6);
	Check(log.events.size() == 1 && active.voice.decoded_samples == 2 &&
	          active.voice.blocks.front().data_cursor == 0 && active.voice.samples[2] == 0,
	      "loop callback pause consumed another iteration");
	Ngs2VoiceParamHeader exit_loop {sizeof(Ngs2VoiceParamHeader), 0,
	                                type == Ngs2RackType::Sampler ? 0x10000004u : 0x40010004u};
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&active.voice), &exit_loop);
	active.voice.SetEvent(32);
	active.Render(5);
	Check(active.voice.samples == std::vector<float> {0.25f, 0.5f, 0.25f, 0.5f, 0.75f} &&
	          active.voice.blocks.empty() && log.events.size() == 1,
	      "exiting infinite loop lost its final iteration or ignored callback flags");

	Fixture pending(48000, 1, type);
	pending.Queue(pcm, 1, 1);
	pending.Queue(pcm + 1, 1, 1, UINT32_MAX);
	pending.Queue(pcm + 2, 1, 0);
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&pending.voice), &exit_loop);
	pending.Render(3);
	Check(pending.voice.samples == std::vector<float> {0.25f, 0.5f, 0.75f} &&
	          pending.voice.blocks.empty(),
	      "exiting a pending loop skipped its first iteration");
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&pending.voice), &exit_loop);
}

void TestAtrac9Looping() {
	// Four encoded mono frames with one nonzero spectral coefficient, then superframe padding.
	// The block skips one frame plus one sample and ends inside the third frame.
	std::array<uint8_t, 192> encoded {};
	for (size_t i = 0; i < 4; ++i) {
		std::copy(ATRAC9_MONO_BLOCK.begin(), ATRAC9_MONO_BLOCK.end(),
		          encoded.begin() + i * ATRAC9_MONO_BLOCK.size());
		if (i != 0) encoded[i * ATRAC9_MONO_BLOCK.size()] |= 0x80;
	}
	const auto make_voice = [&](uint32_t repeats, LoopCallbacks& log) {
		auto f = std::make_unique<Fixture>(24000);
		f->voice.SetupSampler({NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0});
		f->voice.state          = Ngs2VoicePlayState::Playing;
		f->voice.callback       = reinterpret_cast<uintptr_t>(&RecordLoopCallback);
		f->voice.callback_data  = reinterpret_cast<uintptr_t>(&log);
		f->voice.callback_flags = 3;
		const Ngs2WaveformBlock block {0, encoded.size(), repeats, 129, 137, 0, 123};
		struct Blocks {
			Ngs2VoiceParamHeader     header {32, 0, 0x10000001};
			const uint8_t*           data;
			uint32_t                 flags, count;
			const Ngs2WaveformBlock* blocks;
		} param {{32, 0, 0x10000001}, encoded.data(), 0, 1, &block};
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f->voice), &param.header) == OK,
		      "compressed loop queue failed");
		return f;
	};
	LoopCallbacks baseline_log, finite_log, split_log, infinite_log;
	auto baseline = make_voice(0, baseline_log);
	baseline->Render(274);
	const auto reference = baseline->voice.samples;
	Check(std::ranges::any_of(reference, [](float sample) { return std::abs(sample) > 0.00001f; }),
	      "synthetic compressed fixture produced no decoded signal");
	auto finite = make_voice(2, finite_log);
	finite->Render(822);
	Check(finite->voice.state == Ngs2VoicePlayState::Empty &&
	          finite->voice.decoded_samples == 411 && finite->voice.decoded_bytes == 576 &&
	          finite_log.events == std::vector<std::pair<uint32_t, uint32_t>> {{2, 1}, {2, 2}, {1, 2}},
	      "compressed finite loop lost samples, input progress, or completion callbacks");
	for (size_t i = 0; i < 3; ++i) {
		Check(std::equal(reference.begin(), reference.end() - 1,
		                 finite->voice.samples.begin() + i * 274),
		      "compressed loop did not restore decoder history and skip samples");
		const float tail = i == 2 ? reference.back() : std::lerp(reference[272], reference[0], 0.5f);
		Check(finite->voice.samples[i * 274 + 273] == tail,
		      "compressed loop dropped the final frame or broke boundary interpolation");
	}
	auto split = make_voice(2, split_log);
	split->Render(273);
	auto joined = split->voice.samples;
	split->Render(549);
	joined.insert(joined.end(), split->voice.samples.begin(), split->voice.samples.end());
	Check(joined == finite->voice.samples && split_log.events == finite_log.events,
	      "compressed loop changed across render grains");
	auto infinite = make_voice(UINT32_MAX, infinite_log);
	infinite->Render(300);
	Check(infinite->voice.state == Ngs2VoicePlayState::Playing &&
	          infinite->voice.blocks.front().info.num_repeats == UINT32_MAX,
	      "compressed infinite loop did not rewind");
	Ngs2VoiceParamHeader exit_loop {sizeof(Ngs2VoiceParamHeader), 0, 0x10000004};
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&infinite->voice), &exit_loop);
	infinite->Render(248);
	Check(infinite->voice.state == Ngs2VoicePlayState::Empty &&
	          infinite->voice.decoded_samples == 274 &&
	          infinite_log.events == std::vector<std::pair<uint32_t, uint32_t>> {{2, 1}, {1, 1}},
	      "compressed loop exit failed to finish the active iteration");
}

void TestAtrac9WaveformJoining() {
	std::array<uint8_t, 192> encoded {};
	for (size_t i = 0; i < 4; ++i) {
		std::copy(ATRAC9_MONO_BLOCK.begin(), ATRAC9_MONO_BLOCK.end(),
		          encoded.begin() + i * ATRAC9_MONO_BLOCK.size());
		if (i != 0) encoded[i * ATRAC9_MONO_BLOCK.size()] |= 0x80;
	}
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		Fixture reference(24000, 1, rack);
		reference.voice.SetupSampler({NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0});
		reference.voice.state = Ngs2VoicePlayState::Playing;
		const Ngs2WaveformBlock whole {0, encoded.size(), 0, 0, 512, 0, 123};
		Check(reference.QueueBlocks(encoded.data(), &whole, 1, 0) == OK,
		      "compressed reference block could not be queued");
		reference.Render(1024);
		for (bool nojoin: {false, true}) {
			Fixture f(24000, 1, rack);
			f.SetCapacity(1);
			f.voice.SetupSampler({NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0});
			f.voice.state = Ngs2VoicePlayState::Playing;
			LoopCallbacks callbacks;
			f.voice.callback = reinterpret_cast<uintptr_t>(&RecordLoopCallback);
			f.voice.callback_data = reinterpret_cast<uintptr_t>(&callbacks);
			f.voice.callback_flags = 1;
			const Ngs2WaveformBlock first {0, 36, 0, 0, 512, 0, 123};
			const Ngs2WaveformBlock rest {36, encoded.size() - 36, 99, 99, 99, 0, 123};
			Check(f.QueueBlocks(encoded.data(), &first, 1, 1) == OK,
			      "compressed stream start could not be queued");
			const uint32_t flags = nojoin ? 0x12 : 2;
			if (nojoin) {
				Check(f.QueueBlocks(encoded.data(), &rest, 1, flags) ==
				          static_cast<int32_t>(0x804a8054u) &&
				          f.voice.blocks.size() == 1 && f.voice.blocks.front().info.data_size == 36 &&
				          f.voice.accepts_blocks,
				      "NOJOINT bypassed capacity or changed the full stream");
				f.SetCapacity(2);
			}
			Check(f.QueueBlocks(encoded.data(), &rest, 1, flags) == OK &&
			          f.voice.blocks.size() == (nojoin ? 2u : 1u),
			      "consecutive ONLY_DATA blocks did not follow the joining flag");
			f.Render(1024);
			Check(f.voice.samples == reference.voice.samples && f.voice.decoded_samples == 512 &&
			          f.voice.decoded_bytes == encoded.size() && f.voice.blocks.empty() &&
			          callbacks.events.size() == (nojoin ? 2u : 1u),
			      "compressed joining changed audio, progress, or block completion callbacks");
		}
		Fixture full(24000, 1, rack);
		full.SetCapacity(2);
		full.voice.SetupSampler({NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0});
		const Ngs2WaveformBlock initial[] = {{0, 36, 0, 0, 512}, {36, 36, 0, 0, 512}};
		const Ngs2WaveformBlock addition[] = {{72, 36}, {144, 48}};
		Check(full.QueueBlocks(encoded.data(), initial, 2, 1) == OK &&
		          full.QueueBlocks(encoded.data(), addition, 2, 2) ==
		              static_cast<int32_t>(0x804a8054u) &&
		          full.voice.blocks.size() == 2 && full.voice.blocks.back().info.data_size == 36 &&
		          full.voice.accepts_blocks,
		      "overflow after a tentative join partially extended or closed the stream");
	}
}

void TestWaveformReadAddress() {
	Fixture       f(48000, 2);
	f.system.option.num_grain_samples = 1;
	const int16_t pcm[] = {-100, -100, 8192, 4096, 16384, 8192};
	f.Queue(pcm, 2, 1, 1, 1);
	const auto next_data = [&]() {
		Ngs2SamplerVoiceState state {};
		Ngs2VoiceGetState(reinterpret_cast<uintptr_t>(&f.voice), &state.voice_state, sizeof(state));
		return state.waveform_data;
	};
	Check(next_data() == pcm + 2, "read address ignored skipped source samples");
	f.Render(1);
	Check(next_data() == pcm + 4, "read address did not advance by a stereo frame");
	f.Render(1);
	Check(next_data() == pcm + 2, "read address did not wrap at a loop boundary");
	f.Render(2);
	Check(f.voice.blocks.empty() && next_data() == pcm + 6,
	      "starved read address did not preserve the consumed waveform end");
	f.Queue(pcm + 2, 2, 0);
	Check(next_data() == pcm + 2, "refilled read address retained the previous waveform end");
	f.Render(2);
	Check(next_data() == pcm + 6, "completed read address did not preserve waveform end");
	f.voice.SetupSampler({0x12, 2, 48000});
	Check(next_data() == nullptr, "setup retained an old waveform address");
}

struct CallbackEvent {
	Ngs2VoiceInternal* voice;
	uint32_t           event;
	uint32_t           calls = 0;
	uint32_t           trigger = 1;
};

void KYTY_SYSV_ABI ApplyCallbackEvent(const Ngs2VoiceCallbackInfo* info) {
	auto& context = *reinterpret_cast<CallbackEvent*>(info->data);
	if (++context.calls == context.trigger) {
		context.voice->SetEvent(context.event);
	}
}

void TestCallbackStopsConsumption() {
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		for (uint32_t event: {2u, 4u, 8u, 16u}) {
			Fixture       f(48000, 1, rack);
			const int16_t pcm[] = {8192, 16384};
			f.Queue(pcm, 1, 1);
			f.Queue(pcm + 1, 1, 1);
			CallbackEvent context {&f.voice, event};
			f.voice.callback       = reinterpret_cast<uintptr_t>(&ApplyCallbackEvent);
			f.voice.callback_data  = reinterpret_cast<uintptr_t>(&context);
			f.voice.callback_flags = 1;
			f.Render(2);
			Check(f.voice.state != Ngs2VoicePlayState::Playing && context.calls == 1,
			      "completion callback failed to interrupt playback");
			Check(f.voice.samples[0] == 0.0f && f.voice.samples[1] == 0.0f &&
			          f.voice.decoded_samples == 1 && f.voice.blocks.size() == 1 &&
			          f.voice.blocks.front().data_cursor == 0,
			      "callback pause/stop consumed the next queued block");
			if (event == 16) {
				f.voice.SetEvent(32);
				f.Render(2);
				Check(f.voice.samples == std::vector<float> {0.25f, 0.5f} &&
				          f.voice.decoded_samples == 2,
				      "callback pause failed to preserve queued audio for resume");
			}
		}
	}
}

void TestCallbackPauseDuringLargeStep() {
	Fixture       f(48000);
	const int16_t pcm[] = {8192, 16384, 24576, 4096};
	f.Queue(pcm, 1, 1);
	f.Queue(pcm + 1, 1, 1);
	f.Queue(pcm + 2, 2, 1);
	f.voice.sample_step = uint64_t(120000) << 32u;
	CallbackEvent context {&f.voice, 16};
	f.voice.callback       = reinterpret_cast<uintptr_t>(&ApplyCallbackEvent);
	f.voice.callback_data  = reinterpret_cast<uintptr_t>(&context);
	f.voice.callback_flags = 1;
	f.Render(2);
	Check(context.calls == 1 && f.voice.decoded_samples == 1 && f.voice.sample_phase == 0 &&
	          !f.voice.has_samples,
	      "pause advanced across additional blocks in a large resampling step");
	f.voice.SetEvent(32);
	f.Render(2);
	Check(f.voice.samples[0] == 0.25f && std::abs(f.voice.samples[1] - 0.4375f) < 1e-7f &&
	          context.calls == 3,
	      "resume lost fractional position or read past the next short block");
}

void TestCallbackPauseDuringLookahead() {
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		Fixture f(24000, 1, rack);
		f.system.option.num_grain_samples = 1;
		int16_t pcm[] = {8192, 16384};
		f.Queue(pcm, 1, 1);
		f.Queue(pcm + 1, 1, 0);
		CallbackEvent context {&f.voice, 16, 0, 2};
		f.voice.callback = reinterpret_cast<uintptr_t>(&ApplyCallbackEvent);
		f.voice.callback_data = reinterpret_cast<uintptr_t>(&context);
		f.voice.callback_flags = 1;
		f.Render(1);
		Check(f.voice.samples[0] == 0.25f, "initial source sample was not rendered");
		f.Render(2);
		Check(!f.voice.has_samples && f.voice.state == Ngs2VoicePlayState::Paused &&
		          context.calls == 2 && f.State().num_decoded_samples == 2 &&
		          f.voice.sample_phase == (uint64_t(24000) << 32u),
		      "lookahead callback pause consumed output or lost decoded progress");
		pcm[0] = pcm[1] = -32768;
		f.voice.SetEvent(32);
		f.Render(3);
		Check(f.voice.samples == std::vector<float> {0.375f, 0.5f, 0.5f} &&
		          f.voice.state == Ngs2VoicePlayState::Empty,
		      "lookahead pause failed to preserve owned samples or fractional playback position");
	}
}

struct CallbackReplacement {
	Fixture& fixture;
	bool setup;
	uint32_t trigger, calls = 0;
	const std::array<int16_t, 4> pcm {16384, 8192, 4096, 2048};
};

void KYTY_SYSV_ABI ReplaceCallbackSource(const Ngs2VoiceCallbackInfo* info) {
	auto& replacement = *reinterpret_cast<CallbackReplacement*>(info->data);
	if (++replacement.calls != replacement.trigger) {
		return;
	}
	auto& f = replacement.fixture;
	f.voice.callback_flags = 0;
	if (replacement.setup) {
		f.voice.SetupSampler({0x12, 1, 48000});
	}
	f.Queue(replacement.pcm.data(), 2, replacement.setup ? 0 : 4);
	f.voice.SetEvent(1);
}

void TestCallbackReplacesSource() {
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		for (bool setup: {false, true}) {
			for (uint32_t trigger: {1u, 2u}) {
				Fixture f(24000, 2, rack);
				f.system.option.num_grain_samples = 1;
				CallbackReplacement replacement {f, setup, trigger};
				const int16_t pcm[] = {8192, 4096};
				for (uint32_t i = 0; i < trigger; ++i) {
					f.Queue(pcm, 1, 1);
				}
				f.voice.callback = reinterpret_cast<uintptr_t>(&ReplaceCallbackSource);
				f.voice.callback_data = reinterpret_cast<uintptr_t>(&replacement);
				f.voice.callback_flags = 1;
				f.Render(4);
				Check(!f.voice.has_samples && f.voice.samples.size() == (setup ? 4u : 8u) &&
				          std::ranges::all_of(f.voice.samples, [](float value) { return value == 0; }) &&
				          f.voice.blocks.size() == 1 && f.voice.blocks.front().data_cursor == 0,
				      "callback source replacement used an old frame or channel layout");
				f.Render(4);
				const std::vector<float> expected = setup
				    ? std::vector<float> {0.5f, 0.25f, 0, 0}
				    : std::vector<float> {0.5f, 0.3125f, 0.125f, 0.125f,
				                          0.25f, 0.15625f, 0.0625f, 0.0625f};
				Check(f.voice.samples == expected && f.voice.state == Ngs2VoicePlayState::Empty,
				      "callback replacement failed to play the new source on the next render");
			}
		}
	}
}

void TestUpstreamCustomSamplerControls() {
	Fixture f(24000, 1, Ngs2RackType::CustomSampler);
	const int16_t pcm[] = {8192, 16384, 24576, 4096};
	f.Queue(pcm, 4, 1);
	f.Render(1);
	Check(f.voice.samples[0] == 0.25f, "custom sampler source rate was rejected");
	f.Queue(pcm + 2, 2, 4);
	f.Render(1);
	Check(f.voice.samples[0] == 0.75f && f.voice.blocks.empty() && f.voice.frame_samples == 2,
	      "replacement blocks retained old data or fractional position");
	struct Pitch {
		Ngs2VoiceParamHeader header {16, 0, 0x40010005};
		float                ratio = 0.0f;
		uint32_t             reserved = 0;
	} pitch;
	const auto handle = reinterpret_cast<uintptr_t>(&f.voice);
	Ngs2VoiceControl(handle, &pitch.header);
	f.Render(2);
	Check(f.voice.samples[0] == 0.4375f && f.voice.samples[1] == 0.4375f,
	      "zero custom sampler pitch advanced playback");
	pitch.ratio = 4.0f;
	Ngs2VoiceControl(handle, &pitch.header);
	f.Render(1);
	Check(f.voice.blocks.empty(), "custom sampler pitch did not advance playback");
	f.rack.option.custom_sampler.custom_rack_option.state_size =
	    sizeof(Ngs2CustomSamplerVoiceState);
	Ngs2CustomSamplerVoiceState state {};
	Check(Ngs2VoiceGetState(handle, &state.voice_state, sizeof(state)) == OK &&
	          state.voice_state.state_flags == Ngs2GetStateFlags(&f.voice),
	      "custom sampler state layout regressed");
}

void TestFilterTailDuringStarvation() {
	Fixture reference(48000), stream(48000);
	SetLowPass(reference);
	SetLowPass(stream);
	std::vector<int16_t> pcm(128, 0);
	pcm[0] = pcm[64] = 16384;
	reference.Queue(pcm.data(), 128, 1);
	reference.Render(128);
	stream.Queue(pcm.data(), 1, 1);
	stream.Render(1);
	Check(stream.voice.samples[0] == reference.voice.samples[0], "initial filter sample differs");
	stream.Render(63);
	Check(stream.voice.has_samples, "starved filter tail was not available for routing");
	for (size_t i = 0; i < 63; ++i) {
		Check(std::abs(stream.voice.samples[i] - reference.voice.samples[i + 1]) < 1e-7f,
		      "filter tail changed at a starvation/grain boundary");
	}
	stream.Queue(pcm.data() + 64, 64, 1);
	stream.Render(64);
	for (size_t i = 0; i < 64; ++i) {
		Check(std::abs(stream.voice.samples[i] - reference.voice.samples[i + 64]) < 1e-7f,
		      "refilled stream reused stale filter history");
	}
	stream.Render(8192);
	stream.Render(32);
	Check(!stream.voice.has_samples && !stream.voice.filters[0].HasHistory(),
	      "fully decayed filter kept a silent stream active");
}

void TestResamplingPhaseAfterStarvation() {
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		for (uint32_t rate: {44100u, 96000u}) {
			Fixture        reference(rate, 1, rack), stream(rate, 1, rack);
			const int16_t  pcm[]  = {0, 0, 16384, 8192};
			const uint32_t frames = rate == 44100 ? 2 : 1;
			reference.Queue(pcm, 4, 0);
			stream.Queue(pcm, 1, 1);
			reference.Render(frames);
			stream.Render(frames);
			Check(stream.voice.blocks.empty() && stream.voice.state == Ngs2VoicePlayState::Playing,
			      "open stream did not enter starvation");
			stream.Render(8);
			Check(!stream.voice.has_samples, "starved stream produced audio without a filter");
			stream.Queue(pcm + 1, 3, 0);
			reference.Render(1);
			stream.Render(1);
			Check(std::abs(stream.voice.samples[0] - reference.voice.samples[0]) < 1e-7f,
			      "stream refill lost resampling phase");
			Check(stream.voice.decoded_samples == reference.voice.decoded_samples,
			      "stream refill lost pending source advancement");
			stream.voice.SetupSampler({0x12, 1, rate});
			Check(stream.voice.sample_phase == 0, "sampler setup retained old resampling phase");
		}
	}
}

void TestFiniteFilterTail() {
	Fixture whole(48000), split(48000);
	SetLowPass(whole);
	SetLowPass(split);
	const int16_t impulse = 16384;
	whole.Queue(&impulse, 1, 0);
	split.Queue(&impulse, 1, 0);
	whole.Render(256);
	for (size_t i = 0; i < whole.voice.samples.size(); ++i) {
		split.Render(1);
		Check(std::abs(split.voice.samples[0] - whole.voice.samples[i]) < 1e-7f,
		      "finite filter tail changed with render grain size");
	}
	Check(split.voice.state == Ngs2VoicePlayState::Playing,
	      "finite voice stopped before its filter tail decayed");
	split.Render(8192);
	Check(split.voice.state == Ngs2VoicePlayState::Empty,
	      "finite voice remained active after its filter tail decayed");
	split.Render(32);
	Check(!split.voice.has_samples, "completed finite filter tail still produced samples");
}

void TestSystemRunCommandsAndQueries() {
	Ngs2SystemOption opt {};
	Ngs2SystemResetOption(&opt);
	Check(opt.sample_rate == 48000, "reset option failed");

	Ngs2Internal system;
	system.option = opt;
	const auto sys_handle = reinterpret_cast<uintptr_t>(&system);

	// User data
	uintptr_t ud = 0;
	Check(Ngs2SystemSetUserData(sys_handle, 0x12345678) == OK, "set user data failed");
	Check(Ngs2SystemGetUserData(sys_handle, &ud) == OK && ud == 0x12345678, "get user data failed");

	// Sample rate
	Check(Ngs2SystemSetSampleRate(sys_handle, 44100) == OK && system.option.sample_rate == 44100, "set sample rate failed");
	Check(Ngs2SystemSetSampleRate(sys_handle, 12345) == NGS2_ERROR_INVALID_SAMPLE_RATE, "invalid sample rate accepted");

	// Lock & Unlock
	Check(Ngs2SystemLock(sys_handle) == OK, "system lock failed");
	Check(Ngs2SystemUnlock(sys_handle) == OK, "system unlock failed");

	// Commands
	struct Cmd {
		uint32_t id;
		uint8_t  flags, type;
		uint16_t count;
		union {
			uint64_t    u64;
			uint32_t    u32;
			int32_t     i32;
			float       f;
			const void* ptr;
		} value;
	};

	Cmd cmds[6] = {};
	cmds[0].id = 0xf00000; // lock
	cmds[1].id = 0xf00001; // unlock
	cmds[2].id = 0xf00002; // userdata
	cmds[2].value.u64 = 0x8888;
	cmds[3].id = 0xf00004; // samplerate
	cmds[3].value.u32 = 96000;
	cmds[4].id = 0xf00005; // grain
	cmds[4].value.u32 = 128;
	cmds[5].id = 0xf00006; // unnamed float
	cmds[5].value.f = 0.75f;

	Check(Ngs2SystemRunCommands(sys_handle, cmds, 6) == OK, "system run commands failed");
	Check(system.user_data == 0x8888, "system userdata cmd failed");
	Check(system.option.sample_rate == 96000, "system samplerate cmd failed");
	Check(system.option.num_grain_samples == 128, "system grain cmd failed");
	Check(std::abs(system.unnamed_param - 0.75f) < 1e-6f, "system unnamed param failed");

	// Slot command
	uint8_t slot_bytes[24] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
	Cmd slot_cmd {};
	slot_cmd.id = 0xf00003 | (2u << 24); // slot 2
	slot_cmd.value.ptr = slot_bytes;
	Check(Ngs2SystemRunCommands(sys_handle, &slot_cmd, 1) == OK, "slot cmd failed");
	Check(system.slot_set[2], "slot 2 not set");
	Check(std::memcmp(system.slot_data[2].data(), slot_bytes, 24) == 0, "slot data mismatch");

	// Clear slot
	slot_cmd.value.ptr = nullptr;
	Check(Ngs2SystemRunCommands(sys_handle, &slot_cmd, 1) == OK, "clear slot failed");
	Check(!system.slot_set[2], "slot 2 not cleared");

	// QueryInfo tests
	uint64_t query_ud = 0;
	Check(Ngs2SystemQueryInfo(sys_handle, 0xf04001, &query_ud, sizeof(query_ud)) == OK, "query userdata failed");
	Check(query_ud == 0x8888, "queried userdata mismatch");

	// Reset slot for query
	slot_cmd.value.ptr = slot_bytes;
	Check(Ngs2SystemRunCommands(sys_handle, &slot_cmd, 1) == OK, "re-set slot failed");
	uint8_t query_slot[24] = {};
	Check(Ngs2SystemQueryInfo(sys_handle, 0xf04002 | (2u << 24), query_slot, 24) == OK, "query slot failed");
	Check(std::memcmp(query_slot, slot_bytes, 24) == 0, "queried slot mismatch");

	// Error handling
	Cmd bad_cmd {};
	bad_cmd.id = 0xdeadbeef;
	Check(Ngs2SystemRunCommands(sys_handle, &bad_cmd, 1) == NGS2_ERROR_INVALID_PARAM_ID, "bad cmd allowed");
	Check(Ngs2SystemQueryInfo(sys_handle, 0xdeadbeef, &query_ud, sizeof(query_ud)) == NGS2_ERROR_INVALID_SYSTEM_QUERY_ID, "bad query allowed");
}

void TestRackRunCommandsAndQueries() {
	Ngs2Internal system;
	system.option.sample_rate = 48000;
	system.option.num_grain_samples = 256;
	Ngs2CustomSamplerRackOption c_opt {};
	c_opt.custom_rack_option.rack_option.size = sizeof(c_opt);
	c_opt.custom_rack_option.rack_option.max_grain_samples = 512;
	c_opt.custom_rack_option.rack_option.max_voices = 1;
	c_opt.custom_rack_option.rack_option.max_ports = 1;
	c_opt.custom_rack_option.rack_option.max_matrices = 1;
	c_opt.custom_rack_option.num_buffers = 1;
	c_opt.custom_rack_option.num_modules = 2;
	c_opt.custom_rack_option.module[0].module_id = 0x15;
	c_opt.custom_rack_option.module[0].source_buffer_id = 0;
	c_opt.custom_rack_option.module[0].dest_buffer_id = 0;
	c_opt.custom_rack_option.module[0].state_size = 64;
	c_opt.custom_rack_option.state_size = sizeof(Ngs2CustomSamplerVoiceState);
	c_opt.max_waveform_blocks = 4;

	RenderRack renderRack(system, 0x4001, c_opt.custom_rack_option.rack_option);
	const auto rack_handle = renderRack.handle;
	auto* rack_ptr = reinterpret_cast<Ngs2RackInternal*>(rack_handle);
	std::memcpy(rack_ptr->name, "CustomTestRack", 15);
	rack_ptr->uid = 42;
	auto& rack = *rack_ptr;

	// User data
	uintptr_t ud = 0;
	Check(Ngs2RackSetUserData(rack_handle, 0x9999) == OK, "rack set user data failed");
	Check(Ngs2RackGetUserData(rack_handle, &ud) == OK && ud == 0x9999, "rack get user data failed");

	// Lock & Unlock
	Check(Ngs2RackLock(rack_handle) == OK, "rack lock failed");
	Check(rack.lock_depth == 1, "rack lock depth mismatch");
	Check(Ngs2RackUnlock(rack_handle) == OK, "rack unlock failed");
	Check(rack.lock_depth == 0, "rack lock depth after unlock mismatch");

	// RackRunCommands
	struct Cmd {
		uint32_t id;
		uint8_t  flags, type;
		uint16_t count;
		union {
			uint64_t    u64;
			uint32_t    u32;
			int32_t     i32;
			float       f;
			const void* ptr;
		} value;
	};
	Cmd cmds[3] = {};
	cmds[0].id = 0xf10000; // lock
	cmds[1].id = 0xf10001; // unlock
	cmds[2].id = 0xf10002; // user data
	cmds[2].value.u64 = 0x7777;
	Check(Ngs2RackRunCommands(rack_handle, cmds, 3) == OK, "rack run commands failed");
	Check(rack.user_data == 0x7777, "rack run commands user data failed");

	// RackGetInfo
	Ngs2RackInfo info {};
	Check(Ngs2RackGetInfo(rack_handle, &info, sizeof(info)) == OK, "rack get info failed");
	Check(std::strcmp(info.name, "CustomTestRack") == 0, "rack get info name mismatch");
	Check(info.rack_id == 0x4001, "rack get info rack_id mismatch");
	Check(info.uid == 42, "rack get info uid mismatch");
	Check(info.type == 0, "rack get info type mismatch"); // sampler/custom_sampler is 0
	Check(info.num_modules == 2, "rack get info num_modules mismatch");
	Check(info.active_voice_count == 0, "rack get info active voices mismatch");

	// Test created rack with active voice
	Ngs2Internal sys {};
	sys.option.max_grain_samples = 512;
	sys.option.num_grain_samples = 256;
	sys.option.sample_rate = 48000;
	Ngs2RackOptionUnion r_opt {};
	r_opt.sampler.rack_option.size = sizeof(Ngs2SamplerRackOption);
	r_opt.sampler.rack_option.max_grain_samples = 512;
	r_opt.sampler.rack_option.max_voices = 2;
	r_opt.sampler.rack_option.max_ports = 8;
	r_opt.sampler.rack_option.max_matrices = 1;
	r_opt.sampler.max_waveform_blocks = 4;
	std::vector<uint8_t> r_buf(sizeof(Ngs2RackInternal) + 2 * sizeof(Ngs2VoiceInternal) + 16384);
	Ngs2ContextBufferInfo c_buf {};
	c_buf.host_buffer = r_buf.data();
	c_buf.host_buffer_size = r_buf.size();
	uintptr_t cr_handle = 0;
	Check(Ngs2RackCreate(reinterpret_cast<uintptr_t>(&sys), 0x1000, &r_opt.common, &c_buf, &cr_handle) == OK, "create rack failed");
	auto* cr_voices = reinterpret_cast<Ngs2VoiceInternal*>(reinterpret_cast<Ngs2RackInternal*>(cr_handle) + 1);
	cr_voices[0].state = Ngs2VoicePlayState::Playing;
	cr_voices[0].channels = 2;
	Ngs2RackInfo cr_info {};
	Check(Ngs2RackGetInfo(cr_handle, &cr_info, sizeof(cr_info)) == OK, "get info for created rack failed");
	Check(cr_info.active_voice_count == 1, "created rack active voice count mismatch");
	Check(cr_info.active_channel_work_count == 2, "created rack active channel count mismatch");
	Check(Ngs2RackDestroy(cr_handle, nullptr) == OK, "destroy created rack failed");

	// RackQueryInfo
	Ngs2RackInfo qinfo {};
	Check(Ngs2RackQueryInfo(rack_handle, 0xf14000, &qinfo, sizeof(qinfo)) == OK, "rack query info failed");
	Check(qinfo.rack_id == 0x4001, "queried rack info mismatch");

	uint64_t q_ud = 0;
	Check(Ngs2RackQueryInfo(rack_handle, 0xf14001, &q_ud, sizeof(q_ud)) == OK && q_ud == 0x7777, "rack query ud failed");

	Ngs2CustomModuleInfo q_mod {};
	Check(Ngs2RackQueryInfo(rack_handle, 0xf14002 | (0u << 24), &q_mod, sizeof(q_mod)) == OK, "rack query mod failed");
	Check(q_mod.module_id == 0x15 && q_mod.state_size == 64, "queried mod mismatch");

	// CustomRackGetModuleInfo
	Ngs2CustomModuleInfo mod_info {};
	Check(Ngs2CustomRackGetModuleInfo(rack_handle, 0, &mod_info, sizeof(mod_info)) == OK, "custom rack get module info failed");
	Check(mod_info.module_id == 0x15 && mod_info.state_size == 64, "module info mismatch");
	Check(Ngs2CustomRackGetModuleInfo(rack_handle, 99, &mod_info, sizeof(mod_info)) == NGS2_ERROR_INVALID_RACK_MODULE_INDEX, "out of range module allowed");
}

void TestVoiceOwnerAndMatrixInfo() {
	Ngs2Internal system;
	system.option                   = Ngs2DefaultSystemOption();
	system.option.sample_rate = 48000;
	system.option.num_grain_samples = 256;
	Ngs2RackOptionUnion option {};
	Ngs2FillDefaultRackOption(0x1000, &option);
	option.sampler.rack_option.max_voices = 1;
	option.sampler.rack_option.max_matrices = 2;
	option.sampler.rack_option.max_ports = 8;
	option.sampler.max_waveform_blocks = 4;
	RenderRack renderRack(system, 0x1000, option.sampler.rack_option);
	uintptr_t voice_handle = 0;
	Check(Ngs2RackGetVoiceHandle(renderRack.handle, 0, &voice_handle) == OK, "get voice handle failed");
	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);
	voice->matrices.resize(2);
	voice->ports.resize(8);
	const auto rack_handle = renderRack.handle;

	uintptr_t owner = 0;
	uint32_t voice_index = 999;
	Check(Ngs2VoiceGetOwner(voice_handle, &owner, &voice_index) == OK, "voice get owner failed");
	Check(owner == rack_handle, "voice get owner rack mismatch");
	Check(voice_index == 0, "voice get owner index mismatch");

	// Matrix info
	Ngs2VoiceMatrixInfo minfo {};
	Check(Ngs2VoiceGetMatrixInfo(voice_handle, 0, &minfo, sizeof(minfo)) == OK, "voice get matrix info failed");
	Check(minfo.num_levels == 0, "initial matrix info should be empty");
	// Set matrix levels
	float levels[4] = {0.1f, 0.2f, 0.3f, 0.4f};
	struct Cmd {
		uint32_t id;
		uint8_t  flags, type;
		uint16_t count;
		union {
			uint32_t     u;
			int32_t      i;
			float        f;
			const float* levels;
		} value;
	} cmd {};
	cmd.id = 5 | (0u << 24); // matrix 0, id 5
	cmd.type = 0x11;
	cmd.count = 4;
	cmd.value.levels = levels;
	Check(Ngs2VoiceRunCommands(voice_handle, &cmd, 1) == OK, "voice run commands matrix levels failed");

	Check(Ngs2VoiceGetMatrixInfo(voice_handle, 0, &minfo, sizeof(minfo)) == OK, "voice get matrix info after set failed");
	Check(minfo.num_levels == 4, "matrix levels count mismatch");
	Check(std::abs(minfo.levels[0] - 0.1f) < 1e-6f && std::abs(minfo.levels[3] - 0.4f) < 1e-6f, "matrix levels values mismatch");

	// A matrix larger than the ABI capacity must report the number of levels
	// actually copied, not the stored count.
	std::vector<float> bigLevels(1370);
	for (size_t i = 0; i < bigLevels.size(); ++i) {
		bigLevels[i] = static_cast<float>(i);
	}
	cmd.count       = 1370;
	cmd.value.levels = bigLevels.data();
	Check(Ngs2VoiceRunCommands(voice_handle, &cmd, 1) == OK, "voice run commands oversized matrix failed");
	Check(Ngs2VoiceGetMatrixInfo(voice_handle, 0, &minfo, sizeof(minfo)) == OK,
	      "voice get matrix info after oversized set failed");
	Check(minfo.num_levels == 37 * 37, "oversized matrix must report the copied level count");
	Check(minfo.levels[37 * 37 - 1] == static_cast<float>(37 * 37 - 1),
	      "oversized matrix copied the wrong levels");

	// Error handling
	Check(Ngs2VoiceGetMatrixInfo(voice_handle, 999, &minfo, sizeof(minfo)) == NGS2_ERROR_INVALID_PARAM_ID, "out of range matrix accepted");
	Check(Ngs2VoiceGetMatrixInfo(0, 0, &minfo, sizeof(minfo)) == NGS2_ERROR_INVALID_VOICE_HANDLE, "null voice handle accepted");
	Check(Ngs2VoiceGetOwner(0, &owner, &voice_index) == NGS2_ERROR_INVALID_VOICE_HANDLE, "null voice handle for owner accepted");
	Check(Ngs2VoiceGetOwner(voice_handle, nullptr, &voice_index) == NGS2_ERROR_INVALID_OUT_ADDRESS, "null output pointer accepted");
}

// Verify: FftQuerySize, FftInit, FftProcess (forward/inverse).
static void TestFftPhase2() {
	// --- FftQuerySize: Boundary and Valid Cases ---
	uint64_t sz = 0;
	Check(Ngs2FftQuerySize(7, 2, &sz) != OK,  "FftQuerySize: length 7 should fail");
	Check(Ngs2FftQuerySize(13, 2, &sz) != OK, "FftQuerySize: length 13 should fail");
	Check(Ngs2FftQuerySize(8, 2, nullptr) != OK, "FftQuerySize: out_size null should fail");

	// length=8, flags=2 → count=64/2=32 → size = 32*32+64 = 1088
	Check(Ngs2FftQuerySize(8, 2, &sz) == OK, "FftQuerySize(8,2) should succeed");
	Check(sz == 1088, "FftQuerySize(8,2): expected size 1088");

	// length=8, flags=0 → count=64 → size = 64*32+64 = 2112
	Check(Ngs2FftQuerySize(8, 0, &sz) == OK, "FftQuerySize(8,0) should succeed");
	Check(sz == 2112, "FftQuerySize(8,0): expected size 2112");

	// Check for all valid lengths (8–12) with flags=2
	const uint32_t expectedSamples[5] = {64u, 128u, 256u, 512u, 1024u};
	for (int len = 8; len <= 12; ++len) {
		Check(Ngs2FftQuerySize(static_cast<uint32_t>(len), 2, &sz) == OK,
		      "FftQuerySize loop length 8-12 failed");
		const uint64_t expectedSz =
		    static_cast<uint64_t>(expectedSamples[len - 8] / 2u) * 32u + 64u;
		Check(sz == expectedSz, "FftQuerySize: size mismatch in loop");
	}

	// --- FftInit: Parameter Validation ---
	void* handle = nullptr;
	std::vector<uint8_t> tooSmall(64);
	Check(Ngs2FftInit(tooSmall.data(), tooSmall.size(), 8, 2, &handle) != OK,
	      "FftInit: buffer too small should fail");
	Check(Ngs2FftInit(nullptr, 1088, 8, 2, &handle) != OK,
	      "FftInit: work null should fail");
	Check(Ngs2FftInit(tooSmall.data(), 1088, 8, 2, nullptr) != OK,
	      "FftInit: out_handle null should fail");
	Check(Ngs2FftInit(tooSmall.data(), 1088, 7, 2, &handle) != OK,
	      "FftInit: length 7 should fail");
	Check(Ngs2FftInit(tooSmall.data(), 1088, std::numeric_limits<int>::min(), 2, &handle) != OK,
	      "FftInit: INT_MIN length must fail without signed overflow");

	// Correct init (flags=2, length=8)
	Ngs2FftQuerySize(8, 2, &sz);
	std::vector<uint8_t> workBuf(sz);
	handle = nullptr;
	Check(Ngs2FftInit(workBuf.data(), sz, 8, 2, &handle) == OK,
	      "FftInit flags=2 should succeed");
	Check(handle == workBuf.data(), "FftInit: handle must point to the buffer");

	// Verify the header was written correctly
	int32_t  readLen  = 0;
	uint32_t readFlgs = 0xdead;
	std::memcpy(&readLen,  workBuf.data() + 0x10, sizeof(readLen));
	std::memcpy(&readFlgs, workBuf.data() + 0x14, sizeof(readFlgs));
	Check(readLen == 8, "FftInit: length in header mismatch");
	Check(readFlgs == 2, "FftInit: flags in header mismatch");

	// The sine/cosine table address must be 64-byte aligned
	uint64_t tablePtr = 0;
	std::memcpy(&tablePtr, workBuf.data() + 0x00, sizeof(tablePtr));
	Check((tablePtr & 0x3fu) == 0, "FftInit: table address not aligned to 64 bytes");
	Check(tablePtr >= reinterpret_cast<uintptr_t>(workBuf.data()),
	      "FftInit: table address outside the buffer");

	// --- FftProcess: null guard ---
	Check(Ngs2FftProcess(nullptr, nullptr, nullptr, nullptr, 0, 0) != OK,
	      "FftProcess: handle null should fail");
	{
		float dummyIn[256]  = {};
		float dummyOut[256] = {};
		Check(Ngs2FftProcess(nullptr, dummyIn, dummyOut, nullptr, 0, 0) != OK,
		      "FftProcess: handle null should fail (2)");
	}

	// --- FftProcess flags=0: pass-through (copies work_buf to out) ---
	Ngs2FftQuerySize(8, 0, &sz);
	std::vector<uint8_t> workF0(sz);
	void* handleF0 = nullptr;
	Check(Ngs2FftInit(workF0.data(), sz, 8, 0, &handleF0) == OK, "FftInit flags=0 should succeed");
	const size_t points0 = 256; // 1<<8
	std::vector<float> dummyIn(points0 * 2, 1.0f);
	std::vector<float> scratchF0(points0 * 2);
	for (size_t i = 0; i < scratchF0.size(); ++i) {
		scratchF0[i] = static_cast<float>(i) * 0.25f - 10.0f;
	}
	std::vector<float> outF0(points0 * 2, -1.0f);
	Check(Ngs2FftProcess(handleF0, dummyIn.data(), outF0.data(), scratchF0.data(), 0, 0) == OK,
	      "FftProcess flags=0 should succeed");
	Check(outF0 == scratchF0, "FftProcess flags=0: output must equal scratchF0");

	// --- FftProcess flags=2: forward + inverse (round-trip identity) ---
	for (int len = 8; len <= 10; ++len) { // test on 64, 128, 256 samples (fast)
		Ngs2FftQuerySize(static_cast<uint32_t>(len), 2, &sz);
		std::vector<uint8_t> fftWork(sz);
		void* fftHandle = nullptr;
		Check(Ngs2FftInit(fftWork.data(), sz, len, 2, &fftHandle) == OK,
		      "FftInit loop should succeed");

		const size_t N = size_t{1} << len;
		// Test signal: sine + cosine to guarantee a non-trivial spectrum
		std::vector<float> timeIn(N);
		for (size_t i = 0; i < N; ++i) {
			timeIn[i] = static_cast<float>(
			    std::sin(0.13 * static_cast<double>(i)) +
			    0.5 * std::cos(0.37 * static_cast<double>(i)));
		}

		// Forward: mode=0x5 (forward, internal order, no scaling)
		// Output: N float (internal-order layout, used as IFFT input)
		std::vector<float> intSpec(N, 0.0f);
		Check(Ngs2FftProcess(fftHandle, timeIn.data(), intSpec.data(), nullptr, 0, 0x5) == OK,
		      "FftProcess forward (internal) should succeed");

		// Inverse: mode=0xC (inverse, internal order, scaling 1/N) -> round-trip identity
		std::vector<float> invScaled(N, 0.0f);
		Check(Ngs2FftProcess(fftHandle, intSpec.data(), invScaled.data(), nullptr, 0, 0xC) == OK,
		      "FftProcess inverse (internal, scaled) should succeed");

		// Verify round trip (the <= comparison also rejects NaN)
		bool roundTrip = true;
		for (size_t i = 0; i < N; ++i) {
			if (!(std::abs(invScaled[i] - timeIn[i]) <= 1e-3f)) {
				roundTrip = false;
				break;
			}
		}
		Check(roundTrip, "FftProcess: round-trip FFT->IFFT did not reproduce the original signal");

		// Forward mode=0xD (forward + scale 1/N, internal order) must equal intSpec / N
		std::vector<float> intSpecScaled(N, 0.0f);
		Check(Ngs2FftProcess(fftHandle, timeIn.data(), intSpecScaled.data(), nullptr, 0, 0xD) == OK,
		      "FftProcess forward (internal, scaled) should succeed");
		const float scaleF = 1.0f / static_cast<float>(N);
		bool specScaleOk = true;
		for (size_t i = 0; i < N; ++i) {
			if (!(std::abs(intSpecScaled[i] - intSpec[i] * scaleF) <= 1e-5f)) {
				specScaleOk = false;
				break;
			}
		}
		Check(specScaleOk, "FftProcess: mode=0xD (forward+scale, internal) mismatch");
	}

	// --- Natural order round trip: 256 points, tone at bin 96 ---
	// The forward natural layout writes four quarter-spectrum blocks; the
	// inverse must unpack the same layout for the pair to round-trip.
	{
		constexpr int len = 8;
		Ngs2FftQuerySize(static_cast<uint32_t>(len), 2, &sz);
		std::vector<uint8_t> fftWork(sz);
		void* fftHandle = nullptr;
		Check(Ngs2FftInit(fftWork.data(), sz, len, 2, &fftHandle) == OK,
		      "FftInit natural round-trip should succeed");
		const size_t N = size_t{1} << len;
		std::vector<float> timeIn(N);
		for (size_t i = 0; i < N; ++i) {
			timeIn[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 96.0 * i / N));
		}
		std::vector<float> spectrum(N * 2, 0.0f);
		Check(Ngs2FftProcess(fftHandle, timeIn.data(), spectrum.data(), nullptr, 0, 0x1) == OK,
		      "FftProcess forward (natural) should succeed");
		std::vector<float> restored(N, 0.0f);
		Check(Ngs2FftProcess(fftHandle, spectrum.data(), restored.data(), nullptr, 0, 0x8) == OK,
		      "FftProcess inverse (natural, scaled) should succeed");
		bool naturalOk = true;
		for (size_t i = 0; i < N; ++i) {
			if (!(std::abs(restored[i] - timeIn[i]) <= 1e-3f)) {
				naturalOk = false;
				break;
			}
		}
		Check(naturalOk, "FftProcess: natural-order round trip did not reproduce the tone");
	}
}

// Check for PCM I16, PCM float32, null format, null output.

static void TestWaveformFrameInfoPhase2() {
	uint32_t frameSize = 0, numSamples = 0, unitsPerFrame = 0, delaySamples = 0;

	// --- Null guard ---
	{
		Ngs2WaveformFormat fmt{};
		fmt.waveform_type = 0x12u; // PCM I16L
		fmt.num_channels  = 2;
		fmt.sample_rate   = 48000;
		Check(Ngs2GetWaveformFrameInfo(nullptr, &frameSize, &numSamples, &unitsPerFrame, &delaySamples)
		      == NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "GetWaveformFrameInfo: format null should return INVALID_WAVEFORM_FORMAT");
		Check(Ngs2GetWaveformFrameInfo(&fmt, nullptr, &numSamples, &unitsPerFrame, &delaySamples)
		      == NGS2_ERROR_INVALID_OUT_ADDRESS, "GetWaveformFrameInfo: frame_size null should fail");
		Check(Ngs2GetWaveformFrameInfo(&fmt, &frameSize, nullptr, &unitsPerFrame, &delaySamples)
		      == NGS2_ERROR_INVALID_OUT_ADDRESS, "GetWaveformFrameInfo: num_samples null should fail");
		Check(Ngs2GetWaveformFrameInfo(&fmt, &frameSize, &numSamples, nullptr, &delaySamples)
		      == NGS2_ERROR_INVALID_OUT_ADDRESS, "GetWaveformFrameInfo: units_per_frame null should fail");
		Check(Ngs2GetWaveformFrameInfo(&fmt, &frameSize, &numSamples, &unitsPerFrame, nullptr)
		      == NGS2_ERROR_INVALID_OUT_ADDRESS, "GetWaveformFrameInfo: delay_samples null should fail");
	}

	// --- Invalid format ---
	{
		Ngs2WaveformFormat bad{};
		bad.waveform_type = 0x12u;
		bad.num_channels  = 0;   // invalid
		bad.sample_rate   = 48000;
		Check(Ngs2GetWaveformFrameInfo(&bad, &frameSize, &numSamples, &unitsPerFrame, &delaySamples)
		      == NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "GetWaveformFrameInfo: num_channels=0 should fail");

		bad.num_channels  = 2;
		bad.sample_rate   = 0;   // invalid
		Check(Ngs2GetWaveformFrameInfo(&bad, &frameSize, &numSamples, &unitsPerFrame, &delaySamples)
		      == NGS2_ERROR_INVALID_WAVEFORM_FORMAT, "GetWaveformFrameInfo: sample_rate=0 should fail");
	}

	// --- PCM 16-bit stereo (type 0x12) ---
	{
		Ngs2WaveformFormat fmt{};
		fmt.waveform_type = 0x12u;
		fmt.num_channels  = 2;
		fmt.sample_rate   = 48000;
		Check(Ngs2GetWaveformFrameInfo(&fmt, &frameSize, &numSamples, &unitsPerFrame, &delaySamples)
		      == OK, "GetWaveformFrameInfo PCM16 stereo should succeed");
		// frame = 2 channels * 2 bytes = 4 bytes
		Check(frameSize     == 4,  "GetWaveformFrameInfo PCM16 stereo: frame_size expected 4");
		Check(numSamples    == 1,  "GetWaveformFrameInfo PCM16: num_frame_samples expected 1");
		Check(unitsPerFrame == 1,  "GetWaveformFrameInfo PCM16: units_per_frame expected 1");
		Check(delaySamples  == 0,  "GetWaveformFrameInfo PCM16: delay_samples expected 0");
	}

	// --- PCM 32-bit float mono (type 0x18) ---
	{
		Ngs2WaveformFormat fmt{};
		fmt.waveform_type = 0x18u;
		fmt.num_channels  = 1;
		fmt.sample_rate   = 44100;
		Check(Ngs2GetWaveformFrameInfo(&fmt, &frameSize, &numSamples, &unitsPerFrame, &delaySamples)
		      == OK, "GetWaveformFrameInfo PCM32F mono should succeed");
		// frame = 1 channel * 4 bytes = 4 bytes
		Check(frameSize     == 4,  "GetWaveformFrameInfo PCM32F mono: frame_size expected 4");
		Check(numSamples    == 1,  "GetWaveformFrameInfo PCM32F: num_frame_samples expected 1");
		Check(unitsPerFrame == 1,  "GetWaveformFrameInfo PCM32F: units_per_frame expected 1");
		Check(delaySamples  == 0,  "GetWaveformFrameInfo PCM32F: delay_samples expected 0");
	}

	// --- 24-bit 6-channel PCM (type 0x14) ---
	{
		Ngs2WaveformFormat fmt{};
		fmt.waveform_type = 0x14u;
		fmt.num_channels  = 6;
		fmt.sample_rate   = 48000;
		Check(Ngs2GetWaveformFrameInfo(&fmt, &frameSize, &numSamples, &unitsPerFrame, &delaySamples)
		      == OK, "GetWaveformFrameInfo PCM24 6ch should succeed");
		// frame = 6 * 3 = 18 bytes
		Check(frameSize == 18, "GetWaveformFrameInfo PCM24 6ch: frame_size expected 18");
	}

	// --- Unknown waveform type ---
	{
		Ngs2WaveformFormat fmt{};
		fmt.waveform_type = 0x99u; // unknown
		fmt.num_channels  = 2;
		fmt.sample_rate   = 48000;
		Check(Ngs2GetWaveformFrameInfo(&fmt, &frameSize, &numSamples, &unitsPerFrame, &delaySamples)
		      == NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "GetWaveformFrameInfo: unknown type should fail");
	}
}

// Validate using an in-memory handler that reads from a buffer.
struct ParseWaveformUserTestCtx {
	const std::vector<uint8_t>* data;
};

static int32_t KYTY_SYSV_ABI TestParseReadHandler(uintptr_t ud, uint32_t offset, void* buf, size_t sz) {
	const auto* ctx = reinterpret_cast<const ParseWaveformUserTestCtx*>(ud);
	if (ctx == nullptr || ctx->data == nullptr) {
		return -1;
	}
	if (offset + sz > ctx->data->size()) {
		return -1;
	}
	std::memcpy(buf, ctx->data->data() + offset, sz);
	return 0;
}

static void TestParseWaveformUserPhase2() {
	// --- Null guard ---
	{
		Ngs2WaveformInfo info{};
		Check(Ngs2ParseWaveformUser(0, 0, &info) != OK,
		      "ParseWaveformUser: handler=0 should fail");
		Check(Ngs2ParseWaveformUser(1, 0, nullptr) == NGS2_ERROR_INVALID_OUT_ADDRESS,
		      "ParseWaveformUser: out_info=null should return INVALID_OUT_ADDRESS");
	}

	// Build an in-memory RIFF/WAVE PCM I16 stereo buffer
	// Structure: RIFF <size> WAVE | fmt <16> <pcm_hdr> | data <size> <samples>
	// Use a 16-bit 48 kHz stereo WAV with 4 sample frames
	const uint16_t numChannels     = 2;
	const uint32_t sampleRate      = 48000;
	const uint16_t bitsPerSample   = 16;
	const uint16_t blockAlign      = numChannels * (bitsPerSample / 8);
	const uint32_t byteRate        = sampleRate * blockAlign;
	const uint32_t numFrames       = 4; // 4 interleaved stereo sample frames
	const uint32_t dataSize        = numFrames * blockAlign;

	// fmt chunk: 16 bytes
	std::vector<uint8_t> riff;
	auto appendU8  = [&](uint8_t v)  { riff.push_back(v); };
	auto appendLE16 = [&](uint16_t v) {
		riff.push_back(static_cast<uint8_t>(v));
		riff.push_back(static_cast<uint8_t>(v >> 8));
	};
	auto appendLE32 = [&](uint32_t v) {
		riff.push_back(static_cast<uint8_t>(v));
		riff.push_back(static_cast<uint8_t>(v >>  8));
		riff.push_back(static_cast<uint8_t>(v >> 16));
		riff.push_back(static_cast<uint8_t>(v >> 24));
	};
	auto appendFourCC = [&](const char* cc) {
		riff.push_back(static_cast<uint8_t>(cc[0]));
		riff.push_back(static_cast<uint8_t>(cc[1]));
		riff.push_back(static_cast<uint8_t>(cc[2]));
		riff.push_back(static_cast<uint8_t>(cc[3]));
	};
	(void)appendU8;

	// RIFF header (12 bytes)
	const uint32_t riffBodySize = 4 + 8 + 16 + 8 + dataSize; // "WAVE" + fmt chunk + data chunk
	appendFourCC("RIFF");
	appendLE32(riffBodySize);
	appendFourCC("WAVE");
	// fmt chunk (8+16 = 24 bytes)
	appendFourCC("fmt ");
	appendLE32(16);
	appendLE16(1);                   // PCM
	appendLE16(numChannels);
	appendLE32(sampleRate);
	appendLE32(byteRate);
	appendLE16(blockAlign);
	appendLE16(bitsPerSample);
	// data chunk
	appendFourCC("data");
	appendLE32(dataSize);
	for (uint32_t i = 0; i < numFrames * numChannels; ++i) {
		appendLE16(static_cast<uint16_t>(i * 100));
	}

	ParseWaveformUserTestCtx ctx{&riff};
	Ngs2WaveformInfo winfo{};
	const int parseResult = Ngs2ParseWaveformUser(
	    reinterpret_cast<uintptr_t>(&TestParseReadHandler),
	    reinterpret_cast<uintptr_t>(&ctx),
	    &winfo);
	Check(parseResult == OK, "ParseWaveformUser: parsing a valid PCM RIFF should succeed");
	Check(winfo.format.waveform_type == 0x12u, "ParseWaveformUser: waveform_type should be PCM I16");
	Check(winfo.format.num_channels  == numChannels, "ParseWaveformUser: num_channels mismatch");
	Check(winfo.format.sample_rate   == sampleRate,  "ParseWaveformUser: sample_rate mismatch");
	Check(winfo.num_samples          == numFrames,   "ParseWaveformUser: num_samples mismatch");

	// --- Handler returning a non-WAVE file (unknown format) ---
	{
		const std::vector<uint8_t> junk(32, 0x00);
		ParseWaveformUserTestCtx bctx{&junk};
		Ngs2WaveformInfo bad{};
		const int r = Ngs2ParseWaveformUser(
		    reinterpret_cast<uintptr_t>(&TestParseReadHandler),
		    reinterpret_cast<uintptr_t>(&bctx),
		    &bad);
		Check(r == NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT,
		      "ParseWaveformUser: Non-WAVE data should return UNKNOWN_WAVEFORM_FORMAT");
	}
}

static std::vector<uint8_t> MakeVagFile(uint32_t channelByte, uint32_t frames, uint32_t rate);

static void TestWaveformFileExtentLimits() {
	auto appendLE16 = [](std::vector<uint8_t>& v, uint16_t x) {
		v.push_back(static_cast<uint8_t>(x));
		v.push_back(static_cast<uint8_t>(x >> 8));
	};
	auto appendLE32 = [](std::vector<uint8_t>& v, uint32_t x) {
		v.push_back(static_cast<uint8_t>(x));
		v.push_back(static_cast<uint8_t>(x >> 8));
		v.push_back(static_cast<uint8_t>(x >> 16));
		v.push_back(static_cast<uint8_t>(x >> 24));
	};
	auto appendFourCC = [](std::vector<uint8_t>& v, const char* cc) {
		v.insert(v.end(), {static_cast<uint8_t>(cc[0]), static_cast<uint8_t>(cc[1]),
		                   static_cast<uint8_t>(cc[2]), static_cast<uint8_t>(cc[3])});
	};

	// Minimal PCM16 mono 8 kHz RIFF with a 4-byte data payload.
	std::vector<uint8_t> riff;
	appendFourCC(riff, "RIFF");
	appendLE32(riff, 4 + 8 + 16 + 8 + 4);
	appendFourCC(riff, "WAVE");
	appendFourCC(riff, "fmt ");
	appendLE32(riff, 16);
	appendLE16(riff, 1);
	appendLE16(riff, 1);
	appendLE32(riff, 8000);
	appendLE32(riff, 16000);
	appendLE16(riff, 2);
	appendLE16(riff, 16);
	appendFourCC(riff, "data");
	appendLE32(riff, 4);
	appendLE16(riff, 100);
	appendLE16(riff, 200);

	// Unique per-invocation directory so concurrent runs cannot collide; the
	// guard removes only this directory on scope exit.
	static std::atomic<uint32_t> dir_counter {0};
	const auto dir =
	    std::filesystem::temp_directory_path() /
	    ("ngs2_extent_" +
	     std::to_string(static_cast<uint64_t>(
	                        std::chrono::steady_clock::now().time_since_epoch().count())) +
	     "_" + std::to_string(dir_counter.fetch_add(1)));
	std::filesystem::create_directories(dir);
	struct DirGuard {
		std::filesystem::path path;
		~DirGuard() {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	} dir_guard {dir};
	g_ngs2GuestRoot = dir;
	const auto wavPath = (dir / "ngs2_extent_test.wav").string();
	const auto vagPath = (dir / "ngs2_extent_test.vag").string();
	const char* guestWav = "/app0/ngs2_extent_test.wav";
	const char* guestVag = "/app0/ngs2_extent_test.vag";

	// A small RIFF followed by trailing garbage must parse using only the
	// declared extent.
	{
		std::vector<uint8_t> file = riff;
		file.insert(file.end(), 4096, 0xAB);
		{
			std::ofstream out(wavPath, std::ios::binary);
			out.write(reinterpret_cast<const char*>(file.data()), file.size());
		}
		Ngs2WaveformInfo info {};
		Check(Ngs2ParseWaveformFile(guestWav, 0, &info) == OK,
		      "ParseWaveformFile rejected a RIFF with trailing data");
		Check(info.num_samples == 2 && info.data_offset == riff.size() - 4,
		      "ParseWaveformFile returned wrong geometry for trailing-data RIFF");
		// A host path outside the guest mount must not be opened directly.
		Check(Ngs2ParseWaveformFile(wavPath.c_str(), 0, &info) ==
		          NGS2_ERROR_INVALID_WAVEFORM_DATA,
		      "ParseWaveformFile opened a file outside the guest mount");
	}

	// A RIFF whose declared extent runs past the end of the file must fail
	// before the payload is read.
	{
		std::vector<uint8_t> file = riff;
		appendLE32(file, 0); // placeholder overwritten below
		file[4]  = 0xFF;
		file[5]  = 0xFF;
		file[6]  = 0xFF;
		file[7]  = 0x7F; // riff size ~2 GiB, far beyond the file
		{
			std::ofstream out(wavPath, std::ios::binary);
			out.write(reinterpret_cast<const char*>(file.data()), file.size());
		}
		Ngs2WaveformInfo info {};
		Check(Ngs2ParseWaveformFile(guestWav, 0, &info) == NGS2_ERROR_INVALID_WAVEFORM_DATA,
		      "ParseWaveformFile accepted a RIFF extent beyond the file size");
	}

	// A WAV smaller than the header probe must still parse: the probe is
	 // bounded by the remaining file size instead of failing the stream.
	{
		std::vector<uint8_t> wav46 = riff;
		wav46.resize(riff.size() - 2); // drop one PCM16 sample -> 46 bytes total
		const uint32_t size46 = static_cast<uint32_t>(wav46.size()) - 8;
		wav46[4]  = static_cast<uint8_t>(size46);
		wav46[5]  = static_cast<uint8_t>(size46 >> 8);
		wav46[6]  = static_cast<uint8_t>(size46 >> 16);
		wav46[7]  = static_cast<uint8_t>(size46 >> 24);
		wav46[40] = 2; // data chunk payload: a single PCM16 frame
		wav46[41] = 0;
		wav46[42] = 0;
		wav46[43] = 0;
		const auto smallPath = (dir / "ngs2_extent_small.wav").string();
		{
			std::ofstream out(smallPath, std::ios::binary);
			out.write(reinterpret_cast<const char*>(wav46.data()), wav46.size());
		}
		Ngs2WaveformInfo info {};
		Check(Ngs2ParseWaveformFile("/app0/ngs2_extent_small.wav", 0, &info) == OK,
		      "ParseWaveformFile rejected a 46-byte WAV");
		Check(info.num_samples == 1 && info.data_offset == 44,
		      "ParseWaveformFile returned wrong geometry for the 46-byte WAV");
	}

	// data_offset is added in 64 bits: an offset near the 32-bit wrap pushes
	// the payload past the ABI field and must be rejected.
	{
		const uint64_t big_offset = 0xfffffff0ull;
		const auto     big_path   = dir / "ngs2_extent_big.wav";
		{
			std::ofstream out(big_path, std::ios::binary | std::ios::in);
			out.seekp(static_cast<std::streamoff>(big_offset));
			out.write(reinterpret_cast<const char*>(riff.data()),
			          static_cast<std::streamsize>(riff.size()));
		}
		Ngs2WaveformInfo info {};
		Check(Ngs2ParseWaveformFile("/app0/ngs2_extent_big.wav", 0xfffffff0u, &info) ==
		          NGS2_ERROR_INVALID_WAVEFORM_DATA,
		      "ParseWaveformFile accepted an offset overflowing data_offset");
	}

	// A VAG file must be bounded by its declared payload as well.
	{
		auto vag = MakeVagFile(1, 2, 48000);
		vag[0x30 + 16 + 1] = 2; // mark the last frame so it is not dropped
		vag.insert(vag.end(), 2048, 0xCD);
		{
			std::ofstream out(vagPath, std::ios::binary);
			out.write(reinterpret_cast<const char*>(vag.data()), vag.size());
		}
		Ngs2WaveformInfo info {};
		Check(Ngs2ParseWaveformFile(guestVag, 0, &info) == OK,
		      "ParseWaveformFile rejected a valid VAG file");
		Check(info.format.waveform_type == SCE_NGS2_WAVEFORM_TYPE_VAG && info.num_samples == 56,
		      "ParseWaveformFile returned wrong VAG geometry");
	}
}

static void TestWaveformBlockLimits() {
	// VAG block math must stay in 64 bits: a sample position near the 32-bit
	// wrap used to produce a wrapped frame index and a huge data_size.
	Ngs2WaveformFormat vag8 {};
	vag8.waveform_type = SCE_NGS2_WAVEFORM_TYPE_VAG;
	vag8.num_channels  = 8;
	vag8.sample_rate   = 48000;
	Ngs2WaveformBlock block {};
	Check(Ngs2CalcWaveformBlock(&vag8, 0xFFFFFFF0u, 0x20u, &block) == OK,
	      "VAG CalcWaveformBlock failed near the 32-bit boundary");
	const uint64_t first_frame = 0xFFFFFFF0ull / 28;
	const uint64_t past_frame  = (0xFFFFFFF0ull + 0x20u + 27) / 28;
	Check(block.data_offset == first_frame * (16ull * 8) &&
	          block.data_size == (past_frame - first_frame) * (16ull * 8) &&
	          block.num_samples == 0x20 && block.num_skip_samples == 0xFFFFFFF0u % 28,
	      "VAG CalcWaveformBlock wrapped block arithmetic");

	// The requested sample count is reported even when it exceeds one frame;
	// data_size stays aligned to whole VAG frames.
	Check(Ngs2CalcWaveformBlock(&vag8, 0, 0xFFFFFFFFu, &block) == OK,
	      "VAG CalcWaveformBlock rejected a maximal sample span");
	Check(block.num_samples == 0xFFFFFFFFu &&
	          block.data_size == ((0xFFFFFFFFull + 27) / 28) * (16ull * 8),
	      "VAG CalcWaveformBlock did not report the requested sample count");
}

// Phase 3 Test: VAG Codec, Custom Gain, Channel Mixer, Pitch Shift DSP

static void PutBe32(std::vector<uint8_t>& data, size_t offset, uint32_t val) {
	for (size_t i = 0; i < 4; ++i) {
		data[offset + i] = static_cast<uint8_t>(val >> (24 - 8 * i));
	}
}

static std::vector<uint8_t> MakeVagFile(uint32_t channelByte, uint32_t frames, uint32_t rate = 48000) {
	const uint32_t channels = channelByte == 0 ? 1 : channelByte;
	std::vector<uint8_t> data(48 + frames * channels * 16u, 0);
	std::memcpy(data.data(), "VAGp", 4);
	PutBe32(data, 4, 0x20001);
	PutBe32(data, 0x0c, frames * channels * 16u);
	PutBe32(data, 0x10, rate);
	data[0x1e] = static_cast<uint8_t>(channelByte);
	return data;
}

static void SetVagUnit(std::vector<uint8_t>& data, uint32_t channels, uint32_t frame, uint32_t ch,
                       uint8_t predictorShift, uint8_t flags, uint8_t nibbles) {
	uint8_t* unit = data.data() + 48 + (frame * channels + ch) * 16u;
	unit[0] = predictorShift;
	unit[1] = flags;
	std::memset(unit + 2, nibbles, 14);
}

static void TestVagPhase3() {
	// 1. VAG header parsing
	auto vagData = MakeVagFile(2, 3, 44100); // 2 channels, 3 frames (each frame block: 2*16 = 32 bytes)
	// Set the end flag on the second frame so it is not treated as a dropped frame
	SetVagUnit(vagData, 2, 1, 0, 0, 2, 0);

	Ngs2WaveformInfo info {};
	int res = Ngs2ParseWaveformData(vagData.data(), vagData.size(), &info);
	Check(res == OK, "VAG Parse: parsing failed for a valid VAG file");
	Check(info.format.waveform_type == SCE_NGS2_WAVEFORM_TYPE_VAG, "VAG Parse: waveform_type is wrong");
	Check(info.format.num_channels == 2, "VAG Parse: channels are wrong");
	Check(info.format.sample_rate == 44100, "VAG Parse: sample_rate is wrong");
	Check(info.audio_unit_size == 32, "VAG Parse: audio_unit_size is wrong");
	Check(info.num_audio_unit_samples == 28, "VAG Parse: num_audio_unit_samples is wrong");
	Check(info.num_samples == 84, "VAG Parse: num_samples is wrong for 3 frames");
	Check(info.num_blocks == 1, "VAG Parse: num_blocks is wrong");

	// 2. VAG loop parsing
	auto loopVag = MakeVagFile(1, 4, 48000); // 1 channel, 4 frames
	// Frame 1: loop start (flag 6) -> sample 28
	SetVagUnit(loopVag, 1, 1, 0, 0, 6, 0);
	// Frame 2: loop end (flag 3) -> sample 84
	SetVagUnit(loopVag, 1, 2, 0, 0, 3, 0);
	Ngs2WaveformInfo loopInfo {};
	res = Ngs2ParseWaveformData(loopVag.data(), loopVag.size(), &loopInfo);
	Check(res == OK, "VAG Parse Loop: parsing failed");
	Check(loopInfo.loop_begin_position == 28, "VAG Parse Loop: loop_begin is wrong");
	Check(loopInfo.loop_end_position == 84, "VAG Parse Loop: loop_end is wrong");
	Check(loopInfo.num_blocks == 3, "VAG Parse Loop: num_blocks should be 3");
	Check(loopInfo.blocks[0].num_repeats == 0, "VAG Parse Loop: block 0 must not repeat");
	Check(loopInfo.blocks[1].num_repeats == UINT32_MAX, "VAG Parse Loop: block 1 must repeat forever");

	// 3. Ngs2CalcWaveformBlock for VAG
	Ngs2WaveformFormat vagFmt {};
	vagFmt.waveform_type = SCE_NGS2_WAVEFORM_TYPE_VAG;
	vagFmt.num_channels  = 1;
	vagFmt.sample_rate   = 48000;
	Ngs2WaveformBlock calcBlock {};
	res = Ngs2CalcWaveformBlock(&vagFmt, 10, 50, &calcBlock);
	Check(res == OK, "VAG CalcBlock failed");
	Check(calcBlock.data_offset == 0, "VAG CalcBlock offset is wrong");
	Check(calcBlock.data_size == 48, "VAG CalcBlock data_size is wrong (3 frames * 16 bytes)");
	Check(calcBlock.num_skip_samples == 10, "VAG CalcBlock skip samples are wrong");
	Check(calcBlock.num_samples == 50, "VAG CalcBlock num_samples is wrong (requested count)");

	// 4. Ngs2GetWaveformFrameInfo for VAG
	uint32_t frameSize = 0, frameSamples = 0, unitsPerFrame = 0, delaySamples = 0;
	res = Ngs2GetWaveformFrameInfo(&vagFmt, &frameSize, &frameSamples, &unitsPerFrame, &delaySamples);
	Check(res == OK, "VAG GetWaveformFrameInfo failed");
	Check(frameSize == 16, "VAG GetWaveformFrameInfo frameSize is wrong");
	Check(frameSamples == 28, "VAG GetWaveformFrameInfo frameSamples is wrong");
	Check(delaySamples == 0, "VAG GetWaveformFrameInfo delaySamples is wrong");

	// 5. ADPCM VAG decoding test with a real render
	// Build a VAG file with frame 0 = nibble 7 (shift 12, pred 0) and frame 1 = nibble 0 (shift 12, pred 1: coeff 60)
	auto decData = MakeVagFile(1, 2, 48000);
	SetVagUnit(decData, 1, 0, 0, 0x0c, 0, 0x77); // Frame 0: shift 12, pred 0 -> samples = 7
	SetVagUnit(decData, 1, 1, 0, 0x1c, 0, 0x00); // Frame 1: shift 12, pred 1 -> ADPCM decay

	Ngs2WaveformBlock testBlocks[2] = {
		{48, 16, 0, 0, 28, 0, 0},
		{64, 16, 0, 0, 28, 0, 0}
	};

	uintptr_t sysHandle = 0;
	Ngs2SystemOption sysOpt = Ngs2DefaultSystemOption();
	sysOpt.num_grain_samples = 64;
	sysOpt.max_grain_samples = 64;
	sysOpt.sample_rate       = 48000;
	sysOpt.max_voice_channels = 2;
	std::vector<uint8_t> sysBuf(sizeof(Ngs2Internal));
	Ngs2ContextBufferInfo sbi {sysBuf.data(), sysBuf.size(), {0}};
	res = Ngs2SystemCreate(&sysOpt, &sbi, &sysHandle);
	Check(res == OK && sysHandle != 0, "SystemCreate failed for VAG test");

	uintptr_t samplerRack = 0;
	Ngs2RackOptionUnion sOption {};
	Ngs2FillDefaultRackOption(0x1000, &sOption);
	sOption.sampler.rack_option.max_voices = 1;
	sOption.sampler.rack_option.max_grain_samples = 64;
	std::vector<uint8_t> rackBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbi {rackBuf.data(), rackBuf.size(), {0}};
	res = Ngs2RackCreate(sysHandle, 0x1000, &sOption.sampler.rack_option, &rbi, &samplerRack);
	Check(res == OK && samplerRack != 0, "RackCreate Sampler failed for VAG test");

	uintptr_t masterRack = 0;
	Ngs2RackOptionUnion mOption {};
	Ngs2FillDefaultRackOption(0x3000, &mOption);
	mOption.mastering.rack_option.max_voices = 1;
	mOption.mastering.rack_option.max_grain_samples = 64;
	std::vector<uint8_t> mBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo mbi {mBuf.data(), mBuf.size(), {0}};
	res = Ngs2RackCreate(sysHandle, 0x3000, &mOption.mastering.rack_option, &mbi, &masterRack);
	Check(res == OK && masterRack != 0, "RackCreate Mastering failed for VAG test");

	uintptr_t sVoice = 0, mVoice = 0;
	Ngs2RackGetVoiceHandle(samplerRack, 0, &sVoice);
	Ngs2RackGetVoiceHandle(masterRack, 0, &mVoice);

	// Setup master: 1 channel
	struct { Ngs2VoiceParamHeader h; uint32_t ch; } mSetup {{sizeof(Ngs2VoiceParamHeader) + 4, 0, 0x30000000}, 1};
	Ngs2VoiceControl(mVoice, &mSetup.h);
	struct { Ngs2VoiceParamHeader h; uint32_t outId; uint32_t r; } mOut {{sizeof(Ngs2VoiceParamHeader) + 8, 0, 0x30000005}, 0, 0};
	Ngs2VoiceControl(mVoice, &mOut.h);
	Ngs2VoiceEventParam mPlay {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 1};
	Ngs2VoiceControl(mVoice, &mPlay.header);

	// Setup sampler for VAG
	struct { Ngs2VoiceParamHeader h; Ngs2WaveformFormat fmt; } sSetup {
		{sizeof(Ngs2VoiceParamHeader) + sizeof(Ngs2WaveformFormat), 0, 0x10000000},
		{SCE_NGS2_WAVEFORM_TYPE_VAG, 1, 48000, 0, 0, 0}
	};
	Ngs2VoiceControl(sVoice, &sSetup.h);

	// Patch sampler -> master
	Ngs2VoicePatchParam patch {{sizeof(Ngs2VoicePatchParam), 0, 0x00000005}, 0, 0, masterRack};
	Ngs2VoiceControl(sVoice, &patch.header);

	// Add VAG blocks
	struct {
		Ngs2VoiceParamHeader h;
		const uint8_t* data;
		uint32_t flags, count;
		const Ngs2WaveformBlock* blocks;
	} addBlocks {
		{sizeof(Ngs2VoiceParamHeader) + 24, 0, 0x10000001},
		decData.data(), 0, 2, testBlocks
	};
	Ngs2VoiceControl(sVoice, &addBlocks.h);

	// Play
	Ngs2VoiceControl(sVoice, &mPlay.header);

	// Render
	std::vector<float> renderOut(64, 0.0f);
	Ngs2RenderBufferInfo rinfo {renderOut.data(), renderOut.size() * sizeof(float), 0x18, 1};
	res = Ngs2SystemRender(sysHandle, &rinfo, 1);
	Check(res == OK, "SystemRender failed for VAG test");

	// Verify: the first 28 samples must be ~ 7 / 32768.0f
	const float expected7 = 7.0f / 32768.0f;
	for (size_t i = 0; i < 28; ++i) {
		Check(std::abs(renderOut[i] - expected7) < 1e-4f, "VAG decode sample != 7 in frame 0");
	}
	// And the following samples of frame 1 must follow the decay {6, 5, 4, 3, 2, 1, 0, 0}
	constexpr int32_t decay[8] = {6, 5, 4, 3, 2, 1, 0, 0};
	for (size_t i = 0; i < 8; ++i) {
		const float expDecay = decay[i] / 32768.0f;
		Check(std::abs(renderOut[28 + i] - expDecay) < 1e-4f, "VAG ADPCM decay is wrong");
	}

	Ngs2RackDestroy(samplerRack, nullptr);
	Ngs2RackDestroy(masterRack, nullptr);
	Ngs2SystemDestroy(sysHandle, nullptr);
}

static void TestVagSetup(Fixture& f, uint32_t rate) {
	struct {
		Ngs2VoiceParamHeader h;
		Ngs2WaveformFormat   fmt;
	} setup {{sizeof(Ngs2VoiceParamHeader) + sizeof(Ngs2WaveformFormat), 0, 0x10000000},
	         {SCE_NGS2_WAVEFORM_TYPE_VAG, 1, rate, 0, 0, 0}};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &setup.h) == OK,
	      "VAG sampler setup failed");
	f.voice.state = Ngs2VoicePlayState::Playing;
}

static void TestVagPhase3Fixes() {
	// 3.2: predictor history must persist across frames of the same block.
	{
		Fixture f(48000);
		TestVagSetup(f, 48000);
		auto vag = MakeVagFile(1, 2, 48000);
		SetVagUnit(vag, 1, 0, 0, 0x0c, 0, 0x77); // pred 0 -> constant 7
		SetVagUnit(vag, 1, 1, 0, 0x1c, 0, 0x00); // pred 1 -> ADPCM decay from history
		Ngs2WaveformBlock block {48, 32, 0, 0, 56, 0, 0};
		Check(f.QueueBlocks(vag.data(), &block, 1, 0) == OK, "VAG history queue failed");
		f.Render(56);
		const float seven = 7.0f / 32768.0f;
		for (uint32_t i = 0; i < 28; ++i) {
			Check(std::abs(f.voice.samples[i] - seven) < 1e-4f,
			      "VAG frame 0 did not decode to constant 7");
		}
		constexpr int32_t decay[8] = {6, 5, 4, 3, 2, 1, 0, 0};
		for (uint32_t i = 0; i < 8; ++i) {
			Check(std::abs(f.voice.samples[28 + i] - decay[i] / 32768.0f) < 1e-4f,
			      "VAG predictor history was lost inside the block");
		}
	}

	// 3.1: at 24 kHz on a 48 kHz system the lookahead decode must not rewind
	// the cursor of the frame still being played.
	{
		Fixture f(48000);
		TestVagSetup(f, 24000);
		auto vag = MakeVagFile(1, 2, 24000);
		SetVagUnit(vag, 1, 0, 0, 0x0c, 0, 0x77); // frame 0: constant 7
		SetVagUnit(vag, 1, 1, 0, 0x0c, 0, 0x00); // frame 1: constant 0
		Ngs2WaveformBlock block {48, 32, 0, 0, 56, 0, 0};
		Check(f.QueueBlocks(vag.data(), &block, 1, 0) == OK, "VAG lookahead queue failed");
		f.Render(128);
		const float seven = 7.0f / 32768.0f;
		Check(std::abs(f.voice.samples[10] - seven) < 1e-4f &&
		          std::abs(f.voice.samples[50] - seven) < 1e-4f,
		      "VAG resampled frame 0 did not play constant 7");
		for (uint32_t i = 70; i < 110; ++i) {
			Check(std::abs(f.voice.samples[i]) < 1e-4f,
			      "VAG lookahead replayed the previous frame at the boundary");
		}
	}

	// 7.1: a staged frame with skipped samples must interpolate toward its
	// first playable sample. 24 kHz source on a 48 kHz system, a 4-sample
	// jump and a 0 -> 0.5 transition give 0.25 at the boundary.
	{
		Fixture f(48000);
		TestVagSetup(f, 24000);
		auto vag = MakeVagFile(1, 2, 24000);
		SetVagUnit(vag, 1, 0, 0, 0x00, 0, 0x00); // frame 0: constant 0
		SetVagUnit(vag, 1, 1, 0, 0x00, 0, 0x44); // frame 1: constant 0.5
		// The first four decoded samples of frame 1 differ from the rest so a
		// skipped prefix is observable at the boundary.
		uint8_t* unit1 = vag.data() + 48 + 16;
		unit1[2]       = 0x00;
		unit1[3]       = 0x00;
		Ngs2WaveformBlock blocks[2] = {
		    {48, 16, 0, 0, 28, 0, 0},
		    {64, 16, 0, 4, 24, 0, 0},
		};
		Check(f.QueueBlocks(vag.data(), blocks, 2, 0) == OK, "VAG skip queue failed");
		f.Render(128);
		Check(std::abs(f.voice.samples[54]) < 1e-4f, "VAG frame 0 tail is not silent");
		Check(std::abs(f.voice.samples[55] - 0.25f) < 1e-4f,
		      "VAG lookahead ignored the staged frame's skipped samples");
		Check(std::abs(f.voice.samples[56] - 0.5f) < 1e-4f,
		      "VAG frame 1 did not continue at 0.5 after the boundary");
	}

	// 3.3: CLEAR with a replacement block must keep the VAG decoder selected.
	{
		Fixture f(48000);
		TestVagSetup(f, 48000);
		auto vag = MakeVagFile(1, 1, 48000);
		SetVagUnit(vag, 1, 0, 0, 0x0c, 0, 0x77);
		Ngs2WaveformBlock block {48, 16, 0, 0, 28, 0, 0};
		Check(f.QueueBlocks(vag.data(), &block, 1, 0) == OK, "VAG clear queue failed");
		Check(f.voice.is_vag, "VAG setup did not select the VAG decoder");
		Check(f.QueueBlocks(vag.data(), &block, 1, 4) == OK,
		      "VAG CLEAR queue failed");
		Check(f.voice.is_vag, "CLEAR dropped the replacement block onto the PCM decoder");
		f.Render(28);
		Check(std::abs(f.voice.samples[0] - 7.0f / 32768.0f) < 1e-4f,
		      "VAG block after CLEAR did not decode as VAG");
	}
}

static void TestCustomGainPhase3() {
	uintptr_t sysHandle = 0;
	Ngs2SystemOption sysOpt = Ngs2DefaultSystemOption();
	sysOpt.num_grain_samples  = 64;
	sysOpt.max_grain_samples  = 64;
	sysOpt.sample_rate        = 48000;
	sysOpt.max_voice_channels = 2;
	std::vector<uint8_t> sysBuf(sizeof(Ngs2Internal));
	Ngs2ContextBufferInfo sbi {sysBuf.data(), sysBuf.size(), {0}};
	int res = Ngs2SystemCreate(&sysOpt, &sbi, &sysHandle);
	Check(res == OK && sysHandle != 0, "SystemCreate failed for Gain test");

	// Custom Submixer Rack with Gain module (ID 0x15)
	Ngs2CustomSubmixerRackOption subOpt {};
	auto& custom = subOpt.custom_rack_option;
	custom.rack_option.size              = sizeof(subOpt);
	custom.rack_option.max_grain_samples = 64;
	custom.rack_option.max_voices        = 1;
	custom.rack_option.max_matrices      = 1;
	custom.rack_option.max_ports         = 1;
	custom.num_buffers                   = 1;
	custom.num_modules                   = 1;
	custom.module[0].module_id           = SCE_NGS2_CUSTOM_MODULE_ID_GAIN;
	custom.module[0].source_buffer_id    = 0;
	custom.module[0].dest_buffer_id      = 0;
	subOpt.max_channels                  = 2;
	subOpt.max_inputs                    = 1;

	std::vector<uint8_t> rBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbi {rBuf.data(), rBuf.size(), {0}};
	uintptr_t gainRack = 0;
	res = Ngs2RackCreate(sysHandle, 0x4002, &custom.rack_option, &rbi, &gainRack);
	Check(res == OK && gainRack != 0, "RackCreate CustomSubmixer failed for Gain test");

	uintptr_t voice = 0;
	Ngs2RackGetVoiceHandle(gainRack, 0, &voice);

	// Setup custom submixer: 2 channels
	Ngs2SubmixerVoiceSetupParam vsetup {{sizeof(Ngs2SubmixerVoiceSetupParam), 0, 0x20000000}, 2, 0};
	Ngs2VoiceControl(voice, &vsetup.header);

	// Set the initial gain via SCE_NGS2_CUSTOM_VOICE_PARAM_GAIN (0x40001500)
	Ngs2CustomVoiceGainParam gparam {};
	gparam.header = {sizeof(Ngs2CustomVoiceGainParam), 0, SCE_NGS2_CUSTOM_VOICE_PARAM_GAIN};
	gparam.gain[0] = 2.0f;
	gparam.gain[1] = 0.5f;
	res = Ngs2VoiceControl(voice, &gparam.header);
	Check(res == OK, "VoiceControl Gain failed");

	// Start the voice
	Ngs2VoiceEventParam playEv {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 1};
	Ngs2VoiceControl(voice, &playEv.header);

	// Simulate input samples through a connected source
	auto* vInt = reinterpret_cast<Ngs2VoiceInternal*>(voice);
	Fixture src(48000, 2);
	src.voice.ports.push_back({vInt, 0, 1.0f, -1});
	int16_t ones[64 * 2];
	for (int i = 0; i < 64 * 2; ++i) ones[i] = 32767;
	src.Queue(ones, 64, 0);

	// Set the target gain to test the ramp
	gparam.gain[0] = 4.0f;
	gparam.gain[1] = 0.0f;
	Ngs2VoiceControl(voice, &gparam.header);

	// Gain module processing
	std::vector<Ngs2VoiceInternal*> voicesList {&src.voice};
	Ngs2RenderVoice(*vInt, voicesList, 64);

	float* b = vInt->Buffer(0, 64);

	// Verification: The first sample must start from the previous value (2.0f and 0.5f)
	// and the last sample from the ramp should approach the new target (4.0f and 0.0f)
	Check(std::abs(b[0] - 2.0f) < 0.01f, "Gain ramp ch0 start is wrong");
	Check(std::abs(b[64] - 0.5f) < 0.01f, "Gain ramp ch1 start is wrong");
	Check(b[63] > 3.8f, "Gain ramp ch0 end does not reach the target");
	Check(b[64 + 63] < 0.1f, "Gain ramp ch1 end does not reach the target");

	Ngs2RackDestroy(gainRack, nullptr);
	Ngs2SystemDestroy(sysHandle, nullptr);
}

static void TestCustomChannelMixerPhase3() {
	uintptr_t sysHandle = 0;
	Ngs2SystemOption sysOpt = Ngs2DefaultSystemOption();
	sysOpt.num_grain_samples  = 64;
	sysOpt.max_grain_samples  = 64;
	sysOpt.sample_rate        = 48000;
	sysOpt.max_voice_channels = 2;
	std::vector<uint8_t> sysBuf(sizeof(Ngs2Internal));
	Ngs2ContextBufferInfo sbi {sysBuf.data(), sysBuf.size(), {0}};
	int res = Ngs2SystemCreate(&sysOpt, &sbi, &sysHandle);
	Check(res == OK && sysHandle != 0, "SystemCreate failed for Mixer test");

	// Custom Submixer with Channel Mixer module (ID 0x17)
	Ngs2CustomSubmixerRackOption subOpt {};
	auto& custom = subOpt.custom_rack_option;
	custom.rack_option.size              = sizeof(subOpt);
	custom.rack_option.max_grain_samples = 64;
	custom.rack_option.max_voices        = 1;
	custom.rack_option.max_matrices      = 1;
	custom.rack_option.max_ports         = 1;
	custom.num_buffers                   = 1;
	custom.num_modules                   = 1;
	custom.module[0].module_id           = SCE_NGS2_CUSTOM_MODULE_ID_CHANNEL_MIXER;
	custom.module[0].source_buffer_id    = 0;
	custom.module[0].dest_buffer_id      = 0;
	subOpt.max_channels                  = 2;
	subOpt.max_inputs                    = 1;

	std::vector<uint8_t> rBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbi {rBuf.data(), rBuf.size(), {0}};
	uintptr_t mixerRack = 0;
	res = Ngs2RackCreate(sysHandle, 0x4002, &custom.rack_option, &rbi, &mixerRack);
	Check(res == OK && mixerRack != 0, "RackCreate CustomSubmixer failed for Mixer test");

	uintptr_t voice = 0;
	Ngs2RackGetVoiceHandle(mixerRack, 0, &voice);

	Ngs2SubmixerVoiceSetupParam vsetup {{sizeof(Ngs2SubmixerVoiceSetupParam), 0, 0x20000000}, 2, 0};
	Ngs2VoiceControl(voice, &vsetup.header);

	// Configure the channel mixer matrix: swap channels (L -> R, R -> L)
	Ngs2CustomVoiceChannelMixerParam mparam {};
	mparam.header = {sizeof(Ngs2CustomVoiceChannelMixerParam), 0, SCE_NGS2_CUSTOM_VOICE_PARAM_CHANNEL_MIXER};
	mparam.level[0][1] = 1.0f; // Dst 0 (L) receives Src 1 (R)
	mparam.level[1][0] = 1.0f; // Dst 1 (R) receives Src 0 (L)
	res = Ngs2VoiceControl(voice, &mparam.header);
	Check(res == OK, "VoiceControl ChannelMixer failed");

	Ngs2VoiceEventParam playEv {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 1};
	Ngs2VoiceControl(voice, &playEv.header);

	auto* vInt = reinterpret_cast<Ngs2VoiceInternal*>(voice);
	Fixture src(48000, 2);
	src.voice.ports.push_back({vInt, 0, 1.0f, -1});
	int16_t mixInput[64 * 2];
	for (int i = 0; i < 64; ++i) {
		mixInput[2 * i]     = 24576; // Ch 0 = 0.75f
		mixInput[2 * i + 1] = 8192;  // Ch 1 = 0.25f
	}
	src.Queue(mixInput, 64, 0);

	std::vector<Ngs2VoiceInternal*> voicesList {&src.voice};
	Ngs2RenderVoice(*vInt, voicesList, 64);

	float* b = vInt->Buffer(0, 64);

	// After mixing the channels must be swapped: Ch 0 receives 0.25f, Ch 1 receives 0.75f
	for (size_t i = 0; i < 64; ++i) {
		Check(std::abs(b[i] - 0.25f) < 0.01f, "ChannelMixer Ch0 swap is wrong");
		Check(std::abs(b[64 + i] - 0.75f) < 0.01f, "ChannelMixer Ch1 swap is wrong");
	}

	Ngs2RackDestroy(mixerRack, nullptr);
	Ngs2SystemDestroy(sysHandle, nullptr);
}

static void TestCustomPitchShiftPhase3() {
	uintptr_t sysHandle = 0;
	Ngs2SystemOption sysOpt = Ngs2DefaultSystemOption();
	sysOpt.num_grain_samples  = 64;
	sysOpt.max_grain_samples  = 64;
	sysOpt.sample_rate        = 48000;
	sysOpt.max_voice_channels = 2;
	std::vector<uint8_t> sysBuf(sizeof(Ngs2Internal));
	Ngs2ContextBufferInfo sbi {sysBuf.data(), sysBuf.size(), {0}};
	int res = Ngs2SystemCreate(&sysOpt, &sbi, &sysHandle);
	Check(res == OK && sysHandle != 0, "SystemCreate failed for PitchShift test");

	// Custom Submixer with Pitch Shift module (ID 0x20)
	Ngs2CustomSubmixerRackOption subOpt {};
	auto& custom = subOpt.custom_rack_option;
	custom.rack_option.size              = sizeof(subOpt);
	custom.rack_option.max_grain_samples = 64;
	custom.rack_option.max_voices        = 1;
	custom.rack_option.max_matrices      = 1;
	custom.rack_option.max_ports         = 1;
	custom.num_buffers                   = 1;
	custom.num_modules                   = 1;
	custom.module[0].module_id           = SCE_NGS2_CUSTOM_MODULE_ID_PITCH_SHIFT;
	custom.module[0].source_buffer_id    = 0;
	custom.module[0].dest_buffer_id      = 0;
	subOpt.max_channels                  = 2;
	subOpt.max_inputs                    = 1;

	std::vector<uint8_t> rBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbi {rBuf.data(), rBuf.size(), {0}};
	uintptr_t pitchRack = 0;
	res = Ngs2RackCreate(sysHandle, 0x4002, &custom.rack_option, &rbi, &pitchRack);
	Check(res == OK && pitchRack != 0, "RackCreate CustomSubmixer failed for PitchShift test");

	uintptr_t voice = 0;
	Ngs2RackGetVoiceHandle(pitchRack, 0, &voice);

	Ngs2SubmixerVoiceSetupParam vsetup {{sizeof(Ngs2SubmixerVoiceSetupParam), 0, 0x20000000}, 2, 0};
	Ngs2VoiceControl(voice, &vsetup.header);

	// Set pitch shift: +700 cents, channel 1 bypassed (bypass_mask = 2: bit 1 set)
	Ngs2CustomVoicePitchShiftParam pparam {};
	pparam.header = {sizeof(Ngs2CustomVoicePitchShiftParam), 0, SCE_NGS2_CUSTOM_VOICE_PARAM_PITCH_SHIFT};
	pparam.cents  = 700;
	pparam.bypass_mask = 2; // bypass Ch 1
	res = Ngs2VoiceControl(voice, &pparam.header);
	Check(res == OK, "VoiceControl PitchShift failed");

	Ngs2VoiceEventParam playEv {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 1};
	Ngs2VoiceControl(voice, &playEv.header);

	auto* vInt = reinterpret_cast<Ngs2VoiceInternal*>(voice);
	Fixture src(48000, 2);
	src.voice.ports.push_back({vInt, 0, 1.0f, -1});
	int16_t psInput[64 * 2];
	for (int i = 0; i < 64; ++i) {
		psInput[2 * i]     = 16384; // Ch 0 = 0.5f
		psInput[2 * i + 1] = 10922; // Ch 1 = ~0.333f
	}
	src.Queue(psInput, 64, 0);

	std::vector<Ngs2VoiceInternal*> voicesList {&src.voice};
	Ngs2RenderVoice(*vInt, voicesList, 64);

	float* b = vInt->Buffer(0, 64);

	// Channel 1 is bypassed, so its samples must not be modified by the processor
	for (size_t i = 0; i < 64; ++i) {
		Check(std::abs(b[64 + i] - (10922.0f / 32768.0f)) < 0.01f, "PitchShift Ch1 bypass is wrong, value was modified");
	}

	Ngs2RackDestroy(pitchRack, nullptr);
	Ngs2SystemDestroy(sysHandle, nullptr);
}

void TestCustomFilterModule() {
	Ngs2SystemOption opt {};
	Ngs2SystemResetOption(&opt);
	opt.num_grain_samples = 64;
	opt.sample_rate = 48000;

	Ngs2ContextBufferInfo sysBufInfo {};
	Ngs2SystemQueryBufferSize(&opt, &sysBufInfo);
	std::vector<uint8_t> sysBuf(sysBufInfo.host_buffer_size);
	sysBufInfo.host_buffer = sysBuf.data();

	uintptr_t sysHandle = 0;
	int res = Ngs2SystemCreate(&opt, &sysBufInfo, &sysHandle);
	Check(res == OK && sysHandle != 0, "SystemCreate failed for CustomFilter test");

	Ngs2CustomSubmixerRackOption subOpt {};
	auto& custom = subOpt.custom_rack_option;
	custom.rack_option.size              = sizeof(subOpt);
	custom.rack_option.max_grain_samples = 64;
	custom.rack_option.max_voices        = 1;
	custom.rack_option.max_matrices      = 1;
	custom.rack_option.max_ports         = 1;
	custom.num_buffers                   = 1;
	custom.num_modules                   = 1;
	custom.module[0].module_id           = SCE_NGS2_CUSTOM_MODULE_ID_FILTER;
	custom.module[0].source_buffer_id    = 0;
	custom.module[0].dest_buffer_id      = 0;
	subOpt.max_channels                  = 2;
	subOpt.max_inputs                    = 1;

	std::vector<uint8_t> rBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbi {rBuf.data(), rBuf.size(), {0}};
	uintptr_t filterRack = 0;
	res = Ngs2RackCreate(sysHandle, 0x4002, &custom.rack_option, &rbi, &filterRack);
	Check(res == OK && filterRack != 0, "RackCreate CustomSubmixer failed for Filter test");

	uintptr_t voice = 0;
	Ngs2RackGetVoiceHandle(filterRack, 0, &voice);

	Ngs2SubmixerVoiceSetupParam vsetup {{sizeof(Ngs2SubmixerVoiceSetupParam), 0, 0x20000000}, 2, 0};
	Ngs2VoiceControl(voice, &vsetup.header);

	// Set a biquad low-pass filter at 1000 Hz, Q=0.707, Level=1.0
	Ngs2CustomVoiceFilterParam fparam {};
	fparam.header = {sizeof(Ngs2CustomVoiceFilterParam), 0, SCE_NGS2_CUSTOM_VOICE_PARAM_FILTER};
	fparam.channel_mask = 0; // both channels filtered
	fparam.type = 1;         // Low-pass
	fparam.param.fcq.frequency = 1000.0f;
	fparam.param.fcq.q         = 0.707f;
	fparam.param.fcq.level     = 1.0f;
	res = Ngs2VoiceControl(voice, &fparam.header);
	Check(res == OK, "VoiceControl CustomFilter 0x40001300 failed");

	Ngs2VoiceEventParam playEv {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 1};
	Ngs2VoiceControl(voice, &playEv.header);

	auto* vInt = reinterpret_cast<Ngs2VoiceInternal*>(voice);
	Fixture src(48000, 2);
	src.voice.ports.push_back({vInt, 0, 1.0f, -1});
	int16_t fInput[64 * 2];
	for (int i = 0; i < 64; ++i) {
		fInput[2 * i]     = 16384; // Ch 0 = 0.5f (DC step)
		fInput[2 * i + 1] = 16384; // Ch 1 = 0.5f
	}
	src.Queue(fInput, 64, 0);

	std::vector<Ngs2VoiceInternal*> voicesList {&src.voice};
	Ngs2RenderVoice(*vInt, voicesList, 64);

	float* b = vInt->Buffer(0, 64);
	// The 1000 Hz low-pass filter smooths the DC step: at the start b[0] is below the steady-state value
	Check(b[0] > 0.0f && b[0] < 0.5f, "Custom Filter Ch0 output unexpected");
	Check(b[64] > 0.0f && b[64] < 0.5f, "Custom Filter Ch1 output unexpected");

	Ngs2RackDestroy(filterRack, nullptr);
	Ngs2SystemDestroy(sysHandle, nullptr);
}

void TestCustomFilterChannelRelayout() {
	Ngs2SystemOption opt {};
	Ngs2SystemResetOption(&opt);
	opt.num_grain_samples = 64;
	opt.sample_rate       = 48000;

	Ngs2ContextBufferInfo sysBufInfo {};
	Ngs2SystemQueryBufferSize(&opt, &sysBufInfo);
	std::vector<uint8_t> sysBuf(sysBufInfo.host_buffer_size);
	sysBufInfo.host_buffer = sysBuf.data();

	uintptr_t sysHandle = 0;
	int res = Ngs2SystemCreate(&opt, &sysBufInfo, &sysHandle);
	Check(res == OK && sysHandle != 0, "SystemCreate failed for FilterRelayout test");

	Ngs2CustomSubmixerRackOption subOpt {};
	auto& custom = subOpt.custom_rack_option;
	custom.rack_option.size              = sizeof(subOpt);
	custom.rack_option.max_grain_samples = 64;
	custom.rack_option.max_voices        = 1;
	custom.rack_option.max_matrices      = 1;
	custom.rack_option.max_ports         = 1;
	custom.num_buffers                   = 1;
	custom.num_modules                   = 1;
	custom.module[0].module_id           = SCE_NGS2_CUSTOM_MODULE_ID_FILTER;
	custom.module[0].source_buffer_id    = 0;
	custom.module[0].dest_buffer_id      = 0;
	subOpt.max_channels                  = 2;
	subOpt.max_inputs                    = 1;

	std::vector<uint8_t> rBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbi {rBuf.data(), rBuf.size(), {0}};
	uintptr_t filterRack = 0;
	res = Ngs2RackCreate(sysHandle, 0x4002, &custom.rack_option, &rbi, &filterRack);
	Check(res == OK && filterRack != 0, "RackCreate CustomSubmixer failed for FilterRelayout");

	uintptr_t voice = 0;
	Ngs2RackGetVoiceHandle(filterRack, 0, &voice);
	auto* vInt = reinterpret_cast<Ngs2VoiceInternal*>(voice);

	// Configure the module filter while the voice runs on one channel. The
	// custom-submixer setup control carries two equal channel counts.
	Ngs2SubmixerVoiceSetupParam vsetup1 {{sizeof(Ngs2SubmixerVoiceSetupParam), 0, 0x40020000}, 1, 1};
	Ngs2VoiceControl(voice, &vsetup1.header);

	Ngs2CustomVoiceFilterParam fparam {};
	fparam.header = {sizeof(Ngs2CustomVoiceFilterParam), 0, SCE_NGS2_CUSTOM_VOICE_PARAM_FILTER};
	fparam.channel_mask = 0;
	fparam.type = 1;
	fparam.param.fcq.frequency = 1000.0f;
	fparam.param.fcq.q         = 0.707f;
	fparam.param.fcq.level     = 1.0f;
	res = Ngs2VoiceControl(voice, &fparam.header);
	Check(res == OK, "VoiceControl CustomFilter relayout failed");
	Check(vInt->modules[0].filter.enabled && vInt->modules[0].filter.history.size() == 1,
	      "module filter was not configured for one channel");

	// Reconfigure the voice for two channels: the module filter state must be
	// reset so no stale per-channel history survives the layout change.
	Ngs2SubmixerVoiceSetupParam vsetup2 {{sizeof(Ngs2SubmixerVoiceSetupParam), 0, 0x40020000}, 2, 2};
	res = Ngs2VoiceControl(voice, &vsetup2.header);
	Check(res == OK, "VoiceControl CustomFilter relayout to two channels failed");
	Check(!vInt->modules[0].filter.enabled && !vInt->modules[0].filter_configured,
	      "module filter state survived a channel layout change");

	Ngs2VoiceEventParam playEv {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 1};
	Ngs2VoiceControl(voice, &playEv.header);

	Fixture src(48000, 2);
	src.voice.ports.push_back({vInt, 0, 1.0f, -1});
	int16_t fInput[64 * 2];
	for (int i = 0; i < 64; ++i) {
		fInput[2 * i]     = 16384;
		fInput[2 * i + 1] = 16384;
	}
	src.Queue(fInput, 64, 0);

	std::vector<Ngs2VoiceInternal*> voicesList {&src.voice};
	Ngs2RenderVoice(*vInt, voicesList, 64);

	float* b = vInt->Buffer(0, 64);
	// With the filter reset the module passes audio through untouched.
	Check(b[0] == 0.5f && b[64] == 0.5f, "Custom Filter relayout did not reset the module filter");

	Ngs2RackDestroy(filterRack, nullptr);
	Ngs2SystemDestroy(sysHandle, nullptr);
}

// --- Tests for Stream API, Scheduler, Modules, and Reporting ---
struct TestStreamCommandRecord {
	uint32_t id;
	uint8_t  flags;
	uint8_t  type;
	uint16_t count;
	union {
		uint64_t    u64;
		uint32_t    u32;
		int32_t     i32;
		float       f;
		const void* ptr;
	} value;
};

struct TestModuleQueueRing {
	const void* buffer;
	uint32_t    reserved;
	uint32_t    reserved2;
	uint64_t    count;
	uint64_t    first;
};

struct TestModuleQueue {
	uint64_t            reserved;
	TestModuleQueueRing rings[2];
	uint32_t            lock;
	uint8_t             currentRing;
	uint8_t             reservedByte[3];
	uint32_t            itemSize;
	uint32_t            ringLength;
};

static int32_t KYTY_SYSV_ABI TestStreamBufferAlloc(Ngs2ContextBufferInfo* info) {
	if (info != nullptr) {
		static std::vector<uint8_t> s_alloc_buf(131072);
		info->host_buffer = s_alloc_buf.data();
	}
	return 0;
}

static int         g_streamAllocCalls       = 0;
static int         g_streamFreeCalls        = 0;
static int32_t     g_streamAllocResult      = 0;
static bool        g_streamAllocGaveBuffer  = true;
static bool        g_streamSawUserData      = false;
static uintptr_t   g_streamExpectedUserData = 0;
static void*       g_streamLastAllocBuffer  = nullptr;
static std::vector<uint8_t> g_streamAllocStorage(0x106c0);

static int32_t KYTY_SYSV_ABI TestStreamAllocCounting(Ngs2ContextBufferInfo* info) {
	++g_streamAllocCalls;
	if (info->user_data == g_streamExpectedUserData) {
		g_streamSawUserData = true;
	}
	// The buffer pointer is returned regardless of the result code so the
	// error-with-buffer path can be exercised; the size is reset to simulate
	// a callback that clears the record.
	info->host_buffer_size = 0;
	if (!g_streamAllocGaveBuffer) {
		info->host_buffer = nullptr;
	} else {
		info->host_buffer = g_streamAllocStorage.data();
	}
	g_streamLastAllocBuffer = info->host_buffer;
	return g_streamAllocResult;
}

static int32_t KYTY_SYSV_ABI TestStreamFreeCounting(Ngs2ContextBufferInfo* info) {
	++g_streamFreeCalls;
	if (info->host_buffer != g_streamLastAllocBuffer) {
		Check(false, "stream free handler received a different buffer than alloc produced");
	}
	return 0;
}

static void TestStreamAndModulesPhase4() {
	// 1. sceNgs2StreamResetOption
	Check(Ngs2StreamResetOption(nullptr) == NGS2_ERROR_INVALID_OUT_ADDRESS,
	      "StreamResetOption on nullptr must return INVALID_OUT_ADDRESS");
	Check(Ngs2StreamResetOption(reinterpret_cast<void*>(0x10)) == NGS2_ERROR_INVALID_OUT_ADDRESS,
	      "StreamResetOption on an invalid guest pointer must return INVALID_OUT_ADDRESS");

	Ngs2StreamOption opt {};
	int res = Ngs2StreamResetOption(&opt);
	Check(res == OK, "StreamResetOption failed");
	Check(opt.size == sizeof(Ngs2StreamOption), "StreamOption.size does not match");
	Check(opt.num_blocks == 4, "StreamOption.num_blocks default is wrong");
	Check(opt.buffer_size_1 == 0x100, "StreamOption.buffer_size_1 is wrong");
	Check(opt.buffer_size_2 == 0x10, "StreamOption.buffer_size_2 is wrong");
	Check(opt.buffer_size_3 == 0x10000, "StreamOption.buffer_size_3 is wrong");
	Check(std::abs(opt.default_param - 0.5f) < 1e-6f, "StreamOption.default_param is wrong");

	// 2. sceNgs2StreamQueryBufferSize
	Check(Ngs2StreamQueryBufferSize(&opt, nullptr) == NGS2_ERROR_INVALID_OUT_ADDRESS,
	      "StreamQueryBufferSize on null buffer_info must return INVALID_OUT_ADDRESS");
	Check(Ngs2StreamQueryBufferSize(&opt, reinterpret_cast<void*>(0x10)) == NGS2_ERROR_INVALID_OUT_ADDRESS,
	      "StreamQueryBufferSize on an invalid out pointer must return INVALID_OUT_ADDRESS");

	Ngs2ContextBufferInfo bufInfo {};
	res = Ngs2StreamQueryBufferSize(nullptr, &bufInfo);
	Check(res == OK, "StreamQueryBufferSize with null opt failed");
	Check(bufInfo.host_buffer_size == 0x106c0ULL, "StreamQueryBufferSize default size must be 0x106c0");

	Check(Ngs2StreamQueryBufferSize(reinterpret_cast<const void*>(0x10), &bufInfo) == SCE_NGS2_ERROR_INVALID_OPTION_SIZE,
	      "StreamQueryBufferSize with an invalid opt must return INVALID_OPTION_SIZE");

	Ngs2StreamOption badOpt = opt;
	badOpt.num_blocks = 0;
	Check(Ngs2StreamQueryBufferSize(&badOpt, &bufInfo) == SCE_NGS2_ERROR_INVALID_NUM_BLOCKS,
	      "num_blocks == 0 must return INVALID_NUM_BLOCKS");
	badOpt.num_blocks = 33;
	Check(Ngs2StreamQueryBufferSize(&badOpt, &bufInfo) == SCE_NGS2_ERROR_INVALID_NUM_BLOCKS,
	      "num_blocks > 32 must return INVALID_NUM_BLOCKS");

	badOpt = opt;
	badOpt.buffer_size_3 = 0;
	Check(Ngs2StreamQueryBufferSize(&badOpt, &bufInfo) == SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_3_ZERO,
	      "buffer_size_3 == 0 must return STREAM_BUFFER_SIZE_3_ZERO");

	badOpt = opt;
	badOpt.buffer_size_1 = 0;
	Check(Ngs2StreamQueryBufferSize(&badOpt, &bufInfo) == SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_1_ZERO,
	      "buffer_size_1 == 0 must return STREAM_BUFFER_SIZE_1_ZERO");

	badOpt = opt;
	badOpt.buffer_size_2 = 0;
	Check(Ngs2StreamQueryBufferSize(&badOpt, &bufInfo) == SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_2_ZERO,
	      "buffer_size_2 == 0 must return STREAM_BUFFER_SIZE_2_ZERO");

	badOpt = opt;
	badOpt.size = 0x80;
	Check(Ngs2StreamQueryBufferSize(&badOpt, &bufInfo) == SCE_NGS2_ERROR_INVALID_OPTION_SIZE,
	      "wrong size must return INVALID_OPTION_SIZE");

	// A wrong size is reported before the remaining fields are validated.
	Ngs2StreamOption zeroedOpt {};
	zeroedOpt.size = 0x80;
	Check(Ngs2StreamQueryBufferSize(&zeroedOpt, &bufInfo) == SCE_NGS2_ERROR_INVALID_OPTION_SIZE,
	      "wrong size with zeroed fields must return INVALID_OPTION_SIZE");

	// 3. Stream creation and destruction
	Ngs2SystemOption sysOpt {};
	Ngs2SystemResetOption(&sysOpt);
	sysOpt.max_grain_samples = 256;
	sysOpt.num_grain_samples = 256;
	sysOpt.sample_rate       = 48000;
	std::vector<uint8_t> sysBuf(1024 * 1024, 0);
	Ngs2ContextBufferInfo sysBufInfo {sysBuf.data(), sysBuf.size(), {0}};
	uintptr_t sysHandle = 0;
	res = Ngs2SystemCreate(&sysOpt, &sysBufInfo, &sysHandle);
	Check(res == OK && sysHandle != 0, "SystemCreate failed for Stream test");

	uintptr_t streamHandle = 0;
	Check(Ngs2StreamCreate(sysHandle, &opt, nullptr, &streamHandle) == SCE_NGS2_ERROR_INVALID_BUFFER_INFO,
	      "StreamCreate on null buffer_info must return INVALID_BUFFER_INFO");
	Check(Ngs2StreamCreate(sysHandle, &opt, reinterpret_cast<const void*>(0x10), &streamHandle) == SCE_NGS2_ERROR_INVALID_BUFFER_INFO,
	      "StreamCreate on invalid buffer_info must return INVALID_BUFFER_INFO");

	Ngs2ContextBufferInfo emptyHost {};
	Check(Ngs2StreamCreate(sysHandle, &opt, &emptyHost, &streamHandle) == SCE_NGS2_ERROR_INVALID_BUFFER_INFO,
	      "StreamCreate with null host_buffer must return INVALID_BUFFER_INFO");

	bufInfo.host_buffer = reinterpret_cast<void*>(0x100000);
	Check(Ngs2StreamCreate(sysHandle, reinterpret_cast<const void*>(0x10), &bufInfo, &streamHandle) == SCE_NGS2_ERROR_INVALID_OPTION_SIZE,
	      "StreamCreate with an invalid opt must return INVALID_OPTION_SIZE");
	Check(Ngs2StreamCreate(sysHandle, &opt, &bufInfo, nullptr) == NGS2_ERROR_INVALID_OUT_ADDRESS,
	      "StreamCreate with a null out handle must return INVALID_OUT_ADDRESS");
	Check(Ngs2StreamCreate(0xdead, &opt, &bufInfo, &streamHandle) == NGS2_ERROR_INVALID_SYSTEM_HANDLE,
	      "StreamCreate with a null/invalid system must return INVALID_SYSTEM_HANDLE");

	std::vector<uint8_t> streamMem(0x106c0);
	bufInfo.host_buffer = streamMem.data();
	bufInfo.host_buffer_size = streamMem.size();
	res = Ngs2StreamCreate(sysHandle, &opt, &bufInfo, &streamHandle);
	Check(res == OK && streamHandle != 0, "StreamCreate failed with valid parameters");

	// 4. StreamCreateWithAllocator
	Ngs2BufferAllocator alloc {};
	Check(Ngs2StreamCreateWithAllocator(sysHandle, &opt, nullptr, &streamHandle) == SCE_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR,
	      "StreamCreateWithAllocator on nullptr must return INVALID_BUFFER_ALLOCATOR");
	Check(Ngs2StreamCreateWithAllocator(sysHandle, &opt, reinterpret_cast<const void*>(0x10), &streamHandle) == SCE_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR,
	      "StreamCreateWithAllocator on an invalid pointer must return INVALID_BUFFER_ALLOCATOR");
	Check(Ngs2StreamCreateWithAllocator(sysHandle, &opt, &alloc, &streamHandle) == SCE_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR,
	      "StreamCreateWithAllocator with null alloc_handler must return INVALID_BUFFER_ALLOCATOR");

	alloc.alloc_handler = &TestStreamBufferAlloc;
	uintptr_t streamAllocHandle = 0;
	res = Ngs2StreamCreateWithAllocator(sysHandle, &opt, &alloc, &streamAllocHandle);
	Check(res == OK && streamAllocHandle != 0, "StreamCreateWithAllocator failed with a valid allocator");

	// Allocator failure must fall back to internally owned storage: the caller's
	// free handler must never run for it (no double free).
	g_streamAllocCalls      = 0;
	g_streamFreeCalls       = 0;
	g_streamAllocResult     = -1;
	g_streamAllocGaveBuffer = false;
	g_streamExpectedUserData = 0xBEEF;
	Ngs2BufferAllocator failAlloc {};
	failAlloc.alloc_handler = &TestStreamAllocCounting;
	failAlloc.free_handler  = &TestStreamFreeCounting;
	failAlloc.user_data     = g_streamExpectedUserData;
	uintptr_t failHandle = 0;
	res = Ngs2StreamCreateWithAllocator(sysHandle, &opt, &failAlloc, &failHandle);
	Check(res == OK && failHandle != 0, "StreamCreateWithAllocator must succeed with internal fallback");
	Check(g_streamAllocCalls == 1 && g_streamSawUserData,
	      "alloc handler was not invoked exactly once with the allocator user_data");
	Ngs2ContextBufferInfo failBufInfo {};
	res = Ngs2StreamDestroy(failHandle, &failBufInfo);
	Check(res == OK, "StreamDestroy of fallback stream failed");
	Check(g_streamFreeCalls == 0, "free handler ran for internally owned fallback storage");
	Check(failBufInfo.host_buffer_size == 0x106c0,
	      "StreamDestroy must report the real fallback buffer size");

	// An allocator error that still produces a buffer must release it once
	// during creation; the internal fallback must not free it again.
	g_streamAllocCalls       = 0;
	g_streamFreeCalls        = 0;
	g_streamAllocResult      = -1;
	g_streamAllocGaveBuffer  = true;
	g_streamSawUserData      = false;
	uintptr_t errHandle = 0;
	res = Ngs2StreamCreateWithAllocator(sysHandle, &opt, &failAlloc, &errHandle);
	Check(res == OK && errHandle != 0,
	      "StreamCreateWithAllocator must succeed when alloc fails with a buffer");
	Check(g_streamAllocCalls == 1 && g_streamSawUserData,
	      "failed alloc handler was not invoked exactly once");
	Check(g_streamFreeCalls == 1,
	      "buffer from a failed alloc must be freed exactly once during creation");
	Ngs2ContextBufferInfo errBufInfo {};
	res = Ngs2StreamDestroy(errHandle, &errBufInfo);
	Check(res == OK, "StreamDestroy of error-fallback stream failed");
	Check(g_streamFreeCalls == 1, "free handler ran again for fallback storage");
	Check(errBufInfo.host_buffer_size == 0x106c0,
	      "StreamDestroy must report the real fallback buffer size");

	// Successful caller allocation: free handler runs exactly once with the
	// buffer produced by the alloc callback.
	g_streamAllocCalls       = 0;
	g_streamFreeCalls        = 0;
	g_streamAllocResult      = 0;
	g_streamAllocGaveBuffer  = true;
	g_streamSawUserData      = false;
	uintptr_t okHandle = 0;
	res = Ngs2StreamCreateWithAllocator(sysHandle, &opt, &failAlloc, &okHandle);
	Check(res == OK && okHandle != 0, "StreamCreateWithAllocator failed on second attempt");
	Check(g_streamAllocCalls == 1 && g_streamSawUserData,
	      "alloc handler did not receive the allocator user_data");
	Ngs2ContextBufferInfo okBufInfo {};
	res = Ngs2StreamDestroy(okHandle, &okBufInfo);
	Check(res == OK, "StreamDestroy of allocated stream failed");
	Check(g_streamFreeCalls == 1, "free handler must run exactly once for caller storage");
	Check(okBufInfo.host_buffer_size == 0,
	      "successful caller allocation must keep the callback buffer metadata");

	// 5. sceNgs2StreamRunCommands
	Check(Ngs2StreamDestroy(0, nullptr) == SCE_NGS2_ERROR_INVALID_STREAM_HANDLE,
	      "StreamDestroy on 0 must return INVALID_STREAM_HANDLE");
	Check(Ngs2StreamRunCommands(0, nullptr, 0) == SCE_NGS2_ERROR_INVALID_STREAM_HANDLE,
	      "StreamRunCommands on stream 0 must return INVALID_STREAM_HANDLE");
	Check(Ngs2StreamRunCommands(0x100, reinterpret_cast<const void*>(0x10), 1) == SCE_NGS2_ERROR_INVALID_STREAM_COMMANDS,
	      "StreamRunCommands with invalid commands must return INVALID_STREAM_COMMANDS");

	TestStreamCommandRecord recs[2] {};
	recs[0].id = 0xf20000; // nop/lock
	recs[1].id = 0xf20002; // state
	recs[1].value.u32 = 16;
	res = Ngs2StreamRunCommands(streamHandle, recs, 2);
	Check(res == OK, "StreamRunCommands failed with valid commands");

	recs[1].value.u32 = 99; // Invalid state
	Check(Ngs2StreamRunCommands(streamHandle, recs, 2) == SCE_NGS2_ERROR_INVALID_STREAM_STATE_COMMAND,
	      "StreamRunCommands with a wrong state must return INVALID_STREAM_STATE_COMMAND");

	// 6. sceNgs2StreamQueryInfo
	Check(Ngs2StreamQueryInfo(0, 0xf24001, nullptr, 0) == SCE_NGS2_ERROR_INVALID_STREAM_HANDLE,
	      "StreamQueryInfo on stream 0 must return INVALID_STREAM_HANDLE");
	uint64_t qinfo = 0;
	Check(Ngs2StreamQueryInfo(streamHandle, 0xf24001, &qinfo, 4) == NGS2_ERROR_INVALID_OUT_SIZE,
	      "StreamQueryInfo 0xf24001 with size < 8 must return INVALID_OUT_SIZE");
	Check(Ngs2StreamQueryInfo(streamHandle, 0xf24099, &qinfo, 8) == SCE_NGS2_ERROR_INVALID_STREAM_QUERY_ID,
	      "StreamQueryInfo with an invalid query id must return INVALID_STREAM_QUERY_ID");
	// Recognized but unimplemented queries must return a defined error and
	// leave the output buffer untouched instead of reporting success.
	qinfo = 0xdeadbeef;
	Check(Ngs2StreamQueryInfo(streamHandle, 0xf24001, &qinfo, sizeof(qinfo)) ==
	          SCE_NGS2_ERROR_INVALID_STREAM_QUERY_ID,
	      "StreamQueryInfo 0xf24001 must report the unimplemented query");
	Check(qinfo == 0xdeadbeef, "StreamQueryInfo modified the output on failure");

	// 7. Stream destruction
	res = Ngs2StreamDestroy(streamHandle, nullptr);
	Check(res == OK, "StreamDestroy streamHandle failed");
	res = Ngs2StreamDestroy(streamAllocHandle, nullptr);
	Check(res == OK, "StreamDestroy streamAllocHandle failed");

	// 8. Scheduler & Module Enums & Reports
	Check(Ngs2JobSchedulerResetOption(nullptr) == NGS2_ERROR_INVALID_OUT_ADDRESS,
	      "JobSchedulerResetOption with nullptr must return INVALID_OUT_ADDRESS");
	Ngs2JobSchedulerOption jsOpt {};
	res = Ngs2JobSchedulerResetOption(&jsOpt);
	Check(res == OK, "JobSchedulerResetOption failed");
	Check(jsOpt.num_threads == 1, "JobSchedulerOption default is wrong");

	void* cfgTable[4] = {};
	Check(Ngs2ModuleEnumConfigs(cfgTable, 4) == 0, "ModuleEnumConfigs must return 0");
	Check(Ngs2ModuleArrayEnumItems(nullptr, nullptr, 0) == 0, "ModuleArrayEnumItems with nullptr must return 0");
	Check(Ngs2ModuleQueueEnumItems(nullptr, nullptr, 0) == 0, "ModuleQueueEnumItems with nullptr must return 0");

	// Test array enum
	alignas(8) uint8_t arrayData[64] = {};
	const uint32_t itemSize = 4;
	const uint32_t count = 3;
	const uint8_t flag = 1;
	arrayData[8] = flag;
	std::memcpy(arrayData + 0xc, &itemSize, sizeof(itemSize));
	std::memcpy(arrayData + 0x14 + 4 * flag, &count, sizeof(count));
	const uint32_t sampleVal1 = 0x11111111;
	const uint32_t sampleVal2 = 0x22222222;
	const uint32_t sampleVal3 = 0x33333333;
	std::memcpy(arrayData + 0x20, &sampleVal1, 4);
	std::memcpy(arrayData + 0x24, &sampleVal2, 4);
	std::memcpy(arrayData + 0x28, &sampleVal3, 4);
	uint32_t outItems[4] = {};
	const int enumCount = Ngs2ModuleArrayEnumItems(arrayData, outItems, 2);
	Check(enumCount == 3, "ModuleArrayEnumItems count is wrong");
	Check(outItems[0] == sampleVal1 && outItems[1] == sampleVal2 && outItems[2] == 0,
	      "ModuleArrayEnumItems item copy is wrong");

	// Test queue enum
	TestModuleQueue q {};
	const uint32_t qItems[] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
	q.rings[0].buffer = qItems;
	q.rings[0].count  = 3;
	q.rings[0].first  = 3;
	q.itemSize        = sizeof(qItems[0]);
	q.ringLength      = 4;
	uint32_t qOut[4] = {};
	const int qEnumCount = Ngs2ModuleQueueEnumItems(&q, qOut, 3);
	Check(qEnumCount == 3, "ModuleQueueEnumItems count is wrong");
	Check(qOut[0] == qItems[3] && qOut[1] == qItems[0] && qOut[2] == qItems[1] && qOut[3] == 0,
	      "ModuleQueueEnumItems ring copy is wrong");

	// Test report register/unregister
	Check(Ngs2ReportRegisterHandler(nullptr, nullptr) == 0, "ReportRegisterHandler failed");
	Check(Ngs2ReportUnregisterHandler(nullptr) == 0, "ReportUnregisterHandler failed");

	Ngs2SystemDestroy(sysHandle, nullptr);
}

static uintptr_t g_nestedDestroyHandle = 0;
static int32_t   KYTY_SYSV_ABI TestStreamFreeDestroyOther(Ngs2ContextBufferInfo* info) {
	if (g_nestedDestroyHandle != 0) {
		Ngs2StreamDestroy(g_nestedDestroyHandle, nullptr);
		g_nestedDestroyHandle = 0;
	}
	return 0;
}

static void TestRegistryLifetimeAndLocking() {
	// --- 2.2: a destroyed system disappears from enumeration ---
	Ngs2SystemOption sysOpt {};
	Ngs2SystemResetOption(&sysOpt);
	sysOpt.num_grain_samples = 256;
	sysOpt.sample_rate       = 48000;
	std::vector<uint8_t> sysBuf(sizeof(Ngs2Internal));
	Ngs2ContextBufferInfo sbi {sysBuf.data(), sysBuf.size(), {0}};
	uintptr_t sysHandle = 0;
	Check(Ngs2SystemCreate(&sysOpt, &sbi, &sysHandle) == OK && sysHandle != 0,
	      "SystemCreate failed for registry test");

	uintptr_t handles[8] = {};
	const int  before    = Ngs2SystemEnumHandles(handles, 8);
	bool       found     = false;
	for (int i = 0; i < before && i < 8; ++i) {
		found = found || handles[i] == sysHandle;
	}
	Check(found, "created system missing from Ngs2SystemEnumHandles");
	Check(Ngs2SystemDestroy(sysHandle, nullptr) == OK, "SystemDestroy failed");
	const int after = Ngs2SystemEnumHandles(handles, 8);
	for (int i = 0; i < after && i < 8; ++i) {
		Check(handles[i] != sysHandle, "destroyed system still enumerated");
	}

	// Recreate a system for the rack tests.
	uintptr_t sys2 = 0;
	Check(Ngs2SystemCreate(&sysOpt, &sbi, &sys2) == OK && sys2 != 0,
	      "second SystemCreate failed for registry test");

	// --- 2.1/2.4: rack registry checks and lock ownership ---
	Ngs2RackOptionUnion optA {}, optB {};
	Ngs2FillDefaultRackOption(0x1000, &optA);
	Ngs2FillDefaultRackOption(0x1000, &optB);
	optA.sampler.rack_option.max_voices = 1;
	optB.sampler.rack_option.max_voices = 1;
	std::vector<uint8_t> rackBufA(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	std::vector<uint8_t> rackBufB(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbiA {rackBufA.data(), rackBufA.size(), {0}};
	Ngs2ContextBufferInfo rbiB {rackBufB.data(), rackBufB.size(), {0}};
	uintptr_t rackA = 0, rackB = 0;
	Check(Ngs2RackCreate(sys2, 0x1000, &optA.sampler.rack_option, &rbiA, &rackA) == OK,
	      "RackCreate A failed");
	Check(Ngs2RackCreate(sys2, 0x1000, &optB.sampler.rack_option, &rbiB, &rackB) == OK,
	      "RackCreate B failed");

	uintptr_t voiceHandle = 0;
	Check(Ngs2RackGetVoiceHandle(rackA, 0, &voiceHandle) == OK && voiceHandle != 0,
	      "RackGetVoiceHandle on live rack failed");

	// Lock A, then try to unlock B on the same system: must fail and leave A locked.
	Check(Ngs2RackLock(rackA) == OK, "RackLock A failed");
	Check(Ngs2RackUnlock(rackB) == NGS2_ERROR_INVALID_RACK_HANDLE,
	      "unlocking a different rack must fail");
	Check(Ngs2RackUnlock(rackA) == OK, "balanced unlock of the locked rack failed");

	// Destroying the rack invalidates its voice handle lookup.
	Check(Ngs2RackDestroy(rackB, nullptr) == OK, "RackDestroy B failed");
	Check(Ngs2RackGetVoiceHandle(rackB, 0, &voiceHandle) == NGS2_ERROR_INVALID_RACK_HANDLE,
	      "RackGetVoiceHandle on a destroyed rack must fail");
	Check(Ngs2RackGetVoiceHandle(rackA, 0, &voiceHandle) == OK,
	      "RackGetVoiceHandle on surviving rack failed");
	Check(Ngs2RackDestroy(rackA, nullptr) == OK, "RackDestroy A failed");

	// --- 2.6: a stream free handler may destroy another stream ---
	Ngs2StreamOption sopt {};
	Ngs2StreamResetOption(&sopt);
	Ngs2BufferAllocator nestedAlloc {};
	nestedAlloc.alloc_handler = &TestStreamBufferAlloc;
	nestedAlloc.free_handler  = &TestStreamFreeDestroyOther;
	std::vector<uint8_t> streamMem(0x106c0);
	Ngs2ContextBufferInfo streamBuf {streamMem.data(), streamMem.size(), {0}};

	uintptr_t streamOuter = 0, streamInner = 0;
	Check(Ngs2StreamCreateWithAllocator(sys2, &sopt, &nestedAlloc, &streamOuter) == OK,
	      "outer stream create failed");
	Check(Ngs2StreamCreate(sys2, &sopt, &streamBuf, &streamInner) == OK,
	      "inner stream create failed");
	g_nestedDestroyHandle = streamInner;
	// The free handler runs after the stream is detached from the registry, so
	// destroying another stream inside it must not deadlock or corrupt state.
	Check(Ngs2StreamDestroy(streamOuter, nullptr) == OK,
	      "StreamDestroy with nested free handler failed");
	Check(Ngs2StreamDestroy(streamInner, nullptr) == SCE_NGS2_ERROR_INVALID_STREAM_HANDLE,
	      "nested stream destroy inside free handler did not run");

	Ngs2SystemDestroy(sys2, nullptr);
}

// --- Phase 4: filters, pitch shift, WAVE ---

// RBJ low-pass biquad coefficient b0 for the given design rate.
static double ExpectedBiquadB0(float frequency, float q, float level, uint32_t rate) {
	const double omega      = (2.0 * std::numbers::pi) * frequency / rate;
	const double alpha      = std::sin(omega) / (2.0 * q);
	const double inverse_a0 = 1.0 / (1.0 + alpha);
	return (1.0 - std::cos(omega)) * 0.5 * inverse_a0 * level;
}

static void TestCustomFilterDesignRatePhase4() {
	Ngs2SystemOption opt {};
	Ngs2SystemResetOption(&opt);
	opt.num_grain_samples = 64;
	opt.max_grain_samples = 64;
	opt.sample_rate       = 48000;
	opt.max_voice_channels = 2;
	std::vector<uint8_t> sysBuf(sizeof(Ngs2Internal));
	Ngs2ContextBufferInfo sbi {sysBuf.data(), sysBuf.size(), {0}};
	uintptr_t sysHandle = 0;
	Check(Ngs2SystemCreate(&opt, &sbi, &sysHandle) == OK, "SystemCreate failed");

	Ngs2CustomSubmixerRackOption subOpt {};
	auto& custom = subOpt.custom_rack_option;
	custom.rack_option.size              = sizeof(subOpt);
	custom.rack_option.max_grain_samples = 64;
	custom.rack_option.max_voices        = 1;
	custom.rack_option.max_matrices      = 1;
	custom.rack_option.max_ports         = 1;
	custom.num_buffers                   = 1;
	custom.num_modules                   = 1;
	custom.module[0].module_id           = SCE_NGS2_CUSTOM_MODULE_ID_FILTER;
	custom.module[0].source_buffer_id    = 0;
	custom.module[0].dest_buffer_id      = 0;
	subOpt.max_channels                  = 2;
	subOpt.max_inputs                    = 1;

	std::vector<uint8_t> rBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbi {rBuf.data(), rBuf.size(), {0}};
	uintptr_t rack = 0;
	Check(Ngs2RackCreate(sysHandle, 0x4002, &custom.rack_option, &rbi, &rack) == OK,
	      "CustomSubmixer rack create failed");
	uintptr_t voice = 0;
	Ngs2RackGetVoiceHandle(rack, 0, &voice);
	auto* vInt = reinterpret_cast<Ngs2VoiceInternal*>(voice);

	Ngs2SubmixerVoiceSetupParam vsetup {{sizeof(Ngs2SubmixerVoiceSetupParam), 0, 0x20000000}, 2, 0};
	Ngs2VoiceControl(voice, &vsetup.header);

	// The module filter runs on system-rate audio: the coefficients must be
	// designed for the system rate, not the (unset) source rate.
	Ngs2CustomVoiceFilterParam fparam {};
	fparam.header = {sizeof(Ngs2CustomVoiceFilterParam), 0, SCE_NGS2_CUSTOM_VOICE_PARAM_FILTER};
	fparam.channel_mask = 0;
	fparam.type = 1;
	fparam.param.fcq.frequency = 15000.0f;
	fparam.param.fcq.q         = 0.707f;
	fparam.param.fcq.level     = 1.0f;
	Check(Ngs2VoiceControl(voice, &fparam.header) == OK, "custom filter control failed");
	Ngs2VoiceEventParam playEv {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 1};
	Ngs2VoiceControl(voice, &playEv.header);
	Check(vInt->modules[0].filter.design_rate == 48000 &&
	          std::abs(vInt->modules[0].filter.b0 -
	                   ExpectedBiquadB0(15000.0f, 0.707f, 1.0f, 48000)) < 1e-10,
	      "custom module filter was not designed at the system rate");

	// A system rate change must redesign the coefficients at the next render.
	Check(Ngs2SystemSetSampleRate(sysHandle, 44100) == OK, "SystemSetSampleRate failed");
	Fixture src(48000, 2);
	src.voice.ports.push_back({vInt, 0, 1.0f, -1});
	const int16_t pcm[64 * 2] = {};
	src.Queue(pcm, 64, 0);
	std::vector<Ngs2VoiceInternal*> voices {&src.voice};
	Ngs2RenderVoice(*vInt, voices, 64);
	Check(vInt->modules[0].filter.design_rate == 44100 &&
	          std::abs(vInt->modules[0].filter.b0 -
	                   ExpectedBiquadB0(15000.0f, 0.707f, 1.0f, 44100)) < 1e-10,
	      "custom module filter was not redesigned after a rate change");

	Ngs2RackDestroy(rack, nullptr);
	Ngs2SystemDestroy(sysHandle, nullptr);
}

static void TestMasteringLfeCutoffPhase4() {
	Ngs2Internal system;
	system.option                   = Ngs2DefaultSystemOption();
	system.option.num_grain_samples = 4;
	Ngs2RackInternal rack {};
	rack.ngs  = &system;
	rack.type = Ngs2RackType::Mastering;
	Ngs2VoiceInternal voice {};
	voice.rack     = &rack;
	voice.channels = 4;
	voice.state    = Ngs2VoicePlayState::Playing;
	const std::vector<Ngs2VoiceInternal*> voices;

	Ngs2MasteringVoiceEffectParam lfe {};
	lfe.header = {sizeof(lfe), 0, SCE_NGS2_MASTERING_VOICE_PARAM_LFE};
	lfe.enable_flag = 1;
	lfe.value       = 80;
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &lfe.header) == OK,
	      "mastering LFE control failed");
	Ngs2RenderVoice(voice, voices, 4);
	const float a1_80 = voice.mastering_lfe.sections[0].a1;

	// Same design rate but a different cutoff must redesign the filter.
	lfe.value = 120;
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &lfe.header) == OK,
	      "mastering LFE reconfigure failed");
	voice.rendered = false;
	Ngs2RenderVoice(voice, voices, 4);
	Check(voice.mastering_lfe.design_cutoff == 120 &&
	          voice.mastering_lfe.sections[0].a1 != a1_80,
	      "LFE cutoff change did not redesign the filter coefficients");
}

static void TestPitchShiftDrainPhase4() {
	Ngs2SystemOption sysOpt = Ngs2DefaultSystemOption();
	sysOpt.num_grain_samples = 64;
	sysOpt.max_grain_samples = 64;
	sysOpt.sample_rate       = 48000;
	sysOpt.max_voice_channels = 2;
	std::vector<uint8_t> sysBuf(sizeof(Ngs2Internal));
	Ngs2ContextBufferInfo sbi {sysBuf.data(), sysBuf.size(), {0}};
	uintptr_t sysHandle = 0;
	Check(Ngs2SystemCreate(&sysOpt, &sbi, &sysHandle) == OK, "SystemCreate failed");

	Ngs2CustomSubmixerRackOption subOpt {};
	auto& custom = subOpt.custom_rack_option;
	custom.rack_option.size              = sizeof(subOpt);
	custom.rack_option.max_grain_samples = 64;
	custom.rack_option.max_voices        = 1;
	custom.rack_option.max_matrices      = 1;
	custom.rack_option.max_ports         = 1;
	custom.num_buffers                   = 1;
	custom.num_modules                   = 1;
	custom.module[0].module_id           = SCE_NGS2_CUSTOM_MODULE_ID_PITCH_SHIFT;
	custom.module[0].source_buffer_id    = 0;
	custom.module[0].dest_buffer_id      = 0;
	subOpt.max_channels                  = 2;
	subOpt.max_inputs                    = 1;

	std::vector<uint8_t> rBuf(sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal));
	Ngs2ContextBufferInfo rbi {rBuf.data(), rBuf.size(), {0}};
	uintptr_t rack = 0;
	Check(Ngs2RackCreate(sysHandle, 0x4002, &custom.rack_option, &rbi, &rack) == OK,
	      "CustomSubmixer rack create failed");
	uintptr_t voice = 0;
	Ngs2RackGetVoiceHandle(rack, 0, &voice);
	auto* vInt = reinterpret_cast<Ngs2VoiceInternal*>(voice);

	Ngs2SubmixerVoiceSetupParam vsetup {{sizeof(Ngs2SubmixerVoiceSetupParam), 0, 0x20000000}, 2, 0};
	Ngs2VoiceControl(voice, &vsetup.header);
	Ngs2CustomVoicePitchShiftParam pparam {};
	pparam.header = {sizeof(Ngs2CustomVoicePitchShiftParam), 0, SCE_NGS2_CUSTOM_VOICE_PARAM_PITCH_SHIFT};
	pparam.cents  = 700;
	Check(Ngs2VoiceControl(voice, &pparam.header) == OK, "pitch shift control failed");
	Ngs2VoiceEventParam playEv {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 1};
	Ngs2VoiceControl(voice, &playEv.header);

	// A clip shorter than the pitch-shift latency must still emit its tail.
	Fixture src(48000, 2);
	src.voice.ports.push_back({vInt, 0, 1.0f, -1});
	std::vector<int16_t> pcm(512 * 2, 16384);
	src.Queue(pcm.data(), 512, 0);
	std::vector<Ngs2VoiceInternal*> voices {&src.voice};

	float tail_peak = 0.0f;
	bool  input_done = false;
	for (uint32_t grain = 0; grain < 96; ++grain) {
		vInt->rendered       = false;
		src.voice.rendered   = false;
		Ngs2RenderVoice(*vInt, voices, 64);
		if (!input_done && src.voice.blocks.empty() && !src.voice.has_samples) {
			input_done = true;
		}
		if (input_done) {
			const float* b = vInt->Buffer(0, 64);
			for (uint32_t i = 0; i < 64; ++i) {
				tail_peak = std::max(tail_peak, std::abs(b[i]));
			}
		}
	}
	Check(input_done, "pitch drain fixture never reached end of input");
	Check(tail_peak > 0.01f, "pitch shift tail stayed silent after the input ended");

	Ngs2RackDestroy(rack, nullptr);
	Ngs2SystemDestroy(sysHandle, nullptr);
}

static void TestExtensibleFloatWaveformPhase4() {
	auto appendLE16 = [](std::vector<uint8_t>& v, uint16_t x) {
		v.push_back(static_cast<uint8_t>(x));
		v.push_back(static_cast<uint8_t>(x >> 8));
	};
	auto appendLE32 = [](std::vector<uint8_t>& v, uint32_t x) {
		v.push_back(static_cast<uint8_t>(x));
		v.push_back(static_cast<uint8_t>(x >> 8));
		v.push_back(static_cast<uint8_t>(x >> 16));
		v.push_back(static_cast<uint8_t>(x >> 24));
	};
	auto appendFourCC = [](std::vector<uint8_t>& v, const char* cc) {
		v.insert(v.end(), {static_cast<uint8_t>(cc[0]), static_cast<uint8_t>(cc[1]),
		                   static_cast<uint8_t>(cc[2]), static_cast<uint8_t>(cc[3])});
	};

	// WAVE_FORMAT_EXTENSIBLE, mono 32-bit IEEE float: fmt chunk is 40 bytes
	// with cbSize=22, valid bits, channel mask and the float SubFormat GUID.
	std::vector<uint8_t> riff;
	appendFourCC(riff, "RIFF");
	appendLE32(riff, 4 + 8 + 40 + 8 + 8);
	appendFourCC(riff, "WAVE");
	appendFourCC(riff, "fmt ");
	appendLE32(riff, 40);
	appendLE16(riff, 0xfffe);
	appendLE16(riff, 1);
	appendLE32(riff, 48000);
	appendLE32(riff, 192000);
	appendLE16(riff, 4);
	appendLE16(riff, 32);
	appendLE16(riff, 22);
	appendLE16(riff, 32);
	appendLE32(riff, 0x4);
	appendLE16(riff, 0x0003); // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
	appendLE16(riff, 0x0000); // rest of Data1
	appendLE16(riff, 0x0000); // Data2
	appendLE16(riff, 0x0010); // Data3
	riff.insert(riff.end(), {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}); // Data4
	appendFourCC(riff, "data");
	appendLE32(riff, 8);
	appendLE32(riff, 0x3f800000);
	appendLE32(riff, 0xbf800000);

	Ngs2WaveformInfo info {};
	Check(Ngs2ParseWaveformData(riff.data(), riff.size(), &info) == OK,
	      "extensible IEEE float WAVE was rejected");
	Check(info.format.waveform_type == 0x18 && info.format.num_channels == 1 &&
	          info.format.sample_rate == 48000 && info.num_samples == 2,
	      "extensible IEEE float geometry is wrong");
}

static void TestAtrac9GeometryChannelsPhase4() {
	// Discrete 12-channel ATRAC9 @ 48 kHz (same config the codec tests use).
	const Ngs2WaveformFormat format {NGS2_WAVEFORM_TYPE_ATRAC9, 12, 48000, 0x3072c0fe};
	Ngs2WaveformBlock      block {};
	Check(Ngs2CalcWaveformBlock(&format, 0, 256, &block) == OK,
	      "CalcWaveformBlock rejected a valid 12-channel ATRAC9 format");
	Check(block.data_size == 3072 && block.num_samples == 256,
	      "CalcWaveformBlock returned wrong ATRAC9 geometry");

	uint32_t frame_size = 0, frame_samples = 0, units = 0, delay = 0;
	Check(Ngs2GetWaveformFrameInfo(&format, &frame_size, &frame_samples, &units, &delay) == OK &&
	          frame_size == 3072 && frame_samples == 1024 && units == 4,
	      "GetWaveformFrameInfo rejected a valid 12-channel ATRAC9 format");

	// Channel counts must still match the codec, and non-ATRAC9 formats keep
	// the eight-channel limit.
	const Ngs2WaveformFormat bad_channels {NGS2_WAVEFORM_TYPE_ATRAC9, 13, 48000, 0x3072c0fe};
	Check(Ngs2CalcWaveformBlock(&bad_channels, 0, 256, &block) == NGS2_ERROR_INVALID_WAVEFORM_FORMAT &&
	          Ngs2GetWaveformFrameInfo(&bad_channels, &frame_size, &frame_samples, &units,
	                                   &delay) == NGS2_ERROR_INVALID_WAVEFORM_FORMAT,
	      "ATRAC9 geometry accepted a channel count the codec does not provide");
	const Ngs2WaveformFormat pcm_wide {0x12, 9, 48000, 0};
	Check(Ngs2CalcWaveformBlock(&pcm_wide, 0, 256, &block) == NGS2_ERROR_INVALID_WAVEFORM_FORMAT,
	      "PCM geometry accepted more than eight channels");
}

static void TestSystemInfoAndRackRenderCountPhase6() {
	// Oversized info buffers are accepted; undersized ones are rejected.
	Ngs2SystemOption opt {};
	Ngs2SystemResetOption(&opt);
	opt.num_grain_samples = 64;
	opt.sample_rate       = 48000;
	std::vector<uint8_t> sysBuf(sizeof(Ngs2Internal));
	Ngs2ContextBufferInfo sbi {sysBuf.data(), sysBuf.size(), {0}};
	uintptr_t sysHandle = 0;
	Check(Ngs2SystemCreate(&opt, &sbi, &sysHandle) == OK, "SystemCreate failed");
	Ngs2SystemInfo info {};
	Check(Ngs2SystemGetInfo(sysHandle, &info, sizeof(info)) == OK,
	      "SystemGetInfo with the exact size failed");
	Check(Ngs2SystemGetInfo(sysHandle, &info, sizeof(info) + 64) == OK,
	      "SystemGetInfo must accept a buffer larger than the struct");
	Check(info.system_handle == sysHandle && info.sample_rate == 48000,
	      "SystemGetInfo wrote wrong fields into an oversized buffer");
	Check(Ngs2SystemGetInfo(sysHandle, &info, sizeof(info) - 1) == NGS2_ERROR_INVALID_OUT_SIZE,
	      "SystemGetInfo must reject a buffer smaller than the struct");
	Ngs2SystemDestroy(sysHandle, nullptr);

	// render_count increments once per rack per system render.
	Ngs2Internal system;
	system.option                   = Ngs2DefaultSystemOption();
	system.option.num_grain_samples = 4;
	Ngs2RackOptionUnion sampler_option {};
	Ngs2FillDefaultRackOption(0x1000, &sampler_option);
	sampler_option.common.max_voices = 1;
	RenderRack sampler(system, 0x1000, sampler_option.common);
	int16_t          out[8] = {};
	Ngs2RenderBufferInfo buf {out, sizeof(out), 0x12, 1};
	const auto       sys = reinterpret_cast<uintptr_t>(&system);
	Check(Ngs2SystemRender(sys, &buf, 1) == OK, "first system render failed");
	Check(Ngs2SystemRender(sys, &buf, 1) == OK, "second system render failed");
	Ngs2RackInfo rinfo {};
	Check(Ngs2RackGetInfo(sampler.handle, &rinfo, sizeof(rinfo)) == OK &&
	          rinfo.render_count == 2,
	      "rack render_count must increment once per system render");
}

} // namespace

int main() {
	TestProgressDrivenWaveformRing();
	TestStereoPan();
	TestWaveformQueueCapacity();
	TestWaveformRingReuse();
	TestAtrac9Vibration();
	TestAtrac9Discrete();
	TestPcm16RenderOutput();
	TestPatchToRackHandle();
	TestMasteringGain();
	TestMasteringDownmix();
	TestMasteringUnknownControl();
	TestPcmWaveformFormats();
	TestCustomFilterModule();
	TestCustomFilterChannelRelayout();
	TestReverbDryLevel();
	TestReverbMaskAndLevels();
	TestReverbRoomFilter();
	TestReverbLifecycleAndDecayRange();
	TestReverbTail();
	TestGeomStereoPlacement();
	TestGeomDoppler();
	TestGeomRolloffAndFlags();
	TestCustomMasteringChannelMixer();
	TestCustomVoiceStateCapacity();
	TestSubmixerSetupAndRouting();
	TestCustomSideChainUserFx();
	TestWaveformBlockGeometry();
	TestAtrac9ResampleAfterStarvation();
	TestAtrac9ResampleAcrossFrames();
	TestResamplingPhaseAfterStarvation();
	TestSamplerReuseResetsRouting();
	TestDeferredSamplerSetup();
	TestLoopCallbacksAndExit(Ngs2RackType::Sampler);
	TestLoopCallbacksAndExit(Ngs2RackType::CustomSampler);
	TestAtrac9Looping();
	TestAtrac9WaveformJoining();
	TestWaveformReadAddress();
	TestFilterTailDuringStarvation();
	TestFiniteFilterTail();
	TestLowPassResponse();
	TestFilterHistoryAndChannels();
	TestFilterUsesOutputRate();
	TestFilterControlLayout();
	TestUpstreamCustomSamplerControls();
	TestCallbackStopsConsumption();
	TestCallbackPauseDuringLargeStep();
	TestCallbackPauseDuringLookahead();
	TestCallbackReplacesSource();
	TestStreamingResample();
	TestPitchLoopAndSkip();
	TestPitchClamps();
	TestStarvationAndPause();
	TestPcmByteUnitStreaming();
	TestPcmSkipAcrossBlocks();
	TestPcmBlocksEndWithTheirData();
	TestMonoRateAndRouting();
	TestDefaultMatrix();
	TestMatrixOutputChannels();
	TestCustomPcmStillPlays();
	TestClearReopensClosedList();
	TestCancelSkipsBlocksNotStarted();
	TestSetupQuery();
	TestPortInfo();
	TestPlayStateBeforeRender();
	TestStatePublication();
#define RUN_TEST(fn) do { std::printf("Running %s...\n", #fn); std::fflush(stdout); fn(); } while(0)
	RUN_TEST(TestStopThenPlayBeforeRender);
	RUN_TEST(TestSystemRunCommandsAndQueries);
	RUN_TEST(TestRackRunCommandsAndQueries);
	RUN_TEST(TestVoiceOwnerAndMatrixInfo);
	RUN_TEST(TestFftPhase2);
	RUN_TEST(TestWaveformFrameInfoPhase2);
	RUN_TEST(TestParseWaveformUserPhase2);
	RUN_TEST(TestVagPhase3);
	RUN_TEST(TestVagPhase3Fixes);
	RUN_TEST(TestWaveformFileExtentLimits);
	RUN_TEST(TestWaveformBlockLimits);
	RUN_TEST(TestCustomGainPhase3);
	RUN_TEST(TestCustomChannelMixerPhase3);
	RUN_TEST(TestCustomPitchShiftPhase3);
	RUN_TEST(TestStreamAndModulesPhase4);
	RUN_TEST(TestRegistryLifetimeAndLocking);
	RUN_TEST(TestCustomFilterDesignRatePhase4);
	RUN_TEST(TestMasteringLfeCutoffPhase4);
	RUN_TEST(TestPitchShiftDrainPhase4);
	RUN_TEST(TestExtensibleFloatWaveformPhase4);
	RUN_TEST(TestAtrac9GeometryChannelsPhase4);
	RUN_TEST(TestSystemInfoAndRackRenderCountPhase6);
	std::puts("Ngs2SamplerTests: all cases passed");
}
