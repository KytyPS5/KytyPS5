// Exercise sampler controls and rendering without a host audio device.
#include "libs/ngs2.cpp"

#include <bit>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <numbers>
#include <thread>

using namespace Libs::Audio::Ngs2;

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "Ngs2SamplerTests: %s\n", message);
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
	// 24 kHz ATRAC9: frames of 128 samples, superframes of 4 frames in 192 bytes.
	const Ngs2WaveformFormat format {NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0};
	Ngs2WaveformBlock        block {};
	// Whether the block for `n` samples from `pos` has this offset, size and skip.
	auto is = [&](const Ngs2WaveformFormat& f, uint32_t pos, uint32_t n, size_t offset, size_t size,
	              uint32_t skip) {
		return Ngs2CalcWaveformBlock(&f, pos, n, &block) == OK && block.data_offset == offset &&
		       block.data_size == size && block.num_skip_samples == skip && block.num_samples == n;
	};
	// The skip holds the one-frame decoder delay, which can carry the end into the next superframe.
	Check(is(format, 513, 1, 192, 192, 129) && is(format, 400, 100, 0, 384, 528) &&
	          is(format, 0, 0, 0, 192, 128),
	      "ATRAC9 block calculation lost the decoder delay or the source offset");
	// Channel counts and sample rates are not checked. 0x3072c0fe holds 12 streams of 4 frames in
	// 64 bytes at 48 kHz.
	const Ngs2WaveformFormat streams {NGS2_WAVEFORM_TYPE_ATRAC9, 13, 0, 0x3072c0fe};
	Check(is(streams, 0, 256, 0, 3072, 256),
	      "block calculation checked the format or lost the streams of a multi-stream config");
	// Sample rate indexes 0 and 6 (44.1 kHz) and an unknown sync byte have no frames.
	for (const uint32_t config: {0xfe0005f0u, 0xfe6005f0u, 0x004005f0u}) {
		const Ngs2WaveformFormat frameless {NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, config};
		Check(Ngs2CalcWaveformBlock(&frameless, 0, 1, &block) == NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT,
		      "an ATRAC9 config without frames was given a block");
	}
	Check(Ngs2CalcWaveformBlock(&format, 0, 1, nullptr) == NGS2_ERROR_INVALID_OUT_ADDRESS,
	      "block calculation without an output block did not fail");
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
	custom.module[1]        = custom.module[0];
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
	} fx {{40, 0, 0x40001920}, MultiplyBySideChain, {7, 0, 0}};
	// The control's bits 5-7 are not read.
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK &&
	          Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &fx.header) == OK,
	      "custom submixer user FX setup failed");
	// Another module id, or an index past the modules even where the option holds the id, is
	// rejected and applies nothing.
	auto rejected    = fx;
	rejected.handler = nullptr;
	for (uint32_t id: {0x40009900u, 0x40001901u}) {
		rejected.header.id = id;
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &rejected.header) ==
		              NGS2_ERROR_INVALID_VOICE_PARAM_ID &&
		          voice.modules[0].handler == MultiplyBySideChain,
		      "a custom control for another module was applied");
	}
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
	Check(Ngs2SystemRender(system_handle, &output, 1) == OK && output_samples[0] == 24575 &&
	          output_samples[1] == -24575 && output_samples[2] == 32767 &&
	          output_samples[3] == -32767,
	      "PCM16 output did not truncate the mixed voices into -32767..32767");
	Check(Ngs2SystemRender(system_handle, &output, 1) == OK &&
	          std::ranges::all_of(output_samples, [](int16_t sample) { return sample == 0; }),
	      "PCM16 output retained samples from the previous render");
}

void TestInt16Conversion() {
	struct Case {
		float   sample;
		int16_t expected;
	};
	for (const auto [sample, expected]:
	     {Case {0.75f, 24575}, Case {-6e-5f, -1}, Case {1.5f, 32767}, Case {-1.0f, -32767},
	      Case {-2.0f, -32767}, Case {std::numeric_limits<float>::quiet_NaN(), 0}}) {
		Check(Ngs2ToInt16(sample) == expected, "a sample converted to the wrong int16 value");
	}
}

// Two mastering voices into one render buffer: an int16 buffer holds one voice's grain, a float
// buffer their sum.
void TestMasteringVoicesShareOutput() {
	Ngs2Internal system;
	system.option                   = Ngs2DefaultSystemOption();
	system.option.num_grain_samples = 4;
	Ngs2RackOptionUnion sampler_option {}, mastering_option {};
	Ngs2FillDefaultRackOption(0x1000, &sampler_option);
	Ngs2FillDefaultRackOption(0x3000, &mastering_option);
	sampler_option.common.max_voices   = 2;
	sampler_option.common.max_ports    = 1;
	mastering_option.common.max_voices = 2;
	RenderRack sampler(system, 0x1000, sampler_option.common);
	RenderRack master(system, 0x3000, mastering_option.common);
	const auto system_handle = reinterpret_cast<uintptr_t>(&system);
	auto*      samplers      = &sampler.Voice();
	auto*      masters       = &master.Voice();
	// Voice 0 plays L 0.25 and R 0.125, voice 1 mono 0.5.
	const int16_t stereo[8] = {8192, 4096, 8192, 4096, 8192, 4096, 8192, 4096};
	const int16_t mono[4]   = {16384, 16384, 16384, 16384};
	auto          play      = [&] {
		samplers[0].SetupSampler({0x12, 2, 48000});
		samplers[0].blocks.push_back(
		    {reinterpret_cast<const uint8_t*>(stereo), {0, sizeof(stereo), 0, 0, 4}});
		samplers[1].SetupSampler({0x12, 1, 48000});
		samplers[1].blocks.push_back(
		    {reinterpret_cast<const uint8_t*>(mono), {0, sizeof(mono), 0, 0, 4}});
		for (uint32_t i = 0; i < 2; ++i) {
			samplers[i].ports[0].dest = masters + i;
			samplers[i].SetEvent(1);
			masters[i].channels = 2 - i;
			masters[i].state    = Ngs2VoicePlayState::Playing;
		}
	};
	// Whether every frame of a 2ch grain holds `left` and `right`.
	auto frames_are = [](const auto* pcm, auto left, auto right) {
		bool same = true;
		for (uint32_t i = 0; i < 4; ++i) {
			same = same && pcm[i * 2] == left && pcm[i * 2 + 1] == right;
		}
		return same;
	};
	play();
	int16_t pcm16[8] {};
	int16_t unused[8];
	std::ranges::fill(unused, int16_t {0x7f7f});
	const Ngs2RenderBufferInfo buffers16[2] = {{pcm16, sizeof(pcm16), 0x12, 2},
	                                           {unused, sizeof(unused), 0x12, 2}};
	Check(Ngs2SystemRender(system_handle, buffers16, 2) == OK &&
	          (frames_are(pcm16, 8191, 4095) || frames_are(pcm16, 16383, 0)) &&
	          std::ranges::all_of(unused, [](int16_t sample) { return sample == 0; }),
	      "an int16 buffer did not hold one voice's grain or a buffer without voices was kept");
	play();
	float                      pcm[8];
	const Ngs2RenderBufferInfo buffer {pcm, sizeof(pcm), 0x18, 2};
	Check(Ngs2SystemRender(system_handle, &buffer, 1) == OK && frames_are(pcm, 0.75f, 0.125f),
	      "a float buffer did not sum the voices");
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

Ngs2RackOptionUnion SingleVoiceRackOption(uint32_t rack_id) {
	Ngs2RackOptionUnion option {};
	Ngs2FillDefaultRackOption(rack_id, &option);
	option.common.max_voices = 1;
	return option;
}

// A sampler voice feeding a mastering voice, rendered in grains of 4 samples.
struct MasteringRender {
	Ngs2Internal         system;
	RenderRack           sampler;
	RenderRack           master;
	Ngs2VoiceInternal&   source;
	Ngs2VoiceInternal&   voice;
	std::vector<int16_t> input;

	MasteringRender()
	    : sampler(system, 0x1000, SingleVoiceRackOption(0x1000).common),
	      master(system, 0x3000, SingleVoiceRackOption(0x3000).common), source(sampler.Voice()),
	      voice(master.Voice()) {
		system.option                   = Ngs2DefaultSystemOption();
		system.option.num_grain_samples = 4;
	}

	int Control(uint32_t cid, uint32_t first, uint32_t second = 0, uint16_t size = 16) {
		struct {
			Ngs2VoiceParamHeader header;
			uint32_t             values[2];
		} param {{size, 0, 0x30000000u | cid}, {first, second}};
		return Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &param.header);
	}

	// Sets the mastering voice up for `channels` inputs and plays it.
	int Setup(uint32_t channels) {
		const int result = Control(0, channels);
		voice.SetEvent(1);
		return result;
	}

	int Gain(float fbw, float lfe) {
		return Control(4, std::bit_cast<uint32_t>(fbw), std::bit_cast<uint32_t>(lfe));
	}

	int Limiter(uint32_t enable_flag, float threshold) {
		return Control(3, enable_flag, std::bit_cast<uint32_t>(threshold));
	}

	int Matrix(uint32_t type, const float* levels, uint32_t count, uint16_t size = 24) {
		Ngs2VoiceMatrixLevelsParam param {{size, 0, 0x30000001}, type, count, levels};
		return Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &param.header);
	}

	Ngs2MasteringVoiceState State() {
		Ngs2MasteringVoiceState state {};
		Check(Ngs2VoiceGetState(reinterpret_cast<uintptr_t>(&voice), &state.voice_state,
		                        sizeof(state)) == OK,
		      "mastering state query failed");
		return state;
	}

	// Plays `frames` copies of `frame` into the mastering voice.
	void Play(const std::vector<int16_t>& frame, uint32_t frames) {
		source.SetupSampler({0x12, static_cast<uint32_t>(frame.size()), 48000});
		input.clear();
		for (uint32_t i = 0; i < frames; ++i) {
			input.insert(input.end(), frame.begin(), frame.end());
		}
		source.blocks.push_back({reinterpret_cast<const uint8_t*>(input.data()),
		                         {0, input.size() * sizeof(int16_t), 0, 0, frames}});
		source.ports[0].dest = &voice;
		source.SetEvent(1);
	}

	// Renders one grain into a float buffer of `channels` interleaved channels.
	int Render(float* pcm, uint32_t channels) {
		Ngs2RenderBufferInfo output {
		    pcm, system.option.num_grain_samples * channels * sizeof(float), 0x18, channels};
		return Ngs2SystemRender(reinterpret_cast<uintptr_t>(&system), &output, 1);
	}
};

void TestMasteringGainRamp() {
	MasteringRender m;
	float           pcm[32];
	auto check = [&](const std::array<float, 4>& fbw, const std::array<float, 4>& lfe,
	                 const char* message) {
		for (uint32_t i = 0; i < 4; ++i) {
			for (uint32_t c = 0; c < 8; ++c) {
				Check(pcm[i * 8 + c] == (c == 3 ? lfe[i] : fbw[i]), message);
			}
		}
	};
	// The first grain of a gain starts at the clamped gains, even without input.
	Check(m.Setup(8) == OK && m.Gain(40.0f, -0.5f) == OK && m.Render(pcm, 8) == OK &&
	          std::ranges::all_of(pcm, [](float sample) { return sample == 0.0f; }),
	      "silent 7.1ch mastering voice failed to render");
	m.Play(std::vector<int16_t>(8, 8192), 20);
	Check(m.Gain(-40.0f, 1.5f) == OK && m.Render(pcm, 8) == OK,
	      "7.1ch mastering voice failed to render a gain change");
	check({8.0f, 4.0f, 0.0f, -4.0f}, {-0.125f, 0.0f, 0.125f, 0.25f},
	      "mastering gain did not ramp from the previous grain's clamped full band and LFE levels");
	// Setup disables the gain, but a gain set before the next grain ramps from the previous one.
	Check(m.Control(0, 8) == OK && m.Gain(3.0f, 3.0f) == OK && m.Render(pcm, 8) == OK,
	      "7.1ch mastering voice failed to render after Setup");
	check({-8.0f, -5.8125f, -3.625f, -1.4375f}, {0.375f, 0.46875f, 0.5625f, 0.65625f},
	      "a gain set right after Setup did not ramp from the gain before Setup");
	// A grain without the gain plays at unity, and the next gain starts without a ramp.
	Check(m.Setup(8) == OK && m.Render(pcm, 8) == OK, "7.1ch mastering voice failed to render");
	check({0.25f, 0.25f, 0.25f, 0.25f}, {0.25f, 0.25f, 0.25f, 0.25f},
	      "Setup did not disable the gain");
	Check(m.Gain(2.0f, 2.0f) == OK && m.Render(pcm, 8) == OK,
	      "7.1ch mastering voice failed to render");
	check({0.5f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f},
	      "a gain after a grain without gain ramped");
	// Every PLAY, also on a playing voice, restarts the gain without a ramp.
	Check(m.Setup(8) == OK && m.Gain(3.0f, 3.0f) == OK && m.Render(pcm, 8) == OK,
	      "7.1ch mastering voice failed to render after a repeated PLAY");
	check({0.75f, 0.75f, 0.75f, 0.75f}, {0.75f, 0.75f, 0.75f, 0.75f},
	      "the first gain after PLAY on a playing voice ramped");
}

void TestMasteringLfeDesignRate() {
	constexpr std::array<std::array<uint32_t, 2>, 12> rates = {{{11025, 11025},
	                                                            {11026, 12000},
	                                                            {12001, 22050},
	                                                            {22051, 24000},
	                                                            {24001, 44100},
	                                                            {44101, 48000},
	                                                            {48001, 88200},
	                                                            {96000, 88200},
	                                                            {96001, 176400},
	                                                            {176401, 192000},
	                                                            {192000, 192000},
	                                                            {192001, 11025}}};
	for (const auto& [rate, design_rate]: rates) {
		Check(Ngs2MasteringLfeDesignRate(rate) == design_rate,
		      "a system rate selected the wrong LFE filter design rate");
	}
	// A 96 kHz system uses the 88200 Hz design of the 120 Hz filter.
	Ngs2VoiceInternal::Mastering mastering;
	Ngs2SetMasteringLfe(mastering, 120, 96000);
	const auto& [first, second] = mastering.lfe;
	Check(std::abs(first.gain / 2.71270006e-10f - 1.0f) < 1e-6f &&
	          std::abs(first.a1 - 1.98911059f) < 1e-6f &&
	          std::abs(first.a2 + 0.989155829f) < 1e-6f && second.gain == 1.0f &&
	          std::abs(second.a1 - 1.99539697f) < 1e-6f &&
	          std::abs(second.a2 + 0.995493948f) < 1e-6f,
	      "a 96 kHz system did not use the 88200 Hz LFE filter design");
}

// The coefficients {gain, a1, a2} of both LFE sections.
using LfeCoefficients = std::array<std::array<float, 3>, 2>;

// Two LFE sections filtering one sample at a time.
struct LfeReference {
	std::array<std::array<float, 4>, 2> history {};

	float Filter(const LfeCoefficients& k, float x) {
		for (size_t s = 0; s < 2; ++s) {
			auto& h                = history[s];
			const auto [g, a1, a2] = k[s];
			const float y          = g * x + 2.0f * g * h[0] + g * h[1] + a1 * h[2] + a2 * h[3];
			h                      = {x, h[0], y, h[2]};
			x                      = y;
		}
		return x;
	}
};

void TestMasteringLfeFilter() {
	// The 48 kHz filters of 80 Hz and 120 Hz.
	constexpr LfeCoefficients lfe_80 = {
	    {{6.09809425e-10f, 1.9866643f, -0.986732125f}, {1.0f, 1.99433744f, -0.994482934f}}};
	constexpr LfeCoefficients lfe_120 = {
	    {{3.07265147e-09f, 1.98001206f, -0.98016417f}, {1.0f, 1.9914093f, -0.991736054f}}};
	MasteringRender m;
	// 5.1ch: L 0.25 throughout, LFE impulses of 0.5 at frames 0 and 20.
	m.Play({8192, 0, 0, 0, 0, 0}, 32);
	m.input[0 * 6 + 3] = m.input[20 * 6 + 3] = 16384;
	float        pcm[24];
	LfeReference reference;
	// Checks a grain whose LFE starts with `first`, filtered by `k` unless it is null.
	auto check = [&](const LfeCoefficients* k, float first, const char* message) {
		for (uint32_t i = 0; i < 4; ++i) {
			const float x        = i == 0 ? first : 0.0f;
			const float expected = k == nullptr ? x : reference.Filter(*k, x);
			Check(pcm[i * 6] == 0.25f &&
			          std::abs(pcm[i * 6 + 3] - expected) <= 1e-5f * std::abs(expected),
			      message);
		}
	};
	// The enable flag is clamped to 1 and fc to 80..120 Hz, up to 80 Hz selecting 80 Hz.
	Check(m.Setup(6) == OK && m.Control(2, 7, 0) == OK && m.Render(pcm, 6) == OK,
	      "5.1ch mastering voice failed to render with the LFE filter");
	check(&lfe_80, 0.5f, "the LFE filter did not run the clamped 80 Hz filter on channel 3");
	// A changed cutoff keeps the history; fc above 80 Hz selects 120 Hz.
	Check(m.Control(2, 1, 100) == OK && m.Render(pcm, 6) == OK,
	      "5.1ch mastering voice failed to render an LFE change");
	check(&lfe_120, 0.0f, "an LFE cutoff change did not keep the history with the 120 Hz filter");
	// A cleared enable flag leaves channel 3 and freezes the history.
	Check(m.Control(2, 0, 100) == OK && m.Render(pcm, 6) == OK,
	      "5.1ch mastering voice failed to render a disabled LFE filter");
	check(nullptr, 0.0f, "a disabled LFE filter changed channel 3");
	Check(m.Control(2, 1, 1000) == OK && m.Render(pcm, 6) == OK,
	      "5.1ch mastering voice failed to render a re-enabled LFE filter");
	check(&lfe_120, 0.0f, "a re-enabled LFE filter did not go on from the frozen history");
	// A voice without channel 3 leaves the history.
	const auto history = m.voice.mastering->lfe;
	Check(m.Control(0, 3) == OK && m.Control(2, 1, 120) == OK && m.Render(pcm, 3) == OK &&
	          std::memcmp(&history, &m.voice.mastering->lfe, sizeof(history)) == 0,
	      "the LFE filter ran on a voice without channel 3");
	// PLAY zeroes the history.
	reference = {};
	Check(m.Setup(6) == OK && m.Control(2, 1, 120) == OK && m.Render(pcm, 6) == OK,
	      "5.1ch mastering voice failed to render after PLAY");
	check(&lfe_120, 0.5f, "PLAY did not zero the LFE filter history");
	// Setup turns the filter off.
	Check(m.Control(0, 6) == OK && m.Render(pcm, 6) == OK,
	      "5.1ch mastering voice failed to render");
	check(nullptr, 0.0f, "Setup did not turn the LFE filter off");
	// After a grain without the filter, its control zeroes the history.
	reference = {};
	Check(m.Control(2, 1, 120) == OK && m.Render(pcm, 6) == OK,
	      "5.1ch mastering voice failed to render a restarted LFE filter");
	check(&lfe_120, 0.0f, "an LFE filter after a grain without it kept its history");
}

void TestMasteringLimiter() {
	MasteringRender m;
	// 5.1ch: L 0.125 and LFE 0.5, the LFE at 0.0625 in frames 36..67.
	m.Play({4096, 0, 0, 16384, 0, 0}, 4888);
	for (uint32_t i = 36; i < 68; ++i) {
		m.input[i * 6 + 3] = 2048;
	}
	float        pcm[64 * 6];
	const double attack  = std::exp(-1.0f / 480.0f);
	const double release = std::exp(-1.0f / 4800.0f);
	// Checks `count` frames from `frame` on against a gain going from `gain` toward `target` with
	// coefficient `a`, channel 0 playing `left` and channel 3 `lfe`.
	auto follows = [&](uint32_t frame, uint32_t count, double& gain, double target, double a,
	                   float left, float lfe) {
		bool ok = true;
		for (uint32_t i = frame; i < frame + count; ++i) {
			gain = a * gain + (1.0 - a) * target;
			ok   = ok && std::abs(pcm[i * 6] - left * gain) <= 1e-6 &&
			       std::abs(pcm[i * 6 + 3] - lfe * gain) <= 1e-6;
		}
		return ok;
	};
	// A grain at the threshold 0.25 from unity gain: the LFE's peak limits every channel.
	auto fresh = [&] {
		double gain = 1.0;
		return follows(0, 4, gain, 0.5, attack, 0.125f, 0.5f);
	};
	auto unlimited = [&] {
		return pcm[0] == 0.125f && pcm[3] == 0.5f && pcm[18] == 0.125f && pcm[21] == 0.5f;
	};
	auto state = [&](float peak, float press) {
		const auto s = m.State();
		return s.limiter_peak_level == peak && s.limiter_press_level == press;
	};
	Check(m.Setup(6) == OK && m.Limiter(1, 0.25f) == OK && m.Render(pcm, 6) == OK && fresh(),
	      "the limiter did not limit every channel by their common peak");
	// Each block of 32 samples sets its own target: the first one attacks, the second releases.
	m.system.option.num_grain_samples = 64;
	double gain                       = m.State().limiter_press_level;
	Check(m.Render(pcm, 6) == OK && follows(0, 32, gain, 0.5, attack, 0.125f, 0.5f) &&
	          follows(32, 32, gain, 1.0, release, 0.125f, 0.0625f) &&
	          state(0.125f, m.voice.mastering->limiter_gain),
	      "the limiter did not attack and release by blocks of 32 samples");
	m.system.option.num_grain_samples = 4;
	// A cleared enable flag passes the audio at unity gain, from which the limiter restarts.
	Check(m.Limiter(0, 0.25f) == OK && m.Render(pcm, 6) == OK && unlimited() && state(0.0f, 1.0f),
	      "a bypassed limiter changed the audio or its state");
	Check(m.Limiter(1, 0.25f) == OK && m.Render(pcm, 6) == OK && fresh(),
	      "a re-enabled limiter did not start at unity gain");
	m.voice.SetEvent(1);
	Check(m.Render(pcm, 6) == OK && fresh(), "PLAY did not restore unity limiter gain");
	// Setup turns the limiter off and clears its state; a grain without it restores unity gain.
	Check(m.Control(0, 6) == OK && m.Render(pcm, 6) == OK && unlimited() && state(0.0f, 0.0f),
	      "Setup did not turn the limiter off or clear its state");
	Check(m.Limiter(1, 0.25f) == OK && m.Render(pcm, 6) == OK && fresh(),
	      "a limiter after a grain without it did not start at unity gain");
	// From unity gain toward 0.75, steps of four samples stop moving the gain in block 150, which
	// then takes the target.
	m.voice.SetEvent(1);
	m.system.option.num_grain_samples = 32;
	Check(m.Limiter(1, 0.375f) == OK, "the limiter control failed");
	for (uint32_t i = 0; i < 149; ++i) {
		Check(m.Render(pcm, 6) == OK, "5.1ch mastering voice failed to render");
	}
	const float settling = m.State().limiter_press_level;
	Check(settling != 0.75f && std::abs(settling - 0.75f) < 0.01f && m.Render(pcm, 6) == OK &&
	          state(0.5f, 0.75f),
	      "the limiter gain did not settle on its target in block 150");
	// The threshold is clamped to 0..1 and kept when NaN; time constants are whole samples.
	m.system.option.sample_rate = 22050;
	const auto& mastering       = *m.voice.mastering;
	Check(m.Limiter(1, 3.0f) == OK && mastering.limiter_threshold == 1.0f &&
	          mastering.limiter_attack == std::exp(-1.0f / 220.0f) &&
	          mastering.limiter_release == std::exp(-1.0f / 2205.0f) && m.Limiter(1, -1.0f) == OK &&
	          mastering.limiter_threshold == 0.0f &&
	          m.Limiter(1, std::numeric_limits<float>::quiet_NaN()) == OK &&
	          std::isnan(mastering.limiter_threshold),
	      "the limiter threshold or time constants were wrong");
}

void TestMasteringPeakMeters() {
	MasteringRender m;
	m.system.option.num_grain_samples = 64;
	// 5.1ch at L 0.5, R 0.25, C 0.125, LFE 0.0625, SL 0.03125, SR -0.25, and a quarter of that in
	// grains 2..9, 11, 12 and 16.
	m.Play({16384, 8192, 4096, 2048, 1024, -8192}, 17 * 64);
	for (uint32_t grain: {2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 16}) {
		for (uint32_t i = grain * 64 * 6; i < (grain + 1) * 64 * 6; ++i) {
			m.input[i] /= 4;
		}
	}
	// The input meter reads every channel, the output meter the stereo downmix after a gain of 0.5.
	struct Peaks {
		std::array<float, 6> input;
		std::array<float, 2> output;
	};
	constexpr float h    = std::numbers::sqrt2_v<float> / 2;
	const Peaks     loud = {{0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 0.25f},
	                        {(0.5f + 0.125f * h + 0.03125f * h) * 0.5f,
	                         std::abs(0.25f + 0.125f * h - 0.25f * h) * 0.5f}};
	// The loud peaks with the input meter's scaled by `input` and the output meter's by `output`.
	auto scaled = [&](float input, float output) {
		auto peaks = loud;
		std::ranges::for_each(peaks.input, [&](float& peak) { peak *= input; });
		std::ranges::for_each(peaks.output, [&](float& peak) { peak *= output; });
		return peaks;
	};
	const Peaks quiet = scaled(0.25f, 0.25f);
	const Peaks none  = scaled(0.0f, 0.0f);
	float       pcm[64 * 2];
	// Renders a grain and checks both meters, every other channel reading zero.
	auto render = [&](const Peaks& expected) {
		Check(m.Render(pcm, 2) == OK, "5.1ch mastering voice failed to render");
		const auto state = m.State();
		bool       ok    = true;
		for (size_t c = 0; c < 37; ++c) {
			const float in  = c < 6 ? expected.input[c] : 0.0f;
			const float out = c < 2 ? expected.output[c] : 0.0f;
			ok              = ok && std::abs(state.input_peak_height[c] - in) <= 1e-5f * in &&
			                  std::abs(state.output_peak_height[c] - out) <= 1e-5f * out;
		}
		return ok;
	};
	Check(m.Setup(6) == OK && m.Control(6, 3) == NGS2_ERROR_INVALID_PEAK_METER_DETECT_TYPE &&
	          render(none),
	      "a peak meter detect type above 2 was accepted");
	Check(m.Control(6, 1) == OK && m.Gain(0.5f, 0.5f) == OK && render(loud),
	      "the peak meters did not read before the converter and after the gain");
	// The window holds 8 grains, and the same detect type keeps it.
	for (uint32_t i = 0; i < 6; ++i) {
		Check(render(loud), "the peak meter window dropped a grain early");
	}
	Check(m.Control(6, 1) == OK && render(loud), "repeating the detect type restarted the window");
	Check(render(quiet), "the peak meter window kept a grain past its 8 blocks");
	// A detect type change restarts the window; RMS takes the energy of every sample in it.
	const float rms = std::sqrt(17.0f / 32.0f);
	Check(m.Control(6, 2) == OK && render(loud), "a detect type change did not restart the window");
	Check(render(scaled(rms, rms)), "the RMS did not average the window's samples");
	m.voice.SetEvent(1);
	Check(render(quiet), "PLAY did not restart the peak meter window");
	Check(m.Control(6, 0) == OK && render(none), "detect type 0 reported peaks");
	Check(m.Control(6, 1) == OK && render(loud), "peak meters did not restart after detect type 0");
	// Setup clears the reports and turns the meters off; a grain without them restarts the window.
	Check(m.Control(0, 6) == OK && m.State().input_peak_height[0] == 0.0f && render(none),
	      "Setup did not clear or turn off the peak meters");
	Check(m.Control(6, 1) == OK && render(scaled(0.25f, 0.5f)),
	      "peak meters after a grain without them kept their window");
}

void TestMasteringDownmix() {
	MasteringRender m;
	constexpr float h = std::numbers::sqrt2_v<float> / 2;
	// 5.1ch: L 0.25, R 0.125, C 0.0625, LFE 0.5, SL 0.03125, SR 0.015625.
	m.Play({8192, 4096, 2048, 16384, 1024, 512}, 16);
	float pcm[8];
	auto  check = [&](const std::array<float, 4>& left, const std::array<float, 4>& right,
	                  const char* message) {
		for (uint32_t i = 0; i < 4; ++i) {
			Check(pcm[i * 2] == left[i] && pcm[i * 2 + 1] == right[i], message);
		}
	};
	// Game levels are dst-major: R feeds L twice, SL feeds R four times.
	float levels[12] = {};
	levels[0 * 6 + 1] = 2.0f;
	levels[1 * 6 + 4] = 4.0f;
	Check(m.Setup(6) == OK && m.Matrix(9, levels, 12) == OK && m.Render(pcm, 2) == OK,
	      "5.1ch mastering voice failed to render into stereo");
	check({0.25f, 0.25f, 0.25f, 0.25f}, {0.125f, 0.125f, 0.125f, 0.125f},
	      "5.1ch -> 2.0ch matrix levels were not read dst-major");
	// A changed level ramps over one grain; a level that is never positive is skipped.
	levels[0 * 6 + 1] = 0.0f;
	levels[1 * 6 + 0] = -1.0f;
	Check(m.Matrix(9, levels, 12) == OK && m.Render(pcm, 2) == OK,
	      "5.1ch mastering voice failed to render a matrix change");
	check({0.25f, 0.1875f, 0.125f, 0.0625f}, {0.125f, 0.125f, 0.125f, 0.125f},
	      "5.1ch -> 2.0ch matrix change did not ramp or mixed a negative level");
	// Setup restores the default levels, ramping from the previous grain's.
	Check(m.Control(0, 6) == OK && m.Render(pcm, 2) == OK && pcm[0] == 0.0f && pcm[1] == 0.125f,
	      "Setup did not ramp from the previous matrix levels");
	Check(m.Render(pcm, 2) == OK, "5.1ch mastering voice failed to render after Setup");
	const float left  = 0.25f + h * 0.0625f + h * 0.03125f;
	const float right = 0.125f + h * 0.0625f + h * 0.015625f;
	check({left, left, left, left}, {right, right, right, right},
	      "5.1ch -> 2.0ch default levels were wrong");
	Check(std::all_of(m.voice.samples.begin() + 2 * 4, m.voice.samples.end(),
	                  [](float sample) { return sample == 0.0f; }),
	      "the converter did not zero the channels past the output");
}

void TestMasteringMonoStereoConversion() {
	MasteringRender m;
	m.Play({8192}, 4);
	float pcm[8];
	Check(m.Setup(1) == OK && m.Render(pcm, 2) == OK, "mono mastering voice failed to render");
	for (uint32_t i = 0; i < 4; ++i) {
		Check(pcm[i * 2] == 0.25f && pcm[i * 2 + 1] == 0.0f,
		      "mono mastering voice did not play in the first output channel only");
	}
	const float downmix = 0.75f * std::numbers::sqrt2_v<float> / 2;
	m.Play({8192, 16384}, 4);
	Check(m.Setup(2) == OK && m.Render(pcm, 1) == OK &&
	          std::all_of(pcm, pcm + 4, [&](float sample) { return sample == downmix; }),
	      "stereo mastering voice did not downmix into a mono output");
}

void TestMasteringUntypedConversions() {
	MasteringRender m;
	constexpr float h = std::numbers::sqrt2_v<float> / 2;
	// 3ch L 0.25, R 0.125, C 0.0625: the centre folds into L and R and channel 2 stays silent.
	m.Play({8192, 4096, 2048}, 4);
	float pcm[32];
	Check(m.Setup(3) == OK && m.Render(pcm, 8) == OK, "3ch mastering voice failed to render");
	for (uint32_t i = 0; i < 4; ++i) {
		Check(pcm[i * 8] == 0.25f + h * 0.0625f && pcm[i * 8 + 1] == 0.125f + h * 0.0625f &&
		          std::all_of(pcm + i * 8 + 2, pcm + i * 8 + 8,
		                      [](float sample) { return sample == 0.0f; }),
		      "3ch -> 3ch default levels were wrong");
	}
	// 4ch L, R, SL 0.0625, SR 0.03125 into stereo: each surround joins its side at -3 dB.
	m.Play({8192, 4096, 2048, 1024}, 4);
	Check(m.Setup(4) == OK && m.Render(pcm, 2) == OK, "4ch mastering voice failed to render");
	for (uint32_t i = 0; i < 4; ++i) {
		Check(pcm[i * 2] == 0.25f + h * 0.0625f && pcm[i * 2 + 1] == 0.125f + h * 0.03125f,
		      "4ch -> 2ch default levels were wrong");
	}
}

void TestMasteringMatrixSpill() {
	MasteringRender m;
	m.Play({8192, 4096}, 12);
	// The 2.0ch -> 1.0ch levels are followed by the 2.0ch -> 2.0ch ones, here swapping L and R.
	// The array holds as many levels as the converter, so an uncut count reads only from it.
	std::array<float, NGS2_MASTERING_DEFAULT_LEVELS.size()> levels {};
	levels[3] = levels[4] = 1.0f;
	float pcm[8];
	Check(m.Setup(2) == OK && m.Matrix(4, levels.data(), 0x10002) == OK &&
	          m.Render(pcm, 2) == OK && pcm[0] == 0.25f && pcm[1] == 0.125f,
	      "a level count was not cut to 16 bits");
	Check(m.Matrix(4, levels.data(), 6) == OK && m.Render(pcm, 2) == OK &&
	          m.Render(pcm, 2) == OK && pcm[0] == 0.125f && pcm[1] == 0.25f,
	      "extra 2.0ch -> 1.0ch levels did not overwrite the 2.0ch -> 2.0ch levels");
}

void TestMasteringOutputRange() {
	MasteringRender m;
	m.Play({8192, -8192}, 8);
	float pcm[8];
	Check(m.Setup(2) == OK && m.Control(5, 1) == OK && m.Render(pcm, 2) == OK &&
	          std::ranges::all_of(pcm, [](float sample) { return sample == 0.0f; }),
	      "an output id past the render buffers failed or produced output");
	Check(m.Control(5, 99) == OK && m.voice.output_id == 32, "output id was not clamped to 32");
	Check(m.Control(5, 0) == OK && m.Render(pcm, 2) == OK && pcm[0] == 0.25f && pcm[1] == -0.25f,
	      "mastering voice did not play into render buffer 0");
}

void TestMasteringControlErrors() {
	MasteringRender m;
	const float     levels[64] = {};
	Check(m.Control(0, 8) == OK &&
	          m.Control(0, 0, 0, 15) == NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE &&
	          m.Control(2, 0, 0, 15) == NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE &&
	          m.Control(3, 0, 0, 15) == NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE &&
	          m.Control(6, 0, 0, 15) == NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE &&
	          m.Control(4, 0, 0, 15) == NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE &&
	          m.Control(5, 0, 0, 15) == NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE &&
	          m.Matrix(5, levels, 4, 23) == NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE,
	      "undersized mastering controls were accepted or checked after their values");
	Check(m.Control(0, 0) == NGS2_ERROR_INVALID_NUM_CHANNELS &&
	          m.Control(0, 9) == NGS2_ERROR_INVALID_NUM_CHANNELS && m.voice.channels == 8,
	      "mastering setup accepted a channel count outside 1..maxChannels");
	Check(m.Matrix(16, levels, 4) == NGS2_ERROR_INVALID_MATRIX_TYPE &&
	          m.Matrix(15, levels, 64) == OK && m.Matrix(3, levels, 8) == OK,
	      "mastering matrix type range was wrong");
	Check(m.Control(7, 0) == NGS2_ERROR_INVALID_VOICE_CONTROL_ID &&
	          m.Control(7, 0, 0, 8) == NGS2_ERROR_INVALID_VOICE_CONTROL_ID &&
	          m.Control(0x8000, 0) == NGS2_ERROR_INVALID_VOICE_CONTROL_ID &&
	          m.Control(0xffff, 0, 0, 8) == NGS2_ERROR_INVALID_VOICE_CONTROL_ID,
	      "unknown mastering control was not rejected by id");
}

int32_t KYTY_SYSV_ABI RejectRackBuffer(Ngs2ContextBufferInfo* /*buffer*/) {
	Check(false, "a rack buffer was allocated or freed for a rejected option");
	return -1;
}

void TestMasteringRackOptionRanges() {
	Ngs2Internal              system;
	const auto                system_handle = reinterpret_cast<uintptr_t>(&system);
	const Ngs2BufferAllocator allocator {RejectRackBuffer, RejectRackBuffer, 0};
	// The rack buffer is sized for a valid option.
	auto                  option = SingleVoiceRackOption(0x3000);
	Ngs2ContextBufferInfo buffer {};
	Check(Ngs2RackQueryBufferSize(0x3000, &option.common, &buffer) == OK,
	      "mastering rack size query failed");
	const auto storage = std::make_unique<std::byte[]>(buffer.host_buffer_size);
	buffer.host_buffer = storage.get();
	struct Case {
		uint32_t max_channels, blocks;
		int32_t  error;
	};
	for (const auto [max_channels, blocks, error]:
	     {Case {0, 8, NGS2_ERROR_INVALID_NUM_CHANNELS},
	      Case {38, 8, NGS2_ERROR_INVALID_NUM_CHANNELS},
	      Case {8, 0, NGS2_ERROR_INVALID_PEAK_METER_NUM_BLOCKS},
	      Case {8, 257, NGS2_ERROR_INVALID_PEAK_METER_NUM_BLOCKS}}) {
		option.mastering.max_channels          = max_channels;
		option.mastering.num_peak_meter_blocks = blocks;
		Ngs2ContextBufferInfo query {};
		uintptr_t             handle = 0;
		Check(Ngs2RackQueryBufferSize(0x3000, &option.common, &query) == error &&
		          Ngs2RackCreate(system_handle, 0x3000, &option.common, &buffer, &handle) ==
		              error &&
		          Ngs2RackCreateWithAllocator(system_handle, 0x3000, &option.common, &allocator,
		                                      &handle) == error &&
		          handle == 0,
		      "mastering rack accepted a channel count outside 1..37 or a peak meter block count "
		      "outside 1..256");
	}
	option.mastering.max_channels          = 37;
	option.mastering.num_peak_meter_blocks = 256;
	RenderRack rack(system, 0x3000, option.common);
	Check(rack.Voice().mastering->meter_blocks.size() == 2 * 37 * 256,
	      "a peak meter window was not sized for every channel and block");
}

void TestRackGetVoiceHandleErrors() {
	Ngs2Internal system;
	auto         option = SingleVoiceRackOption(0x1000);
	option.common.max_voices = 3;
	RenderRack rack(system, 0x1000, option.common);
	// The handle of a destroyed rack, whose buffer is still allocated.
	std::unique_ptr<std::byte[]> storage;
	uintptr_t                    destroyed = 0;
	{
		RenderRack dead(system, 0x1000, option.common);
		destroyed = dead.handle;
		storage   = std::move(dead.storage);
	}
	Check(Ngs2RackDestroy(destroyed, nullptr) == NGS2_ERROR_INVALID_RACK_HANDLE,
	      "a destroyed rack was destroyed again");
	constexpr uintptr_t sentinel = 0x5a5a;
	uintptr_t           handle   = sentinel;
	Check(Ngs2RackGetVoiceHandle(destroyed, 0, &handle) == NGS2_ERROR_INVALID_RACK_HANDLE &&
	          Ngs2RackGetVoiceHandle(0, 0, &handle) == NGS2_ERROR_INVALID_RACK_HANDLE &&
	          Ngs2RackGetVoiceHandle(destroyed, 0, nullptr) == NGS2_ERROR_INVALID_RACK_HANDLE &&
	          Ngs2RackGetVoiceHandle(rack.handle, 3, nullptr) == NGS2_ERROR_INVALID_OUT_ADDRESS &&
	          Ngs2RackGetVoiceHandle(rack.handle, 3, &handle) == NGS2_ERROR_INVALID_VOICE_INDEX &&
	          handle == sentinel,
	      "voice handle lookup did not reject the rack, the out address and the index in order");
	Check(Ngs2RackGetVoiceHandle(rack.handle, 2, &handle) == OK &&
	          handle == reinterpret_cast<uintptr_t>(&rack.Voice() + 2),
	      "voice handle lookup did not return the last voice");
}

// The context buffers the test allocator freed.
std::vector<Ngs2ContextBufferInfo> g_freed_buffers;

int32_t KYTY_SYSV_ABI AllocContextBuffer(Ngs2ContextBufferInfo* buffer) {
	buffer->host_buffer = std::malloc(buffer->host_buffer_size);
	return OK;
}

// Frees the buffer, overwrites its argument and fails.
int32_t KYTY_SYSV_ABI FreeContextBuffer(Ngs2ContextBufferInfo* buffer) {
	g_freed_buffers.push_back(*buffer);
	std::free(buffer->host_buffer);
	*buffer = {};
	return -1;
}

bool SameBuffer(const Ngs2ContextBufferInfo& a, const Ngs2ContextBufferInfo& b) {
	return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void TestSystemDestroy() {
	const Ngs2BufferAllocator allocator {AllocContextBuffer, FreeContextBuffer, 0x77};
	auto                      sampler_option = SingleVoiceRackOption(0x1000);
	auto                      master_option  = SingleVoiceRackOption(0x3000);
	uintptr_t                 system = 0, sampler = 0, master = 0, kept = 0;
	Check(Ngs2SystemCreateWithAllocator(nullptr, &allocator, &system) == OK &&
	          Ngs2RackCreateWithAllocator(system, 0x1000, &sampler_option.common, &allocator,
	                                      &sampler) == OK &&
	          Ngs2RackCreateWithAllocator(system, 0x3000, &master_option.common, &allocator,
	                                      &master) == OK &&
	          Ngs2RackCreateWithAllocator(system, 0x3000, &master_option.common, &allocator,
	                                      &kept) == OK,
	      "allocator system or rack creation failed");
	auto*                 rack         = reinterpret_cast<Ngs2RackInternal*>(sampler);
	auto&                 voice        = *reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
	const auto            voice_handle = reinterpret_cast<uintptr_t>(&voice);
	Ngs2ContextBufferInfo sentinel;
	std::memset(&sentinel, 0x5a, sizeof(sentinel));

	// Destroying a rack disconnects the voices patched into it and ignores the free result.
	Ngs2VoicePatchParam patch {{24, 0, 5}, 0, 0, master};
	Check(Ngs2VoiceControl(voice_handle, &patch.header) == OK && voice.ports[0].dest != nullptr,
	      "patch to the mastering rack failed");
	const auto master_buffer = reinterpret_cast<const Ngs2RackInternal*>(master)->buffer_info;
	auto       out           = sentinel;
	Check(Ngs2RackDestroy(master, &out) == OK && SameBuffer(out, master_buffer) &&
	          g_freed_buffers.size() == 1 && SameBuffer(g_freed_buffers[0], master_buffer) &&
	          voice.ports[0].dest == nullptr,
	      "rack destroy did not copy and free its buffer or kept a patch into the rack");
	out = sentinel;
	Check(Ngs2RackDestroy(master, &out) == NGS2_ERROR_INVALID_RACK_HANDLE &&
	          SameBuffer(out, sentinel) && g_freed_buffers.size() == 1,
	      "a destroyed rack was destroyed again or its out-info was written");

	// Destroying the system frees its buffer and detaches its racks, not another system's.
	uintptr_t other = 0, other_rack = 0;
	Check(Ngs2SystemCreateWithAllocator(nullptr, &allocator, &other) == OK &&
	          Ngs2RackCreateWithAllocator(other, 0x1000, &sampler_option.common, &allocator,
	                                      &other_rack) == OK,
	      "second allocator system or rack creation failed");
	patch.dest_handle = kept;
	Check(Ngs2VoiceControl(voice_handle, &patch.header) == OK && voice.ports[0].dest != nullptr,
	      "patch to the kept mastering rack failed");
	const auto system_buffer = reinterpret_cast<const Ngs2Internal*>(system)->buffer_info;
	out                      = sentinel;
	Check(Ngs2SystemDestroy(system, &out) == OK && SameBuffer(out, system_buffer) &&
	          g_freed_buffers.size() == 2 && SameBuffer(g_freed_buffers[1], system_buffer),
	      "system destroy did not copy and free its buffer");
	out = sentinel;
	Check(Ngs2SystemDestroy(system, &out) == NGS2_ERROR_INVALID_SYSTEM_HANDLE &&
	          SameBuffer(out, sentinel) && g_freed_buffers.size() == 2,
	      "a destroyed system was destroyed again or its out-info was written");
	Ngs2SystemInfo info {};
	Check(Ngs2SystemGetInfo(other, &info, sizeof(info)) == OK && info.rack_count == 1,
	      "destroying a system detached another system's rack");

	// The racks stay valid handles on a placeholder system, disconnected from their patches, and
	// their voice calls apply.
	Ngs2VoiceEventParam   play {{sizeof(Ngs2VoiceEventParam), 0, 6}, 1};
	Ngs2SamplerVoiceState state {};
	Check(rack->ngs == &g_detached_system && voice.ports[0].dest == nullptr &&
	          Ngs2VoiceControl(voice_handle, &play.header) == OK &&
	          Ngs2VoiceGetState(voice_handle, &state.voice_state, sizeof(state)) == OK &&
	          (state.voice_state.state_flags & 1) != 0,
	      "a rack of a destroyed system was not detached, kept its patch or ignored a voice call");
	Check(Ngs2RackDestroy(kept, nullptr) == OK && Ngs2RackDestroy(sampler, nullptr) == OK &&
	          Ngs2RackDestroy(other_rack, nullptr) == OK &&
	          Ngs2SystemDestroy(other, nullptr) == OK && g_freed_buffers.size() == 6,
	      "a rack of a destroyed system was not destroyed with its buffer");
}

// Runs `call` on another thread and `meanwhile` on this one, then fails unless `call` returned
// within 5 s.
template <typename Call, typename Meanwhile>
void RunOnOtherThread(Call call, Meanwhile meanwhile, const char* message) {
	std::packaged_task<void()> task(call);
	auto                       done = task.get_future();
	std::thread                thread(std::move(task));
	meanwhile();
	Check(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready, message);
	thread.join();
}

void TestUserLocks() {
	const Ngs2BufferAllocator allocator {AllocContextBuffer, FreeContextBuffer, 0};
	auto                      option = SingleVoiceRackOption(0x1000);
	uintptr_t                 system = 0, rack = 0;
	Check(Ngs2SystemCreateWithAllocator(nullptr, &allocator, &system) == OK &&
	          Ngs2RackCreateWithAllocator(system, 0x1000, &option.common, &allocator, &rack) == OK,
	      "allocator system or rack creation failed");
	auto*                      internal    = reinterpret_cast<Ngs2RackInternal*>(rack);
	const auto&                system_lock = reinterpret_cast<Ngs2Internal*>(system)->user_lock;
	const auto&                rack_lock   = internal->user_lock;
	float                      pcm[512];
	const Ngs2RenderBufferInfo output {pcm, sizeof(pcm), 0x18, 2};

	// Held locks do not block rendering on other threads, which cannot release them.
	Check(Ngs2SystemLock(system) == OK &&
	          system_lock.owner == Common::Thread::GetThreadIdUnique() &&
	          Ngs2SystemLock(system) == OK && Ngs2RackLock(rack) == OK,
	      "user lock failed or did not record its owner");
	auto render = [&] {
		Check(Ngs2SystemRender(system, &output, 1) == OK && Ngs2SystemUnlock(system) == OK &&
		          Ngs2RackUnlock(rack) == OK,
		      "a render failed under a user lock");
	};
	RunOnOtherThread(render, [] {}, "a held user lock blocked rendering");
	Check(system_lock.depth == 2 && rack_lock.depth == 1, "another thread released a user lock");
	Check(Ngs2SystemUnlock(system) == OK && system_lock.depth == 1 &&
	          Ngs2SystemUnlock(system) == OK && system_lock.depth == 0 &&
	          Ngs2SystemUnlock(system) == OK && system_lock.depth == 0,
	      "user lock depth was not counted");

	// Another thread takes a held lock only once it is released.
	std::atomic_bool released = false;
	Check(Ngs2SystemLock(system) == OK, "user lock failed");
	auto lock = [&] {
		Check(Ngs2SystemLock(system) == OK && released, "a held user lock was taken");
		Ngs2SystemUnlock(system);
	};
	auto release = [&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		released = true;
		Ngs2SystemUnlock(system);
	};
	RunOnOtherThread(lock, release, "a released user lock was not taken");

	// Locks held across destroys are released through valid handles only.
	Check(Ngs2SystemLock(system) == OK && Ngs2SystemDestroy(system, nullptr) == OK,
	      "a locked system was not destroyed");
	Check(Ngs2RackUnlock(rack) == OK && rack_lock.depth == 0 && Ngs2RackLock(rack) == OK &&
	          Ngs2RackDestroy(rack, nullptr) == OK &&
	          Ngs2RackUnlock(rack) == NGS2_ERROR_INVALID_RACK_HANDLE &&
	          Ngs2RackLock(rack) == NGS2_ERROR_INVALID_RACK_HANDLE &&
	          Ngs2RackLock(0) == NGS2_ERROR_INVALID_RACK_HANDLE,
	      "a rack lock held across its system's destroy was not released, or a destroyed rack was "
	      "locked");
}

void TestMasteringVoiceState() {
	MasteringRender m;
	alignas(Ngs2MasteringVoiceState) std::array<uint8_t, 0x140> storage;
	storage.fill(0xa5);
	auto*      state  = reinterpret_cast<Ngs2VoiceState*>(storage.data());
	const auto handle = reinterpret_cast<uintptr_t>(&m.voice);
	Check(m.Setup(2) == OK &&
	          Ngs2VoiceGetState(handle, state, 0x137) == NGS2_ERROR_INVALID_OUT_SIZE &&
	          std::ranges::all_of(storage, [](uint8_t v) { return v == 0xa5; }),
	      "undersized mastering state buffer was accepted or modified");
	Check(Ngs2VoiceGetState(handle, state, storage.size()) == OK && state->state_flags == 1 &&
	          state->error_code == 0 &&
	          std::all_of(storage.begin() + 8, storage.begin() + 0x138,
	                      [](uint8_t v) { return v == 0; }) &&
	          std::all_of(storage.begin() + 0x138, storage.end(),
	                      [](uint8_t v) { return v == 0xa5; }),
	      "mastering state was not filled or was written past its size");
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
	f.Render(7);
	// The skip prefix plays on every pass.
	const float expected[] = {-30000 / 32768.0f, 0.25f, 0.5f, -30000 / 32768.0f, 0.25f, 0.5f, 0};
	for (size_t i = 0; i < 7; ++i) {
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
	          f.voice.WaveformData() == reinterpret_cast<const uint8_t*>(pcm),
	      "byte-unit stream was rejected or its read address passed the skip prefix");
	f.Render(8);
	Check(f.voice.samples == std::vector<float> {0, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0, 0} &&
	          f.voice.blocks.empty() && f.voice.decoded_samples == 5,
	      "byte-unit stream did not play exactly its duration across data-only blocks");
}

void TestPcmSkipAcrossBlocks() {
	// The skip prefix plays on into a block of data alone added without the data-only flag, and
	// does not count as decoded.
	Fixture                 f(48000);
	const int16_t           pcm[] = {1, 2, 3, 8192, 16384};
	const Ngs2WaveformBlock first {0, 4, 0, 3, 2, 0, 0}, rest {4, 6, 0, 0, 0, 0, 0};
	Check(f.QueueBlocks(pcm, &first, 1, 1) == OK && f.QueueBlocks(pcm, &rest, 1, 0x10) == OK &&
	          f.voice.blocks.size() == 2,
	      "a data-only block without the flag was rejected or joined across NOJOINT");
	f.Render(6);
	Check(f.voice.samples ==
	              std::vector<float> {1 / 32768.0f, 2 / 32768.0f, 3 / 32768.0f, 0.25f, 0.5f, 0} &&
	          f.voice.decoded_samples == 2 && f.voice.decoded_bytes == sizeof(pcm) &&
	          f.voice.blocks.empty(),
	      "skip spanning blocks did not play or miscounted decoded data");
}

void TestPcmSkipPlaysAndDataOnlyBlocks() {
	const int16_t pcm[] = {4096, 8192, 12288, 16384};
	// A block of skip samples alone plays them.
	Fixture                 skip(48000);
	const Ngs2WaveformBlock skip_only {0, 6, 0, 3, 0, 0, 0};
	Check(skip.QueueBlocks(pcm, &skip_only, 1, 0) == OK, "a skip-only block was rejected");
	skip.Render(4);
	Check(skip.voice.samples == std::vector<float> {0.125f, 0.25f, 0.375f, 0},
	      "a skip-only block did not play its skip samples");
	// Only a contiguous data-only block of the same user data that does not repeat joins.
	Fixture join(48000);
	join.SetCapacity(8);
	const Ngs2WaveformBlock pieces[] = {{0, 2, 0, 0, 4, 0, 7},
	                                    {2, 2, 0, 0, 0, 0, 7},
	                                    {4, 2, 0, 0, 1, 0, 7},
	                                    {6, 2, 0, 0, 0, 0, 8},
	                                    {8, 0, 1, 0, 0, 0, 8}};
	Check(join.QueueBlocks(pcm, pieces, 5, 1) == OK && join.voice.blocks.size() == 4 &&
	          join.voice.blocks.front().info.data_size == 4,
	      "data-only blocks did not join by samples, user data and repeats");
	// A data-only block after a finished extent is dropped.
	Fixture                 drop(48000);
	const Ngs2WaveformBlock ended[] = {
	    {0, 4, 0, 0, 1, 0, 0}, {6, 2, 0, 0, 0, 0, 0}, {4, 2, 0, 0, 1, 0, 0}};
	Check(drop.QueueBlocks(pcm, ended, 3, 1) == OK, "blocks after an extent were rejected");
	drop.Render(3);
	Check(drop.voice.samples == std::vector<float> {0.125f, 0.375f, 0} &&
	          drop.voice.decoded_samples == 2,
	      "a data-only block after its extent played");
	// A canceled block of skip samples behind the playing block plays nothing.
	const Ngs2WaveformBlock queued[] = {{0, 4, 0, 0, 2, 0, 0}, {0, 4, 0, 2, 0, 0, 0}};
	Check(drop.QueueBlocks(pcm, queued, 2, 1) == OK, "blocks before a cancel were rejected");
	drop.Render(1);
	Check(drop.QueueBlocks(pcm, ended + 2, 1, 8) == OK, "a canceling addition was rejected");
	drop.Render(3);
	Check(drop.voice.samples == std::vector<float> {0.25f, 0.375f, 0},
	      "a canceled block played its skip");
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

int SetFilter(Fixture& f, uint32_t index, uint32_t location, const Ngs2FilterParam& filter,
              uint16_t size = 56) {
	struct Param {
		Ngs2VoiceParamHeader   header;
		Ngs2SamplerFilterParam filter;
	} param {{size, 0, 0x1000000a}, {index, location, filter, 0}};
	f.rack.option.sampler.max_filters = 8;
	return Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &param.header);
}

Ngs2FilterParam Direct(float i0, float i1, float i2, float o1, float o2, uint64_t mask = 0) {
	return {mask, 11, {.direct = {i0, i1, i2, o1, o2}}};
}

void SetLowPass(Fixture& f, float frequency = 1000.0f, uint64_t mask = 0) {
	Check(SetFilter(f, 0, 1, {mask, 1, {.fcq = {frequency, 0.70710678f, 1.0f, 0, 0}}}) == OK,
	      "filter ABI command failed");
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

void TestFilterControlLayout() {
	// The filter is designed at the 48 kHz system rate, not the source rate.
	Fixture f(22050, 2);
	f.rack.option.sampler.max_filters = 8;
	// A 56-byte control command with nonzero reserved words and trailing padding.
	alignas(8) uint32_t words[] = {56,         0x1000000a, 0,          1, 0, 0, 1,
	                               0x45d58800, 0x3fd55555, 0x3f800000, 7, 7, 7, 7};
	Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice),
	                       reinterpret_cast<const Ngs2VoiceParamHeader*>(words)) == OK &&
	          f.voice.filters.size() == 1 && f.voice.filters[0].location == 1,
	      "FCQ filter command was not recognized");
	const double omega    = (2.0 * std::numbers::pi) * 6833.0 / 48000.0;
	const double q        = static_cast<double>(std::bit_cast<float>(0x3fd55555u));
	const double expected = (1 - std::cos(omega)) / (2 * (1 + std::sin(omega) / (2 * q)));
	Check(std::abs(f.voice.filters[0].i0 - expected) < 1e-6,
	      "FCQ fields were read at the wrong offsets");
	words[4] = 2; // Bypass channel 1 in the low half of the 64-bit mask.
	words[5] = 1; // Bypass channel 32 in the high half.
	Fixture wide(48000, 33);
	wide.rack.option.sampler.max_filters = 8;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&wide.voice),
	                 reinterpret_cast<const Ngs2VoiceParamHeader*>(words));
	const std::vector<int16_t> pcm(33, 16384);
	wide.Queue(pcm.data(), 1, 1);
	wide.Render(1);
	Check(wide.voice.samples[1] == 0.5f && wide.voice.samples[32] == 0.5f &&
	          std::abs(wide.voice.samples[0] - expected * 0.5) < 1e-6,
	      "channel mask was not decoded as a 64-bit bypass mask");
}

// The magnitude of the response of `filter` at `hz` of a 48 kHz rate.
double FilterResponse(const Ngs2SamplerFilter& filter, double hz) {
	const auto z = std::polar(1.0, -2 * std::numbers::pi * hz / 48000);
	return std::abs((double(filter.i0) + double(filter.i1) * z + double(filter.i2) * z * z) /
	                (1.0 - double(filter.o1) * z - double(filter.o2) * z * z));
}

void TestFilterDesigns() {
	Fixture     f(48000);
	const auto& filters = f.voice.filters;
	auto        design  = [&](uint32_t type, float fc, float q, float level) {
		Check(SetFilter(f, 0, 0, {0, type, {.fcq = {fc, q, level, 0, 0}}}) == OK,
		      "filter design control failed");
		return std::array {filters[0].i0, filters[0].i1, filters[0].i2, filters[0].o1,
		                   filters[0].o2};
	};
	struct Case {
		uint32_t type;
		double   hz, expected;
	};
	// At a 1 kHz cutoff with Q 2 and level 4.
	constexpr double half_power = std::numbers::sqrt2 / 2;
	const Case       cases[]    = {{1, 1000, 2},  {2, 1000, 2}, {3, 1000, 2},          {4, 1000, 1},
	                               {5, 1000, 0},  {6, 0, 4},    {6, 500, 5.826},       {7, 1000, 2},
	                               {7, 24000, 4}, {8, 1000, 4}, {10, 1000, half_power}};
	for (const auto& c: cases) {
		design(c.type, 1000, 2, 4);
		Check(std::abs(FilterResponse(filters[0], c.hz) - c.expected) <
		          1e-3 * std::max(1.0, c.expected),
		      "a filter type had the wrong response");
	}
	// The cutoff, Q and level are clamped.
	Check(design(1, 0, 2, 1) == design(1, 1, 2, 1) &&
	          design(1, 1000, 0, 1) == design(1, 1000, 1e-5f, 1) &&
	          design(1, 1000, 100, 1) == design(1, 1000, 32, 1) &&
	          design(6, 1000, 2, -1) == design(6, 1000, 2, 0),
	      "a filter design was not clamped");
	design(1, 30000, 2, 1);
	Check(std::abs(FilterResponse(filters[0], 0.499 * 48000) - 2) < 1e-3,
	      "a cutoff above half the rate was not clamped");
	// The one-pole low-pass sums its denominator terms in this order.
	const float w = 500.0f / 48000 * 2 * std::numbers::pi_v<float>;
	const float s = std::sin(w), c = std::cos(w), inverse = 1 / ((s + c) + 1);
	Check(design(9, 500, 2, 4) ==
	          std::array {s * inverse, s * inverse, 0.0f, -((s - c) - 1) * inverse, 0.0f},
	      "the one-pole low-pass rounded differently");
	// A rejected control changes no filter; a longer one is accepted.
	for (uint32_t index: {0u, 1u}) {
		Check(SetFilter(f, index, 0, {0, 1, {}}, 0x37) == NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE &&
		          SetFilter(f, index, 0, {0, 12, {}}) == NGS2_ERROR_INVALID_FILTER_TYPE &&
		          SetFilter(f, index, 2, {0, 1, {}}) == NGS2_ERROR_INVALID_MODULE_LOCATION &&
		          filters.size() == 1 && filters[0].type == 9 && filters[0].location == 0,
		      "an invalid filter size, type or location was applied");
	}
	Check(SetFilter(f, 8, 0, {0, 1, {}}) == NGS2_ERROR_INVALID_VOICE_PARAM_ID &&
	          SetFilter(f, 1, 1, {0, 1, {}}, 0x40) == OK && filters.size() == 2,
	      "a filter index past the maximum was applied or a longer control was rejected");
}

void TestFilterRecurrence() {
	Fixture       f(48000, 2);
	const int16_t pcm[] = {8192, 16384, 16384, -8192, 0, 0, 0, 0, 0, 0};
	f.Queue(pcm, 5, 0);
	// OFF passes both channels through, its history following them.
	Check(SetFilter(f, 0, 0, {0, 0, {}}) == OK, "filter OFF control failed");
	f.Render(2);
	Check(f.voice.samples == std::vector<float> {0.25f, 0.5f, 0.5f, -0.25f},
	      "filter OFF changed the audio");
	// y = x1 + 2 * x2 + 4 * y1 + 8 * y2 with channel 1 masked, then unmasked.
	Check(SetFilter(f, 0, 0, Direct(0, 1, 2, 4, 8, 2)) == OK, "masked DIRECT control failed");
	f.Render(1);
	Check(f.voice.samples == std::vector<float> {5.0f, 0.0f},
	      "a filter did not go on from the input history of OFF");
	Check(SetFilter(f, 0, 0, Direct(0, 1, 2, 4, 8)) == OK, "DIRECT control failed");
	f.Render(1);
	Check(f.voice.samples == std::vector<float> {25.0f, -2.5f},
	      "a filter did not run y = i0 * x + i1 * x1 + i2 * x2 + o1 * y1 + o2 * y2 or unmasked "
	      "a channel without its input history");
	f.voice.SetEvent(1);
	f.Render(1);
	Check(f.voice.samples == std::vector<float> {0.0f, 0.0f},
	      "PLAY did not zero the filter history");
	// The input history of OFF follows a starved grain to zero.
	Fixture       starved(48000);
	const int16_t steps[] = {16384, 16384, 8192};
	Check(SetFilter(starved, 0, 0, {0, 0, {}}) == OK, "filter OFF control failed");
	starved.Queue(steps, 2, 1);
	starved.Render(2);
	starved.Render(2);
	Check(SetFilter(starved, 0, 0, Direct(1, 1, 0, 0, 0)) == OK, "DIRECT control failed");
	starved.Queue(steps + 2, 1, 0);
	starved.Render(1);
	Check(starved.voice.samples[0] == 0.25f, "a starved grain left a stale input history");
}

void TestFilterLocations() {
	Fixture f(48000);
	// The front filter runs first even at a higher index.
	const int16_t pcm[] = {16384, 0};
	f.Queue(pcm, 2, 0);
	Check(SetFilter(f, 0, 1, {0, 0, {}}) == OK &&
	          SetFilter(f, 1, 0, Direct(0.5f, 0, 0, 0, 0)) == OK,
	      "front and rear filter controls failed");
	f.Render(1);
	Check(SetFilter(f, 0, 1, Direct(0, 1, 0, 0, 0)) == OK &&
	          SetFilter(f, 1, 0, Direct(1, 0, 0, 0, 0)) == OK,
	      "front and rear filter changes failed");
	f.Render(1);
	Check(f.voice.samples[0] == 0.25f, "the rear filter did not run after the front one");
	// Location 2 + N filters the send of port N alone.
	Fixture          source(48000);
	Ngs2RackInternal rack {};
	rack.ngs  = &source.system;
	rack.type = Ngs2RackType::Mastering;
	Ngs2VoiceInternal sends[2];
	for (auto& send: sends) {
		send.rack     = &rack;
		send.channels = 1;
		send.state    = Ngs2VoicePlayState::Playing;
	}
	source.voice.ports = {{&sends[0], 0, 1.0f, -1}, {&sends[1], 0, 1.0f, -1}};
	Check(SetFilter(source, 0, 3, Direct(0.5f, 0, 0, 0, 0)) == OK &&
	          SetFilter(source, 1, 4, {0, 0, {}}) == NGS2_ERROR_INVALID_MODULE_LOCATION,
	      "the pre-send locations were not those of the ports");
	source.Queue(pcm, 1, 0);
	const std::vector<Ngs2VoiceInternal*> voices {&source.voice, &sends[0], &sends[1]};
	Ngs2RenderVoice(sends[0], voices, 1);
	Ngs2RenderVoice(sends[1], voices, 1);
	Check(source.voice.samples[0] == 0.5f && sends[0].samples[0] == 0.5f &&
	          sends[1].samples[0] == 0.25f,
	      "a pre-send filter did not filter only its port's send");
	// y = x + 0.5 * y1 before a send: it runs on while the send is muted, then rings on into the
	// send once the source starves.
	Fixture pre(48000);
	pre.voice.ports = {{&sends[0], 0, 1.0f, -1}};
	Check(SetFilter(pre, 0, 2, Direct(1, 0, 0, 0.5f, 0)) == OK, "pre-send filter control failed");
	const std::vector<int16_t> steps(8, 16384);
	pre.Queue(steps.data(), 8, 1);
	for (float volume: {1.0f, 0.0f, 1.0f}) {
		pre.voice.ports[0].volume = volume;
		pre.voice.rendered = sends[0].rendered = false;
		Ngs2RenderVoice(sends[0], {&pre.voice, &sends[0]}, 4);
	}
	Check(sends[0].samples ==
	          std::vector<float> {0.498046875f, 0.2490234375f, 0.12451171875f, 0.062255859375f},
	      "a muted send froze its pre-send filter or a starved source froze its tail");
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

// A block from CalcWaveformBlock plays the decoded samples from its position on: the decoder drops
// no samples itself, so the skip holds the decoder delay.
void TestAtrac9CalculatedBlockPlayback() {
	// Two superframes of four encoded mono frames, each with one nonzero spectral coefficient after
	// its first frame, then superframe padding.
	std::array<uint8_t, 384> encoded {};
	for (size_t i = 0; i < 8; ++i) {
		const auto frame = encoded.begin() + i / 4 * 192 + i % 4 * ATRAC9_MONO_BLOCK.size();
		std::copy(ATRAC9_MONO_BLOCK.begin(), ATRAC9_MONO_BLOCK.end(), frame);
		if (i % 4 != 0) *frame |= 0x80;
	}
	const Ngs2WaveformFormat format {NGS2_WAVEFORM_TYPE_ATRAC9, 1, 24000, 0xfe4005f0};

	auto play = [&](const Ngs2WaveformBlock& block, uint32_t frames) {
		auto f = std::make_unique<Fixture>(24000);
		f->voice.SetupSampler(format);
		f->voice.state = Ngs2VoicePlayState::Playing;
		Check(f->QueueBlocks(encoded.data(), &block, 1, 0) == OK, "ATRAC9 block queue failed");
		f->Render(frames);
		return f->voice.samples;
	};
	// The whole stream, at two output samples per decoded sample. Block sample 400 is decoded
	// sample 528, past the one-frame delay, and its 100 samples end in the second superframe.
	const auto        whole = play({0, encoded.size(), 0, 0, 1024}, 2048);
	Ngs2WaveformBlock block {};
	Check(Ngs2CalcWaveformBlock(&format, 400, 100, &block) == OK, "ATRAC9 block failed");
	const auto part = play(block, 200);
	const auto from = whole.begin() + 2 * 528;
	Check(std::any_of(from, from + 200, [](float sample) { return sample != 0.0f; }) &&
	          std::equal(part.begin(), part.end() - 1, from),
	      "a calculated ATRAC9 block did not play from its sample position");
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
	Check(next_data() == pcm, "read address passed the skip prefix");
	f.Render(1);
	Check(next_data() == pcm + 2, "read address did not advance by a stereo frame");
	f.Render(2);
	Check(next_data() == pcm, "read address did not wrap at a loop boundary");
	f.Render(3);
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
	Check(!stream.voice.has_samples && !stream.voice.filters[0].HasTail(),
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
	// A voice ends in the grain its data runs out, filtered, and its filter tail ends with it.
	Fixture f(48000);
	SetLowPass(f);
	const int16_t impulse = 16384;
	f.Queue(&impulse, 1, 0);
	f.Render(4);
	Check(f.voice.state == Ngs2VoicePlayState::Empty && f.voice.samples[3] != 0,
	      "a finite voice did not end with its last grain filtered");
	f.Render(4);
	Check(!f.voice.has_samples, "a finished voice kept its filter tail");
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
	TestInt16Conversion();
	TestMasteringVoicesShareOutput();
	TestPatchToRackHandle();
	TestMasteringGainRamp();
	TestMasteringLfeDesignRate();
	TestMasteringLfeFilter();
	TestMasteringLimiter();
	TestMasteringPeakMeters();
	TestMasteringDownmix();
	TestMasteringMonoStereoConversion();
	TestMasteringUntypedConversions();
	TestMasteringMatrixSpill();
	TestMasteringOutputRange();
	TestMasteringControlErrors();
	TestMasteringRackOptionRanges();
	TestMasteringVoiceState();
	TestRackGetVoiceHandleErrors();
	TestSystemDestroy();
	TestUserLocks();
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
	TestAtrac9CalculatedBlockPlayback();
	TestWaveformReadAddress();
	TestFilterTailDuringStarvation();
	TestFiniteFilterTail();
	TestFilterHistoryAndChannels();
	TestFilterControlLayout();
	TestFilterDesigns();
	TestFilterRecurrence();
	TestFilterLocations();
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
	TestPcmSkipPlaysAndDataOnlyBlocks();
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
	TestStopThenPlayBeforeRender();
	std::puts("Ngs2SamplerTests: all cases passed");
}
