#include "libs/audio.h"

#include "SDL.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "kernel/semaphore.h"
#include "libatrac9.h"
#include "libs/ajm/hevag_core.h"
#include "libs/audio_internal.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Audio {

namespace {

constexpr int AUDIO_OUT_PORT_TYPE_MAIN      = 0;
constexpr int AUDIO_OUT_PORT_TYPE_BGM       = 1;
constexpr int AUDIO_OUT_PORT_TYPE_VOICE     = 2;
constexpr int AUDIO_OUT_PORT_TYPE_PERSONAL  = 3;
constexpr int AUDIO_OUT_PORT_TYPE_PADSPK    = 4;
constexpr int AUDIO_OUT_PORT_TYPE_VIBRATION = 10;
constexpr int AUDIO_OUT_PORT_TYPE_AUDIO3D   = 126;
constexpr int AUDIO_OUT_PORT_TYPE_AUX       = 127;

constexpr uint32_t AUDIO_OUT_PARAM_FORMAT_MASK = 0x000000ffu;

static bool audio_out_port_type_is_valid(int type) {
	return (type >= AUDIO_OUT_PORT_TYPE_MAIN && type <= AUDIO_OUT_PORT_TYPE_PADSPK) ||
	       type == AUDIO_OUT_PORT_TYPE_VIBRATION || type == AUDIO_OUT_PORT_TYPE_AUDIO3D ||
	       type == AUDIO_OUT_PORT_TYPE_AUX;
}

} // namespace

class Audio {
public:
	using Format = AudioInternal::Format;

	class Id {
	public:
		explicit Id(int id): m_id(id - 1) {}
		[[nodiscard]] int  ToInt() const { return m_id + 1; }
		[[nodiscard]] bool IsValid() const { return m_id >= 0; }

		friend class Audio;

	private:
		Id() = default;
		static Id Invalid() { return {}; }
		static Id Create(int audio_id) {
			Id r;
			r.m_id = audio_id;
			return r;
		}
		[[nodiscard]] int GetId() const { return m_id; }

		int m_id = -1;
	};

	struct OutputParam {
		Id          handle;
		const void* data = nullptr;
	};

	Audio() = default;
	virtual ~Audio();

	KYTY_CLASS_NO_COPY(Audio);

	Id       AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format);
	bool     AudioOutClose(Id handle);
	bool     AudioOutValid(Id handle);
	bool     AudioOutHasDevice(Id handle);
	bool     AudioOutSetVolume(Id handle, uint32_t bitflag, const int* volume);
	uint32_t AudioOutOutputs(OutputParam* params, uint32_t num, bool blocking = true);
	bool     AudioOutGetStatus(Id handle, int* type, int* channels_num);

	Id       AudioInOpen(uint32_t type, uint32_t samples_num, uint32_t freq, Format format);
	bool     AudioInValid(Id handle);
	bool     AudioInHasDevice(Id handle);
	uint32_t AudioInInput(Id handle, void* dest);

	static constexpr int OUT_PORTS_MAX = 32;
	static constexpr int IN_PORTS_MAX  = 8;

private:
	struct PortOut {
		bool     used             = false;
		int      type             = 0;
		uint32_t samples_num      = 0;
		uint32_t freq             = 0;
		Format   format           = Format::Unknown;
		uint64_t last_output_time = 0;
		int      channels_num     = 0;
		int      volume[8]        = {};

		SDL_AudioDeviceID audio_device = 0;
		SDL_AudioSpec     audio_spec   = {};
	};

	struct PortIn {
		bool     used            = false;
		uint32_t type            = 0;
		uint32_t samples_num     = 0;
		uint32_t freq            = 0;
		Format   format          = Format::Unknown;
		uint64_t last_input_time = 0;
		int      channels_num    = 0;

		SDL_AudioDeviceID audio_device = 0;
		SDL_AudioSpec     audio_spec   = {};
	};

	Common::Mutex m_mutex;
	PortOut       m_out_ports[OUT_PORTS_MAX];
	PortIn        m_in_ports[IN_PORTS_MAX];

	static bool            FormatIsFloat(Format format);
	static bool            FormatIsStd(Format format);
	static uint32_t        BytesPerSample(Format format);
	static uint32_t        FrameSize(const PortOut& port);
	static SDL_AudioFormat SdlFormat(Format format);
	static bool            OpenSdlDevice(PortOut* port);
	static void            CloseSdlDevice(PortOut* port);
	static bool            OpenSdlCaptureDevice(PortIn* port);
	static void            CloseSdlCaptureDevice(PortIn* port);
	static const void*     PrepareOutputBuffer(const PortOut& port, const void* data,
	                                           std::vector<uint8_t>* buffer);
	static bool            QueueSdlAudio(PortOut* port, const void* data, bool blocking);
};

static Audio* g_audio = nullptr;

namespace AudioInternal {

int AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format) {
	if (g_audio == nullptr) {
		return 0;
	}

	auto id = g_audio->AudioOutOpen(type, samples_num, freq, format);
	return id.IsValid() ? id.ToInt() : 0;
}

void AudioOutClose(int handle) {
	if (g_audio != nullptr && handle > 0) {
		(void)g_audio->AudioOutClose(Audio::Id(handle));
	}
}

bool AudioOutHasDevice(int handle) {
	return g_audio != nullptr && handle > 0 && g_audio->AudioOutHasDevice(Audio::Id(handle));
}

uint32_t AudioOutOutputs(const OutputParam* params, uint32_t num, bool blocking) {
	if (g_audio == nullptr || params == nullptr || num == 0) {
		return 0;
	}

	std::vector<Audio::OutputParam> output_params;
	output_params.reserve(num);
	for (uint32_t i = 0; i < num; i++) {
		if (params[i].handle > 0 && params[i].data != nullptr) {
			output_params.push_back(
			    Audio::OutputParam {Audio::Id(params[i].handle), params[i].data});
		}
	}

	if (output_params.empty()) {
		return 0;
	}

	return g_audio->AudioOutOutputs(output_params.data(),
	                                static_cast<uint32_t>(output_params.size()), blocking);
}

} // namespace AudioInternal

void Initialize() {
	EXIT_IF(g_audio != nullptr);

	g_audio = new Audio;
}

void Shutdown() {
	Audio3d::Shutdown();
	delete g_audio;
	g_audio = nullptr;
}

Audio::~Audio() {
	for (auto& port: m_out_ports) {
		CloseSdlDevice(&port);
	}
	for (auto& port: m_in_ports) {
		CloseSdlCaptureDevice(&port);
	}
}

bool Audio::FormatIsFloat(Format format) {
	return (format == Format::FloatMono || format == Format::FloatStereo ||
	        format == Format::Float8Ch || format == Format::Float8ChStd);
}

bool Audio::FormatIsStd(Format format) {
	return (format == Format::Signed16bit8ChStd || format == Format::Float8ChStd);
}

uint32_t Audio::BytesPerSample(Format format) {
	return FormatIsFloat(format) ? sizeof(float) : sizeof(int16_t);
}

uint32_t Audio::FrameSize(const PortOut& port) {
	return BytesPerSample(port.format) * port.channels_num;
}

SDL_AudioFormat Audio::SdlFormat(Format format) {
	return FormatIsFloat(format) ? AUDIO_F32SYS : AUDIO_S16SYS;
}

bool Audio::OpenSdlDevice(PortOut* port) {
	EXIT_IF(port == nullptr);

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		LOGF("AudioOut: SDL audio init failed: %s\n", SDL_GetError());
		return false;
	}

	SDL_AudioSpec desired {};
	desired.freq     = static_cast<int>(port->freq);
	desired.format   = SdlFormat(port->format);
	desired.channels = static_cast<Uint8>(port->channels_num);
	desired.samples  = static_cast<Uint16>(port->samples_num);
	desired.callback = nullptr;

	SDL_AudioSpec obtained {};

	port->audio_device =
	    SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
	if (port->audio_device == 0) {
		LOGF("AudioOut: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
		return false;
	}

	port->audio_spec = obtained;
	SDL_PauseAudioDevice(port->audio_device, 0);

	LOGF("AudioOut: opened SDL device (%d Hz, %u ch, format 0x%04x)\n", obtained.freq,
	     obtained.channels, obtained.format);
	return true;
}

void Audio::CloseSdlDevice(PortOut* port) {
	EXIT_IF(port == nullptr);

	if (port->audio_device != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
		SDL_ClearQueuedAudio(port->audio_device);
		SDL_CloseAudioDevice(port->audio_device);
	}

	port->audio_device = 0;
	port->audio_spec   = {};
}

bool Audio::OpenSdlCaptureDevice(PortIn* port) {
	EXIT_IF(port == nullptr);

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		LOGF("AudioIn: SDL audio init failed: %s\n", SDL_GetError());
		return false;
	}

	SDL_AudioSpec desired {};
	desired.freq     = static_cast<int>(port->freq);
	desired.format   = SdlFormat(port->format);
	desired.channels = static_cast<Uint8>(port->channels_num);
	desired.samples  = static_cast<Uint16>(port->samples_num);
	desired.callback = nullptr;

	SDL_AudioSpec obtained {};
	port->audio_device = SDL_OpenAudioDevice(nullptr, SDL_TRUE, &desired, &obtained, 0);
	if (port->audio_device == 0) {
		LOGF("AudioIn: SDL_OpenAudioDevice failed: %s; using silence fallback\n", SDL_GetError());
		return false;
	}

	port->audio_spec = obtained;
	SDL_PauseAudioDevice(port->audio_device, 0);
	LOGF("AudioIn: opened SDL capture device (%d Hz, %u ch, format 0x%04x)\n", obtained.freq,
	     obtained.channels, obtained.format);
	return true;
}

void Audio::CloseSdlCaptureDevice(PortIn* port) {
	EXIT_IF(port == nullptr);

	if (port->audio_device != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
		SDL_ClearQueuedAudio(port->audio_device);
		SDL_CloseAudioDevice(port->audio_device);
	}

	port->audio_device = 0;
	port->audio_spec   = {};
}

const void* Audio::PrepareOutputBuffer(const PortOut& port, const void* data,
                                       std::vector<uint8_t>* buffer) {
	EXIT_IF(data == nullptr);
	EXIT_IF(buffer == nullptr);

	const auto frames           = port.samples_num;
	const auto channels         = static_cast<uint32_t>(port.channels_num);
	const auto bytes_per_sample = BytesPerSample(port.format);
	const auto src_size         = frames * channels * bytes_per_sample;

	bool volume_changed = false;
	for (uint32_t ch = 0; ch < channels; ch++) {
		if (port.volume[ch] != 32768) {
			volume_changed = true;
			break;
		}
	}

	if (!volume_changed && !FormatIsStd(port.format)) {
		return data;
	}

	buffer->resize(src_size);

	static constexpr uint32_t STD_8CH_MAP[8] = {0, 1, 2, 3, 6, 7, 4, 5};

	if (FormatIsFloat(port.format)) {
		auto*       dst = reinterpret_cast<float*>(buffer->data());
		const auto* src = static_cast<const float*>(data);

		for (uint32_t frame = 0; frame < frames; frame++) {
			for (uint32_t ch = 0; ch < channels; ch++) {
				const auto src_ch =
				    (FormatIsStd(port.format) && channels == 8 ? STD_8CH_MAP[ch] : ch);
				dst[frame * channels + ch] = src[frame * channels + src_ch] *
				                             (static_cast<float>(port.volume[ch]) / 32768.0f);
			}
		}
	} else {
		auto*       dst = reinterpret_cast<int16_t*>(buffer->data());
		const auto* src = static_cast<const int16_t*>(data);

		for (uint32_t frame = 0; frame < frames; frame++) {
			for (uint32_t ch = 0; ch < channels; ch++) {
				const auto src_ch =
				    (FormatIsStd(port.format) && channels == 8 ? STD_8CH_MAP[ch] : ch);
				int64_t sample =
				    static_cast<int64_t>(src[frame * channels + src_ch]) * port.volume[ch] / 32768;
				if (sample > std::numeric_limits<int16_t>::max()) {
					sample = std::numeric_limits<int16_t>::max();
				} else if (sample < std::numeric_limits<int16_t>::min()) {
					sample = std::numeric_limits<int16_t>::min();
				}
				dst[frame * channels + ch] = static_cast<int16_t>(sample);
			}
		}
	}

	return buffer->data();
}

bool Audio::QueueSdlAudio(PortOut* port, const void* data, bool blocking) {
	EXIT_IF(port == nullptr);

	if (port->audio_device == 0 || data == nullptr) {
		return false;
	}

	std::vector<uint8_t> prepared_buffer;
	const void*          prepared_data = PrepareOutputBuffer(*port, data, &prepared_buffer);
	const auto           prepared_size = FrameSize(*port) * port->samples_num;

	std::vector<uint8_t> convert_buffer;
	const void*          queue_data = prepared_data;
	uint32_t             queue_size = prepared_size;

	SDL_AudioCVT cvt {};
	const int    cvt_result =
	    SDL_BuildAudioCVT(&cvt, SdlFormat(port->format), static_cast<Uint8>(port->channels_num),
	                      static_cast<int>(port->freq), port->audio_spec.format,
	                      port->audio_spec.channels, port->audio_spec.freq);

	if (cvt_result < 0) {
		LOGF("AudioOut: SDL_BuildAudioCVT failed: %s\n", SDL_GetError());
		return false;
	}

	if (cvt_result > 0) {
		convert_buffer.resize(prepared_size * cvt.len_mult);
		std::memcpy(convert_buffer.data(), prepared_data, prepared_size);

		cvt.buf = convert_buffer.data();
		cvt.len = static_cast<int>(prepared_size);

		if (SDL_ConvertAudio(&cvt) < 0) {
			LOGF("AudioOut: SDL_ConvertAudio failed: %s\n", SDL_GetError());
			return false;
		}

		queue_data = cvt.buf;
		queue_size = static_cast<uint32_t>(cvt.len_cvt);
	}

	if (blocking) {
		constexpr uint64_t target_latency_us = 40000;
		const auto buffer_us = port->freq != 0 ? (1000000ULL * port->samples_num) / port->freq : 0;
		const auto buffers =
		    buffer_us != 0 ? static_cast<uint32_t>((target_latency_us + buffer_us - 1) / buffer_us)
		                   : 2u;
		const auto min_queued_size = queue_size * std::clamp(buffers, 2u, 16u);
		const auto wait_start      = LibKernel::KernelGetProcessTime();
		while (SDL_GetQueuedAudioSize(port->audio_device) > min_queued_size) {
			if (LibKernel::KernelGetProcessTime() - wait_start > 200000) {
				SDL_ClearQueuedAudio(port->audio_device);
				break;
			}
			Common::Thread::SleepMicro(1000);
		}
	}

	if (SDL_QueueAudio(port->audio_device, queue_data, queue_size) < 0) {
		LOGF("AudioOut: SDL_QueueAudio failed: %s\n", SDL_GetError());
		return false;
	}

	return true;
}

Audio::Id Audio::AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format) {
	Common::LockGuard lock(m_mutex);

	for (int id = 0; id < OUT_PORTS_MAX; id++) {
		if (!m_out_ports[id].used) {
			auto& port = m_out_ports[id];

			port.used             = true;
			port.type             = type;
			port.samples_num      = samples_num;
			port.freq             = freq;
			port.format           = format;
			port.last_output_time = 0;

			switch (format) {
				case Format::Signed16bitMono:
				case Format::FloatMono: port.channels_num = 1; break;
				case Format::Signed16bitStereo:
				case Format::FloatStereo: port.channels_num = 2; break;
				case Format::Signed16bit8Ch:
				case Format::Float8Ch:
				case Format::Signed16bit8ChStd:
				case Format::Float8ChStd: port.channels_num = 8; break;
				default: EXIT("unknown format");
			}

			for (int i = 0; i < port.channels_num; i++) {
				port.volume[i] = 32768;
			}

			if (type != AUDIO_OUT_PORT_TYPE_VIBRATION) {
				OpenSdlDevice(&port);
			}

			return Id::Create(id);
		}
	}

	return Id::Invalid();
}

bool Audio::AudioOutClose(Id handle) {
	Common::LockGuard lock(m_mutex);

	if (AudioOutValid(handle)) {
		auto& port = m_out_ports[handle.GetId()];

		CloseSdlDevice(&port);
		port = {};

		return true;
	}

	return false;
}

bool Audio::AudioOutValid(Id handle) {
	Common::LockGuard lock(m_mutex);

	return (handle.GetId() >= 0 && handle.GetId() < OUT_PORTS_MAX &&
	        m_out_ports[handle.GetId()].used);
}

bool Audio::AudioOutHasDevice(Id handle) {
	Common::LockGuard lock(m_mutex);

	return (handle.GetId() >= 0 && handle.GetId() < OUT_PORTS_MAX &&
	        m_out_ports[handle.GetId()].used && m_out_ports[handle.GetId()].audio_device != 0);
}

bool Audio::AudioOutGetStatus(Id handle, int* type, int* channels_num) {
	Common::LockGuard lock(m_mutex);

	if (AudioOutValid(handle)) {
		auto& port = m_out_ports[handle.GetId()];

		*type         = port.type;
		*channels_num = port.channels_num;

		return true;
	}

	return false;
}

bool Audio::AudioOutSetVolume(Id handle, uint32_t bitflag, const int* volume) {
	Common::LockGuard lock(m_mutex);

	if (AudioOutValid(handle)) {
		auto& port = m_out_ports[handle.GetId()];

		for (int i = 0; i < port.channels_num; i++, bitflag >>= 1u) {
			auto bit = bitflag & 0x1u;

			if (bit == 1) {
				int src_index = i;
				if (port.format == Format::Float8ChStd ||
				    port.format == Format::Signed16bit8ChStd) {
					switch (i) {
						case 4: src_index = 6; break;
						case 5: src_index = 7; break;
						case 6: src_index = 4; break;
						case 7: src_index = 5; break;
						default:;
					}
				}
				port.volume[i] = volume[src_index];

				LOGF("\t port.volume[%d] = volume[%d] (%d)\n", i, src_index, volume[src_index]);
			}
		}

		return true;
	}

	return false;
}

uint32_t Audio::AudioOutOutputs(OutputParam* params, uint32_t num, bool blocking) {
	EXIT_NOT_IMPLEMENTED(num == 0);
	EXIT_NOT_IMPLEMENTED(!AudioOutValid(params[0].handle));

	const auto& first_port = m_out_ports[params[0].handle.GetId()];

	uint64_t block_time   = (1000000 * first_port.samples_num) / first_port.freq;
	uint64_t current_time = LibKernel::KernelGetProcessTime();

	uint64_t max_wait_time = 0;

	for (uint32_t i = 0; i < num; i++) {
		uint64_t next_time = m_out_ports[params[i].handle.GetId()].last_output_time + block_time;
		uint64_t wait_time = (next_time > current_time ? next_time - current_time : 0);
		max_wait_time      = (wait_time > max_wait_time ? wait_time : max_wait_time);
	}

	bool any_port_has_device = false;
	for (uint32_t i = 0; i < num; i++) {
		if (m_out_ports[params[i].handle.GetId()].audio_device != 0) {
			any_port_has_device = true;
			break;
		}
	}

	// One real output device is enough to pace the whole synchronized batch. Applying the fallback
	// when a vibration port is present would rate-limit the device-backed ports to exactly 1x and
	// prevent their SDL queues from building an underrun cushion.
	if (blocking && max_wait_time != 0 && !any_port_has_device) {
		Common::Thread::SleepMicro(max_wait_time);
	}

	for (uint32_t i = 0; i < num; i++) {
		auto& port = m_out_ports[params[i].handle.GetId()];

		QueueSdlAudio(&port, params[i].data, blocking);
	}

	for (uint32_t i = 0; i < num; i++) {
		m_out_ports[params[i].handle.GetId()].last_output_time = LibKernel::KernelGetProcessTime();
	}

	return first_port.samples_num;
}

Audio::Id Audio::AudioInOpen(uint32_t type, uint32_t samples_num, uint32_t freq, Format format) {
	Common::LockGuard lock(m_mutex);

	for (int id = 0; id < IN_PORTS_MAX; id++) {
		if (!m_in_ports[id].used) {
			auto& port = m_in_ports[id];

			port.used            = true;
			port.type            = type;
			port.samples_num     = samples_num;
			port.freq            = freq;
			port.format          = format;
			port.last_input_time = 0;

			switch (format) {
				case Format::Signed16bitMono: port.channels_num = 1; break;
				case Format::Signed16bitStereo: port.channels_num = 2; break;
				default: EXIT("unknown format");
			}

			OpenSdlCaptureDevice(&port);

			return Id::Create(id);
		}
	}

	return Id::Invalid();
}

bool Audio::AudioInValid(Id handle) {
	Common::LockGuard lock(m_mutex);

	return (handle.GetId() >= 0 && handle.GetId() < IN_PORTS_MAX &&
	        m_in_ports[handle.GetId()].used);
}

bool Audio::AudioInHasDevice(Id handle) {
	Common::LockGuard lock(m_mutex);

	return handle.GetId() >= 0 && handle.GetId() < IN_PORTS_MAX &&
	       m_in_ports[handle.GetId()].used && m_in_ports[handle.GetId()].audio_device != 0;
}

uint32_t Audio::AudioInInput(Id handle, void* dest) {
	EXIT_NOT_IMPLEMENTED(!AudioInValid(handle));
	EXIT_NOT_IMPLEMENTED(dest == nullptr);

	auto&      port       = m_in_ports[handle.GetId()];
	const auto frame_size = BytesPerSample(port.format) * static_cast<uint32_t>(port.channels_num);
	const auto block_size = frame_size * port.samples_num;

	uint64_t block_time   = (1000000 * port.samples_num) / port.freq;
	uint64_t current_time = LibKernel::KernelGetProcessTime();

	uint64_t next_time = m_in_ports[handle.GetId()].last_input_time + block_time;
	uint64_t wait_time = (next_time > current_time ? next_time - current_time : 0);

	if (port.audio_device != 0) {
		const uint64_t deadline = current_time + block_time * 2u + 10000u;
		while (SDL_GetQueuedAudioSize(port.audio_device) < block_size &&
		       LibKernel::KernelGetProcessTime() < deadline) {
			Common::Thread::SleepMicro(1000);
		}

		const auto captured = SDL_DequeueAudio(port.audio_device, dest, block_size);
		if (captured < block_size) {
			std::memset(static_cast<uint8_t*>(dest) + captured, 0, block_size - captured);
		}
	} else {
		Common::Thread::SleepMicro(wait_time);
		std::memset(dest, 0, block_size);
	}

	port.last_input_time = LibKernel::KernelGetProcessTime();

	return port.samples_num;
}

namespace AudioOut {

LIB_NAME("AudioOut", "AudioOut");

struct AudioOutOutputParam {
	int         handle;
	const void* ptr;
};

struct AudioOutPortState {
	uint16_t output;
	uint8_t  channel;
	uint8_t  reserved1[1];
	int16_t  volume;
	uint16_t reroute_counter;
	uint64_t flag;
	uint64_t reserved2[2];
};

int KYTY_SYSV_ABI AudioOutInit() {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI AudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq,
                               uint32_t param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %d\n"
	     "\t index   = %d\n"
	     "\t len     = %u\n"
	     "\t freq    = %u\n",
	     user_id, type, index, len, freq);

	if (!audio_out_port_type_is_valid(type)) {
		return AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
	}
	EXIT_NOT_IMPLEMENTED(index != 0);

	Audio::Format format       = Audio::Format::Unknown;
	const auto    format_param = param & AUDIO_OUT_PARAM_FORMAT_MASK;

	switch (format_param) {
		case 0: format = Audio::Format::Signed16bitMono; break;
		case 1: format = Audio::Format::Signed16bitStereo; break;
		case 2: format = Audio::Format::Signed16bit8Ch; break;
		case 3: format = Audio::Format::FloatMono; break;
		case 4: format = Audio::Format::FloatStereo; break;
		case 5: format = Audio::Format::Float8Ch; break;
		case 6: format = Audio::Format::Signed16bit8ChStd; break;
		case 7: format = Audio::Format::Float8ChStd; break;
		default:;
	}

	LOGF("\t param   = %u (format=%u, %s)\n", param, format_param,
	     Common::EnumName(format).c_str());

	EXIT_NOT_IMPLEMENTED(format == Audio::Format::Unknown);

	EXIT_IF(g_audio == nullptr);

	auto id = g_audio->AudioOutOpen(type, len, freq, format);

	if (!id.IsValid()) {
		return AUDIO_OUT_ERROR_PORT_FULL;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI AudioOutClose(int handle) {
	PRINT_NAME();

	if (!g_audio->AudioOutClose(Audio::Id(handle))) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOutGetPortState(int handle, AudioOutPortState* state) {
	PRINT_NAME();

	int type         = 0;
	int channels_num = 0;

	if (!g_audio->AudioOutGetStatus(Audio::Id(handle), &type, &channels_num)) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	state->reroute_counter = 0;
	state->volume          = 127;

	switch (type) {
		case AUDIO_OUT_PORT_TYPE_MAIN:
		case AUDIO_OUT_PORT_TYPE_BGM:
		case AUDIO_OUT_PORT_TYPE_AUDIO3D:
			state->output  = 1;
			state->channel = (channels_num > 2 ? 2 : channels_num);
			break;
		case AUDIO_OUT_PORT_TYPE_VOICE:
		case AUDIO_OUT_PORT_TYPE_PERSONAL:
			state->output  = 0x40;
			state->channel = 1;
			break;
		case AUDIO_OUT_PORT_TYPE_PADSPK:
		case AUDIO_OUT_PORT_TYPE_VIBRATION:
			state->output  = 4;
			state->channel = 1;
			break;
		case AUDIO_OUT_PORT_TYPE_AUX:
			state->output  = 0x80;
			state->channel = 0;
			break;
		default: EXIT("unknown port type: %d\n", type);
	}

	LOGF("\t output  = %" PRIu16 "\n"
	     "\t channel = %" PRIu8 "\n",
	     state->output, state->channel);

	return OK;
}

int KYTY_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol) {
	PRINT_NAME();

	LOGF("\t handle = %d\n"
	     "\t flag   = %u\n",
	     handle, flag);

	EXIT_IF(g_audio == nullptr);
	EXIT_NOT_IMPLEMENTED(vol == nullptr);

	if (!g_audio->AudioOutSetVolume(Audio::Id(handle), flag, vol)) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return OK;
}

int KYTY_SYSV_ABI AudioOutOutputs(AudioOutOutputParam* param, uint32_t num) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param == nullptr);

	Audio::OutputParam params[Audio::OUT_PORTS_MAX];

	EXIT_IF(g_audio == nullptr);

	for (uint32_t i = 0; i < num; i++) {
		params[i].handle = Audio::Id(param[i].handle);
		params[i].data   = param[i].ptr;

		if (!g_audio->AudioOutValid(params[i].handle)) {
			return AUDIO_OUT_ERROR_INVALID_PORT;
		}
	}

	return static_cast<int>(g_audio->AudioOutOutputs(params, num));
}

int KYTY_SYSV_ABI AudioOutOutput(int handle, const void* ptr) {
	// EXIT_NOT_IMPLEMENTED(ptr == nullptr);

	Audio::OutputParam params[1];

	EXIT_IF(g_audio == nullptr);

	params[0].handle = Audio::Id(handle);
	params[0].data   = ptr;

	if (!g_audio->AudioOutValid(params[0].handle)) {
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return static_cast<int>(g_audio->AudioOutOutputs(params, 1));
}

} // namespace AudioOut

namespace AudioIn {

LIB_NAME("AudioIn", "AudioIn");

constexpr int AUDIO_IN_SILENT_STATE_DEVICE_NONE = 0x1;

int KYTY_SYSV_ABI AudioInOpen(int user_id, uint32_t type, uint32_t index, uint32_t len,
                              uint32_t freq, uint32_t param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t type    = %u\n"
	     "\t index   = %d\n"
	     "\t len     = %u\n"
	     "\t freq    = %u\n",
	     user_id, type, index, len, freq);

	if (user_id != 255 && user_id != 1) {
		LOGF("\t temporary: accepting unsupported audio input user_id %d\n", user_id);
	}
	EXIT_NOT_IMPLEMENTED(type != 1);
	EXIT_NOT_IMPLEMENTED(index != 0);

	Audio::Format format = Audio::Format::Unknown;

	switch (param) {
		case 0: format = Audio::Format::Signed16bitMono; break;
		case 2: format = Audio::Format::Signed16bitStereo; break;
		default:
			LOGF("\t temporary: using signed 16-bit stereo for unsupported audio input param %u\n",
			     param);
			format = Audio::Format::Signed16bitStereo;
			break;
	}

	LOGF("\t param   = %u (%s)\n", param, Common::EnumName(format).c_str());

	EXIT_IF(g_audio == nullptr);

	auto id = g_audio->AudioInOpen(type, len, freq, format);

	if (!id.IsValid()) {
		return AUDIO_IN_ERROR_PORT_FULL;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI AudioInInput(int handle, void* dest) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(dest == nullptr);

	EXIT_IF(g_audio == nullptr);

	if (!g_audio->AudioInValid(Audio::Id(handle))) {
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}

	return static_cast<int>(g_audio->AudioInInput(Audio::Id(handle), dest));
}

int KYTY_SYSV_ABI AudioInGetSilentState(int handle) {
	PRINT_NAME();

	EXIT_IF(g_audio == nullptr);

	if (!g_audio->AudioInValid(Audio::Id(handle))) {
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}

	return g_audio->AudioInHasDevice(Audio::Id(handle)) ? 0 : AUDIO_IN_SILENT_STATE_DEVICE_NONE;
}

} // namespace AudioIn

namespace VoiceQoS {

LIB_NAME("VoiceQoS", "VoiceQoS");

int KYTY_SYSV_ABI VoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type) {
	PRINT_NAME();

	LOGF("\t mem_block = %016" PRIx64 "\n"
	     "\t mem_size = %" PRIu32 "\n"
	     "\t app_type = %" PRId32 "\n",
	     reinterpret_cast<uint64_t>(mem_block), mem_size, app_type);

	return OK;
}

} // namespace VoiceQoS

namespace Acm {

LIB_NAME("Acm", "Acm");

struct AcmBatchInfo {
	void*  buffer;
	size_t offset;
	size_t buffer_size;
};

struct AcmBatchError {
	uint32_t reserved[8];
};

static std::atomic_uint32_t                         g_acm_next_context {1};
static std::atomic_uint32_t                         g_acm_next_batch {1};
static std::mutex                                   g_acm_mutex;
static std::unordered_set<AcmContextId>             g_acm_contexts;
static std::unordered_map<AcmBatchId, AcmContextId> g_acm_batches;

constexpr int      ACM_ERROR_INVALID_CONTEXT   = static_cast<int>(0x81940002u);
constexpr int      ACM_ERROR_INVALID_BATCH     = static_cast<int>(0x81940003u);
constexpr int      ACM_ERROR_INVALID_PARAMETER = static_cast<int>(0x81940006u);
constexpr int      ACM_ERROR_BATCHBUFFER_FULL  = static_cast<int>(0x81940009u);
constexpr int      ACM_ERROR_ALIGNMENT         = static_cast<int>(0x8194000au);
constexpr size_t   ACM_COMMAND_SIZE            = 16;
constexpr uint32_t ACM_FFT_ZERO_2ND_HALF       = 1;
constexpr uint32_t ACM_FFT_SPLIT_INPUT         = 2;
constexpr uint32_t ACM_FFT_SPLIT_OUTPUT        = 4;
constexpr uint32_t ACM_IFFT_DROP_1ST_HALF      = 1;
constexpr uint32_t ACM_PANNER_KEYON            = 1;
constexpr uint32_t ACM_PANNER_SAMPLES          = 256;

struct AcmConvReverbBlockSize {
	uint16_t memory_size;
	uint16_t runtime_size;
};

struct AcmConvReverbChannel {
	void*                   blocks;
	AcmConvReverbBlockSize* sizes;
};

struct AcmConvReverbIn {
	uint32_t               block_size;
	uint32_t               block_count;
	uint32_t               channel_count;
	uint32_t               format;
	uint32_t               history_position;
	uint32_t               history_count;
	AcmConvReverbChannel** channels;
	float**                pcm;
};

struct AcmConvReverbIr {
	uint32_t               block_size;
	uint32_t               block_count;
	uint32_t               channel_count;
	uint32_t               format;
	float*                 block_gains;
	AcmConvReverbChannel** channels;
};

struct AcmConvReverbOut {
	uint32_t block_size;
	uint32_t channel_count;
	float**  temps;
	float**  pcm;
};

static bool acm_conv_input_valid(const AcmConvReverbIn* input);
static bool acm_conv_ir_out_valid(const AcmConvReverbIn& input, const AcmConvReverbIr* ir,
                                  const AcmConvReverbOut* output);
static void acm_conv_store_input(AcmConvReverbIn* input);
static void acm_conv_render(const AcmConvReverbIn& input, const AcmConvReverbIr& ir, float gain,
                            AcmConvReverbOut* output);

static int acm_reserve_batch(AcmBatchInfo* info, size_t bytes) {
	if (info == nullptr || info->buffer == nullptr || info->offset > info->buffer_size) {
		return ACM_ERROR_INVALID_PARAMETER;
	}
	if (bytes > info->buffer_size - info->offset) {
		return ACM_ERROR_BATCHBUFFER_FULL;
	}
	std::memset(static_cast<uint8_t*>(info->buffer) + info->offset, 0, bytes);
	info->offset += bytes;
	return OK;
}

static float acm_half_to_float(uint16_t value) {
	const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
	uint32_t       exp  = (value >> 10u) & 0x1fu;
	uint32_t       mant = value & 0x3ffu;
	uint32_t       bits = 0;
	if (exp == 0) {
		if (mant == 0) {
			bits = sign;
		} else {
			exp = 113;
			while ((mant & 0x400u) == 0) {
				mant <<= 1u;
				exp--;
			}
			bits = sign | (exp << 23u) | ((mant & 0x3ffu) << 13u);
		}
	} else if (exp == 0x1fu) {
		bits = sign | 0x7f800000u | (mant << 13u);
	} else {
		bits = sign | ((exp + 112u) << 23u) | (mant << 13u);
	}
	return std::bit_cast<float>(bits);
}

static uint16_t acm_float_to_half(float value) {
	const uint32_t bits = std::bit_cast<uint32_t>(value);
	const uint32_t sign = (bits >> 16u) & 0x8000u;
	const uint32_t abs  = bits & 0x7fffffffu;
	if (abs >= 0x7f800000u) {
		return static_cast<uint16_t>(sign | (abs > 0x7f800000u ? 0x7e00u : 0x7c00u));
	}
	if (abs > 0x477fefffu) {
		return static_cast<uint16_t>(sign | 0x7c00u);
	}
	if (abs < 0x33000001u) {
		return static_cast<uint16_t>(sign);
	}
	if (abs < 0x38800000u) {
		const uint32_t shift = 113u - (abs >> 23u);
		const uint32_t mant  = (abs & 0x7fffffu) | 0x800000u;
		return static_cast<uint16_t>(sign | ((mant + (1u << (shift + 12u))) >> (shift + 13u)));
	}
	return static_cast<uint16_t>(sign | (((abs + 0x1000u) >> 13u) - 0x1c000u));
}

static float acm_read_value(const void* data, size_t index, int format) {
	return format == 0 ? static_cast<const float*>(data)[index]
	                   : acm_half_to_float(static_cast<const uint16_t*>(data)[index]);
}

static void acm_write_value(void* data, size_t index, int format, float value) {
	if (format == 0) {
		static_cast<float*>(data)[index] = value;
	} else {
		static_cast<uint16_t*>(data)[index] = acm_float_to_half(value);
	}
}

static bool acm_fft_size_valid(int size) {
	return size == 512 || size == 1024 || size == 2048;
}

static int acm_validate_fft(int size, int count, int input_format, const void* const input[],
                            int output_format, void* const output[], uint32_t flags) {
	if (!acm_fft_size_valid(size) || count <= 0 || input == nullptr || output == nullptr ||
	    (input_format != 0 && input_format != 1) || (output_format != 0 && output_format != 1) ||
	    (flags & ~7u) != 0) {
		return ACM_ERROR_INVALID_PARAMETER;
	}
	const auto input_count  = static_cast<size_t>(count) * ((flags & ACM_FFT_SPLIT_INPUT) ? 2 : 1);
	const auto output_count = static_cast<size_t>(count) * ((flags & ACM_FFT_SPLIT_OUTPUT) ? 2 : 1);
	for (size_t i = 0; i < input_count; i++) {
		if (input[i] == nullptr) {
			return ACM_ERROR_INVALID_PARAMETER;
		}
	}
	for (size_t i = 0; i < output_count; i++) {
		if (output[i] == nullptr) {
			return ACM_ERROR_INVALID_PARAMETER;
		}
	}
	return OK;
}

static void acm_fft_write_output(void* const output[], int operation, bool split, int format,
                                 const std::vector<float>& values) {
	if (!split) {
		for (size_t i = 0; i < values.size(); i++) {
			acm_write_value(output[operation], i, format, values[i]);
		}
		return;
	}
	const auto half = values.size() / 2;
	for (size_t i = 0; i < half; i++) {
		acm_write_value(output[operation * 2], i, format, values[i]);
		acm_write_value(output[operation * 2 + 1], i, format, values[half + i]);
	}
}

static void acm_fft_transform(std::vector<std::complex<double>>* values, bool inverse) {
	auto&      data = *values;
	const auto n    = data.size();
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1u;
		for (; (j & bit) != 0; bit >>= 1u) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(data[i], data[j]);
		}
	}
	const auto pi = std::acos(-1.0);
	for (size_t length = 2; length <= n; length <<= 1u) {
		const auto angle = (inverse ? 2.0 : -2.0) * pi / static_cast<double>(length);
		const std::complex<double> step(std::cos(angle), std::sin(angle));
		for (size_t base = 0; base < n; base += length) {
			std::complex<double> factor(1.0, 0.0);
			for (size_t i = 0; i < length / 2; i++) {
				const auto even             = data[base + i];
				const auto odd              = data[base + i + length / 2] * factor;
				data[base + i]              = even + odd;
				data[base + i + length / 2] = even - odd;
				factor *= step;
			}
		}
	}
	if (inverse) {
		for (auto& value: data) {
			value /= static_cast<double>(n);
		}
	}
}

int KYTY_SYSV_ABI AcmContextCreate(AcmContextId* context) {
	PRINT_NAME();

	if (context == nullptr) {
		return ACM_ERROR_INVALID_PARAMETER;
	}

	*context = g_acm_next_context.fetch_add(1, std::memory_order_relaxed);
	{
		std::scoped_lock lock(g_acm_mutex);
		g_acm_contexts.insert(*context);
	}

	LOGF("\t context = %" PRIu32 "\n", *context);

	return OK;
}

int KYTY_SYSV_ABI AcmContextDestroy(AcmContextId context) {
	PRINT_NAME();
	LOGF("\t context = %" PRIu32 "\n", context);
	std::scoped_lock lock(g_acm_mutex);
	if (g_acm_contexts.erase(context) == 0) {
		return ACM_ERROR_INVALID_CONTEXT;
	}
	std::erase_if(g_acm_batches, [context](const auto& item) { return item.second == context; });
	return OK;
}

int KYTY_SYSV_ABI AcmBatchStartBuffer(AcmContextId context, const void* batch_commands,
                                      size_t batch_size, AcmBatchError* batch_error,
                                      AcmBatchId* batch) {
	PRINT_NAME();

	if (batch == nullptr || batch_commands == nullptr || batch_size == 0) {
		return ACM_ERROR_INVALID_PARAMETER;
	}

	if (batch_error != nullptr) {
		std::memset(batch_error, 0, sizeof(AcmBatchError));
	}

	std::scoped_lock lock(g_acm_mutex);
	if (!g_acm_contexts.contains(context)) {
		return ACM_ERROR_INVALID_CONTEXT;
	}
	*batch                = g_acm_next_batch.fetch_add(1, std::memory_order_relaxed);
	g_acm_batches[*batch] = context;

	return OK;
}

int KYTY_SYSV_ABI AcmBatchStartBuffers(AcmContextId context, uint32_t batch_info_count,
                                       const AcmBatchInfo* const batch_info[],
                                       AcmBatchError* batch_error, AcmBatchId* batch) {
	PRINT_NAME();

	if (batch_info_count == 0 || batch_info == nullptr || batch == nullptr) {
		return ACM_ERROR_INVALID_PARAMETER;
	}
	for (uint32_t i = 0; i < batch_info_count; i++) {
		if (batch_info[i] == nullptr || batch_info[i]->buffer == nullptr ||
		    batch_info[i]->offset > batch_info[i]->buffer_size) {
			return ACM_ERROR_INVALID_PARAMETER;
		}
	}

	if (batch_error != nullptr) {
		std::memset(batch_error, 0, sizeof(AcmBatchError));
	}

	std::scoped_lock lock(g_acm_mutex);
	if (!g_acm_contexts.contains(context)) {
		return ACM_ERROR_INVALID_CONTEXT;
	}
	*batch                = g_acm_next_batch.fetch_add(1, std::memory_order_relaxed);
	g_acm_batches[*batch] = context;

	return OK;
}

int KYTY_SYSV_ABI AcmBatchWait(AcmContextId context, AcmBatchId batch, uint32_t timeout) {
	PRINT_NAME();
	(void)timeout;
	std::scoped_lock lock(g_acm_mutex);
	if (!g_acm_contexts.contains(context)) {
		return ACM_ERROR_INVALID_CONTEXT;
	}
	const auto it = g_acm_batches.find(batch);
	if (it == g_acm_batches.end() || it->second != context) {
		return ACM_ERROR_INVALID_BATCH;
	}
	g_acm_batches.erase(it);
	return OK;
}

int KYTY_SYSV_ABI AcmBatchJobNotification(AcmBatchInfo* batch_info, uint8_t value,
                                          volatile void* notification) {
	PRINT_NAME();
	if (value == 0 || notification == nullptr) {
		return ACM_ERROR_INVALID_PARAMETER;
	}
	if ((reinterpret_cast<uintptr_t>(notification) & 127u) != 0) {
		return ACM_ERROR_ALIGNMENT;
	}
	const auto result = acm_reserve_batch(batch_info, 2 * ACM_COMMAND_SIZE);
	if (result != OK) {
		return result;
	}
	*static_cast<volatile uint8_t*>(notification) = value;
	return OK;
}

int KYTY_SYSV_ABI AcmConvReverbSharedInput(AcmBatchInfo* batch_info, uint32_t block_count, void* in,
                                           uint32_t count, const void* const ir[],
                                           const float* gain, void* const out[]) {
	PRINT_NAME();
	if (block_count != 1 || in == nullptr || count == 0 || ir == nullptr || out == nullptr) {
		return ACM_ERROR_INVALID_PARAMETER;
	}
	auto* input = static_cast<AcmConvReverbIn*>(in);
	if (!acm_conv_input_valid(input)) {
		return ACM_ERROR_INVALID_PARAMETER;
	}
	for (uint32_t i = 0; i < count; i++) {
		if ((gain != nullptr && !std::isfinite(gain[i])) ||
		    !acm_conv_ir_out_valid(*input, static_cast<const AcmConvReverbIr*>(ir[i]),
		                           static_cast<const AcmConvReverbOut*>(out[i]))) {
			return ACM_ERROR_INVALID_PARAMETER;
		}
	}
	const auto reserve_result = acm_reserve_batch(batch_info, 1024);
	if (reserve_result != OK) {
		return reserve_result;
	}
	acm_conv_store_input(input);
	for (uint32_t i = 0; i < count; i++) {
		acm_conv_render(*input, *static_cast<const AcmConvReverbIr*>(ir[i]),
		                gain != nullptr ? gain[i] : 1.0f, static_cast<AcmConvReverbOut*>(out[i]));
	}
	return OK;
}

int KYTY_SYSV_ABI AcmConvReverbSharedIr(AcmBatchInfo* batch_info, uint32_t block_count,
                                        const void* ir, uint32_t count, void* const in[],
                                        const float* gain, void* const out[]) {
	PRINT_NAME();
	if (block_count != 1 || ir == nullptr || count == 0 || in == nullptr || out == nullptr) {
		return ACM_ERROR_INVALID_PARAMETER;
	}
	const auto* impulse = static_cast<const AcmConvReverbIr*>(ir);
	for (uint32_t i = 0; i < count; i++) {
		auto* input = static_cast<AcmConvReverbIn*>(in[i]);
		if ((gain != nullptr && !std::isfinite(gain[i])) || !acm_conv_input_valid(input) ||
		    !acm_conv_ir_out_valid(*input, impulse, static_cast<const AcmConvReverbOut*>(out[i]))) {
			return ACM_ERROR_INVALID_PARAMETER;
		}
	}
	const auto reserve_result = acm_reserve_batch(batch_info, 1024);
	if (reserve_result != OK) {
		return reserve_result;
	}
	for (uint32_t i = 0; i < count; i++) {
		auto* input = static_cast<AcmConvReverbIn*>(in[i]);
		acm_conv_store_input(input);
		acm_conv_render(*input, *impulse, gain != nullptr ? gain[i] : 1.0f,
		                static_cast<AcmConvReverbOut*>(out[i]));
	}
	return OK;
}

int KYTY_SYSV_ABI AcmFft(AcmBatchInfo* batch_info, int size, int count, int input_format,
                         const void* const input[], int output_format, void* const output[],
                         uint32_t flags) {
	PRINT_NAME();
	const auto validation =
	    acm_validate_fft(size, count, input_format, input, output_format, output, flags);
	if (validation != OK) {
		return validation;
	}
	const auto command_count =
	    2u + static_cast<uint32_t>(count) * (7u + ((flags & ACM_FFT_SPLIT_INPUT) ? 1u : 0u) +
	                                         ((flags & ACM_FFT_SPLIT_OUTPUT) ? 1u : 0u));
	const auto reserve_result = acm_reserve_batch(batch_info, command_count * ACM_COMMAND_SIZE);
	if (reserve_result != OK) {
		return reserve_result;
	}

	const bool zero_second_half = (flags & ACM_FFT_ZERO_2ND_HALF) != 0;
	const bool split_input      = (flags & ACM_FFT_SPLIT_INPUT) != 0;
	const bool split_output     = (flags & ACM_FFT_SPLIT_OUTPUT) != 0;
	const auto n                = static_cast<size_t>(size);
	for (int operation = 0; operation < count; operation++) {
		std::vector<std::complex<double>> samples(n);
		const auto                        provided = zero_second_half ? n / 2 : n;
		for (size_t i = 0; i < provided; i++) {
			const auto part  = split_input && i >= n / 2 ? 1 : 0;
			const auto index = split_input ? i % (n / 2) : i;
			samples[i] = acm_read_value(input[operation * (split_input ? 2 : 1) + part], index,
			                            input_format);
		}
		acm_fft_transform(&samples, false);
		std::vector<float> packed(n, 0.0f);
		for (size_t k = 0; k <= n / 2; k++) {
			const auto sum = samples[k];
			if (k == 0) {
				packed[0] = static_cast<float>(sum.real());
			} else if (k == n / 2) {
				packed[1] = static_cast<float>(sum.real());
			} else {
				packed[k * 2]     = static_cast<float>(sum.real());
				packed[k * 2 + 1] = static_cast<float>(sum.imag());
			}
		}
		acm_fft_write_output(output, operation, split_output, output_format, packed);
	}
	return OK;
}

int KYTY_SYSV_ABI AcmIfft(AcmBatchInfo* batch_info, int size, int count, int input_format,
                          const void* const input[], int output_format, void* const output[],
                          uint32_t flags) {
	PRINT_NAME();
	const auto validation = acm_validate_fft(size, count, input_format, input, output_format,
	                                         output, flags & ~ACM_IFFT_DROP_1ST_HALF);
	if (validation != OK || (flags & ~7u) != 0) {
		return validation != OK ? validation : ACM_ERROR_INVALID_PARAMETER;
	}
	const auto command_count =
	    2u + static_cast<uint32_t>(count) *
	             (7u + ((flags & ACM_FFT_SPLIT_INPUT) ? 1u : 0u) +
	              ((flags & (ACM_IFFT_DROP_1ST_HALF | ACM_FFT_SPLIT_OUTPUT)) != 0 ? 1u : 0u));
	const auto reserve_result = acm_reserve_batch(batch_info, command_count * ACM_COMMAND_SIZE);
	if (reserve_result != OK) {
		return reserve_result;
	}

	const bool split_input  = (flags & ACM_FFT_SPLIT_INPUT) != 0;
	const bool split_output = (flags & ACM_FFT_SPLIT_OUTPUT) != 0;
	const bool drop_first   = (flags & ACM_IFFT_DROP_1ST_HALF) != 0;
	const auto n            = static_cast<size_t>(size);
	for (int operation = 0; operation < count; operation++) {
		std::vector<float> packed(n, 0.0f);
		for (size_t i = 0; i < n; i++) {
			const auto part  = split_input && i >= n / 2 ? 1 : 0;
			const auto index = split_input ? i % (n / 2) : i;
			packed[i] = acm_read_value(input[operation * (split_input ? 2 : 1) + part], index,
			                           input_format);
		}
		std::vector<std::complex<double>> spectrum(n);
		spectrum[0]     = {packed[0], 0.0};
		spectrum[n / 2] = {packed[1], 0.0};
		for (size_t k = 1; k < n / 2; k++) {
			spectrum[k]     = {packed[k * 2], packed[k * 2 + 1]};
			spectrum[n - k] = std::conj(spectrum[k]);
		}
		acm_fft_transform(&spectrum, true);
		std::vector<float> samples(n, 0.0f);
		for (size_t t = 0; t < n; t++) {
			samples[t] = static_cast<float>(spectrum[t].real());
		}
		if (drop_first) {
			samples.erase(samples.begin(), samples.begin() + static_cast<ptrdiff_t>(n / 2));
		}
		acm_fft_write_output(output, operation, split_output, output_format, samples);
	}
	return OK;
}

int KYTY_SYSV_ABI AcmPanner(AcmBatchInfo* batch_info, uint32_t in_count, const float* const in[],
                            uint32_t biquad_count, uint32_t biquad_update_count, uint32_t out_count,
                            const void* const parameter[], void* const state[],
                            const float* const out_init[], float* const out[]) {
	PRINT_NAME();
	if (in_count == 0 || in == nullptr || biquad_count > 2 || biquad_update_count > 1 ||
	    out_count == 0 || out_count > 36 || parameter == nullptr || state == nullptr ||
	    out == nullptr) {
		return ACM_ERROR_INVALID_PARAMETER;
	}
	for (uint32_t voice = 0; voice < in_count; voice++) {
		if (in[voice] == nullptr || parameter[voice] == nullptr || state[voice] == nullptr) {
			return ACM_ERROR_INVALID_PARAMETER;
		}
	}
	for (uint32_t channel = 0; channel < out_count; channel++) {
		if (out[channel] == nullptr) {
			return ACM_ERROR_INVALID_PARAMETER;
		}
	}
	const auto reserve_result = acm_reserve_batch(batch_info, 20 * ACM_COMMAND_SIZE);
	if (reserve_result != OK) {
		return reserve_result;
	}

	for (uint32_t channel = 0; channel < out_count; channel++) {
		if (out_init != nullptr && out_init[channel] != nullptr) {
			std::memcpy(out[channel], out_init[channel], ACM_PANNER_SAMPLES * sizeof(float));
		} else {
			std::memset(out[channel], 0, ACM_PANNER_SAMPLES * sizeof(float));
		}
	}
	const auto coefficient_sets  = 1u << biquad_update_count;
	const auto coefficient_count = biquad_count * coefficient_sets * 5u;
	for (uint32_t voice = 0; voice < in_count; voice++) {
		const auto* bytes         = static_cast<const uint8_t*>(parameter[voice]);
		const auto  voice_flags   = *reinterpret_cast<const uint32_t*>(bytes);
		const auto* coefficients  = reinterpret_cast<const float*>(bytes + sizeof(uint32_t));
		const auto* target_gain   = coefficients + coefficient_count;
		auto*       voice_state   = static_cast<float*>(state[voice]);
		auto*       filter_state  = voice_state;
		auto*       previous_gain = voice_state + biquad_count * 2u;
		for (uint32_t sample = 0; sample < ACM_PANNER_SAMPLES; sample++) {
			float value = in[voice][sample];
			for (uint32_t biquad = 0; biquad < biquad_count; biquad++) {
				const auto update =
				    biquad_update_count == 0 ? 0u : sample / (ACM_PANNER_SAMPLES / 2u);
				const auto* c        = coefficients + (biquad * coefficient_sets + update) * 5u;
				auto&       z1       = filter_state[biquad * 2u];
				auto&       z2       = filter_state[biquad * 2u + 1u];
				const auto  filtered = c[0] * value + z1;
				z1                   = c[1] * value - c[3] * filtered + z2;
				z2                   = c[2] * value - c[4] * filtered;
				value                = filtered;
			}
			const auto blend =
			    static_cast<float>(sample + 1u) / static_cast<float>(ACM_PANNER_SAMPLES);
			for (uint32_t channel = 0; channel < out_count; channel++) {
				const auto gain = (voice_flags & ACM_PANNER_KEYON) != 0
				                      ? target_gain[channel]
				                      : previous_gain[channel] +
				                            (target_gain[channel] - previous_gain[channel]) * blend;
				out[channel][sample] += value * gain;
			}
		}
		std::memcpy(previous_gain, target_gain, out_count * sizeof(float));
	}
	return OK;
}

} // namespace Acm

namespace Audio3d {

LIB_NAME("Audio3d", "Audio3d");

namespace Semaphore = LibKernel::Semaphore;

constexpr int AUDIO3D_ERROR_INVALID_PARAMETER = static_cast<int>(0x80ea0004u);
constexpr int AUDIO3D_ERROR_INVALID_PORT      = static_cast<int>(0x80ea0002u);
constexpr int AUDIO3D_ERROR_INVALID_OBJECT    = static_cast<int>(0x80ea0003u);
constexpr int AUDIO3D_ERROR_OUT_OF_RESOURCES  = static_cast<int>(0x80ea0006u);
constexpr int AUDIO3D_ERROR_NOT_READY         = static_cast<int>(0x80ea0007u);

constexpr uint32_t AUDIO3D_FORMAT_S16            = 0;
constexpr uint32_t AUDIO3D_FORMAT_FLOAT          = 1;
constexpr uint32_t AUDIO3D_ATTRIBUTE_PCM         = 0x00000001;
constexpr uint32_t AUDIO3D_ATTRIBUTE_PRIORITY    = 0x00000002;
constexpr uint32_t AUDIO3D_ATTRIBUTE_POSITION    = 0x00000003;
constexpr uint32_t AUDIO3D_ATTRIBUTE_SPREAD      = 0x00000004;
constexpr uint32_t AUDIO3D_ATTRIBUTE_GAIN        = 0x00000005;
constexpr uint32_t AUDIO3D_ATTRIBUTE_PASSTHROUGH = 0x00000006;
constexpr uint32_t AUDIO3D_ATTRIBUTE_RESET_STATE = 0x00000007;
constexpr uint32_t AUDIO3D_ATTRIBUTE_RESTRICTED  = 0x0000000a;

struct Audio3dOpenParameters {
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
	// uint32_t num_beds;
};

struct Audio3dData {
	enum class State { Empty, Ready, Play };

	std::atomic<State> state = State::Empty;
	std::vector<float> stereo_mix;
};

struct Audio3dPcm {
	uint32_t    format;
	uint32_t    reserved;
	const void* sample_buffer;
	uint32_t    num_samples;
	uint32_t    reserved2;
};

struct Audio3dPosition {
	float x;
	float y;
	float z;
};

struct Audio3dAttribute {
	uint32_t    attribute_id;
	uint32_t    reserved;
	const void* value;
	size_t      value_size;
};

struct Audio3dObject {
	bool            used        = false;
	uint32_t        priority    = 0;
	Audio3dPosition position    = {};
	float           spread      = 0.0f;
	float           gain        = 1.0f;
	uint32_t        passthrough = 0;
	bool            restricted  = false;
};

static_assert(sizeof(Audio3dPcm) == 24);
static_assert(sizeof(Audio3dAttribute) == 24);

struct Audio3dInternal {
	Audio3dData*               data          = nullptr;
	Common::Mutex*             data_mutex    = nullptr;
	uint64_t                   data_delay    = 0;
	Semaphore::KernelSema      playback_sema = nullptr;
	Audio3dOpenParameters      params        = {};
	std::vector<Audio3dObject> objects;
	int                        user_id                     = 0;
	int                        output_handle               = 0;
	float                      late_reverb_level           = 0.0f;
	float                      downmix_spread_radius       = 2.0f;
	int                        downmix_spread_height_aware = 0;
	uint32_t                   data_index                  = 0;
	bool                       used                        = false;
	std::atomic_bool           closing                     = false;
	std::atomic_bool           playback_finished           = false;
};

constexpr uint32_t MAX_PORTS = 4;

static Audio3dInternal g_ports[MAX_PORTS] = {};

static void playback_simulate(void* arg) {
	auto* port = static_cast<Audio3dInternal*>(arg);
	EXIT_IF(port == nullptr);
	EXIT_IF(port->data_mutex == nullptr);
	EXIT_IF(port->data == nullptr);

	for (;;) {
		int result = Semaphore::KernelWaitSema(port->playback_sema, 1, nullptr);

		if (result != OK || port->closing) {
			break;
		}

		Audio3dData* play_data = nullptr;

		port->data_mutex->Lock();
		{
			for (uint32_t i = 0; i < port->params.queue_depth; i++) {
				uint32_t index = (port->data_index + i) % port->params.queue_depth;

				if (port->data[index].state == Audio3dData::State::Play) {
					play_data = &port->data[index];
					break;
				}
			}
		}
		port->data_mutex->Unlock();

		EXIT_IF(play_data == nullptr);

		if (play_data != nullptr) {
			if (port->output_handle > 0 && !play_data->stereo_mix.empty()) {
				const AudioInternal::OutputParam output {port->output_handle,
				                                         play_data->stereo_mix.data()};
				AudioInternal::AudioOutOutputs(&output, 1, true);
			} else {
				Common::Thread::SleepMicro(port->data_delay);
			}
			port->data_mutex->Lock();
			std::fill(play_data->stereo_mix.begin(), play_data->stereo_mix.end(), 0.0f);
			play_data->state = Audio3dData::State::Empty;
			port->data_mutex->Unlock();
		}
	}

	port->playback_finished = true;
}

int KYTY_SYSV_ABI Audio3dInitialize(int64_t reserved) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(reserved != 0);

	return OK;
}

void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(sizeof(Audio3dOpenParameters) != 0x20);
	if (p == nullptr) {
		return;
	}

	*p = Audio3dOpenParameters();
}

int KYTY_SYSV_ABI Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters,
                                  uint32_t* id) {
	PRINT_NAME();

	if (parameters == nullptr || id == nullptr ||
	    parameters->size != sizeof(Audio3dOpenParameters)) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	*id = std::numeric_limits<uint32_t>::max();

	LOGF("\t user_id     = %d\n"
	     "\t granularity = %u\n"
	     "\t rate        = %u\n"
	     "\t max_objects = %u\n"
	     "\t queue_depth = %u\n"
	     "\t buffer_mode = %u\n",
	     user_id, parameters->granularity, parameters->rate, parameters->max_objects,
	     parameters->queue_depth, parameters->buffer_mode);

	const auto max_queue_depth =
	    parameters->granularity == 256
	        ? 64u
	        : (parameters->granularity == 512 ? 31u : (parameters->granularity == 768 ? 20u : 15u));
	if (parameters->buffer_mode != 2 || (user_id != 255 && user_id != 1) || parameters->rate != 0 ||
	    parameters->granularity == 0 || (parameters->granularity % 256) != 0 ||
	    parameters->max_objects > 512 || parameters->queue_depth == 0 ||
	    parameters->queue_depth > max_queue_depth) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	uint32_t port = 0;
	for (; port < MAX_PORTS; port++) {
		if (!g_ports[port].used) {
			break;
		}
	}

	if (port >= MAX_PORTS) {
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	g_ports[port].user_id = user_id;
	g_ports[port].params  = *parameters;
	g_ports[port].used    = true;
	g_ports[port].closing = false;
	g_ports[port].objects.assign(parameters->max_objects, Audio3dObject {});

	EXIT_IF(g_ports[port].data != nullptr);
	EXIT_IF(g_ports[port].data_mutex != nullptr);
	EXIT_IF(g_ports[port].playback_sema != nullptr);

	g_ports[port].data       = new Audio3dData[parameters->queue_depth];
	g_ports[port].data_index = 0;
	g_ports[port].data_mutex = new Common::Mutex;
	g_ports[port].data_delay = (1000000 * static_cast<uint64_t>(parameters->granularity)) / 48000;

	for (uint32_t d = 0; d < parameters->queue_depth; d++) {
		g_ports[port].data[d].state = Audio3dData::State::Empty;
		g_ports[port].data[d].stereo_mix.assign(static_cast<size_t>(parameters->granularity) * 2u,
		                                        0.0f);
	}
	g_ports[port].output_handle =
	    AudioInternal::AudioOutOpen(AUDIO_OUT_PORT_TYPE_AUDIO3D, parameters->granularity, 48000,
	                                AudioInternal::Format::FloatStereo);

	int result = Semaphore::KernelCreateSema(&g_ports[port].playback_sema, "audio3d_play", 0x01, 0,
	                                         static_cast<int>(parameters->queue_depth), nullptr);
	EXIT_NOT_IMPLEMENTED(result != OK);

	g_ports[port].playback_finished = false;
	Common::Thread playback_thread(playback_simulate, &g_ports[port]);
	playback_thread.Detach();

	*id = port;

	return OK;
}

static Audio3dInternal* audio3d_get_port(uint32_t port_id) {
	return port_id < MAX_PORTS && g_ports[port_id].used ? &g_ports[port_id] : nullptr;
}

static Audio3dData* audio3d_get_writable_data(Audio3dInternal* port) {
	if (port == nullptr || port->data == nullptr || port->data_index >= port->params.queue_depth) {
		return nullptr;
	}
	auto* data = &port->data[port->data_index];
	return data->state == Audio3dData::State::Empty ? data : nullptr;
}

static float audio3d_read_sample(const void* buffer, uint32_t format, size_t sample) {
	if (format == AUDIO3D_FORMAT_FLOAT) {
		return std::clamp(static_cast<const float*>(buffer)[sample], -1.0f, 1.0f);
	}
	return static_cast<float>(static_cast<const int16_t*>(buffer)[sample]) / 32768.0f;
}

static void audio3d_object_gains(const Audio3dObject& object, float* left, float* right) {
	if (object.passthrough == 1) {
		*left  = object.gain;
		*right = 0.0f;
		return;
	}
	if (object.passthrough == 2) {
		*left  = 0.0f;
		*right = object.gain;
		return;
	}
	const auto horizontal = std::abs(object.position.x) + std::abs(object.position.z);
	const auto pan =
	    horizontal > 0.000001f ? std::clamp(object.position.x / horizontal, -1.0f, 1.0f) : 0.0f;
	*left  = std::sqrt((1.0f - pan) * 0.5f) * object.gain;
	*right = std::sqrt((1.0f + pan) * 0.5f) * object.gain;
}

static int audio3d_mix_object_pcm(Audio3dInternal* port, Audio3dObject* object,
                                  const Audio3dPcm& pcm) {
	if ((pcm.format != AUDIO3D_FORMAT_S16 && pcm.format != AUDIO3D_FORMAT_FLOAT) ||
	    pcm.sample_buffer == nullptr || pcm.num_samples > port->params.granularity ||
	    (reinterpret_cast<uintptr_t>(pcm.sample_buffer) &
	     (pcm.format == AUDIO3D_FORMAT_FLOAT ? alignof(float) - 1u : alignof(int16_t) - 1u)) != 0) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	auto* data = audio3d_get_writable_data(port);
	if (data == nullptr) {
		return AUDIO3D_ERROR_NOT_READY;
	}
	float left  = 0.0f;
	float right = 0.0f;
	audio3d_object_gains(*object, &left, &right);
	for (uint32_t sample = 0; sample < pcm.num_samples; sample++) {
		const auto value = audio3d_read_sample(pcm.sample_buffer, pcm.format, sample);
		data->stereo_mix[static_cast<size_t>(sample) * 2u] += value * left;
		data->stereo_mix[static_cast<size_t>(sample) * 2u + 1u] += value * right;
	}
	return OK;
}

int KYTY_SYSV_ABI Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id,
                                          const void* attribute, size_t attribute_size) {
	PRINT_NAME();

	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (attribute == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	LOGF("\t attribute_id = 0x%" PRIx32 "\n", attribute_id);

	switch (attribute_id) {
		case 0x10001:
			if (attribute_size != sizeof(float)) {
				return AUDIO3D_ERROR_INVALID_PARAMETER;
			}
			port->late_reverb_level = std::clamp(*static_cast<const float*>(attribute), 0.0f, 1.0f);
			LOGF("\t late_reverb_level = %f\n", port->late_reverb_level);
			break;
		case 0x10002:
			if (attribute_size != sizeof(float)) {
				return AUDIO3D_ERROR_INVALID_PARAMETER;
			}
			port->downmix_spread_radius = std::max(0.0f, *static_cast<const float*>(attribute));
			LOGF("\t downmix_spread_radius = %f\n", port->downmix_spread_radius);
			break;
		case 0x10003:
			if (attribute_size != sizeof(int)) {
				return AUDIO3D_ERROR_INVALID_PARAMETER;
			}
			port->downmix_spread_height_aware = *static_cast<const int*>(attribute) != 0;
			LOGF("\t downmix_spread_height_aware = %d\n", port->downmix_spread_height_aware);
			break;
		default: break;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level,
                                           uint32_t* queue_available) {
	PRINT_NAME();

	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (queue_level == nullptr && queue_available == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	uint32_t empty_num = 0;

	port->data_mutex->Lock();
	{
		for (uint32_t i = 0; i < port->params.queue_depth; i++) {
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Empty) {
				empty_num++;
			} else {
				break;
			}
		}
	}
	port->data_mutex->Unlock();

	EXIT_IF(empty_num > port->params.queue_depth);

	LOGF("\t queue_available = %u\n", empty_num);

	if (queue_level != nullptr) {
		*queue_level = port->params.queue_depth - empty_num;
	}
	if (queue_available != nullptr) {
		*queue_available = empty_num;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortAdvance(uint32_t port_id) {
	PRINT_NAME();

	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	port->data_mutex->Lock();
	{
		uint32_t current_index = port->data_index;
		uint32_t next_index    = (current_index + 1) % port->params.queue_depth;

		if (port->data[current_index].state != Audio3dData::State::Empty ||
		    port->data[next_index].state != Audio3dData::State::Empty) {
			port->data_mutex->Unlock();
			return AUDIO3D_ERROR_NOT_READY;
		}
		port->data[current_index].state = Audio3dData::State::Ready;

		port->data_index = next_index;

		LOGF("\t %u -> %u\n", current_index, next_index);
	}
	port->data_mutex->Unlock();

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortPush(uint32_t port_id, uint32_t blocking) {
	PRINT_NAME();

	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	if (blocking > 1) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	LOGF("\t blocking = %u\n", blocking);

	int          data_num   = 0;
	Audio3dData* first_data = nullptr;

	port->data_mutex->Lock();
	{
		for (uint32_t i = 0; i < port->params.queue_depth; i++) {
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Ready) {
				if (first_data == nullptr) {
					first_data = &port->data[index];
				}
				port->data[index].state = Audio3dData::State::Play;
				data_num++;
			}
		}
	}
	port->data_mutex->Unlock();

	LOGF("\t push num = %d\n", data_num);

	if (data_num > 0) {
		Semaphore::KernelSignalSema(port->playback_sema, data_num);

		if (blocking == 1 && first_data != nullptr) {
			auto wait_time = port->data_delay / 8;
			while (first_data->state != Audio3dData::State::Empty) {
				Common::Thread::SleepMicro(wait_time);
			}
		}
	}
	if (data_num == 0 && blocking == 0) {
		return AUDIO3D_ERROR_NOT_READY;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortClose(uint32_t port_id) {
	PRINT_NAME();
	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	port->closing = true;
	if (port->playback_sema != nullptr) {
		Semaphore::KernelSignalSema(port->playback_sema, 1);
		for (uint32_t i = 0; i < 500 && !port->playback_finished; i++) {
			Common::Thread::SleepMicro(1000);
		}
		Semaphore::KernelDeleteSema(port->playback_sema);
		port->playback_sema = nullptr;
	}
	if (port->output_handle > 0) {
		AudioInternal::AudioOutClose(port->output_handle);
	}
	delete[] port->data;
	delete port->data_mutex;
	port->data          = nullptr;
	port->data_mutex    = nullptr;
	port->output_handle = 0;
	port->data_index    = 0;
	port->objects.clear();
	port->used              = false;
	port->closing           = false;
	port->playback_finished = true;
	return OK;
}

void Shutdown() {
	for (uint32_t port = 0; port < MAX_PORTS; port++) {
		if (g_ports[port].used) {
			(void)Audio3dPortClose(port);
		}
	}
}

int KYTY_SYSV_ABI Audio3dObjectReserve(uint32_t port_id, uint32_t* object_id) {
	PRINT_NAME();
	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (object_id == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	*object_id = std::numeric_limits<uint32_t>::max();
	Common::LockGuard lock(*port->data_mutex);
	for (uint32_t id = 0; id < port->objects.size(); id++) {
		if (!port->objects[id].used) {
			port->objects[id]      = {};
			port->objects[id].used = true;
			*object_id             = id;
			return OK;
		}
	}
	return AUDIO3D_ERROR_OUT_OF_RESOURCES;
}

int KYTY_SYSV_ABI Audio3dObjectUnreserve(uint32_t port_id, uint32_t object_id) {
	PRINT_NAME();
	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	Common::LockGuard lock(*port->data_mutex);
	if (object_id >= port->objects.size() || !port->objects[object_id].used) {
		return AUDIO3D_ERROR_INVALID_OBJECT;
	}
	port->objects[object_id] = {};
	return OK;
}

static bool audio3d_attribute_size_valid(const Audio3dAttribute& attribute) {
	switch (attribute.attribute_id) {
		case AUDIO3D_ATTRIBUTE_PCM:
			return attribute.value != nullptr && attribute.value_size == sizeof(Audio3dPcm);
		case AUDIO3D_ATTRIBUTE_PRIORITY:
		case AUDIO3D_ATTRIBUTE_PASSTHROUGH:
			return attribute.value != nullptr && attribute.value_size == sizeof(uint32_t);
		case AUDIO3D_ATTRIBUTE_POSITION:
			return attribute.value != nullptr && attribute.value_size == sizeof(Audio3dPosition);
		case AUDIO3D_ATTRIBUTE_SPREAD:
		case AUDIO3D_ATTRIBUTE_GAIN:
			return attribute.value != nullptr && attribute.value_size == sizeof(float);
		case AUDIO3D_ATTRIBUTE_RESET_STATE: return attribute.value_size == 0;
		case AUDIO3D_ATTRIBUTE_RESTRICTED:
			return attribute.value != nullptr && (attribute.value_size == sizeof(bool) ||
			                                      attribute.value_size == sizeof(uint32_t));
		default: return true;
	}
}

int KYTY_SYSV_ABI Audio3dObjectSetAttributes(uint32_t port_id, uint32_t object_id,
                                             size_t                  num_attributes,
                                             const Audio3dAttribute* attributes) {
	PRINT_NAME();
	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (num_attributes == 0 || attributes == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	std::unordered_set<uint32_t> ids;
	for (size_t i = 0; i < num_attributes; i++) {
		if (!ids.insert(attributes[i].attribute_id).second ||
		    !audio3d_attribute_size_valid(attributes[i])) {
			return AUDIO3D_ERROR_INVALID_PARAMETER;
		}
	}

	Common::LockGuard lock(*port->data_mutex);
	if (object_id >= port->objects.size() || !port->objects[object_id].used) {
		return AUDIO3D_ERROR_INVALID_OBJECT;
	}
	auto* object = &port->objects[object_id];
	if (std::any_of(attributes, attributes + num_attributes,
	                [](const Audio3dAttribute& item) {
		                return item.attribute_id == AUDIO3D_ATTRIBUTE_PCM;
	                }) &&
	    audio3d_get_writable_data(port) == nullptr) {
		return AUDIO3D_ERROR_NOT_READY;
	}

	for (size_t i = 0; i < num_attributes; i++) {
		const auto& attribute = attributes[i];
		if (attribute.attribute_id == AUDIO3D_ATTRIBUTE_RESET_STATE) {
			*object      = {};
			object->used = true;
		}
	}
	for (size_t i = 0; i < num_attributes; i++) {
		const auto& attribute = attributes[i];
		switch (attribute.attribute_id) {
			case AUDIO3D_ATTRIBUTE_PRIORITY:
				object->priority = *static_cast<const uint32_t*>(attribute.value);
				break;
			case AUDIO3D_ATTRIBUTE_POSITION:
				object->position = *static_cast<const Audio3dPosition*>(attribute.value);
				break;
			case AUDIO3D_ATTRIBUTE_SPREAD:
				object->spread = std::clamp(*static_cast<const float*>(attribute.value), 0.0f,
				                            static_cast<float>(2.0 * std::acos(-1.0)));
				break;
			case AUDIO3D_ATTRIBUTE_GAIN:
				object->gain = std::max(0.0f, *static_cast<const float*>(attribute.value));
				break;
			case AUDIO3D_ATTRIBUTE_PASSTHROUGH: {
				const auto value = *static_cast<const uint32_t*>(attribute.value);
				if (value > 2) {
					return AUDIO3D_ERROR_INVALID_PARAMETER;
				}
				object->passthrough = value;
				break;
			}
			case AUDIO3D_ATTRIBUTE_RESTRICTED:
				object->restricted = attribute.value_size == sizeof(bool)
				                         ? *static_cast<const bool*>(attribute.value)
				                         : *static_cast<const uint32_t*>(attribute.value) != 0;
				break;
			default: break;
		}
	}
	for (size_t i = 0; i < num_attributes; i++) {
		if (attributes[i].attribute_id == AUDIO3D_ATTRIBUTE_PCM) {
			return audio3d_mix_object_pcm(port, object,
			                              *static_cast<const Audio3dPcm*>(attributes[i].value));
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Audio3dBedWrite(uint32_t port_id, uint32_t num_channels, uint32_t format,
                                  const void* buffer, uint32_t num_samples) {
	PRINT_NAME();
	auto* port = audio3d_get_port(port_id);
	if (port == nullptr) {
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if ((num_channels != 2 && num_channels != 6 && num_channels != 8) ||
	    (format != AUDIO3D_FORMAT_S16 && format != AUDIO3D_FORMAT_FLOAT) || buffer == nullptr ||
	    num_samples > port->params.granularity ||
	    (reinterpret_cast<uintptr_t>(buffer) &
	     (format == AUDIO3D_FORMAT_FLOAT ? alignof(float) - 1u : alignof(int16_t) - 1u)) != 0) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	Common::LockGuard lock(*port->data_mutex);
	auto*             data = audio3d_get_writable_data(port);
	if (data == nullptr) {
		return AUDIO3D_ERROR_NOT_READY;
	}
	for (uint32_t sample = 0; sample < num_samples; sample++) {
		const auto base  = static_cast<size_t>(sample) * num_channels;
		float      left  = audio3d_read_sample(buffer, format, base);
		float      right = audio3d_read_sample(buffer, format, base + 1u);
		if (num_channels >= 6) {
			const auto center = audio3d_read_sample(buffer, format, base + 2u);
			const auto lfe    = audio3d_read_sample(buffer, format, base + 3u);
			left += center * 0.70710678f + lfe * 0.5f +
			        audio3d_read_sample(buffer, format, base + 4u) * 0.70710678f;
			right += center * 0.70710678f + lfe * 0.5f +
			         audio3d_read_sample(buffer, format, base + 5u) * 0.70710678f;
			if (num_channels == 8) {
				left += audio3d_read_sample(buffer, format, base + 6u) * 0.5f;
				right += audio3d_read_sample(buffer, format, base + 7u) * 0.5f;
			}
		}
		data->stereo_mix[static_cast<size_t>(sample) * 2u] += left;
		data->stereo_mix[static_cast<size_t>(sample) * 2u + 1u] += right;
	}
	return OK;
}

int KYTY_SYSV_ABI Audio3dBedWrite2(uint32_t port_id, uint32_t num_channels, uint32_t format,
                                   const void* buffer, uint32_t num_samples, uint32_t output_route,
                                   bool restricted) {
	(void)restricted;
	if (output_route > 2) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	return Audio3dBedWrite(port_id, num_channels, format, buffer, num_samples);
}

int KYTY_SYSV_ABI Audio3dAudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq,
                                      uint32_t param) {
	return AudioOut::AudioOutOpen(user_id, type, index, len, freq, param);
}

int32_t KYTY_SYSV_ABI Audio3dAudioOutOutput(int32_t handle, const void* data) {
	if (handle <= 0 || data == nullptr) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	const AudioInternal::OutputParam output {handle, data};
	return AudioInternal::AudioOutOutputs(&output, 1, false) != 0 ? OK : AUDIO3D_ERROR_INVALID_PORT;
}

int32_t KYTY_SYSV_ABI Audio3dAudioOutOutputs(AudioOut::AudioOutOutputParam* params, uint32_t num) {
	if (params == nullptr || num == 0) {
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	std::vector<AudioInternal::OutputParam> outputs(num);
	for (uint32_t i = 0; i < num; i++) {
		outputs[i].handle = params[i].handle;
		outputs[i].data   = params[i].ptr;
		if (outputs[i].handle <= 0 || outputs[i].data == nullptr) {
			return AUDIO3D_ERROR_INVALID_PARAMETER;
		}
	}
	return AudioInternal::AudioOutOutputs(outputs.data(), num, false) != 0
	           ? OK
	           : AUDIO3D_ERROR_INVALID_PORT;
}

} // namespace Audio3d

namespace Ngs2 {

LIB_NAME("Ngs2", "Ngs2");

constexpr uint32_t NGS2_WAVEFORM_TYPE_ATRAC9 = 0x40;

struct Ngs2SystemOption {
	size_t    size                     = 0;
	char      name[64]                 = {};
	uintptr_t job_scheduler_options[4] = {};
	uint32_t  flags                    = 0;
	uint32_t  max_grain_samples        = 0;
	uint32_t  num_grain_samples        = 0;
	uint32_t  sample_rate              = 0;
	uint32_t  max_voice_channels       = 0;
	uint32_t  reserved[5]              = {};
};

struct Ngs2RackOption {
	size_t   size                   = 0;
	char     name[64]               = {};
	uint32_t flags                  = 0;
	uint32_t max_grain_samples      = 0;
	uint32_t max_voices             = 0;
	uint32_t max_input_delay_blocks = 0;
	uint32_t max_matrices           = 0;
	uint32_t max_ports              = 0;
	uint32_t max_voice_channels     = 0;
	uint32_t max_output_channels    = 0;
	uint32_t reserved[18]           = {};
};

struct Ngs2MasteringRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SubmixerRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       max_envelope_points   = 0;
	uint32_t       max_filters           = 0;
	uint32_t       max_inputs            = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SamplerRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channel_works        = 0;
	uint32_t       max_codec_caches         = 0;
	uint32_t       max_waveform_blocks      = 0;
	uint32_t       max_envelope_points      = 0;
	uint32_t       max_filters              = 0;
	uint32_t       max_atrac9_decoders      = 0;
	uint32_t       max_atrac9_channel_works = 0;
	uint32_t       max_ajm_atrac9_decoders  = 0;
	uint32_t       num_peak_meter_blocks    = 0;
};

struct Ngs2ReverbRackOption {
	Ngs2RackOption rack_option;
	uint32_t       max_channels = 0;
	uint32_t       reverb_size  = 0;
};

struct Ngs2CustomModuleOption {
	uint32_t size = 0;
};

struct Ngs2CustomRackModuleInfo {
	const Ngs2CustomModuleOption* option           = nullptr;
	uint32_t                      module_id        = 0;
	uint32_t                      source_buffer_id = 0;
	uint32_t                      extra_buffer_id  = 0;
	uint32_t                      dest_buffer_id   = 0;
	uint32_t                      state_offset     = 0;
	uint32_t                      state_size       = 0;
	uint32_t                      reserved         = 0;
	uint32_t                      reserved2        = 0;
};

struct Ngs2CustomRackPortInfo {
	uint32_t source_buffer_id = 0;
	uint32_t reserved         = 0;
};

struct Ngs2CustomRackOption {
	Ngs2RackOption           rack_option;
	uint32_t                 state_size  = 0;
	uint32_t                 num_buffers = 0;
	uint32_t                 num_modules = 0;
	uint32_t                 reserved    = 0;
	Ngs2CustomRackModuleInfo module[24];
	Ngs2CustomRackPortInfo   port[16];
};

struct Ngs2CustomSubmixerRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channels = 0;
	uint32_t             max_inputs   = 0;
};

struct Ngs2CustomMasteringRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channels = 0;
	uint32_t             max_inputs   = 0;
};

struct Ngs2CustomSamplerRackOption {
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channel_works        = 0;
	uint32_t             max_waveform_blocks      = 0;
	uint32_t             max_atrac9_decoders      = 0;
	uint32_t             max_atrac9_channel_works = 0;
	uint32_t             max_ajm_atrac9_decoders  = 0;
	uint32_t             max_codec_caches         = 0;
};

union Ngs2RackOptionUnion {
	Ngs2RackOption                common;
	Ngs2SamplerRackOption         sampler;
	Ngs2MasteringRackOption       mastering;
	Ngs2SubmixerRackOption        submixer;
	Ngs2ReverbRackOption          reverb;
	Ngs2CustomSubmixerRackOption  custom_submixer;
	Ngs2CustomMasteringRackOption custom_mastering;
	Ngs2CustomSamplerRackOption   custom_sampler;
};

struct Ngs2ContextBufferInfo {
	void*     host_buffer      = nullptr;
	size_t    host_buffer_size = 0;
	uintptr_t reserved[5]      = {};
	uintptr_t user_data        = 0;
};

struct Ngs2SystemInfo {
	char                  name[64]      = {};
	uintptr_t             system_handle = 0;
	Ngs2ContextBufferInfo buffer_info;
	uint32_t              uid               = 0;
	uint32_t              min_grain_samples = 0;
	uint32_t              max_grain_samples = 0;
	uint32_t              state_flags       = 0;
	uint32_t              rack_count        = 0;
	float                 last_render_ratio = 0.0f;
	uint64_t              last_render_tick  = 0;
	uint64_t              render_count      = 0;
	uint32_t              sample_rate       = 0;
	uint32_t              num_grain_samples = 0;
};

struct Ngs2RenderBufferInfo {
	void*    buffer        = nullptr;
	size_t   buffer_size   = 0;
	uint32_t waveform_type = 0;
	uint32_t num_channels  = 0;
};

struct Ngs2WaveformFormat {
	uint32_t waveform_type = 0;
	uint32_t num_channels  = 0;
	uint32_t sample_rate   = 0;
	uint32_t config_data   = 0;
	uint32_t frame_margin  = 0;
	uint32_t frame_offset  = 0;
};

struct Ngs2WaveformBlock {
	uintptr_t data_offset      = 0;
	size_t    data_size        = 0;
	uint32_t  num_repeats      = 0;
	uint32_t  num_skip_samples = 0;
	uint32_t  num_samples      = 0;
	uint32_t  reserved         = 0;
	uintptr_t user_data        = 0;
};

struct Ngs2WaveformInfo {
	Ngs2WaveformFormat format;
	uint32_t           data_offset              = 0;
	uint32_t           data_size                = 0;
	uint32_t           loop_begin_position      = 0;
	uint32_t           loop_end_position        = 0;
	uint32_t           num_samples              = 0;
	uint32_t           audio_unit_size          = 0;
	uint32_t           num_audio_unit_samples   = 0;
	uint32_t           num_audio_unit_per_frame = 0;
	uint32_t           audio_frame_size         = 0;
	uint32_t           num_audio_frame_samples  = 0;
	uint32_t           num_delay_samples        = 0;
	uint32_t           num_blocks               = 0;
	Ngs2WaveformBlock  blocks[4];
};

struct Ngs2PanParam {
	float angle     = 0.0f;
	float distance  = 0.0f;
	float fbw_level = 0.0f;
	float lfe_level = 0.0f;
};

struct Ngs2PanWork {
	float    speaker_angles[8] = {};
	float    unit_angle        = 0.0f;
	uint32_t num_speakers      = 0;
};

struct Ngs2GeomVector {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Ngs2GeomCone {
	float inner_level = 0.0f;
	float inner_angle = 0.0f;
	float outer_level = 0.0f;
	float outer_angle = 0.0f;
};

struct Ngs2GeomRolloff {
	uint32_t model              = 0;
	float    max_distance       = 0.0f;
	float    rolloff_factor     = 0.0f;
	float    reference_distance = 0.0f;
};

struct Ngs2GeomListenerParam {
	Ngs2GeomVector position;
	Ngs2GeomVector orient_front;
	Ngs2GeomVector orient_up;
	Ngs2GeomVector velocity;
	float          sound_speed = 0.0f;
	uint32_t       reserved[2] = {};
};

struct Ngs2GeomListenerWork {
	float          matrix[4][4] = {};
	Ngs2GeomVector velocity;
	float          sound_speed = 0.0f;
	uint32_t       coordinate  = 0;
	uint32_t       reserved[3] = {};
};

struct Ngs2GeomSourceParam {
	Ngs2GeomVector  position;
	Ngs2GeomVector  velocity;
	Ngs2GeomVector  direction;
	Ngs2GeomCone    cone;
	Ngs2GeomRolloff rolloff;
	float           doppler_factor = 0.0f;
	float           fbw_level      = 0.0f;
	float           lfe_level      = 0.0f;
	float           max_level      = 0.0f;
	float           min_level      = 0.0f;
	float           radius         = 0.0f;
	uint32_t        num_speakers   = 0;
	uint32_t        matrix_format  = 0;
	uint32_t        reserved[2]    = {};
};

struct Ngs2GeomA3dAttribute {
	Ngs2GeomVector position;
	float          volume      = 0.0f;
	uint32_t       reserved[4] = {};
};

struct Ngs2GeomAttribute {
	float                pitch_ratio = 0.0f;
	float                level[64]   = {};
	Ngs2GeomA3dAttribute a3d_attrib;
	uint32_t             reserved[4] = {};
};

using Ngs2BufferAllocHandler = int32_t KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);
using Ngs2BufferFreeHandler  = int32_t  KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);

struct Ngs2BufferAllocator {
	Ngs2BufferAllocHandler alloc_handler = nullptr;
	Ngs2BufferFreeHandler  free_handler  = nullptr;
	uintptr_t              user_data     = 0;
};

struct Ngs2Internal {
	Ngs2SystemOption      option;
	Ngs2ContextBufferInfo buffer_info;
	Ngs2BufferAllocator   allocator;
	Ngs2Internal*         next         = nullptr;
	uint64_t              render_count = 0;
	uint32_t              uid          = 0;
	Common::Mutex         mutex;
};

enum class Ngs2RackType {
	Sampler,
	Submixer,
	Mastering,
	Reverb,
	CustomSubmixer,
	CustomMastering,
	CustomSampler,
};

struct Ngs2RackInternal {
	Ngs2Internal*         ngs  = nullptr;
	Ngs2RackInternal*     next = nullptr;
	Ngs2RackType          type = Ngs2RackType::Sampler;
	Ngs2RackOptionUnion   option;
	Ngs2ContextBufferInfo buffer_info;
	Ngs2BufferAllocator   allocator;
};

enum class Ngs2VoicePlayState { Empty, Playing, Paused, Stopped };

enum class Ngs2VoicePlayEvent { None, Play, Pause, Resume, Stop, StopImm, Kill };

struct Ngs2VoiceInternal {
	Ngs2VoicePlayEvent                     event          = Ngs2VoicePlayEvent::None;
	Ngs2VoicePlayState                     state          = Ngs2VoicePlayState::Empty;
	Ngs2RackInternal*                      rack           = nullptr;
	uintptr_t                              callback       = 0;
	uintptr_t                              callback_data  = 0;
	uint32_t                               callback_flags = 0;
	Ngs2WaveformFormat                     waveform_format {};
	const uint8_t*                         waveform_data = nullptr;
	std::array<Ngs2WaveformBlock, 32>      waveform_blocks {};
	uint32_t                               num_waveform_blocks    = 0;
	uint32_t                               current_waveform_block = 0;
	uint32_t                               current_repeat         = 0;
	double                                 sample_position        = 0.0;
	float                                  pitch_ratio            = 1.0f;
	float                                  volume                 = 1.0f;
	uint32_t                               waveform_flags         = 0;
	uint32_t                               frame_offset           = 0;
	bool                                   waveform_continuous    = false;
	bool                                   exit_loop              = false;
	uint64_t                               num_decoded_samples    = 0;
	uint64_t                               decoded_data_size      = 0;
	uint64_t                               waveform_user_data     = 0;
	std::array<Ajm::HeVagChannelState, 8>  hevag_channel_state {};
	std::array<Ajm::HeVagChannelState, 8>  hevag_block_start_state {};
	std::array<std::array<int16_t, 28>, 8> hevag_samples {};
	uint32_t                               hevag_block = std::numeric_limits<uint32_t>::max();
	uint32_t                               hevag_unit  = std::numeric_limits<uint32_t>::max();
};

struct Ngs2VoiceParamHeader {
	uint16_t size;
	int16_t  next;
	uint32_t id;
};

struct Ngs2VoiceEventParam {
	Ngs2VoiceParamHeader header;
	uint32_t             event_id;
};

struct Ngs2VoicePatchParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             dest_input_id;
	uintptr_t            dest_handle;
};

struct Ngs2VoiceMatrixLevelsParam {
	Ngs2VoiceParamHeader header;
	uint32_t             matrix_id;
	uint32_t             num_levels;
	const float*         levels;
};

struct Ngs2VoicePortMatrixParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	int32_t              matrix_id;
};

struct Ngs2VoicePortVolumeParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	float                level;
};

struct Ngs2VoicePortDelayParam {
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             num_samples;
};

struct Ngs2VoiceCallbackParam {
	Ngs2VoiceParamHeader header;
	uintptr_t            callback;
	uintptr_t            callback_data;
	uint32_t             flags;
	uint32_t             reserved;
};

struct Ngs2SamplerVoiceSetupParam {
	Ngs2VoiceParamHeader header;
	Ngs2WaveformFormat   format;
	uint32_t             flags;
	uint32_t             reserved;
};

struct Ngs2SamplerVoiceWaveformBlocksParam {
	Ngs2VoiceParamHeader     header;
	const void*              data;
	uint32_t                 flags;
	uint32_t                 num_blocks;
	const Ngs2WaveformBlock* blocks;
};

struct Ngs2SamplerVoiceWaveformAddressParam {
	Ngs2VoiceParamHeader header;
	const void*          from;
	const void*          to;
};

struct Ngs2SamplerVoiceWaveformFrameOffsetParam {
	Ngs2VoiceParamHeader header;
	uint32_t             frame_offset;
	uint32_t             reserved;
};

struct Ngs2SamplerVoicePitchParam {
	Ngs2VoiceParamHeader header;
	float                ratio;
	uint32_t             reserved;
};

union Ngs2CommandValue {
	float       f;
	double      d;
	int32_t     i;
	uint32_t    u;
	uint64_t    w;
	const void* p;
};

struct Ngs2CommandParam {
	uint32_t         param_id;
	uint8_t          flags;
	uint8_t          value_type;
	uint16_t         num_values;
	Ngs2CommandValue value;
};

struct Ngs2VoiceSetupCommandParam {
	uint32_t num_channels;
	uint32_t flags;
};

struct Ngs2WaveformRelocateParam {
	const void* from;
	const void* to;
};

static_assert(sizeof(Ngs2CommandParam) == 16);

struct Ngs2VoiceState {
	uint32_t state_flags;
	int32_t  error_code;
};

struct Ngs2SubmixerVoiceState {
	Ngs2VoiceState voice_state;
	float          envelope_height;
	float          peak_height;
	float          compressor_height;
};

struct Ngs2CustomMasteringVoiceState {
	Ngs2VoiceState voice_state;
	uint32_t       reserved;
	uint32_t       reserved2;
};

using Ngs2CustomSubmixerVoiceState = Ngs2CustomMasteringVoiceState;

struct Ngs2CustomSamplerVoiceState {
	Ngs2VoiceState voice_state;
	const void*    waveform_data;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	uint32_t       reserved;
	uint32_t       reserved2;
};

struct Ngs2SamplerVoiceState {
	Ngs2VoiceState voice_state;
	float          envelope_height;
	float          peak_height;
	uint32_t       reserved;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	const void*    waveform_data;
};

static Ngs2Internal*        g_ngs_list     = nullptr;
static Ngs2RackInternal*    g_racks_list   = nullptr;
static std::atomic_uint32_t g_next_ngs_uid = 1;
static Common::Mutex        g_racks_mutex;

static_assert(sizeof(Ngs2SystemOption) == 144);
static_assert(sizeof(Ngs2SystemInfo) == 184);
static_assert(sizeof(Ngs2RackOption) == 176);
static_assert(sizeof(Ngs2VoiceState) == 8);
static_assert(sizeof(Ngs2SubmixerVoiceState) == 20);
static_assert(sizeof(Ngs2CustomMasteringVoiceState) == 16);
static_assert(sizeof(Ngs2CustomSamplerVoiceState) == 48);
static_assert(sizeof(Ngs2SamplerVoiceState) == 56);
static_assert(sizeof(Ngs2WaveformFormat) == 24);
static_assert(sizeof(Ngs2WaveformBlock) == 40);
static_assert(sizeof(Ngs2WaveformInfo) == 232);

static uint32_t Ngs2GetStateFlags(const Ngs2VoiceInternal* voice) {
	switch (voice->state) {
		case Ngs2VoicePlayState::Empty: return 0;
		case Ngs2VoicePlayState::Playing: return 0x3;
		case Ngs2VoicePlayState::Paused: return 0x5;
		case Ngs2VoicePlayState::Stopped: return 0xb;
	}

	return 0;
}

static Ngs2SystemOption Ngs2DefaultSystemOption() {
	Ngs2SystemOption option {};
	option.size              = sizeof(Ngs2SystemOption);
	option.max_grain_samples = 512;
	option.num_grain_samples = 256;
	option.sample_rate       = 48000;
	return option;
}

int KYTY_SYSV_ABI Ngs2SystemResetOption(Ngs2SystemOption* option) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);

	*option = Ngs2DefaultSystemOption();
	return OK;
}

static Ngs2Internal* Ngs2CreateSystemInternal(const Ngs2SystemOption*      option,
                                              const Ngs2ContextBufferInfo* buffer_info) {
	auto* ngs = new (buffer_info->host_buffer) Ngs2Internal;

	ngs->option      = *option;
	ngs->buffer_info = *buffer_info;
	ngs->uid         = g_next_ngs_uid.fetch_add(1, std::memory_order_relaxed);
	ngs->next        = g_ngs_list;
	g_ngs_list       = ngs;

	return ngs;
}

static bool Ngs2RackIsCustom(Ngs2RackType type) {
	switch (type) {
		case Ngs2RackType::CustomSubmixer:
		case Ngs2RackType::CustomMastering:
		case Ngs2RackType::CustomSampler: return true;
		default: return false;
	}
}

int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option,
                                            Ngs2ContextBufferInfo*  buffer_info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	std::memset(buffer_info, 0, sizeof(Ngs2ContextBufferInfo));
	buffer_info->host_buffer_size = sizeof(Ngs2Internal);

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption*      option,
                                   const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size < sizeof(Ngs2Internal));

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	auto* ngs = Ngs2CreateSystemInternal(option, buffer_info);

	*handle = reinterpret_cast<uintptr_t>(ngs);

	return OK;
}

static void Ngs2FillDefaultRackOption(uint32_t rack_id, Ngs2RackOptionUnion* option) {
	EXIT_NOT_IMPLEMENTED(option == nullptr);

	*option = {};

	switch (rack_id) {
		case 0x1000:
			option->sampler.rack_option.size                   = sizeof(Ngs2SamplerRackOption);
			option->sampler.rack_option.max_grain_samples      = 512;
			option->sampler.rack_option.max_voices             = 256;
			option->sampler.rack_option.max_input_delay_blocks = 0;
			option->sampler.rack_option.max_matrices           = 1;
			option->sampler.rack_option.max_ports              = 8;
			option->sampler.max_channel_works                  = 256;
			option->sampler.max_codec_caches                   = 32;
			option->sampler.max_waveform_blocks                = 4;
			option->sampler.max_envelope_points                = 4;
			option->sampler.max_filters                        = 8;
			option->sampler.max_atrac9_decoders                = 256;
			option->sampler.max_atrac9_channel_works           = 256;
			option->sampler.max_ajm_atrac9_decoders            = 0;
			option->sampler.num_peak_meter_blocks              = 8;
			break;
		case 0x2000:
			option->submixer.rack_option.size                   = sizeof(Ngs2SubmixerRackOption);
			option->submixer.rack_option.max_grain_samples      = 512;
			option->submixer.rack_option.max_voices             = 1;
			option->submixer.rack_option.max_input_delay_blocks = 1;
			option->submixer.rack_option.max_matrices           = 1;
			option->submixer.rack_option.max_ports              = 8;
			option->submixer.max_channels                       = 8;
			option->submixer.max_envelope_points                = 4;
			option->submixer.max_filters                        = 8;
			option->submixer.max_inputs                         = 1;
			option->submixer.num_peak_meter_blocks              = 8;
			break;
		case 0x2001:
			option->reverb.rack_option.size                   = sizeof(Ngs2ReverbRackOption);
			option->reverb.rack_option.max_grain_samples      = 512;
			option->reverb.rack_option.max_voices             = 1;
			option->reverb.rack_option.max_input_delay_blocks = 1;
			option->reverb.rack_option.max_matrices           = 1;
			option->reverb.rack_option.max_ports              = 8;
			option->reverb.max_channels                       = 8;
			option->reverb.reverb_size                        = 1;
			break;
		case 0x3000:
			option->mastering.rack_option.size                   = sizeof(Ngs2MasteringRackOption);
			option->mastering.rack_option.max_grain_samples      = 512;
			option->mastering.rack_option.max_voices             = 1;
			option->mastering.rack_option.max_input_delay_blocks = 1;
			option->mastering.rack_option.max_matrices           = 0;
			option->mastering.rack_option.max_ports              = 0;
			option->mastering.max_channels                       = 8;
			option->mastering.num_peak_meter_blocks              = 8;
			break;
		case 0x4002:
			option->custom_submixer.custom_rack_option.rack_option.size =
			    sizeof(Ngs2CustomSubmixerRackOption);
			option->custom_submixer.custom_rack_option.rack_option.max_grain_samples      = 512;
			option->custom_submixer.custom_rack_option.rack_option.max_voices             = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_input_delay_blocks = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_matrices           = 1;
			option->custom_submixer.custom_rack_option.rack_option.max_ports              = 8;
			option->custom_submixer.custom_rack_option.state_size =
			    sizeof(Ngs2CustomSubmixerVoiceState);
			option->custom_submixer.custom_rack_option.num_buffers = 1;
			option->custom_submixer.max_channels                   = 8;
			option->custom_submixer.max_inputs                     = 1;
			break;
		case 0x4001:
			option->custom_sampler.custom_rack_option.rack_option.size =
			    sizeof(Ngs2CustomSamplerRackOption);
			option->custom_sampler.custom_rack_option.rack_option.max_grain_samples      = 512;
			option->custom_sampler.custom_rack_option.rack_option.max_voices             = 256;
			option->custom_sampler.custom_rack_option.rack_option.max_input_delay_blocks = 0;
			option->custom_sampler.custom_rack_option.rack_option.max_matrices           = 1;
			option->custom_sampler.custom_rack_option.rack_option.max_ports              = 8;
			option->custom_sampler.custom_rack_option.state_size =
			    sizeof(Ngs2CustomSamplerVoiceState);
			option->custom_sampler.custom_rack_option.num_buffers = 1;
			option->custom_sampler.max_channel_works              = 256;
			option->custom_sampler.max_waveform_blocks            = 4;
			option->custom_sampler.max_atrac9_decoders            = 256;
			option->custom_sampler.max_atrac9_channel_works       = 256;
			option->custom_sampler.max_ajm_atrac9_decoders        = 0;
			option->custom_sampler.max_codec_caches               = 32;
			break;
		case 0x4003:
			option->custom_mastering.custom_rack_option.rack_option.size =
			    sizeof(Ngs2CustomMasteringRackOption);
			option->custom_mastering.custom_rack_option.rack_option.max_grain_samples      = 512;
			option->custom_mastering.custom_rack_option.rack_option.max_voices             = 1;
			option->custom_mastering.custom_rack_option.rack_option.max_input_delay_blocks = 1;
			option->custom_mastering.custom_rack_option.rack_option.max_matrices           = 0;
			option->custom_mastering.custom_rack_option.rack_option.max_ports              = 0;
			option->custom_mastering.custom_rack_option.state_size =
			    sizeof(Ngs2CustomMasteringVoiceState);
			option->custom_mastering.custom_rack_option.num_buffers = 1;
			option->custom_mastering.max_channels                   = 8;
			option->custom_mastering.max_inputs                     = 1;
			break;
		default: EXIT("unknown rack_id for default option: 0x%" PRIx32 "\n", rack_id);
	}
}

int KYTY_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option,
                                          Ngs2ContextBufferInfo* buffer_info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option     = nullptr, using reset defaults for rack_id 0x%" PRIx32 "\n", rack_id);
	}

	LOGF("\t rack_id    = 0x%" PRIx32 "\n"
	     "\t max_voices = %u\n",
	     rack_id, option->max_voices);

	buffer_info->host_buffer_size =
	    sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal) * option->max_voices;

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption*    option,
                                                const Ngs2BufferAllocator* allocator,
                                                uintptr_t*                 handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);

	auto default_option = Ngs2DefaultSystemOption();
	if (option == nullptr) {
		option = &default_option;
		LOGF("\t option            = nullptr, using reset defaults\n");
	}

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	LOGF("\t name              = %.64s\n"
	     "\t flags             = %u\n"
	     "\t max_grain_samples = %u\n"
	     "\t num_grain_samples = %u\n"
	     "\t sample_rate       = %u\n"
	     "\t max_voice_channels = %u\n"
	     "\t alloc_handler     = 0x%016" PRIx64 "\n"
	     "\t free_handler      = 0x%016" PRIx64 "\n"
	     "\t user_data         = 0x%016" PRIx64 "\n",
	     option->name, option->flags, option->max_grain_samples, option->num_grain_samples,
	     option->sample_rate, option->max_voice_channels,
	     reinterpret_cast<uint64_t>(allocator->alloc_handler),
	     reinterpret_cast<uint64_t>(allocator->free_handler),
	     static_cast<uint64_t>(allocator->user_data));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = sizeof(Ngs2Internal);
	buf.user_data        = allocator->user_data;

	int result = allocator->alloc_handler(&buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	auto* ngs      = Ngs2CreateSystemInternal(option, &buf);
	ngs->allocator = *allocator;

	*handle = reinterpret_cast<uintptr_t>(ngs);

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemGetInfo(uintptr_t system_handle, Ngs2SystemInfo* info,
                                    size_t info_size) {
	constexpr int32_t ERROR_INVALID_OUT_ADDRESS   = static_cast<int32_t>(0x804a8010u);
	constexpr int32_t ERROR_INVALID_OUT_SIZE      = static_cast<int32_t>(0x804a8011u);
	constexpr int32_t ERROR_INVALID_SYSTEM_HANDLE = static_cast<int32_t>(0x804a8201u);

	if (info == nullptr) {
		return ERROR_INVALID_OUT_ADDRESS;
	}
	if (info_size != sizeof(Ngs2SystemInfo)) {
		return ERROR_INVALID_OUT_SIZE;
	}

	auto* ngs     = reinterpret_cast<Ngs2Internal*>(system_handle);
	auto* current = g_ngs_list;
	while (current != nullptr && current != ngs) {
		current = current->next;
	}
	if (current == nullptr) {
		return ERROR_INVALID_SYSTEM_HANDLE;
	}

	Common::LockGuard lock(ngs->mutex);

	*info = {};
	std::memcpy(info->name, ngs->option.name, sizeof(info->name));
	info->system_handle     = system_handle;
	info->buffer_info       = ngs->buffer_info;
	info->uid               = ngs->uid;
	info->min_grain_samples = 64;
	info->max_grain_samples = ngs->option.max_grain_samples;
	info->state_flags       = 1;
	info->render_count      = ngs->render_count;
	info->sample_rate       = ngs->option.sample_rate;
	info->num_grain_samples = ngs->option.num_grain_samples;

	Common::LockGuard racks_lock(g_racks_mutex);
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (rack->ngs == ngs) {
			info->rack_count++;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t num_samples) {
	PRINT_NAME();
	LOGF("\t system_handle = 0x%016" PRIx64 "\n"
	     "\t num_samples   = %u\n",
	     static_cast<uint64_t>(system_handle), num_samples);

	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	auto* ngs                     = reinterpret_cast<Ngs2Internal*>(system_handle);
	ngs->option.num_grain_samples = num_samples;

	return OK;
}

int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle, Ngs2ContextBufferInfo* buffer_info) {
	constexpr int32_t ERROR_FAIL                  = static_cast<int32_t>(0x804a8001u);
	constexpr int32_t ERROR_INVALID_SYSTEM_HANDLE = static_cast<int32_t>(0x804a8201u);
	PRINT_NAME();
	LOGF("\t system_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(system_handle));

	if (buffer_info != nullptr) {
		*buffer_info = {};
	}
	if (system_handle == 0) {
		return ERROR_INVALID_SYSTEM_HANDLE;
	}

	auto* ngs = reinterpret_cast<Ngs2Internal*>(system_handle);
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
			if (rack->ngs == ngs) {
				return ERROR_FAIL;
			}
		}
	}

	auto** link = &g_ngs_list;
	while (*link != nullptr && *link != ngs) {
		link = &(*link)->next;
	}
	if (*link == nullptr) {
		return ERROR_INVALID_SYSTEM_HANDLE;
	}
	*link = ngs->next;

	const auto context_buffer = ngs->buffer_info;
	const auto allocator      = ngs->allocator;
	ngs->~Ngs2Internal();
	if (allocator.free_handler != nullptr) {
		auto free_buffer = context_buffer;
		return allocator.free_handler(&free_buffer);
	}
	if (buffer_info != nullptr) {
		*buffer_info = context_buffer;
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id,
                                 const Ngs2RackOption*        option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size == 0);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option                 = nullptr, using reset defaults for rack_id 0x%" PRIx32
		     "\n",
		     rack_id);
	}

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	LOGF("\t rack_id                = 0x%" PRIx32 "\n"
	     "\t name                   = %.64s\n"
	     "\t flags                  = %u\n"
	     "\t max_grain_samples      = %u\n"
	     "\t max_voices             = %u\n"
	     "\t max_input_delay_blocks = %u\n"
	     "\t max_matrices           = %u\n"
	     "\t max_ports              = %u\n"
	     "\t max_voice_channels     = %u\n"
	     "\t max_output_channels    = %u\n"
	     "\t host_buffer            = 0x%016" PRIx64 "\n"
	     "\t host_buffer_size      = 0x%016" PRIx64 "\n",
	     rack_id, option->name, option->flags, option->max_grain_samples, option->max_voices,
	     option->max_input_delay_blocks, option->max_matrices, option->max_ports,
	     option->max_voice_channels, option->max_output_channels,
	     reinterpret_cast<uint64_t>(buffer_info->host_buffer),
	     static_cast<uint64_t>(buffer_info->host_buffer_size));

	auto* ngs    = reinterpret_cast<Ngs2Internal*>(system_handle);
	auto* rack   = static_cast<Ngs2RackInternal*>(buffer_info->host_buffer);
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

	Common::LockGuard lock(ngs->mutex);

	switch (rack_id) {
		case 0x1000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SamplerRackOption));
			rack->option.sampler = *reinterpret_cast<const Ngs2SamplerRackOption*>(option);
			rack->type           = Ngs2RackType::Sampler;
			break;
		case 0x2000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SubmixerRackOption));
			rack->option.submixer = *reinterpret_cast<const Ngs2SubmixerRackOption*>(option);
			rack->type            = Ngs2RackType::Submixer;
			break;
		case 0x2001:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2ReverbRackOption));
			rack->option.reverb = *reinterpret_cast<const Ngs2ReverbRackOption*>(option);
			rack->type          = Ngs2RackType::Reverb;
			break;
		case 0x3000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2MasteringRackOption));
			rack->option.mastering = *reinterpret_cast<const Ngs2MasteringRackOption*>(option);
			rack->type             = Ngs2RackType::Mastering;
			break;
		case 0x4002:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomSubmixerRackOption));
			rack->option.custom_submixer =
			    *reinterpret_cast<const Ngs2CustomSubmixerRackOption*>(option);
			rack->type = Ngs2RackType::CustomSubmixer;
			break;
		case 0x4003:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomMasteringRackOption));
			rack->option.custom_mastering =
			    *reinterpret_cast<const Ngs2CustomMasteringRackOption*>(option);
			rack->type = Ngs2RackType::CustomMastering;
			break;
		case 0x4001:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomSamplerRackOption));
			rack->option.custom_sampler =
			    *reinterpret_cast<const Ngs2CustomSamplerRackOption*>(option);
			rack->type = Ngs2RackType::CustomSampler;
			break;
		default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
	}

	LOGF("\t type                   = %s\n", Common::EnumName(rack->type).c_str());

	rack->allocator   = Ngs2BufferAllocator();
	rack->buffer_info = *buffer_info;
	rack->ngs         = ngs;

	{
		Common::LockGuard racks_lock(g_racks_mutex);
		rack->next   = g_racks_list;
		g_racks_list = rack;
	}

	for (uint32_t i = 0; i < option->max_voices; i++) {
		voices[i]      = Ngs2VoiceInternal {};
		voices[i].rack = rack;
	}

	*handle = reinterpret_cast<uintptr_t>(rack);

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id,
                                              const Ngs2RackOption*      option,
                                              const Ngs2BufferAllocator* allocator,
                                              uintptr_t*                 handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	Ngs2RackOptionUnion default_option {};
	if (option == nullptr) {
		Ngs2FillDefaultRackOption(rack_id, &default_option);
		option = &default_option.common;
		LOGF("\t option                 = nullptr, using reset defaults for rack_id 0x%" PRIx32
		     "\n",
		     rack_id);
	}

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	LOGF("\t rack_id                = 0x%" PRIx32 "\n"
	     "\t name                   = %.64s\n"
	     "\t flags                  = %u\n"
	     "\t max_grain_samples      = %u\n"
	     "\t max_voices             = %u\n"
	     "\t max_input_delay_blocks = %u\n"
	     "\t max_matrices           = %u\n"
	     "\t max_ports              = %u\n"
	     "\t max_voice_channels     = %u\n"
	     "\t max_output_channels    = %u\n"
	     "\t alloc_handler          = 0x%016" PRIx64 "\n"
	     "\t free_handler           = 0x%016" PRIx64 "\n"
	     "\t user_data              = 0x%016" PRIx64 "\n",
	     rack_id, option->name, option->flags, option->max_grain_samples, option->max_voices,
	     option->max_input_delay_blocks, option->max_matrices, option->max_ports,
	     option->max_voice_channels, option->max_output_channels,
	     reinterpret_cast<uint64_t>(allocator->alloc_handler),
	     reinterpret_cast<uint64_t>(allocator->free_handler),
	     static_cast<uint64_t>(allocator->user_data));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = 0;
	buf.user_data        = allocator->user_data;

	Ngs2RackQueryBufferSize(rack_id, option, &buf);

	EXIT_NOT_IMPLEMENTED(buf.host_buffer_size == 0);

	int result = allocator->alloc_handler(&buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	result = Ngs2RackCreate(system_handle, rack_id, option, &buf, handle);

	if (result == OK) {
		auto* rack      = static_cast<Ngs2RackInternal*>(buf.host_buffer);
		rack->allocator = *allocator;
	}

	return result;
}

int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info) {
	constexpr int32_t ERROR_INVALID_RACK_HANDLE = static_cast<int32_t>(0x804a8202u);

	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	if (buffer_info != nullptr) {
		*buffer_info = {};
	}
	if (rack_handle == 0) {
		return ERROR_INVALID_RACK_HANDLE;
	}

	auto*         rack = reinterpret_cast<Ngs2RackInternal*>(rack_handle);
	Ngs2Internal* ngs  = nullptr;
	{
		Common::LockGuard racks_lock(g_racks_mutex);
		for (auto* current = g_racks_list; current != nullptr; current = current->next) {
			if (current == rack) {
				ngs = current->ngs;
				break;
			}
		}
	}
	if (ngs == nullptr) {
		return ERROR_INVALID_RACK_HANDLE;
	}

	Ngs2ContextBufferInfo context_buffer;
	Ngs2BufferAllocator   allocator;
	{
		Common::LockGuard lock(ngs->mutex);
		Common::LockGuard racks_lock(g_racks_mutex);

		auto** link = &g_racks_list;
		while (*link != nullptr && *link != rack) {
			link = &(*link)->next;
		}
		if (*link == nullptr) {
			return ERROR_INVALID_RACK_HANDLE;
		}

		*link          = rack->next;
		context_buffer = rack->buffer_info;
		allocator      = rack->allocator;
		rack->ngs      = nullptr;
		rack->next     = nullptr;
	}

	if (allocator.free_handler != nullptr) {
		return allocator.free_handler(&context_buffer);
	}
	if (buffer_info != nullptr) {
		*buffer_info = context_buffer;
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackLock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	EXIT_NOT_IMPLEMENTED(rack_handle == 0);

	auto* rack = reinterpret_cast<Ngs2RackInternal*>(rack_handle);

	EXIT_NOT_IMPLEMENTED(rack->ngs == nullptr);

	rack->ngs->mutex.Lock();

	return OK;
}

int KYTY_SYSV_ABI Ngs2RackUnlock(uintptr_t rack_handle) {
	PRINT_NAME();
	LOGF("\t rack_handle = 0x%016" PRIx64 "\n", static_cast<uint64_t>(rack_handle));

	EXIT_NOT_IMPLEMENTED(rack_handle == 0);

	auto* rack = reinterpret_cast<Ngs2RackInternal*>(rack_handle);

	EXIT_NOT_IMPLEMENTED(rack->ngs == nullptr);

	rack->ngs->mutex.Unlock();

	return OK;
}

static void Ngs2ResetHeVagDecoder(Ngs2VoiceInternal* voice);

static void Ngs2ResetVoicePlayback(Ngs2VoiceInternal* voice) {
	voice->current_waveform_block = 0;
	voice->current_repeat         = 0;
	voice->sample_position        = 0.0;
	voice->exit_loop              = false;
	voice->num_decoded_samples    = 0;
	voice->decoded_data_size      = 0;
	voice->waveform_user_data     = 0;
	Ngs2ResetHeVagDecoder(voice);
}

static void Ngs2ApplyVoiceEvent(Ngs2VoiceInternal* voice) {
	switch (voice->event) {
		case Ngs2VoicePlayEvent::None: break;
		case Ngs2VoicePlayEvent::Play:
			if (voice->state == Ngs2VoicePlayState::Empty ||
			    voice->state == Ngs2VoicePlayState::Stopped) {
				Ngs2ResetVoicePlayback(voice);
			}
			voice->state = Ngs2VoicePlayState::Playing;
			break;
		case Ngs2VoicePlayEvent::Pause:
			if (voice->state == Ngs2VoicePlayState::Playing) {
				voice->state = Ngs2VoicePlayState::Paused;
			}
			break;
		case Ngs2VoicePlayEvent::Resume:
			if (voice->state == Ngs2VoicePlayState::Paused) {
				voice->state = Ngs2VoicePlayState::Playing;
			}
			break;
		case Ngs2VoicePlayEvent::Stop:
			if (voice->state == Ngs2VoicePlayState::Playing ||
			    voice->state == Ngs2VoicePlayState::Paused) {
				voice->state = Ngs2VoicePlayState::Stopped;
			}
			break;
		case Ngs2VoicePlayEvent::StopImm:
		case Ngs2VoicePlayEvent::Kill:
			voice->state = Ngs2VoicePlayState::Empty;
			Ngs2ResetVoicePlayback(voice);
			break;
	}
	voice->event = Ngs2VoicePlayEvent::None;
}

static uint32_t Ngs2PcmBytesPerSample(uint32_t waveform_type) {
	switch (waveform_type) {
		case 0x10:
		case 0x11: return 1;
		case 0x12:
		case 0x13: return 2;
		case 0x14:
		case 0x15: return 3;
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19: return 4;
		case 0x1a:
		case 0x1b: return 8;
		default: return 0;
	}
}

static uint32_t Ngs2ReadPcmU32(const uint8_t* data, bool big_endian) {
	if (big_endian) {
		return (static_cast<uint32_t>(data[0]) << 24u) | (static_cast<uint32_t>(data[1]) << 16u) |
		       (static_cast<uint32_t>(data[2]) << 8u) | static_cast<uint32_t>(data[3]);
	}
	return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
	       (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

static uint64_t Ngs2ReadPcmU64(const uint8_t* data, bool big_endian) {
	uint64_t value = 0;
	if (big_endian) {
		for (uint32_t i = 0; i < 8; i++) {
			value = (value << 8u) | data[i];
		}
	} else {
		for (uint32_t i = 0; i < 8; i++) {
			value |= static_cast<uint64_t>(data[i]) << (i * 8u);
		}
	}
	return value;
}

static float Ngs2ReadPcmSample(const uint8_t* data, uint32_t waveform_type) {
	switch (waveform_type) {
		case 0x10: return static_cast<float>(*reinterpret_cast<const int8_t*>(data)) / 128.0f;
		case 0x11: return (static_cast<float>(*data) - 128.0f) / 128.0f;
		case 0x12:
		case 0x13: {
			const auto raw = waveform_type == 0x13
			                     ? static_cast<uint16_t>((data[0] << 8u) | data[1])
			                     : static_cast<uint16_t>(data[0] | (data[1] << 8u));
			return static_cast<float>(static_cast<int16_t>(raw)) / 32768.0f;
		}
		case 0x14:
		case 0x15: {
			uint32_t raw = waveform_type == 0x15
			                   ? ((static_cast<uint32_t>(data[0]) << 16u) |
			                      (static_cast<uint32_t>(data[1]) << 8u) | data[2])
			                   : (data[0] | (static_cast<uint32_t>(data[1]) << 8u) |
			                      (static_cast<uint32_t>(data[2]) << 16u));
			if ((raw & 0x00800000u) != 0) {
				raw |= 0xff000000u;
			}
			return static_cast<float>(static_cast<int32_t>(raw)) / 8388608.0f;
		}
		case 0x16:
		case 0x17:
			return static_cast<float>(
			           static_cast<int32_t>(Ngs2ReadPcmU32(data, waveform_type == 0x17))) /
			       2147483648.0f;
		case 0x18:
		case 0x19: return std::bit_cast<float>(Ngs2ReadPcmU32(data, waveform_type == 0x19));
		case 0x1a:
		case 0x1b:
			return static_cast<float>(
			    std::bit_cast<double>(Ngs2ReadPcmU64(data, waveform_type == 0x1b)));
		default: return 0.0f;
	}
}

static void Ngs2WritePcmSample(uint8_t* data, uint32_t waveform_type, float sample) {
	sample = std::clamp(sample, -1.0f, 1.0f);
	switch (waveform_type) {
		case 0x12:
		case 0x13: {
			const auto value =
			    static_cast<int16_t>(std::lround(sample * (sample < 0.0f ? 32768.0f : 32767.0f)));
			const auto raw                      = static_cast<uint16_t>(value);
			data[waveform_type == 0x13 ? 1 : 0] = static_cast<uint8_t>(raw);
			data[waveform_type == 0x13 ? 0 : 1] = static_cast<uint8_t>(raw >> 8u);
			break;
		}
		case 0x18:
		case 0x19: {
			const auto raw = std::bit_cast<uint32_t>(sample);
			if (waveform_type == 0x19) {
				data[0] = static_cast<uint8_t>(raw >> 24u);
				data[1] = static_cast<uint8_t>(raw >> 16u);
				data[2] = static_cast<uint8_t>(raw >> 8u);
				data[3] = static_cast<uint8_t>(raw);
			} else {
				data[0] = static_cast<uint8_t>(raw);
				data[1] = static_cast<uint8_t>(raw >> 8u);
				data[2] = static_cast<uint8_t>(raw >> 16u);
				data[3] = static_cast<uint8_t>(raw >> 24u);
			}
			break;
		}
		default: break;
	}
}

static void Ngs2ResetHeVagDecoder(Ngs2VoiceInternal* voice) {
	voice->hevag_channel_state     = {};
	voice->hevag_block_start_state = {};
	voice->hevag_samples           = {};
	voice->hevag_block             = std::numeric_limits<uint32_t>::max();
	voice->hevag_unit              = std::numeric_limits<uint32_t>::max();
}

static uint32_t Ngs2BlockSampleCount(const Ngs2WaveformFormat& format,
                                     const Ngs2WaveformBlock&  block) {
	if (format.num_channels == 0) {
		return 0;
	}
	uint64_t data_samples = 0;
	if (format.waveform_type == 0x1c) {
		const auto unit_size = static_cast<uint64_t>(16u) * format.num_channels;
		data_samples         = unit_size != 0 ? block.data_size / unit_size * 28u : 0;
	} else {
		const auto bytes_per_sample = Ngs2PcmBytesPerSample(format.waveform_type);
		const auto frame_size       = static_cast<uint64_t>(bytes_per_sample) * format.num_channels;
		data_samples                = frame_size != 0 ? block.data_size / frame_size : 0;
	}
	data_samples = std::min<uint64_t>(data_samples, std::numeric_limits<uint32_t>::max());
	return block.num_samples != 0 ? std::min(block.num_samples, static_cast<uint32_t>(data_samples))
	                              : static_cast<uint32_t>(data_samples);
}

static bool Ngs2ReadHeVagSample(Ngs2VoiceInternal* voice, uint32_t sample_index,
                                std::array<float, 8>* samples) {
	const auto channels = voice->waveform_format.num_channels;
	if (channels == 0 || channels > 8 ||
	    voice->current_waveform_block >= voice->num_waveform_blocks) {
		return false;
	}
	const auto& block     = voice->waveform_blocks[voice->current_waveform_block];
	const auto  unit_size = static_cast<size_t>(16u) * channels;
	const auto  unit      = sample_index / 28u;
	const auto  in_unit   = sample_index % 28u;
	if (unit_size == 0 || voice->frame_offset > block.data_size ||
	    unit >= (block.data_size - voice->frame_offset) / unit_size) {
		return false;
	}

	if (voice->hevag_block != voice->current_waveform_block) {
		const bool sequential = voice->hevag_block != std::numeric_limits<uint32_t>::max() &&
		                        voice->current_waveform_block == voice->hevag_block + 1u;
		if (!sequential) {
			voice->hevag_channel_state = {};
		}
		voice->hevag_block_start_state = voice->hevag_channel_state;
		voice->hevag_block             = voice->current_waveform_block;
		voice->hevag_unit              = std::numeric_limits<uint32_t>::max();
	} else if (voice->hevag_unit != std::numeric_limits<uint32_t>::max() &&
	           unit < voice->hevag_unit) {
		voice->hevag_channel_state = voice->hevag_block_start_state;
		voice->hevag_unit          = std::numeric_limits<uint32_t>::max();
	}
	uint32_t next_unit =
	    voice->hevag_unit == std::numeric_limits<uint32_t>::max() ? 0 : voice->hevag_unit + 1u;
	while (next_unit <= unit) {
		const auto byte_offset = static_cast<size_t>(block.data_offset) + voice->frame_offset +
		                         static_cast<size_t>(next_unit) * unit_size;
		for (uint32_t channel = 0; channel < channels; channel++) {
			Ajm::HeVagDecodeFrame(voice->waveform_data + byte_offset + channel * 16u,
			                      &voice->hevag_channel_state[channel],
			                      voice->hevag_samples[channel].data());
		}
		voice->hevag_unit = next_unit;
		voice->decoded_data_size += unit_size;
		next_unit++;
	}
	for (uint32_t channel = 0; channel < channels; channel++) {
		(*samples)[channel] = static_cast<float>(voice->hevag_samples[channel][in_unit]) / 32768.0f;
	}
	return true;
}

static bool Ngs2AdvanceVoiceBlock(Ngs2VoiceInternal* voice) {
	while (voice->current_waveform_block < voice->num_waveform_blocks) {
		const auto& block = voice->waveform_blocks[voice->current_waveform_block];
		if (voice->waveform_format.num_channels == 0 ||
		    (voice->waveform_format.waveform_type != 0x1c &&
		     Ngs2PcmBytesPerSample(voice->waveform_format.waveform_type) == 0)) {
			return false;
		}
		const auto block_samples    = Ngs2BlockSampleCount(voice->waveform_format, block);
		const auto skip_samples     = std::min(block.num_skip_samples, block_samples);
		const auto playable_samples = block_samples - skip_samples;
		if (playable_samples != 0 && voice->sample_position < playable_samples) {
			return true;
		}

		const bool infinite = block.num_repeats == std::numeric_limits<uint32_t>::max();
		const bool repeat =
		    !voice->exit_loop && (infinite || voice->current_repeat < block.num_repeats);
		if (repeat && playable_samples != 0) {
			if (!infinite) {
				voice->current_repeat++;
			}
			voice->sample_position = std::max(0.0, voice->sample_position - playable_samples);
			continue;
		}

		voice->current_waveform_block++;
		voice->current_repeat  = 0;
		voice->sample_position = 0.0;
		voice->exit_loop       = false;
	}
	return false;
}

} // namespace Ngs2

namespace Acm {

static bool acm_conv_format_valid(uint32_t format) {
	return format <= 1;
}

static bool acm_conv_block_size_valid(uint32_t size) {
	return size == 256 || size == 512 || size == 1024;
}

static size_t acm_conv_value_size(uint32_t format) {
	return format == 0 ? sizeof(float) : sizeof(uint16_t);
}

static size_t acm_conv_block_offset(const AcmConvReverbChannel& channel, uint32_t block,
                                    uint32_t block_size, uint32_t format) {
	if (channel.sizes == nullptr) {
		return static_cast<size_t>(block) * block_size * 2u * acm_conv_value_size(format);
	}
	size_t offset = 0;
	for (uint32_t i = 0; i < block; i++) {
		offset += channel.sizes[i].memory_size;
	}
	return offset;
}

static size_t acm_conv_runtime_values(const AcmConvReverbChannel& channel, uint32_t block,
                                      uint32_t block_size, uint32_t format) {
	if (channel.sizes == nullptr) {
		return static_cast<size_t>(block_size) * 2u;
	}
	return std::min(static_cast<size_t>(block_size) * 2u,
	                static_cast<size_t>(channel.sizes[block].runtime_size) /
	                    acm_conv_value_size(format));
}

static std::vector<std::complex<double>> acm_conv_read_spectrum(const AcmConvReverbChannel& channel,
                                                                uint32_t block, uint32_t block_size,
                                                                uint32_t format) {
	const auto         fft_size = static_cast<size_t>(block_size) * 2u;
	std::vector<float> packed(fft_size, 0.0f);
	const auto*        data   = static_cast<const uint8_t*>(channel.blocks) +
	                            acm_conv_block_offset(channel, block, block_size, format);
	const auto         values = acm_conv_runtime_values(channel, block, block_size, format);
	for (size_t i = 0; i < values; i++) {
		packed[i] = acm_read_value(data, i, static_cast<int>(format));
	}

	std::vector<std::complex<double>> spectrum(fft_size);
	spectrum[0]          = {packed[0], 0.0};
	spectrum[block_size] = {packed[1], 0.0};
	for (size_t k = 1; k < block_size; k++) {
		spectrum[k]            = {packed[k * 2u], packed[k * 2u + 1u]};
		spectrum[fft_size - k] = std::conj(spectrum[k]);
	}
	return spectrum;
}

static void acm_conv_write_spectrum(AcmConvReverbChannel* channel, uint32_t block,
                                    uint32_t block_size, uint32_t format,
                                    const std::vector<std::complex<double>>& spectrum) {
	auto* data = static_cast<uint8_t*>(channel->blocks) +
	             acm_conv_block_offset(*channel, block, block_size, format);
	acm_write_value(data, 0, static_cast<int>(format), static_cast<float>(spectrum[0].real()));
	acm_write_value(data, 1, static_cast<int>(format),
	                static_cast<float>(spectrum[block_size].real()));
	for (size_t k = 1; k < block_size; k++) {
		acm_write_value(data, k * 2u, static_cast<int>(format),
		                static_cast<float>(spectrum[k].real()));
		acm_write_value(data, k * 2u + 1u, static_cast<int>(format),
		                static_cast<float>(spectrum[k].imag()));
	}
}

static bool acm_conv_input_valid(const AcmConvReverbIn* input) {
	if (input == nullptr || !acm_conv_block_size_valid(input->block_size) ||
	    input->block_count == 0 || input->channel_count == 0 || input->channel_count > 64 ||
	    !acm_conv_format_valid(input->format) || input->channels == nullptr ||
	    input->pcm == nullptr || input->history_position >= input->block_count ||
	    input->history_count > input->block_count) {
		return false;
	}
	for (uint32_t channel = 0; channel < input->channel_count; channel++) {
		if (input->channels[channel] == nullptr || input->channels[channel]->blocks == nullptr ||
		    input->channels[channel]->sizes != nullptr || input->pcm[channel] == nullptr) {
			return false;
		}
	}
	return true;
}

static bool acm_conv_ir_out_valid(const AcmConvReverbIn& input, const AcmConvReverbIr* ir,
                                  const AcmConvReverbOut* output) {
	if (ir == nullptr || output == nullptr || ir->block_size != input.block_size ||
	    output->block_size != input.block_size || ir->block_count == 0 || ir->channel_count == 0 ||
	    ir->channel_count > 64 ||
	    output->channel_count != std::max(input.channel_count, ir->channel_count) ||
	    !acm_conv_format_valid(ir->format) || ir->channels == nullptr || output->temps == nullptr ||
	    output->pcm == nullptr ||
	    (input.channel_count != 1 && ir->channel_count != 1 &&
	     input.channel_count != ir->channel_count)) {
		return false;
	}
	for (uint32_t channel = 0; channel < ir->channel_count; channel++) {
		if (ir->channels[channel] == nullptr || ir->channels[channel]->blocks == nullptr) {
			return false;
		}
	}
	for (uint32_t channel = 0; channel < output->channel_count; channel++) {
		if (output->temps[channel] == nullptr || output->pcm[channel] == nullptr) {
			return false;
		}
	}
	return true;
}

static void acm_conv_store_input(AcmConvReverbIn* input) {
	const auto fft_size = static_cast<size_t>(input->block_size) * 2u;
	const auto position = input->history_count == 0
	                          ? input->history_position
	                          : (input->history_position + 1u) % input->block_count;
	for (uint32_t channel = 0; channel < input->channel_count; channel++) {
		std::vector<std::complex<double>> samples(fft_size);
		for (uint32_t sample = 0; sample < input->block_size; sample++) {
			samples[sample] = {input->pcm[channel][sample], 0.0};
		}
		acm_fft_transform(&samples, false);
		acm_conv_write_spectrum(input->channels[channel], position, input->block_size,
		                        input->format, samples);
	}
	input->history_position = position;
	input->history_count    = std::min(input->history_count + 1u, input->block_count);
}

static void acm_conv_render(const AcmConvReverbIn& input, const AcmConvReverbIr& ir, float gain,
                            AcmConvReverbOut* output) {
	const auto fft_size = static_cast<size_t>(input.block_size) * 2u;
	for (uint32_t output_channel = 0; output_channel < output->channel_count; output_channel++) {
		std::vector<std::complex<double>> sum(fft_size);
		const auto input_channel = input.channel_count == 1 ? 0u : output_channel;
		const auto ir_channel    = ir.channel_count == 1 ? 0u : output_channel;
		const auto blocks        = std::min(input.history_count, ir.block_count);
		for (uint32_t block = 0; block < blocks; block++) {
			const auto input_block =
			    (input.history_position + input.block_count - block) % input.block_count;
			auto input_spectrum = acm_conv_read_spectrum(
			    *input.channels[input_channel], input_block, input.block_size, input.format);
			auto ir_spectrum =
			    acm_conv_read_spectrum(*ir.channels[ir_channel], block, ir.block_size, ir.format);
			const auto block_gain = ir.block_gains != nullptr ? ir.block_gains[block] : 1.0f;
			for (size_t sample = 0; sample < fft_size; sample++) {
				sum[sample] +=
				    input_spectrum[sample] * ir_spectrum[sample] * static_cast<double>(block_gain);
			}
		}
		acm_fft_transform(&sum, true);
		for (uint32_t sample = 0; sample < input.block_size; sample++) {
			output->pcm[output_channel][sample] = output->temps[output_channel][sample] +
			                                      static_cast<float>(sum[sample].real()) * gain;
			output->temps[output_channel][sample] =
			    static_cast<float>(sum[input.block_size + sample].real()) * gain;
		}
	}
}

} // namespace Acm

namespace Ngs2 {

static void Ngs2MixVoice(Ngs2VoiceInternal* voice, std::vector<std::vector<float>>* mixes,
                         const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info,
                         uint32_t num_frames, uint32_t output_sample_rate) {
	if (voice->state != Ngs2VoicePlayState::Playing || voice->waveform_data == nullptr ||
	    num_frames == 0 || output_sample_rate == 0) {
		return;
	}
	const bool source_is_vag   = voice->waveform_format.waveform_type == 0x1c;
	const auto source_bytes    = Ngs2PcmBytesPerSample(voice->waveform_format.waveform_type);
	const auto source_channels = voice->waveform_format.num_channels;
	const auto source_rate     = voice->waveform_format.sample_rate;
	if ((!source_is_vag && source_bytes == 0) || source_channels == 0 || source_channels > 8 ||
	    source_rate == 0) {
		return;
	}
	const auto source_frame_size = static_cast<size_t>(source_bytes) * source_channels;
	const auto step              = static_cast<double>(source_rate) / output_sample_rate *
	                               static_cast<double>(voice->pitch_ratio);

	for (uint32_t frame = 0; frame < num_frames;) {
		if (!Ngs2AdvanceVoiceBlock(voice)) {
			const bool waiting_for_data =
			    voice->num_waveform_blocks == 0 ||
			    (voice->waveform_continuous &&
			     voice->current_waveform_block >= voice->num_waveform_blocks);
			if (!waiting_for_data) {
				voice->state = Ngs2VoicePlayState::Empty;
			}
			break;
		}
		const auto& block         = voice->waveform_blocks[voice->current_waveform_block];
		const auto  block_samples = Ngs2BlockSampleCount(voice->waveform_format, block);
		const auto  skip_samples  = std::min(block.num_skip_samples, block_samples);
		const auto  sample_index  = skip_samples + static_cast<uint32_t>(voice->sample_position);
		if (sample_index >= block_samples) {
			voice->sample_position = static_cast<double>(block_samples - skip_samples);
			continue;
		}
		std::array<float, 8> source_samples {};
		if (source_is_vag) {
			if (!Ngs2ReadHeVagSample(voice, sample_index, &source_samples)) {
				voice->state = Ngs2VoicePlayState::Empty;
				break;
			}
			for (uint32_t channel = 0; channel < source_channels; channel++) {
				source_samples[channel] *= voice->volume;
			}
		} else {
			const auto* source = voice->waveform_data + block.data_offset + voice->frame_offset +
			                     static_cast<size_t>(sample_index) * source_frame_size;
			for (uint32_t channel = 0; channel < source_channels; channel++) {
				source_samples[channel] =
				    Ngs2ReadPcmSample(source + static_cast<size_t>(channel) * source_bytes,
				                      voice->waveform_format.waveform_type) *
				    voice->volume;
			}
		}

		for (uint32_t output = 0; output < num_buffer_info; output++) {
			const auto output_channels = buffer_info[output].num_channels;
			auto&      mix             = (*mixes)[output];
			if (output_channels == 0 || mix.empty()) {
				continue;
			}
			for (uint32_t channel = 0; channel < output_channels; channel++) {
				float sample = 0.0f;
				if (source_channels == 1) {
					sample = source_samples[0];
				} else if (output_channels == 1) {
					for (uint32_t source_channel = 0; source_channel < source_channels;
					     source_channel++) {
						sample += source_samples[source_channel];
					}
					sample /= static_cast<float>(source_channels);
				} else if (channel < source_channels) {
					sample = source_samples[channel];
				}
				mix[static_cast<size_t>(frame) * output_channels + channel] += sample;
			}
		}

		voice->sample_position += step;
		voice->num_decoded_samples++;
		if (!source_is_vag) {
			voice->decoded_data_size += source_frame_size;
		}
		voice->waveform_user_data = block.user_data;
		frame++;
	}
}

int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info,
                                   uint32_t num_buffer_info) {
	static std::atomic_uint32_t render_log_count = 0;
	const auto log_index     = render_log_count.fetch_add(1, std::memory_order_relaxed);
	const bool log_this_call = (log_index < 16 || (log_index % 600) == 0);

	if (log_this_call) {
		PRINT_NAME();
		LOGF("\t call_count      = %" PRIu32 "\n", log_index + 1);
	}

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);
	EXIT_NOT_IMPLEMENTED(num_buffer_info == 0);

	auto* ngs = reinterpret_cast<Ngs2Internal*>(system_handle);

	Common::LockGuard lock(ngs->mutex);

	uint32_t                        num_frames = std::numeric_limits<uint32_t>::max();
	bool                            has_output = false;
	std::vector<std::vector<float>> mixes(num_buffer_info);
	for (uint32_t i = 0; i < num_buffer_info; i++) {
		if (buffer_info[i].buffer != nullptr && buffer_info[i].buffer_size != 0) {
			std::memset(buffer_info[i].buffer, 0, buffer_info[i].buffer_size);
			const auto bytes_per_sample = Ngs2PcmBytesPerSample(buffer_info[i].waveform_type);
			if ((buffer_info[i].waveform_type == 0x12 || buffer_info[i].waveform_type == 0x13 ||
			     buffer_info[i].waveform_type == 0x18 || buffer_info[i].waveform_type == 0x19) &&
			    bytes_per_sample != 0 && buffer_info[i].num_channels != 0) {
				const auto frames = static_cast<uint32_t>(
				    buffer_info[i].buffer_size /
				    (static_cast<size_t>(bytes_per_sample) * buffer_info[i].num_channels));
				num_frames = std::min(num_frames, frames);
				has_output = true;
			}
			if (log_this_call) {
				LOGF("\t buffer[%" PRIu32 "]: size=0x%016" PRIx64 ", waveform_type=0x%08" PRIx32
				     ", channels=%" PRIu32 "\n",
				     i, static_cast<uint64_t>(buffer_info[i].buffer_size),
				     buffer_info[i].waveform_type, buffer_info[i].num_channels);
			}
		}
	}
	if (!has_output) {
		num_frames = 0;
	} else if (ngs->option.num_grain_samples != 0) {
		num_frames = std::min(num_frames, ngs->option.num_grain_samples);
	}
	for (uint32_t i = 0; i < num_buffer_info; i++) {
		if (Ngs2PcmBytesPerSample(buffer_info[i].waveform_type) != 0 &&
		    buffer_info[i].num_channels != 0) {
			mixes[i].resize(static_cast<size_t>(num_frames) * buffer_info[i].num_channels);
		}
	}

	Common::LockGuard racks_lock(g_racks_mutex);
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next) {
		if (rack->ngs == ngs) {
			auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

			for (uint32_t i = 0; i < rack->option.common.max_voices; i++) {
				auto& voice = voices[i];
				Ngs2ApplyVoiceEvent(&voice);
				if (rack->type == Ngs2RackType::Sampler ||
				    rack->type == Ngs2RackType::CustomSampler) {
					Ngs2MixVoice(&voice, &mixes, buffer_info, num_buffer_info, num_frames,
					             ngs->option.sample_rate);
				}
			}
		}
	}

	for (uint32_t output = 0; output < num_buffer_info; output++) {
		const auto bytes_per_sample = Ngs2PcmBytesPerSample(buffer_info[output].waveform_type);
		const auto channels         = buffer_info[output].num_channels;
		if (bytes_per_sample == 0 || channels == 0 || mixes[output].empty()) {
			continue;
		}
		auto* data = static_cast<uint8_t*>(buffer_info[output].buffer);
		for (size_t sample = 0; sample < mixes[output].size(); sample++) {
			Ngs2WritePcmSample(data + sample * bytes_per_sample, buffer_info[output].waveform_type,
			                   mixes[output][sample]);
		}
	}

	ngs->render_count++;

	return OK;
}

constexpr int32_t NGS2_ERROR_INVALID_OUT_ADDRESS            = static_cast<int32_t>(0x804a8010u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_ADDRESS       = static_cast<int32_t>(0x804a8055u);
constexpr int32_t NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA       = static_cast<int32_t>(0x804a8056u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS = static_cast<int32_t>(0x804a8058u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_DATA          = static_cast<int32_t>(0x804a8430u);
constexpr int32_t NGS2_ERROR_INVALID_WAVEFORM_FORMAT        = static_cast<int32_t>(0x804a8431u);
constexpr int32_t NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT        = static_cast<int32_t>(0x804a8432u);
constexpr int32_t NGS2_ERROR_INVALID_PAN_UNIT_ANGLE         = static_cast<int32_t>(0x804a8450u);
constexpr int32_t NGS2_ERROR_INVALID_PAN_SPEAKER            = static_cast<int32_t>(0x804a8451u);
constexpr int32_t NGS2_ERROR_INVALID_PAN_MATRIX_FORMAT      = static_cast<int32_t>(0x804a8452u);
constexpr int32_t NGS2_ERROR_INVALID_PAN_WORK               = static_cast<int32_t>(0x804a8453u);
constexpr int32_t NGS2_ERROR_INVALID_PAN_PARAM              = static_cast<int32_t>(0x804a8454u);

static uint16_t Ngs2ReadLe16(const uint8_t* data) {
	return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8u);
}

static uint32_t Ngs2ReadLe32(const uint8_t* data) {
	return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
	       (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

static uint32_t Ngs2ReadBe32(const uint8_t* data) {
	return (static_cast<uint32_t>(data[0]) << 24u) | (static_cast<uint32_t>(data[1]) << 16u) |
	       (static_cast<uint32_t>(data[2]) << 8u) | static_cast<uint32_t>(data[3]);
}

static bool Ngs2FourCcEquals(const uint8_t* data, const char (&value)[5]) {
	return std::memcmp(data, value, 4) == 0;
}

static uint32_t Ngs2PcmWaveformType(uint16_t format, uint16_t bits) {
	if (format == 1) {
		switch (bits) {
			case 8: return 0x11;
			case 16: return 0x12;
			case 24: return 0x14;
			case 32: return 0x16;
			default: return 0;
		}
	}
	if (format == 3) {
		return bits == 32 ? 0x18u : (bits == 64 ? 0x1au : 0u);
	}
	return 0;
}

static uint32_t Ngs2WaveformBytesPerSample(uint32_t waveform_type) {
	switch (waveform_type) {
		case 0x10:
		case 0x11: return 1;
		case 0x12:
		case 0x13: return 2;
		case 0x14:
		case 0x15: return 3;
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19: return 4;
		case 0x1a:
		case 0x1b: return 8;
		default: return 0;
	}
}

static int32_t Ngs2ParseVagData(const uint8_t* bytes, size_t data_size, Ngs2WaveformInfo* info) {
	constexpr size_t VAG_HEADER_SIZE = 0x30;
	if (data_size < VAG_HEADER_SIZE) {
		return NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA;
	}
	const auto version     = Ngs2ReadBe32(bytes + 0x04);
	const auto stream_size = static_cast<size_t>(Ngs2ReadBe32(bytes + 0x0c));
	const auto sample_rate = Ngs2ReadBe32(bytes + 0x10);
	if (version != 0x00020001u && version != 0x00030000u) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}
	uint32_t   channels      = 1;
	const auto channel_field = Ngs2ReadBe32(bytes + 0x1c);
	if (Ngs2ReadBe32(bytes + 0x18) == 0 && (channel_field & 0xffff00ffu) == 0 && bytes[0x1e] < 16) {
		channels = bytes[0x1e] == 0 ? 1u : bytes[0x1e];
	}
	const auto unit_size = static_cast<size_t>(16u) * channels;
	if (sample_rate < 2000 || sample_rate > 192000 || channels > 8 || stream_size == 0 ||
	    stream_size > data_size - VAG_HEADER_SIZE || unit_size == 0 ||
	    stream_size % unit_size != 0) {
		return stream_size > data_size - VAG_HEADER_SIZE ? NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA
		                                                 : NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	auto     units     = static_cast<uint32_t>(stream_size / unit_size);
	uint32_t loop_unit = std::numeric_limits<uint32_t>::max();
	uint32_t loop_end  = std::numeric_limits<uint32_t>::max();
	for (uint32_t unit = 0; unit < units; unit++) {
		const auto flag =
		    bytes[VAG_HEADER_SIZE + static_cast<size_t>(unit) * unit_size + 1u] & 0x0fu;
		if (flag == 0x06 && loop_unit == std::numeric_limits<uint32_t>::max()) {
			loop_unit = unit;
		} else if (flag == 0x03 && loop_unit != std::numeric_limits<uint32_t>::max() &&
		           loop_end == std::numeric_limits<uint32_t>::max()) {
			loop_end = unit + 1u;
		} else if (flag == 0x07) {
			units = unit;
			break;
		}
	}
	if (units == 0) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	if (loop_unit >= units || loop_end <= loop_unit || loop_end > units) {
		loop_unit = std::numeric_limits<uint32_t>::max();
		loop_end  = std::numeric_limits<uint32_t>::max();
	}

	info->format.waveform_type     = 0x1c;
	info->format.num_channels      = channels;
	info->format.sample_rate       = sample_rate;
	info->format.config_data       = loop_unit != std::numeric_limits<uint32_t>::max() ? 1u : 0u;
	info->data_offset              = VAG_HEADER_SIZE;
	info->data_size                = static_cast<uint32_t>(stream_size);
	info->num_samples              = units * 28u;
	info->audio_unit_size          = static_cast<uint32_t>(unit_size);
	info->num_audio_unit_samples   = 28;
	info->num_audio_unit_per_frame = 1;
	info->audio_frame_size         = static_cast<uint32_t>(unit_size);
	info->num_audio_frame_samples  = 28;

	auto add_block = [&](uint32_t begin, uint32_t end, uint32_t repeats) {
		if (begin >= end || info->num_blocks >= std::size(info->blocks)) {
			return;
		}
		auto& block       = info->blocks[info->num_blocks++];
		block.data_offset = VAG_HEADER_SIZE + static_cast<uintptr_t>(begin) * unit_size;
		block.data_size   = static_cast<size_t>(end - begin) * unit_size;
		block.num_repeats = repeats;
		block.num_samples = (end - begin) * 28u;
	};
	if (loop_unit != std::numeric_limits<uint32_t>::max()) {
		info->loop_begin_position = loop_unit * 28u;
		info->loop_end_position   = loop_end * 28u;
		add_block(0, loop_unit, 0);
		add_block(loop_unit, loop_end, std::numeric_limits<uint32_t>::max());
		add_block(loop_end, units, 0);
	} else {
		add_block(0, units, 0);
	}
	return info->num_blocks != 0 ? OK : NGS2_ERROR_INVALID_WAVEFORM_DATA;
}

static int Ngs2ParseAtrac9Riff(const void* data, size_t data_size, Ngs2WaveformInfo* info) {
	static constexpr uint8_t ATRAC9_GUID[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
	                                            0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
	const auto*              bytes           = static_cast<const uint8_t*>(data);
	if (bytes == nullptr || data_size < 12) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	if (!Ngs2FourCcEquals(bytes, "RIFF") || !Ngs2FourCcEquals(bytes + 8, "WAVE")) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	const uint64_t riff_end64 = 8ull + Ngs2ReadLe32(bytes + 4);
	if (riff_end64 < 12 || riff_end64 > data_size) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	const auto riff_end = static_cast<size_t>(riff_end64);

	const uint8_t* format          = nullptr;
	const uint8_t* fact            = nullptr;
	size_t         waveform_offset = 0;
	uint32_t       waveform_size   = 0;

	for (size_t offset = 12; offset + 8 <= riff_end;) {
		const auto* chunk      = bytes + offset;
		const auto  chunk_size = static_cast<size_t>(Ngs2ReadLe32(chunk + 4));
		const auto  payload    = offset + 8;
		if (chunk_size > riff_end - payload) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}

		if (Ngs2FourCcEquals(chunk, "fmt ")) {
			if (chunk_size < 52) {
				return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
			}
			format = bytes + payload;
		} else if (Ngs2FourCcEquals(chunk, "fact")) {
			if (chunk_size < 12) {
				return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
			}
			fact = bytes + payload;
		} else if (Ngs2FourCcEquals(chunk, "data")) {
			if (payload > std::numeric_limits<uint32_t>::max()) {
				return NGS2_ERROR_INVALID_WAVEFORM_DATA;
			}
			waveform_offset = payload;
			waveform_size   = static_cast<uint32_t>(chunk_size);
		}

		const uint64_t next = static_cast<uint64_t>(payload) + chunk_size + (chunk_size & 1u);
		if (next > riff_end) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		offset = static_cast<size_t>(next);
	}

	if (format == nullptr || fact == nullptr || waveform_offset == 0) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}
	if (Ngs2ReadLe16(format) != 0xfffe ||
	    std::memcmp(format + 24, ATRAC9_GUID, sizeof(ATRAC9_GUID)) != 0) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}

	std::array<uint8_t, ATRAC9_CONFIG_DATA_SIZE> config {};
	std::memcpy(config.data(), format + 44, config.size());
	if (config[0] != 0xfe || (config[1] & 1u) != 0 || ((config[1] >> 1u) & 7u) >= 6u) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	Atrac9CodecInfo codec {};
	void*           decoder = Atrac9GetHandle();
	const bool valid_codec = decoder != nullptr && Atrac9InitDecoder(decoder, config.data()) == 0 &&
	                         Atrac9GetCodecInfo(decoder, &codec) == 0 && codec.channels > 0 &&
	                         codec.samplingRate > 0 && codec.superframeSize > 0 &&
	                         codec.framesInSuperframe > 0 && codec.frameSamples > 0 &&
	                         codec.superframeSize % codec.framesInSuperframe == 0;
	if (decoder != nullptr) {
		Atrac9ReleaseHandle(decoder);
	}
	const uint64_t frame_samples =
	    static_cast<uint64_t>(codec.frameSamples) * static_cast<uint64_t>(codec.framesInSuperframe);
	if (!valid_codec || codec.channels != Ngs2ReadLe16(format + 2) ||
	    codec.samplingRate != static_cast<int>(Ngs2ReadLe32(format + 4)) ||
	    codec.superframeSize != Ngs2ReadLe16(format + 12) ||
	    frame_samples != Ngs2ReadLe16(format + 18) ||
	    frame_samples > std::numeric_limits<uint32_t>::max()) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	info->format.waveform_type = NGS2_WAVEFORM_TYPE_ATRAC9;
	info->format.num_channels  = static_cast<uint32_t>(codec.channels);
	info->format.sample_rate   = static_cast<uint32_t>(codec.samplingRate);
	std::memcpy(&info->format.config_data, config.data(), config.size());
	info->data_offset     = static_cast<uint32_t>(waveform_offset);
	info->data_size       = waveform_size;
	info->num_samples     = Ngs2ReadLe32(fact);
	info->audio_unit_size = static_cast<uint32_t>(codec.superframeSize / codec.framesInSuperframe);
	info->num_audio_unit_samples     = static_cast<uint32_t>(codec.frameSamples);
	info->num_audio_unit_per_frame   = static_cast<uint32_t>(codec.framesInSuperframe);
	info->audio_frame_size           = static_cast<uint32_t>(codec.superframeSize);
	info->num_audio_frame_samples    = static_cast<uint32_t>(frame_samples);
	info->num_delay_samples          = Ngs2ReadLe32(fact + 4);
	info->num_blocks                 = 1;
	info->blocks[0].data_offset      = waveform_offset;
	info->blocks[0].data_size        = waveform_size;
	info->blocks[0].num_skip_samples = Ngs2ReadLe32(fact + 8);
	info->blocks[0].num_samples      = info->num_samples;
	return OK;
}

int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size,
                                        Ngs2WaveformInfo* info) {
	PRINT_NAME();
	LOGF("\t data = 0x%016" PRIx64 ", data_size = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(data), static_cast<uint64_t>(data_size));

	if (info == nullptr) {
		return NGS2_ERROR_INVALID_OUT_ADDRESS;
	}
	*info = {};
	if (data == nullptr) {
		return NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
	}
	if (data_size < 12) {
		return NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA;
	}

	const auto* bytes = static_cast<const uint8_t*>(data);
	if (Ngs2FourCcEquals(bytes, "VAGp")) {
		return Ngs2ParseVagData(bytes, data_size, info);
	}
	if (!Ngs2FourCcEquals(bytes, "RIFF") || !Ngs2FourCcEquals(bytes + 8, "WAVE")) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}
	const auto atrac9_result = Ngs2ParseAtrac9Riff(data, data_size, info);
	if (atrac9_result == OK) {
		return OK;
	}
	if (atrac9_result != NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT &&
	    atrac9_result != NGS2_ERROR_INVALID_WAVEFORM_FORMAT) {
		return atrac9_result;
	}
	*info = {};

	uint16_t format_tag      = 0;
	uint16_t channels        = 0;
	uint16_t bits_per_sample = 0;
	uint16_t block_align     = 0;
	uint32_t sample_rate     = 0;
	uint32_t data_offset     = 0;
	uint32_t pcm_data_size   = 0;
	uint32_t loop_begin      = 0;
	uint32_t loop_end        = 0;
	bool     has_format      = false;
	bool     has_data        = false;

	for (size_t offset = 12; offset + 8 <= data_size;) {
		const auto chunk_size = static_cast<size_t>(Ngs2ReadLe32(bytes + offset + 4));
		const auto payload    = offset + 8;
		if (chunk_size > data_size - payload) {
			return NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA;
		}
		if (Ngs2FourCcEquals(bytes + offset, "fmt ")) {
			if (chunk_size < 16) {
				return NGS2_ERROR_INVALID_WAVEFORM_DATA;
			}
			format_tag      = Ngs2ReadLe16(bytes + payload);
			channels        = Ngs2ReadLe16(bytes + payload + 2);
			sample_rate     = Ngs2ReadLe32(bytes + payload + 4);
			block_align     = Ngs2ReadLe16(bytes + payload + 12);
			bits_per_sample = Ngs2ReadLe16(bytes + payload + 14);
			if (format_tag == 0xfffe && chunk_size >= 40) {
				format_tag = Ngs2ReadLe16(bytes + payload + 24);
			}
			has_format = true;
		} else if (Ngs2FourCcEquals(bytes + offset, "data")) {
			if (payload > std::numeric_limits<uint32_t>::max() ||
			    chunk_size > std::numeric_limits<uint32_t>::max()) {
				return NGS2_ERROR_INVALID_WAVEFORM_DATA;
			}
			data_offset   = static_cast<uint32_t>(payload);
			pcm_data_size = static_cast<uint32_t>(chunk_size);
			has_data      = true;
		} else if (Ngs2FourCcEquals(bytes + offset, "smpl") && chunk_size >= 60 &&
		           Ngs2ReadLe32(bytes + payload + 28) != 0) {
			loop_begin               = Ngs2ReadLe32(bytes + payload + 44);
			const auto inclusive_end = Ngs2ReadLe32(bytes + payload + 48);
			loop_end = inclusive_end == std::numeric_limits<uint32_t>::max() ? inclusive_end
			                                                                 : inclusive_end + 1u;
		}
		const auto padded_size = chunk_size + (chunk_size & 1u);
		if (padded_size > data_size - payload) {
			break;
		}
		offset = payload + padded_size;
	}

	if (!has_format || !has_data) {
		return NGS2_ERROR_INCOMPLETE_WAVEFORM_DATA;
	}
	const auto waveform_type = Ngs2PcmWaveformType(format_tag, bits_per_sample);
	if (waveform_type == 0) {
		return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
	}
	const auto bytes_per_sample = Ngs2WaveformBytesPerSample(waveform_type);
	if (channels == 0 || sample_rate == 0 || sample_rate > 192000 || block_align == 0 ||
	    block_align != channels * bytes_per_sample) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}

	info->format.waveform_type     = waveform_type;
	info->format.num_channels      = channels;
	info->format.sample_rate       = sample_rate;
	info->data_offset              = data_offset;
	info->data_size                = pcm_data_size;
	info->num_samples              = pcm_data_size / block_align;
	info->loop_begin_position      = std::min(loop_begin, info->num_samples);
	info->loop_end_position        = std::min(loop_end, info->num_samples);
	info->audio_unit_size          = block_align;
	info->num_audio_unit_samples   = 1;
	info->num_audio_unit_per_frame = 1;
	info->audio_frame_size         = block_align;
	info->num_audio_frame_samples  = 1;
	info->num_delay_samples        = 0;
	info->num_blocks               = 1;
	info->blocks[0].data_offset    = data_offset;
	info->blocks[0].data_size      = pcm_data_size;
	info->blocks[0].num_samples    = info->num_samples;
	return OK;
}

int KYTY_SYSV_ABI Ngs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos,
                                        uint32_t num_samples, Ngs2WaveformBlock* block) {
	PRINT_NAME();
	LOGF("\t format = 0x%016" PRIx64 ", sample_pos = %" PRIu32 ", num_samples = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(format), sample_pos, num_samples);

	if (block == nullptr) {
		return NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS;
	}
	*block = {};
	if (format == nullptr) {
		return NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
	}
	const auto bytes_per_sample = Ngs2WaveformBytesPerSample(format->waveform_type);
	if (format->waveform_type == 0x1c) {
		if (format->num_channels == 0 || format->num_channels > 8) {
			return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
		}
		const auto unit_size  = static_cast<uint64_t>(16u) * format->num_channels;
		const auto first_unit = sample_pos / 28u;
		const auto skip       = sample_pos % 28u;
		const auto units      = (static_cast<uint64_t>(skip) + num_samples + 27u) / 28u;
		const auto offset     = static_cast<uint64_t>(first_unit) * unit_size;
		const auto size       = units * unit_size;
		if (offset > std::numeric_limits<uintptr_t>::max() ||
		    size > std::numeric_limits<size_t>::max()) {
			return NGS2_ERROR_INVALID_WAVEFORM_DATA;
		}
		block->data_offset      = static_cast<uintptr_t>(offset);
		block->data_size        = static_cast<size_t>(size);
		block->num_skip_samples = skip;
		block->num_samples      = num_samples + skip;
		return OK;
	}
	if (bytes_per_sample == 0 || format->num_channels == 0) {
		return NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
	}
	const auto frame_size = static_cast<uint64_t>(bytes_per_sample) * format->num_channels;
	const auto offset     = static_cast<uint64_t>(sample_pos) * frame_size;
	const auto size       = static_cast<uint64_t>(num_samples) * frame_size;
	if (offset > std::numeric_limits<uintptr_t>::max() ||
	    size > std::numeric_limits<size_t>::max()) {
		return NGS2_ERROR_INVALID_WAVEFORM_DATA;
	}
	block->data_offset = static_cast<uintptr_t>(offset);
	block->data_size   = static_cast<size_t>(size);
	block->num_samples = num_samples;
	return OK;
}

int KYTY_SYSV_ABI Ngs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle,
                              uint32_t num_speakers) {
	PRINT_NAME();
	LOGF("\t work = 0x%016" PRIx64 ", num_speakers = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(work), num_speakers);

	if (work == nullptr) {
		return NGS2_ERROR_INVALID_PAN_WORK;
	}
	if (!std::isfinite(unit_angle) || unit_angle <= 0.0f) {
		return NGS2_ERROR_INVALID_PAN_UNIT_ANGLE;
	}
	if (speaker_angles == nullptr || num_speakers == 0 || num_speakers > 8) {
		return NGS2_ERROR_INVALID_PAN_SPEAKER;
	}

	std::memset(work, 0, sizeof(Ngs2PanWork));
	work->unit_angle   = unit_angle;
	work->num_speakers = num_speakers;
	for (uint32_t i = 0; i < work->num_speakers; i++) {
		if (!std::isfinite(speaker_angles[i])) {
			return NGS2_ERROR_INVALID_PAN_SPEAKER;
		}
		work->speaker_angles[i] = speaker_angles[i];
	}
	return OK;
}

static float Ngs2WrapPanAngle(float value, float unit) {
	value = std::fmod(value, unit);
	return value < 0.0f ? value + unit : value;
}

static float Ngs2PanAngularDistance(float left, float right, float unit) {
	const auto distance = std::abs(Ngs2WrapPanAngle(left, unit) - Ngs2WrapPanAngle(right, unit));
	return std::min(distance, unit - distance);
}

int KYTY_SYSV_ABI Ngs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params,
                                         uint32_t num_params, uint32_t matrix_format,
                                         float* out_volume_matrix) {
	PRINT_NAME();
	LOGF("\t work = 0x%016" PRIx64 ", params = 0x%016" PRIx64 ", num_params = %" PRIu32
	     ", matrix_format = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(work), reinterpret_cast<uint64_t>(params), num_params,
	     matrix_format);

	if (work == nullptr || work->num_speakers == 0 || work->num_speakers > 8 ||
	    !std::isfinite(work->unit_angle) || work->unit_angle <= 0.0f) {
		return NGS2_ERROR_INVALID_PAN_WORK;
	}
	if (num_params != 0 && (params == nullptr || out_volume_matrix == nullptr)) {
		return NGS2_ERROR_INVALID_PAN_PARAM;
	}
	if (matrix_format != 1 && matrix_format != 2 && matrix_format != 6 && matrix_format != 8) {
		return NGS2_ERROR_INVALID_PAN_MATRIX_FORMAT;
	}

	const auto channels        = matrix_format;
	const auto speaker_count   = std::min(work->num_speakers, channels - (channels >= 6 ? 1u : 0u));
	const auto speaker_channel = [channels](uint32_t speaker) {
		return channels >= 6 && speaker >= 3 ? speaker + 1 : speaker;
	};
	for (uint32_t p = 0; p < num_params; p++) {
		auto* matrix = out_volume_matrix + p * channels;
		std::fill_n(matrix, channels, 0.0f);
		if (!std::isfinite(params[p].angle) || !std::isfinite(params[p].distance) ||
		    !std::isfinite(params[p].fbw_level) || !std::isfinite(params[p].lfe_level)) {
			return NGS2_ERROR_INVALID_PAN_PARAM;
		}
		if (channels == 1 || speaker_count == 1) {
			matrix[0] = 1.0f;
		} else {
			uint32_t nearest[2]   = {0, 1};
			float    distances[2] = {
			    Ngs2PanAngularDistance(params[p].angle, work->speaker_angles[0], work->unit_angle),
			    Ngs2PanAngularDistance(params[p].angle, work->speaker_angles[1], work->unit_angle)};
			if (distances[1] < distances[0]) {
				std::swap(distances[0], distances[1]);
				std::swap(nearest[0], nearest[1]);
			}
			for (uint32_t speaker = 2; speaker < speaker_count; speaker++) {
				const auto distance = Ngs2PanAngularDistance(
				    params[p].angle, work->speaker_angles[speaker], work->unit_angle);
				if (distance < distances[0]) {
					distances[1] = distances[0];
					nearest[1]   = nearest[0];
					distances[0] = distance;
					nearest[0]   = speaker;
				} else if (distance < distances[1]) {
					distances[1] = distance;
					nearest[1]   = speaker;
				}
			}
			const auto total                    = distances[0] + distances[1];
			const auto blend                    = total > 0.0f ? distances[0] / total : 0.0f;
			matrix[speaker_channel(nearest[0])] = std::cos(blend * std::acos(-1.0f) * 0.5f);
			matrix[speaker_channel(nearest[1])] = std::sin(blend * std::acos(-1.0f) * 0.5f);
		}
		if (channels >= 6) {
			matrix[3] = std::clamp(params[p].lfe_level, 0.0f, 1.0f);
		}
		const auto distance_gain = params[p].distance > 1.0f ? 1.0f / params[p].distance : 1.0f;
		for (uint32_t channel = 0; channel < channels; channel++) {
			if (channel != 3 || channels < 6) {
				matrix[channel] *= distance_gain;
			}
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(out_listener_param == nullptr);

	std::memset(out_listener_param, 0, sizeof(Ngs2GeomListenerParam));
	out_listener_param->orient_front.z = 1.0f;
	out_listener_param->orient_up.y    = 1.0f;
	out_listener_param->sound_speed    = 343.0f;

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(out_source_param == nullptr);

	std::memset(out_source_param, 0, sizeof(Ngs2GeomSourceParam));
	out_source_param->direction.z                = 1.0f;
	out_source_param->cone.inner_level           = 1.0f;
	out_source_param->cone.inner_angle           = 360.0f;
	out_source_param->cone.outer_level           = 1.0f;
	out_source_param->cone.outer_angle           = 360.0f;
	out_source_param->rolloff.model              = 0;
	out_source_param->rolloff.max_distance       = 1000000.0f;
	out_source_param->rolloff.rolloff_factor     = 1.0f;
	out_source_param->rolloff.reference_distance = 1.0f;
	out_source_param->doppler_factor             = 1.0f;
	out_source_param->fbw_level                  = 1.0f;
	out_source_param->lfe_level                  = 1.0f;
	out_source_param->max_level                  = 1.0f;
	out_source_param->min_level                  = 0.0f;
	out_source_param->num_speakers               = 2;
	out_source_param->matrix_format              = 2;

	return OK;
}

static float Ngs2GeomDot(const Ngs2GeomVector& left, const Ngs2GeomVector& right) {
	return left.x * right.x + left.y * right.y + left.z * right.z;
}

static Ngs2GeomVector Ngs2GeomCross(const Ngs2GeomVector& left, const Ngs2GeomVector& right) {
	return {left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z,
	        left.x * right.y - left.y * right.x};
}

static float Ngs2GeomLength(const Ngs2GeomVector& value) {
	return std::sqrt(std::max(0.0f, Ngs2GeomDot(value, value)));
}

static Ngs2GeomVector Ngs2GeomNormalize(const Ngs2GeomVector& value,
                                        const Ngs2GeomVector& fallback) {
	const auto length = Ngs2GeomLength(value);
	return length > 1.0e-6f ? Ngs2GeomVector {value.x / length, value.y / length, value.z / length}
	                        : fallback;
}

static Ngs2GeomVector Ngs2GeomTransformPoint(const Ngs2GeomListenerWork& listener,
                                             const Ngs2GeomVector&       point) {
	return {listener.matrix[0][0] * point.x + listener.matrix[0][1] * point.y +
	            listener.matrix[0][2] * point.z + listener.matrix[0][3],
	        listener.matrix[1][0] * point.x + listener.matrix[1][1] * point.y +
	            listener.matrix[1][2] * point.z + listener.matrix[1][3],
	        listener.matrix[2][0] * point.x + listener.matrix[2][1] * point.y +
	            listener.matrix[2][2] * point.z + listener.matrix[2][3]};
}

static Ngs2GeomVector Ngs2GeomTransformVector(const Ngs2GeomListenerWork& listener,
                                              const Ngs2GeomVector&       value) {
	return {listener.matrix[0][0] * value.x + listener.matrix[0][1] * value.y +
	            listener.matrix[0][2] * value.z,
	        listener.matrix[1][0] * value.x + listener.matrix[1][1] * value.y +
	            listener.matrix[1][2] * value.z,
	        listener.matrix[2][0] * value.x + listener.matrix[2][1] * value.y +
	            listener.matrix[2][2] * value.z};
}

static float Ngs2GeomDistanceLevel(const Ngs2GeomRolloff& rolloff, float distance) {
	const auto reference = std::max(rolloff.reference_distance, 1.0e-6f);
	const auto maximum   = std::max(rolloff.max_distance, reference);
	const auto factor    = std::max(rolloff.rolloff_factor, 0.0f);
	const auto clamped   = std::clamp(distance, reference, maximum);
	switch (rolloff.model) {
		case 0: return reference / std::max(reference + factor * (distance - reference), reference);
		case 1:
			return maximum > reference
			           ? 1.0f - factor * (distance - reference) / (maximum - reference)
			           : 1.0f;
		case 2: return std::pow(std::max(distance, reference) / reference, -factor);
		case 3: return reference / std::max(reference + factor * (clamped - reference), reference);
		case 4:
			return maximum > reference
			           ? 1.0f - factor * (clamped - reference) / (maximum - reference)
			           : 1.0f;
		case 5: return std::pow(clamped / reference, -factor);
		default: return 1.0f;
	}
}

int KYTY_SYSV_ABI Ngs2GeomCalcListener(const Ngs2GeomListenerParam* param,
                                       Ngs2GeomListenerWork* out_work, uint32_t flags) {
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);

	constexpr auto ERROR_INVALID_LISTENER = static_cast<int32_t>(0x804a8461u);
	constexpr auto ERROR_INVALID_FLAG     = static_cast<int32_t>(0x804a8463u);
	if (param == nullptr || out_work == nullptr) {
		return ERROR_INVALID_LISTENER;
	}
	if ((flags & ~1u) != 0) {
		return ERROR_INVALID_FLAG;
	}

	std::memset(out_work, 0, sizeof(Ngs2GeomListenerWork));
	const auto front   = Ngs2GeomNormalize(param->orient_front, {0.0f, 0.0f, 1.0f});
	const auto up_hint = Ngs2GeomNormalize(param->orient_up, {0.0f, 1.0f, 0.0f});
	const auto right   = Ngs2GeomNormalize((flags & 1u) != 0 ? Ngs2GeomCross(up_hint, front)
	                                                         : Ngs2GeomCross(front, up_hint),
	                                       {1.0f, 0.0f, 0.0f});
	const auto up      = Ngs2GeomNormalize((flags & 1u) != 0 ? Ngs2GeomCross(front, right)
	                                                         : Ngs2GeomCross(right, front),
	                                       {0.0f, 1.0f, 0.0f});
	const Ngs2GeomVector axes[3] = {right, up, front};
	for (uint32_t row = 0; row < 3; row++) {
		out_work->matrix[row][0] = axes[row].x;
		out_work->matrix[row][1] = axes[row].y;
		out_work->matrix[row][2] = axes[row].z;
		out_work->matrix[row][3] = -Ngs2GeomDot(axes[row], param->position);
	}
	out_work->matrix[3][3] = 1.0f;
	out_work->velocity     = param->velocity;
	out_work->sound_speed  = (param->sound_speed > 0.0f ? param->sound_speed : 343.0f);
	out_work->coordinate   = flags & 0x1u;

	return OK;
}

int KYTY_SYSV_ABI Ngs2GeomApply(const Ngs2GeomListenerWork* listener,
                                const Ngs2GeomSourceParam* source, Ngs2GeomAttribute* out_attrib,
                                uint32_t flags) {
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);

	constexpr auto ERROR_INVALID_SOURCE = static_cast<int32_t>(0x804a8462u);
	constexpr auto ERROR_INVALID_FLAG   = static_cast<int32_t>(0x804a8463u);
	constexpr auto ERROR_INVALID_CONE   = static_cast<int32_t>(0x804a8464u);
	if (listener == nullptr || source == nullptr || out_attrib == nullptr) {
		return ERROR_INVALID_SOURCE;
	}
	if ((flags & ~0x1fu) != 0) {
		return ERROR_INVALID_FLAG;
	}
	if (source->cone.inner_angle < 0.0f || source->cone.outer_angle < source->cone.inner_angle ||
	    source->cone.outer_angle > 360.0f) {
		return ERROR_INVALID_CONE;
	}

	std::memset(out_attrib, 0, sizeof(Ngs2GeomAttribute));
	const auto local_position = Ngs2GeomTransformPoint(*listener, source->position);
	const auto distance       = Ngs2GeomLength(local_position);
	const auto to_listener =
	    Ngs2GeomNormalize(Ngs2GeomVector {-local_position.x, -local_position.y, -local_position.z},
	                      {0.0f, 0.0f, -1.0f});
	const auto direction = Ngs2GeomNormalize(Ngs2GeomTransformVector(*listener, source->direction),
	                                         {0.0f, 0.0f, 1.0f});
	const auto cone_angle =
	    std::acos(std::clamp(Ngs2GeomDot(direction, to_listener), -1.0f, 1.0f)) *
	    (180.0f / std::acos(-1.0f));
	float      cone_level = source->cone.inner_level;
	const auto inner_half = source->cone.inner_angle * 0.5f;
	const auto outer_half = source->cone.outer_angle * 0.5f;
	if (cone_angle >= outer_half) {
		cone_level = source->cone.outer_level;
	} else if (cone_angle > inner_half && outer_half > inner_half) {
		const auto blend = (cone_angle - inner_half) / (outer_half - inner_half);
		cone_level       = source->cone.inner_level +
		                   (source->cone.outer_level - source->cone.inner_level) * blend;
	}
	const auto distance_level = std::max(0.0f, Ngs2GeomDistanceLevel(source->rolloff, distance));
	const auto level          = std::clamp(distance_level * cone_level, source->min_level,
	                                       std::max(source->min_level, source->max_level));

	out_attrib->pitch_ratio = 1.0f;
	if ((flags & (1u << 1u)) != 0 && distance > 1.0e-6f) {
		const Ngs2GeomVector radial {local_position.x / distance, local_position.y / distance,
		                             local_position.z / distance};
		const auto           listener_velocity = Ngs2GeomDot(listener->velocity, radial);
		const auto           source_velocity   = Ngs2GeomDot(source->velocity, radial);
		const auto           speed             = std::max(listener->sound_speed, 1.0f);
		const auto           factor            = std::max(source->doppler_factor, 0.0f);
		const auto           denominator       = speed - factor * source_velocity;
		if (std::abs(denominator) > 1.0e-6f) {
			out_attrib->pitch_ratio =
			    std::clamp((speed - factor * listener_velocity) / denominator, 0.125f, 8.0f);
		}
	}
	if ((flags & (1u << 3u)) != 0) {
		out_attrib->a3d_attrib.position = local_position;
		out_attrib->a3d_attrib.volume   = level;
	}

	if ((flags & (1u << 2u)) != 0) {
		const auto channels =
		    std::min<uint32_t>((source->matrix_format == 0 ? 2u : source->matrix_format), 8);
		const auto inputs     = std::clamp(source->num_speakers, 1u, 8u);
		const auto angle      = std::atan2(local_position.x, local_position.z);
		const auto right_gain = std::sin(angle * 0.5f + std::acos(-1.0f) * 0.25f);
		const auto left_gain  = std::cos(angle * 0.5f + std::acos(-1.0f) * 0.25f);
		for (uint32_t input = 0; input < inputs; input++) {
			if (channels == 1) {
				out_attrib->level[input * 8] = level;
			} else {
				out_attrib->level[input * 8]     = level * std::clamp(left_gain, 0.0f, 1.0f);
				out_attrib->level[input * 8 + 1] = level * std::clamp(right_gain, 0.0f, 1.0f);
				if (channels >= 6) {
					out_attrib->level[input * 8 + 3] =
					    level * std::clamp(source->lfe_level, 0.0f, 1.0f);
				}
			}
		}
	}
	return OK;
}

int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id,
                                         uintptr_t* handle) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(rack_handle == 0);

	LOGF("\t voice_id = %u\n", voice_id);

	auto* rack   = reinterpret_cast<Ngs2RackInternal*>(rack_handle);
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack_handle + sizeof(Ngs2RackInternal));

	if (voice_id >= rack->option.common.max_voices) {
		return static_cast<int32_t>(0x804a8351u);
	}

	EXIT_IF(voices[voice_id].rack != rack);

	*handle = reinterpret_cast<uintptr_t>(voices + voice_id);

	return OK;
}

static void Ngs2QueueWaveformBlocks(Ngs2VoiceInternal* voice, const Ngs2WaveformBlock* blocks,
                                    uint32_t num_blocks, uint32_t flags);

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int KYTY_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param_list == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	const auto* param = param_list;

	for (;;) {
		LOGF("\t id   = 0x%08" PRIx32 "\n"
		     "\t size = %" PRIu16 "\n"
		     "\t next = %" PRId16 "\n",
		     param->id, param->size, param->next);

		auto rack_id = param->id >> 16u;

		EXIT_NOT_IMPLEMENTED(((param->id >> 15u) & 0x1u) != 0);

		switch (rack_id) {
			case 0x0000: {
				auto cid = param->id & 0x7fffu;
				switch (cid) {
					case 0x0001: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceMatrixLevelsParam));
						const auto* ml = reinterpret_cast<const Ngs2VoiceMatrixLevelsParam*>(param);
						LOGF("\t matrix_id  = %u\n"
						     "\t num_levels = %u\n"
						     "\t levels     = 0x%016" PRIx64 "\n",
						     ml->matrix_id, ml->num_levels, reinterpret_cast<uint64_t>(ml->levels));
						break;
					}
					case 0x0002: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortVolumeParam));
						const auto* volume =
						    reinterpret_cast<const Ngs2VoicePortVolumeParam*>(param);
						if (volume->port == 0) {
							voice->volume = volume->level;
						}
						LOGF("\t port  = %u\n"
						     "\t level = %f\n",
						     volume->port, volume->level);
						break;
					}
					case 0x0003: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortMatrixParam));
						const auto* pm = reinterpret_cast<const Ngs2VoicePortMatrixParam*>(param);
						LOGF("\t port      = %u\n"
						     "\t matrix_id = %d\n",
						     pm->port, pm->matrix_id);
						break;
					}
					case 0x0004: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePortDelayParam));
						const auto* delay = reinterpret_cast<const Ngs2VoicePortDelayParam*>(param);
						LOGF("\t port        = %u\n"
						     "\t num_samples = %u\n",
						     delay->port, delay->num_samples);
						break;
					}
					case 0x0005: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoicePatchParam));
						const auto* patch = reinterpret_cast<const Ngs2VoicePatchParam*>(param);
						LOGF("\t connect->port          = %u\n"
						     "\t connect->dest_input_id = %u\n"
						     "\t connect->dest_handle   = 0x%016" PRIx64 "\n",
						     patch->port, patch->dest_input_id, patch->dest_handle);
						break;
					}
					case 0x0006: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceEventParam));
						const auto* event = reinterpret_cast<const Ngs2VoiceEventParam*>(param);
						switch (event->event_id) {
							case 0x0001: voice->event = Ngs2VoicePlayEvent::Play; break;
							case 0x0002: voice->event = Ngs2VoicePlayEvent::Stop; break;
							case 0x0004: voice->event = Ngs2VoicePlayEvent::StopImm; break;
							case 0x0008: voice->event = Ngs2VoicePlayEvent::Kill; break;
							case 0x0010: voice->event = Ngs2VoicePlayEvent::Pause; break;
							case 0x0020: voice->event = Ngs2VoicePlayEvent::Resume; break;
							default: EXIT("unknown event_id: 0x%08" PRIx32 "\n", event->event_id);
						}
						LOGF("\t event = %u\n", event->event_id);
						break;
					}
					case 0x0007: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2VoiceCallbackParam));
						const auto* callback =
						    reinterpret_cast<const Ngs2VoiceCallbackParam*>(param);
						voice->callback       = callback->callback;
						voice->callback_data  = callback->callback_data;
						voice->callback_flags = callback->flags;
						LOGF("\t callback      = 0x%016" PRIx64 "\n"
						     "\t callback_data = 0x%016" PRIx64 "\n"
						     "\t flags         = 0x%08" PRIx32 "\n",
						     static_cast<uint64_t>(voice->callback),
						     static_cast<uint64_t>(voice->callback_data), voice->callback_flags);
						break;
					}
					default: EXIT("unknown id: 0x%04" PRIx32 "\n", cid);
				}
				break;
			}
			case 0x1000:
			case 0x4001: {
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Sampler &&
				                     voice->rack->type != Ngs2RackType::CustomSampler);
				const auto cid = param->id & 0xffffu;
				switch (cid) {
					case 0x0000: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2SamplerVoiceSetupParam));
						const auto* setup =
						    reinterpret_cast<const Ngs2SamplerVoiceSetupParam*>(param);
						voice->waveform_format        = setup->format;
						voice->waveform_flags         = setup->flags;
						voice->waveform_continuous    = false;
						voice->current_waveform_block = 0;
						voice->current_repeat         = 0;
						voice->sample_position        = 0.0;
						voice->exit_loop              = false;
						voice->num_decoded_samples    = 0;
						voice->decoded_data_size      = 0;
						Ngs2ResetHeVagDecoder(voice);
						LOGF("\t waveform_type = 0x%08" PRIx32 "\n"
						     "\t channels      = %" PRIu32 "\n"
						     "\t sample_rate   = %" PRIu32 "\n"
						     "\t config_data   = 0x%08" PRIx32 "\n"
						     "\t frame_margin  = %" PRIu32 "\n"
						     "\t frame_offset  = %" PRIu32 "\n"
						     "\t flags         = 0x%08" PRIx32 "\n",
						     setup->format.waveform_type, setup->format.num_channels,
						     setup->format.sample_rate, setup->format.config_data,
						     setup->format.frame_margin, setup->format.frame_offset, setup->flags);
						break;
					}
					case 0x0001: {
						EXIT_NOT_IMPLEMENTED(param->size !=
						                     sizeof(Ngs2SamplerVoiceWaveformBlocksParam));
						const auto* blocks =
						    reinterpret_cast<const Ngs2SamplerVoiceWaveformBlocksParam*>(param);
						voice->waveform_data = static_cast<const uint8_t*>(blocks->data);
						Ngs2QueueWaveformBlocks(voice, blocks->blocks, blocks->num_blocks,
						                        blocks->flags);
						LOGF("\t data       = 0x%016" PRIx64 "\n"
						     "\t flags      = 0x%08" PRIx32 "\n"
						     "\t num_blocks = %" PRIu32 "\n",
						     reinterpret_cast<uint64_t>(blocks->data), blocks->flags,
						     blocks->num_blocks);
						for (uint32_t i = 0; i < blocks->num_blocks && blocks->blocks != nullptr;
						     i++) {
							LOGF("\t block[%" PRIu32 "]: offset=0x%016" PRIx64
							     ", size=0x%016" PRIx64 ", repeats=%" PRIu32 ", skip=%" PRIu32
							     ", samples=%" PRIu32 "\n",
							     i, static_cast<uint64_t>(blocks->blocks[i].data_offset),
							     static_cast<uint64_t>(blocks->blocks[i].data_size),
							     blocks->blocks[i].num_repeats, blocks->blocks[i].num_skip_samples,
							     blocks->blocks[i].num_samples);
						}
						break;
					}
					case 0x0002: {
						EXIT_NOT_IMPLEMENTED(param->size !=
						                     sizeof(Ngs2SamplerVoiceWaveformAddressParam));
						const auto* address =
						    reinterpret_cast<const Ngs2SamplerVoiceWaveformAddressParam*>(param);
						if (voice->waveform_data == address->from) {
							voice->waveform_data = static_cast<const uint8_t*>(address->to);
						}
						break;
					}
					case 0x0003: {
						EXIT_NOT_IMPLEMENTED(param->size !=
						                     sizeof(Ngs2SamplerVoiceWaveformFrameOffsetParam));
						const auto* offset =
						    reinterpret_cast<const Ngs2SamplerVoiceWaveformFrameOffsetParam*>(
						        param);
						voice->frame_offset = offset->frame_offset;
						break;
					}
					case 0x0004: voice->exit_loop = true; break;
					case 0x0005: {
						EXIT_NOT_IMPLEMENTED(param->size != sizeof(Ngs2SamplerVoicePitchParam));
						const auto* pitch =
						    reinterpret_cast<const Ngs2SamplerVoicePitchParam*>(param);
						voice->pitch_ratio = std::max(0.0f, pitch->ratio);
						LOGF("\t pitch_ratio = %f\n", voice->pitch_ratio);
						break;
					}
					default: break;
				}
				break;
			}
			case 0x2000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Submixer); break;
			case 0x2001: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Reverb); break;
			case 0x3000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Mastering); break;
			case 0x4000: {
				EXIT_NOT_IMPLEMENTED(!Ngs2RackIsCustom(voice->rack->type));
				auto cid       = param->id & 0xffffu;
				auto module_id = (cid >> 8u) & 0xffu;
				auto ctl_id    = (cid >> 5u) & 0x7u;
				auto module_no = cid & 0x1fu;
				LOGF("\t custom module_id = 0x%02" PRIx32 ", ctl_id = 0x%" PRIx32
				     ", module_no = %" PRIu32 "\n",
				     module_id, ctl_id, module_no);
				break;
			}
			case 0x4002:
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSubmixer);
				break;
			case 0x4003:
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomMastering);
				break;
			default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
		}

		if (param->next == 0) {
			break;
		}
		param = reinterpret_cast<const Ngs2VoiceParamHeader*>(reinterpret_cast<uintptr_t>(param) +
		                                                      param->next);
	}

	return OK;
}

static void Ngs2QueueWaveformBlocks(Ngs2VoiceInternal* voice, const Ngs2WaveformBlock* blocks,
                                    uint32_t num_blocks, uint32_t flags) {
	if ((flags & (0x4u | 0x8u)) != 0) {
		voice->num_waveform_blocks    = 0;
		voice->current_waveform_block = 0;
		voice->current_repeat         = 0;
		voice->sample_position        = 0.0;
		voice->waveform_continuous    = false;
		Ngs2ResetHeVagDecoder(voice);
	} else if (voice->current_waveform_block != 0) {
		const auto remaining = voice->num_waveform_blocks - voice->current_waveform_block;
		std::move(voice->waveform_blocks.begin() + voice->current_waveform_block,
		          voice->waveform_blocks.begin() + voice->num_waveform_blocks,
		          voice->waveform_blocks.begin());
		voice->num_waveform_blocks    = remaining;
		voice->current_waveform_block = 0;
	}
	if (blocks == nullptr || num_blocks == 0) {
		return;
	}
	if ((flags & 0x1u) != 0) {
		voice->waveform_continuous = true;
	}
	const auto available =
	    static_cast<uint32_t>(voice->waveform_blocks.size()) - voice->num_waveform_blocks;
	const auto count = std::min(num_blocks, available);
	std::copy_n(blocks, count, voice->waveform_blocks.begin() + voice->num_waveform_blocks);
	voice->num_waveform_blocks += count;
}

static void Ngs2SetVoiceEvent(Ngs2VoiceInternal* voice, uint32_t event_id) {
	switch (event_id) {
		case 0x0001: voice->event = Ngs2VoicePlayEvent::Play; break;
		case 0x0002: voice->event = Ngs2VoicePlayEvent::Stop; break;
		case 0x0004: voice->event = Ngs2VoicePlayEvent::StopImm; break;
		case 0x0008: voice->event = Ngs2VoicePlayEvent::Kill; break;
		case 0x0010: voice->event = Ngs2VoicePlayEvent::Pause; break;
		case 0x0020: voice->event = Ngs2VoicePlayEvent::Resume; break;
		default: break;
	}
}

int KYTY_SYSV_ABI Ngs2VoiceRunCommands(uintptr_t voice_handle, const void* commands,
                                       size_t num_commands) {
	static std::atomic_uint32_t command_log_count = 0;

	if (voice_handle == 0 || (commands == nullptr && num_commands != 0)) {
		return static_cast<int32_t>(0x804a8010u);
	}
	if (num_commands > 65536) {
		return static_cast<int32_t>(0x804a8011u);
	}

	auto*             voice  = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);
	const auto*       params = static_cast<const Ngs2CommandParam*>(commands);
	Common::LockGuard lock(voice->rack->ngs->mutex);

	for (size_t index = 0; index < num_commands; index++) {
		const auto& param        = params[index];
		const auto  unit_id      = param.param_id >> 24u;
		const auto  component_id = (param.param_id >> 16u) & 0xffu;
		const auto  type         = (param.param_id >> 14u) & 0x3u;
		const auto  interface_id = param.param_id & 0x3fffu;
		const auto  log_index    = command_log_count.fetch_add(1, std::memory_order_relaxed);
		if (log_index < 4096) {
			LOGF("\t command[%" PRIu64 "]: id=0x%08" PRIx32 ", component=0x%02" PRIx32
			     ", interface=0x%04" PRIx32 ", unit=%" PRIu32 ", type=%" PRIu32
			     ", value_type=0x%02" PRIx8 ", values=%" PRIu16 ", flags=0x%02" PRIx8
			     ", value=0x%016" PRIx64 "\n",
			     static_cast<uint64_t>(index), param.param_id, component_id, interface_id, unit_id,
			     type, param.value_type, param.num_values, param.flags, param.value.w);
		}
		if (type != 0) {
			continue;
		}

		if (component_id == 0x00) {
			switch (interface_id) {
				case 0x0001: {
					const auto* setup =
					    static_cast<const Ngs2VoiceSetupCommandParam*>(param.value.p);
					if (setup != nullptr && log_index < 4096) {
						LOGF("\t voice setup: channels=%" PRIu32 ", flags=0x%08" PRIx32 "\n",
						     setup->num_channels, setup->flags);
					}
					break;
				}
				case 0x0002:
					Ngs2SetVoiceEvent(voice, param.value.u);
					if (log_index < 4096) {
						LOGF("\t voice event: 0x%08" PRIx32 "\n", param.value.u);
					}
					break;
				case 0x0005:
					if (param.value.p != nullptr && param.num_values != 0) {
						const auto* levels  = static_cast<const float*>(param.value.p);
						float       maximum = 0.0f;
						for (uint32_t i = 0; i < param.num_values; i++) {
							maximum = std::max(maximum, std::abs(levels[i]));
						}
						voice->volume = maximum;
					}
					break;
				case 0x0006: voice->volume = param.value.f; break;
				default: break;
			}
			continue;
		}

		if (component_id == 0x01) {
			switch (interface_id) {
				case 0x0000: {
					const auto* format = static_cast<const Ngs2WaveformFormat*>(param.value.p);
					if (format != nullptr) {
						voice->waveform_format = *format;
						Ngs2ResetVoicePlayback(voice);
						if (log_index < 4096) {
							LOGF("\t player format: type=0x%08" PRIx32 ", channels=%" PRIu32
							     ", rate=%" PRIu32 ", config=0x%08" PRIx32 "\n",
							     format->waveform_type, format->num_channels, format->sample_rate,
							     format->config_data);
						}
					}
					break;
				}
				case 0x0001: {
					const auto* blocks = static_cast<const Ngs2WaveformBlock*>(param.value.p);
					Ngs2QueueWaveformBlocks(voice, blocks, param.num_values, param.flags);
					if (log_index < 4096) {
						LOGF("\t player blocks: count=%" PRIu16 ", flags=0x%02" PRIx8 "\n",
						     param.num_values, param.flags);
						for (uint32_t i = 0; i < param.num_values && blocks != nullptr; i++) {
							LOGF("\t block[%" PRIu32 "]: offset=0x%016" PRIx64
							     ", size=0x%016" PRIx64 ", repeats=%" PRIu32 ", skip=%" PRIu32
							     ", samples=%" PRIu32 "\n",
							     i, static_cast<uint64_t>(blocks[i].data_offset),
							     static_cast<uint64_t>(blocks[i].data_size), blocks[i].num_repeats,
							     blocks[i].num_skip_samples, blocks[i].num_samples);
						}
					}
					break;
				}
				case 0x0002:
					voice->waveform_data = static_cast<const uint8_t*>(param.value.p);
					if (log_index < 4096) {
						LOGF("\t player data: 0x%016" PRIx64 "\n",
						     reinterpret_cast<uint64_t>(voice->waveform_data));
					}
					break;
				case 0x0003: voice->pitch_ratio = std::max(0.0f, param.value.f); break;
				case 0x0004: voice->exit_loop = true; break;
				case 0x0006: {
					const auto* relocate =
					    static_cast<const Ngs2WaveformRelocateParam*>(param.value.p);
					if (relocate != nullptr && voice->waveform_data == relocate->from) {
						voice->waveform_data = static_cast<const uint8_t*>(relocate->to);
					}
					break;
				}
				default: break;
			}
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state,
                                    size_t state_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	switch (voice->rack->type) {
		case Ngs2RackType::Submixer: {
			EXIT_NOT_IMPLEMENTED(state_size != sizeof(Ngs2SubmixerVoiceState));
			auto* submixer                    = reinterpret_cast<Ngs2SubmixerVoiceState*>(state);
			*submixer                         = {};
			submixer->voice_state.state_flags = Ngs2GetStateFlags(voice);
			LOGF("\t state_flags = %u\n", submixer->voice_state.state_flags);
			break;
		}
		case Ngs2RackType::CustomSubmixer: {
			const auto configured_size =
			    voice->rack->option.custom_submixer.custom_rack_option.state_size;
			EXIT_NOT_IMPLEMENTED(configured_size < sizeof(Ngs2CustomSubmixerVoiceState));
			EXIT_NOT_IMPLEMENTED(state_size != configured_size);
			std::memset(state, 0, state_size);
			state->state_flags = Ngs2GetStateFlags(voice);
			LOGF("\t state_flags = %u\n", state->state_flags);
			break;
		}
		case Ngs2RackType::CustomMastering: {
			const auto configured_size =
			    voice->rack->option.custom_mastering.custom_rack_option.state_size;
			EXIT_NOT_IMPLEMENTED(configured_size < sizeof(Ngs2CustomMasteringVoiceState));
			EXIT_NOT_IMPLEMENTED(state_size != configured_size);
			std::memset(state, 0, state_size);
			auto* mastering = reinterpret_cast<Ngs2CustomMasteringVoiceState*>(state);
			mastering->voice_state.state_flags = Ngs2GetStateFlags(voice);
			LOGF("\t state_flags = %u\n", mastering->voice_state.state_flags);
			break;
		}
		case Ngs2RackType::CustomSampler: {
			const auto configured_size =
			    voice->rack->option.custom_sampler.custom_rack_option.state_size;
			EXIT_NOT_IMPLEMENTED(configured_size < sizeof(Ngs2CustomSamplerVoiceState));
			EXIT_NOT_IMPLEMENTED(state_size != configured_size);
			std::memset(state, 0, state_size);
			auto* sampler = reinterpret_cast<Ngs2CustomSamplerVoiceState*>(state);
			sampler->voice_state.state_flags = Ngs2GetStateFlags(voice);
			sampler->waveform_data           = voice->waveform_data;
			sampler->num_decoded_samples     = voice->num_decoded_samples;
			sampler->decoded_data_size       = voice->decoded_data_size;
			sampler->user_data               = voice->waveform_user_data;
			LOGF("\t state_flags = %u\n", sampler->voice_state.state_flags);
			break;
		}
		case Ngs2RackType::Sampler: {
			if (state_size != sizeof(Ngs2SamplerVoiceState)) {
				LOGF("\t warning: sampler state_size = 0x%016" PRIx64 ", expected 0x%016" PRIx64
				     "\n",
				     static_cast<uint64_t>(state_size),
				     static_cast<uint64_t>(sizeof(Ngs2SamplerVoiceState)));
			}
			std::memset(state, 0, state_size);

			state->state_flags = Ngs2GetStateFlags(voice);
			if (state_size < sizeof(Ngs2SamplerVoiceState)) {
				LOGF("\t state_flags = %u\n", state->state_flags);
				break;
			}

			auto* sampler                = reinterpret_cast<Ngs2SamplerVoiceState*>(state);
			sampler->envelope_height     = 1.0f;
			sampler->peak_height         = 0.0f;
			sampler->reserved            = 0;
			sampler->num_decoded_samples = voice->num_decoded_samples;
			sampler->decoded_data_size   = voice->decoded_data_size;
			sampler->user_data           = voice->waveform_user_data;
			sampler->waveform_data       = voice->waveform_data;
			LOGF("\t state_flags = %u\n", sampler->voice_state.state_flags);
			break;
		}
		default: EXIT("unknown type: %s\n", Common::EnumName(voice->rack->type).c_str());
	}

	return OK;
}

int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state_flags == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);

	auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(voice_handle);

	Common::LockGuard lock(voice->rack->ngs->mutex);

	*state_flags = Ngs2GetStateFlags(voice);

	LOGF("\t state_flags = %u\n", *state_flags);

	return OK;
}

} // namespace Ngs2

} // namespace Libs::Audio
