#include "common/abi.h"
#include "libs/audio.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <cstring>

namespace Libs {

namespace LibAudioPropagation {

LIB_VERSION("AudioPropagation", 1, "AudioPropagation", 1, 0);

using AudioPropagationHandle = uint64_t;

struct AudioPropagationStructDescriptor {
	uint32_t id;
	size_t   size;
};

struct AudioPropagationSystemMemory {
	AudioPropagationStructDescriptor desc;
	void*                            p_cpu_mem;
	size_t                           size_cpu_mem;
	void*                            p_gpu_mem;
	size_t                           size_gpu_mem;
};

static AudioPropagationHandle AudioPropagationNextHandle() {
	static std::atomic_uint64_t next_handle {1};
	return next_handle.fetch_add(1, std::memory_order_relaxed);
}

static int32_t KYTY_SYSV_ABI AudioPropagationSystemQueryMemory(
    const void* /*options*/, AudioPropagationSystemMemory* out_memory) {
	PRINT_NAME();

	if (out_memory != nullptr) {
		out_memory->p_cpu_mem    = nullptr;
		out_memory->size_cpu_mem = 0;
		out_memory->p_gpu_mem    = nullptr;
		out_memory->size_gpu_mem = 0;
	}

	return 0;
}

static int32_t KYTY_SYSV_ABI
AudioPropagationSystemCreate(const void* /*options*/, AudioPropagationSystemMemory* /*memory*/,
                             AudioPropagationHandle* out_system_handle) {
	PRINT_NAME();

	if (out_system_handle != nullptr) {
		*out_system_handle = AudioPropagationNextHandle();
	}

	return 0;
}

static int32_t KYTY_SYSV_ABI AudioPropagationRoomCreate(AudioPropagationHandle /*system_handle*/,
                                                        AudioPropagationHandle* out_room_handle) {
	PRINT_NAME();

	if (out_room_handle != nullptr) {
		*out_room_handle = AudioPropagationNextHandle();
	}

	return 0;
}

static int32_t KYTY_SYSV_ABI AudioPropagationSystemRegisterMaterial(
    AudioPropagationHandle /*system_handle*/, const void* /*material*/,
    AudioPropagationHandle* out_material_handle) {
	PRINT_NAME();

	if (out_material_handle != nullptr) {
		*out_material_handle = AudioPropagationNextHandle();
	}

	return 0;
}

static int32_t KYTY_SYSV_ABI
AudioPropagationSystemSetAttributes(AudioPropagationHandle /*system_handle*/,
                                    const void* /*attributes*/, uint32_t /*num_attributes*/) {
	PRINT_NAME();

	return 0;
}

static int32_t KYTY_SYSV_ABI AudioPropagationSystemGetRays(AudioPropagationHandle /*system_handle*/,
                                                           void* /*rays*/, uint32_t* num_rays) {
	PRINT_NAME();

	if (num_rays != nullptr) {
		*num_rays = 0;
	}

	return 0;
}

LIB_DEFINE(InitAudio_1_AudioPropagation) {
	LIB_FUNC("7xyAxrusLko", LibAudioPropagation::AudioPropagationSystemQueryMemory);
	LIB_FUNC("aNEqtSHdUSo", LibAudioPropagation::AudioPropagationSystemCreate);
	LIB_FUNC("8bI5h8req30", LibAudioPropagation::AudioPropagationRoomCreate);
	LIB_FUNC("CPLV6G-eXmk", LibAudioPropagation::AudioPropagationSystemRegisterMaterial);
	LIB_FUNC("kIdb+iQUzCs", LibAudioPropagation::AudioPropagationSystemSetAttributes);
	LIB_FUNC("ht-QXT3zGxo", LibAudioPropagation::AudioPropagationSystemGetRays);
}

} // namespace LibAudioPropagation

namespace LibAudioOut {

LIB_VERSION("AudioOut", 1, "AudioOut", 1, 1);

namespace AudioOut = Audio::AudioOut;

LIB_DEFINE(InitAudio_1_AudioOut) {
	LIB_FUNC("JfEPXVxhFqA", AudioOut::AudioOutInit);
	LIB_FUNC("ekNvsT22rsY", AudioOut::AudioOutOpen);
	LIB_FUNC("b+uAV89IlxE", AudioOut::AudioOutSetVolume);
	LIB_FUNC("w3PdaSTSwGE", AudioOut::AudioOutOutputs);
	LIB_FUNC("QOQtbeDqsT4", AudioOut::AudioOutOutput);
	LIB_FUNC("s1--uE9mBFw", AudioOut::AudioOutClose);
	LIB_FUNC("GrQ9s4IrNaQ", AudioOut::AudioOutGetPortState);
}

} // namespace LibAudioOut

namespace LibAudioOut2 {

LIB_VERSION("AudioOut2", 1, "AudioOut", 1, 1);

namespace AudioOut2 = Audio::AudioOut2;

LIB_DEFINE(InitAudio_1_AudioOut2) {
	LIB_FUNC("g2tViFIohHE", AudioOut2::AudioOut2Initialize);
	LIB_FUNC("pDmme7Bgm6E", AudioOut2::AudioOut2ContextQueryMemory);
	LIB_FUNC("t5YrizufpQc", AudioOut2::AudioOut2ContextResetParam);
	LIB_FUNC("0x6o1VVAYSY", AudioOut2::AudioOut2ContextCreate);
	LIB_FUNC("on6ZH7Abo10", AudioOut2::AudioOut2ContextDestroy);
	LIB_FUNC("4dq2rblWlg0", AudioOut2::AudioOut2ContextSetAttributes);
	LIB_FUNC("PE2zHMqLSHs", AudioOut2::AudioOut2ContextAdvance);
	LIB_FUNC("aII9h5nli9U", AudioOut2::AudioOut2ContextPush);
	LIB_FUNC("R7d0F1g2qsU", AudioOut2::AudioOut2ContextGetQueueLevel);
	LIB_FUNC("JK2wamZPzwM", AudioOut2::AudioOut2PortCreate);
	LIB_FUNC("cd+Rtw+D1x8", AudioOut2::AudioOut2PortDestroy);
	LIB_FUNC("8XTArSPyWHk", AudioOut2::AudioOut2PortSetAttributes);
	LIB_FUNC("gatEUKG+Ea4", AudioOut2::AudioOut2PortGetState);
	LIB_FUNC("bkBN+CMLwRc", AudioOut2::AudioOut2GetSystemState);
	LIB_FUNC("xywYcRB7nbQ", AudioOut2::AudioOut2UserCreate);
	LIB_FUNC("IaZXJ9M79uo", AudioOut2::AudioOut2UserDestroy);
	LIB_FUNC("G1YOKDJYX2Y", AudioOut2::AudioOut2GetSpeakerArrayMemorySize);
	LIB_FUNC("+k91hoTuoA8", AudioOut2::AudioOut2SpeakerArrayCreate);
	LIB_FUNC("erCWQR5eKiQ", AudioOut2::AudioOut2SpeakerArrayDestroy);
	LIB_FUNC("4BlZurolOAo", AudioOut2::AudioOut2GetSpeakerArrayCoefficients);
	LIB_FUNC("28QqMnuuJ9Y", AudioOut2::AudioOut2GetSpeakerArrayAmbisonicsCoefficients);
	LIB_FUNC("DImz2Ft9E2g", AudioOut2::AudioOut2GetSpeakerInfo);
	LIB_FUNC("b1HWDUC8zaE", AudioOut2::AudioOut2SetSystemDebugState);
	LIB_FUNC("TViD1EZXkNI", AudioOut2::AudioOut2Set3DLatency);
	LIB_FUNC("XHl38ZNknbs", AudioOut2::AudioOut2MasteringInit);
	LIB_FUNC("v8iOE+j8a5o", AudioOut2::AudioOut2MasteringSetParam);
	LIB_FUNC("VZidxi2cYh0", AudioOut2::AudioOut2MasteringGetState);
	LIB_FUNC("2bbBBOkH4CY", AudioOut2::AudioOut2MasteringTerm);
}

} // namespace LibAudioOut2

namespace LibAudioIn {

LIB_VERSION("AudioIn", 1, "AudioIn", 1, 1);

namespace AudioIn = Audio::AudioIn;

LIB_DEFINE(InitAudio_1_AudioIn) {
	LIB_FUNC("5NE8Sjc7VC8", AudioIn::AudioInOpen);
	LIB_FUNC("LozEOU8+anM", AudioIn::AudioInInput);
}

} // namespace LibAudioIn

namespace LibVoiceQoS {

LIB_VERSION("VoiceQoS", 1, "VoiceQoS", 0, 0);

namespace VoiceQoS = Audio::VoiceQoS;

LIB_DEFINE(InitAudio_1_VoiceQoS) {
	LIB_FUNC("U8IfNl6-Css", VoiceQoS::VoiceQoSInit);
}

} // namespace LibVoiceQoS

namespace LibVoice {

LIB_VERSION("Voice", 1, "Voice", 1, 1);

struct VoiceInitParam {
	int32_t  app_type;
	uint64_t on_event;
	void*    user_data;
	uint8_t  reserved[32 - sizeof(int32_t) - sizeof(uint64_t) - sizeof(void*)];
};

struct VoicePortParam {
	int32_t  port_type;
	uint16_t threshold;
	uint16_t mute;
	float    volume;
	union {
		struct {
			int32_t bitrate;
		} voice;
		struct {
			uint32_t buffer_size;
			int32_t  data_type;
			int32_t  sample_rate;
		} pcmaudio;
		struct {
			int32_t user_id;
			int32_t type;
			int32_t index;
		} device;
	};
};

struct VoicePortInfo {
	int32_t   port_type;
	int32_t   state;
	uint32_t* edge;
	uint32_t  byte_count;
	uint32_t  frame_size;
	uint16_t  edge_count;
	uint16_t  reserved;
};

struct VoiceStartParam {
	void*    container;
	uint32_t mem_size;
	uint8_t  reserved[32 - sizeof(void*) - sizeof(uint32_t)];
};

struct VoicePort {
	bool     used       = false;
	int32_t  port_type  = -1;
	uint32_t bitrate    = 48000;
	uint32_t frame_size = 1;
	uint32_t byte_count = 0;
	uint16_t edge_count = 0;
	float    volume     = 1.0f;
};

static std::array<VoicePort, 256> g_voice_ports;
static uint32_t                   g_next_voice_port_id = 1;
static bool                       g_voice_started      = false;

static uint32_t alloc_voice_port(const VoicePortParam* param) {
	for (uint32_t i = 0; i < static_cast<uint32_t>(g_voice_ports.size()) - 1; i++) {
		const auto id = g_next_voice_port_id;
		g_next_voice_port_id++;
		if (g_next_voice_port_id == 0xff) {
			g_next_voice_port_id = 1;
		}

		if (!g_voice_ports[id].used) {
			auto& port      = g_voice_ports[id];
			port            = {};
			port.used       = true;
			port.port_type  = (param != nullptr ? param->port_type : -1);
			port.bitrate    = 48000;
			port.frame_size = 1;
			port.byte_count = 0;
			port.edge_count = 0;
			port.volume     = (param != nullptr && param->volume > 0.0f ? param->volume : 1.0f);

			if (param != nullptr && param->port_type == 2 && param->voice.bitrate > 0) {
				port.bitrate = static_cast<uint32_t>(param->voice.bitrate);
			}

			return id;
		}
	}

	return 0xff;
}

static VoicePort* get_voice_port(uint32_t port_id) {
	if (port_id >= g_voice_ports.size() || port_id == 0xff || !g_voice_ports[port_id].used) {
		return nullptr;
	}

	return &g_voice_ports[port_id];
}

int KYTY_SYSV_ABI VoiceInit(VoiceInitParam* /*param*/, int32_t /*version*/) {
	PRINT_NAME();
	g_voice_started = false;
	for (auto& port: g_voice_ports) {
		port = {};
	}
	g_next_voice_port_id = 1;
	return 0;
}

int KYTY_SYSV_ABI VoiceSetThreadsParams(void* /*params*/) {
	PRINT_NAME();
	return 0;
}

int KYTY_SYSV_ABI VoiceCreatePort(uint32_t* port_id, const VoicePortParam* param) {
	PRINT_NAME();

	if (port_id == nullptr) {
		return -2142369787;
	}

	const auto id = alloc_voice_port(param);
	if (id == 0xff) {
		return -2142369784;
	}

	*port_id = id;
	return 0;
}

int KYTY_SYSV_ABI VoiceConnectIPortToOPort(uint32_t input_port_id, uint32_t output_port_id) {
	PRINT_NAME();

	auto* input_port  = get_voice_port(input_port_id);
	auto* output_port = get_voice_port(output_port_id);

	if (input_port != nullptr) {
		input_port->edge_count = 1;
	}
	if (output_port != nullptr) {
		output_port->edge_count = 1;
	}

	return 0;
}

int KYTY_SYSV_ABI VoiceDisconnectIPortFromOPort(uint32_t input_port_id, uint32_t output_port_id) {
	PRINT_NAME();

	auto* input_port  = get_voice_port(input_port_id);
	auto* output_port = get_voice_port(output_port_id);

	if (input_port != nullptr) {
		input_port->edge_count = 0;
	}
	if (output_port != nullptr) {
		output_port->edge_count = 0;
	}

	return 0;
}

int KYTY_SYSV_ABI VoiceDeletePort(uint32_t port_id) {
	PRINT_NAME();

	if (auto* port = get_voice_port(port_id); port != nullptr) {
		*port = {};
	}

	return 0;
}

int KYTY_SYSV_ABI VoiceEnd() {
	PRINT_NAME();

	g_voice_started = false;
	for (auto& port: g_voice_ports) {
		port = {};
	}
	g_next_voice_port_id = 1;

	return 0;
}

int KYTY_SYSV_ABI VoiceStart(const VoiceStartParam* /*param*/) {
	PRINT_NAME();
	g_voice_started = true;
	return 0;
}

int KYTY_SYSV_ABI VoiceStop() {
	PRINT_NAME();
	g_voice_started = false;
	return 0;
}

int KYTY_SYSV_ABI VoiceReadFromOPort(uint32_t /*output_port_id*/, void* data, uint32_t* size) {
	PRINT_NAME();

	if (size == nullptr) {
		return -2142369787;
	}

	if (data != nullptr && *size != 0) {
		std::memset(data, 0, *size);
	}

	*size = 0;
	return 0;
}

int KYTY_SYSV_ABI VoiceWriteToIPort(uint32_t /*input_port_id*/, const void* /*data*/,
                                    uint32_t* size, int16_t /*frame_gaps*/) {
	PRINT_NAME();

	if (size == nullptr) {
		return -2142369787;
	}

	return 0;
}

int KYTY_SYSV_ABI VoiceGetPortInfo(uint32_t port_id, VoicePortInfo* info) {
	PRINT_NAME();

	if (info == nullptr) {
		return -2142369787;
	}

	const auto* port = get_voice_port(port_id);

	const auto edge = info->edge;
	std::memset(info, 0, sizeof(*info));
	info->edge       = edge;
	info->port_type  = (port != nullptr ? port->port_type : -1);
	info->state      = (g_voice_started ? 1 : 0);
	info->byte_count = (port != nullptr ? port->byte_count : 0);
	info->frame_size = (port != nullptr && port->frame_size != 0 ? port->frame_size : 1);
	info->edge_count = (port != nullptr ? port->edge_count : 0);

	return 0;
}

int KYTY_SYSV_ABI VoiceGetPortAttr(uint32_t /*port_id*/, int32_t attr, void* value, int32_t size) {
	PRINT_NAME();

	if (value == nullptr || size <= 0) {
		return -2142369787;
	}

	std::memset(value, 0, static_cast<size_t>(size));
	if (attr == 1001 && size >= static_cast<int32_t>(sizeof(bool))) {
		*static_cast<bool*>(value) = true;
	} else if (attr == 1003 && size >= static_cast<int32_t>(sizeof(uint16_t))) {
		*static_cast<uint16_t*>(value) = 0;
	}

	return 0;
}

int KYTY_SYSV_ABI VoiceGetBitRate(uint32_t port_id, uint32_t* bitrate) {
	PRINT_NAME();

	if (bitrate == nullptr) {
		return -2142369787;
	}

	const auto* port = get_voice_port(port_id);
	*bitrate         = (port != nullptr && port->bitrate != 0 ? port->bitrate : 48000);

	return 0;
}

int KYTY_SYSV_ABI VoiceSetVolume(uint32_t port_id, float volume) {
	PRINT_NAME();

	if (auto* port = get_voice_port(port_id); port != nullptr) {
		port->volume = volume;
	}

	return 0;
}

int KYTY_SYSV_ABI VoiceGetVolume(uint32_t port_id, float* volume) {
	PRINT_NAME();

	if (volume == nullptr) {
		return -2142369787;
	}

	const auto* port = get_voice_port(port_id);
	*volume          = (port != nullptr ? port->volume : 1.0f);

	return 0;
}

LIB_DEFINE(InitAudio_1_Voice) {
	LIB_FUNC("9TrhuGzberQ", VoiceInit);
	LIB_FUNC("clyKUyi3RYU", VoiceSetThreadsParams);
	LIB_FUNC("nXpje5yNpaE", VoiceCreatePort);
	LIB_FUNC("oV9GAdJ23Gw", VoiceConnectIPortToOPort);
	LIB_FUNC("ajVj3QG2um4", VoiceDisconnectIPortFromOPort);
	LIB_FUNC("b7kJI+nx2hg", VoiceDeletePort);
	LIB_FUNC("Oo0S5PH7FIQ", VoiceEnd);
	LIB_FUNC("54phPH2LZls", VoiceStart);
	LIB_FUNC("Ao2YNSA7-Qo", VoiceStop);
	LIB_FUNC("cQ6DGsQEjV4", VoiceReadFromOPort);
	LIB_FUNC("YeJl6yDlhW0", VoiceWriteToIPort);
	LIB_FUNC("CrLqDwWLoXM", VoiceGetPortInfo);
	LIB_FUNC("elcxZTEfHZM", VoiceGetPortAttr);
	LIB_FUNC("cJLufzou6bc", VoiceGetBitRate);
	LIB_FUNC("QBFoAIjJoXQ", VoiceSetVolume);
	LIB_FUNC("jjkCjneOYSs", VoiceGetVolume);
}

} // namespace LibVoice

namespace LibAjm {

LIB_VERSION("Ajm", 1, "Ajm", 1, 1);

namespace Ajm = Audio::Ajm;

LIB_DEFINE(InitAudio_1_Ajm) {
	LIB_FUNC("dl+4eHSzUu4", Ajm::AjmInitialize);
	LIB_FUNC("Q3dyFuwGn64", Ajm::AjmModuleRegister);
	LIB_FUNC("Wi7DtlLV+KI", Ajm::AjmModuleUnregister);
	LIB_FUNC("MHur6qCsUus", Ajm::AjmFinalize);
	LIB_FUNC("AxoDrINp4J8", Ajm::AjmInstanceCreate);
	LIB_FUNC("RbLbuKv8zho", Ajm::AjmInstanceDestroy);
	LIB_FUNC("bkRHEYG6lEM", Ajm::AjmMemoryRegister);
	LIB_FUNC("pIpGiaYkHkM", Ajm::AjmMemoryUnregister);
	LIB_FUNC("MmpF1XsQiHw", Ajm::AjmBatchInitialize);
	LIB_FUNC("5tOfnaClcqM", Ajm::AjmBatchStart);
	LIB_FUNC("-qLsfDAywIY", Ajm::AjmBatchWait);
	LIB_FUNC("NVDXiUesSbA", Ajm::AjmBatchCancel);
	LIB_FUNC("WfAiBW8Wcek", Ajm::AjmBatchErrorDump);
	LIB_FUNC("AxhcqVv5AYU", Ajm::AjmStrError);
	LIB_FUNC("ezM2OhNxzck", Ajm::AjmBatchJobInitialize);
	LIB_FUNC("uJ3m8INuikg", Ajm::AjmBatchJobClearContext);
	LIB_FUNC("39WxhR-ePew", Ajm::AjmBatchJobDecode);
	LIB_FUNC("5LLWbpP5xi8", Ajm::AjmBatchJobDecodeSingle);
	LIB_FUNC("SJ3i0DXP8vg", Ajm::AjmBatchJobDecodeSplit);
	LIB_FUNC("SlVIGK1Kl38", Ajm::AjmBatchJobEncode);
	LIB_FUNC("TBWW4aPfWcA", Ajm::AjmBatchJobGetInfo);
	LIB_FUNC("uSrXaxT+oPQ", Ajm::AjmBatchJobGetCodecInfo);
	LIB_FUNC("SkEwpiu3tZg", Ajm::AjmBatchJobSetGaplessDecode);
	LIB_FUNC("Esr9db8S1S0", Ajm::AjmBatchJobGetGaplessDecode);
	LIB_FUNC("81HsnXFbWS4", Ajm::AjmBatchJobSetResampleParameters);
	LIB_FUNC("5ldnD16rYZw", Ajm::AjmBatchJobSetResampleParametersEx);
	LIB_FUNC("JkdNCocpu1M", Ajm::AjmBatchJobGetResampleInfo);
	LIB_FUNC("3cAg7xN995U", Ajm::AjmBatchJobGetStatistics);
	LIB_FUNC("7FZsbyVRM4U", Ajm::AjmBatchJobControl);
	LIB_FUNC("jVCWcthifr8", Ajm::AjmBatchJobRun);
	LIB_FUNC("Z9NVCesiP0Q", Ajm::AjmBatchJobRunSplit);
	LIB_FUNC("1t3ixYNXyuc", Ajm::AjmDecAt9ParseConfigData);
}

} // namespace LibAjm

namespace LibAcm {

LIB_VERSION("Acm", 1, "Acm", 1, 1);

namespace Acm = Audio::Acm;

LIB_DEFINE(InitAudio_1_Acm) {
	LIB_FUNC("ZIXln2K3XMk", Acm::AcmContextCreate);
	LIB_FUNC("jBgBjAj02R8", Acm::AcmContextDestroy);
	LIB_FUNC("tW9W+CAG4FE", Acm::AcmBatchStartBuffer);
	LIB_FUNC("8fe55ktlNVo", Acm::AcmBatchStartBuffers);
	LIB_FUNC("RLN3gRlXJLE", Acm::AcmBatchWait);
	LIB_FUNC("r7z5YQFZo+U", Acm::AcmBatchJobNotification);
	LIB_FUNC("u70oWo92SYQ", Acm::AcmConvReverbSharedInput);
	LIB_FUNC("9nLbWmRDpa8", Acm::AcmConvReverbSharedIr);
	LIB_FUNC("KovqaFbmtsM", Acm::AcmFft);
	LIB_FUNC("DR-ZCmvVR9Q", Acm::AcmIfft);
	LIB_FUNC("LA4RCNKnFjg", Acm::AcmPanner);
}

} // namespace LibAcm

namespace LibAvPlayer {

LIB_VERSION("AvPlayer", 1, "AvPlayer", 1, 0);

namespace AvPlayer = Audio::AvPlayer;

LIB_DEFINE(InitAudio_1_AvPlayer) {
	LIB_FUNC("aS66RI0gGgo", AvPlayer::AvPlayerInit);
	LIB_FUNC("o9eWRkSL+M4", AvPlayer::AvPlayerInitEx);
	LIB_FUNC("HD1YKVU26-M", AvPlayer::AvPlayerPostInit);
	LIB_FUNC("KMcEa+rHsIo", AvPlayer::AvPlayerAddSource);
	LIB_FUNC("x8uvuFOPZhU", AvPlayer::AvPlayerAddSourceEx);
	LIB_FUNC("hdTyRzCXQeQ", AvPlayer::AvPlayerStreamCount);
	LIB_FUNC("d8FcbzfAdQw", AvPlayer::AvPlayerGetStreamInfo);
	LIB_FUNC("ctTAcF5DiKQ", AvPlayer::AvPlayerGetStreamInfoEx);
	LIB_FUNC("BOVKAzRmuTQ", AvPlayer::AvPlayerDisableStream);
	LIB_FUNC("ODJK2sn9w4A", AvPlayer::AvPlayerEnableStream);
	LIB_FUNC("buMCiJftcfw", AvPlayer::AvPlayerChangeStream);
	LIB_FUNC("ET4Gr-Uu07s", AvPlayer::AvPlayerStart);
	LIB_FUNC("NxSdL9t-KXk", AvPlayer::AvPlayerStartEx);
	LIB_FUNC("ZC17w3vB5Lo", AvPlayer::AvPlayerStop);
	LIB_FUNC("9y5v+fGN4Wk", AvPlayer::AvPlayerPause);
	LIB_FUNC("w5moABNwnRY", AvPlayer::AvPlayerResume);
	LIB_FUNC("k-q+xOxdc3E", AvPlayer::AvPlayerSetAvSyncMode);
	LIB_FUNC("N6Oy-EjduiY", AvPlayer::AvPlayerSetAvailableBandwidth);
	LIB_FUNC("OVths0xGfho", AvPlayer::AvPlayerSetLooping);
	LIB_FUNC("av8Z++94rs0", AvPlayer::AvPlayerSetTrickSpeed);
	LIB_FUNC("o3+RWnHViSg", AvPlayer::AvPlayerGetVideoData);
	LIB_FUNC("JdksQu8pNdQ", AvPlayer::AvPlayerGetVideoDataEx);
	LIB_FUNC("Wnp1OVcrZgk", AvPlayer::AvPlayerGetAudioData);
	LIB_FUNC("UbQoYawOsfY", AvPlayer::AvPlayerIsActive);
	LIB_FUNC("wwM99gjFf1Y", AvPlayer::AvPlayerCurrentTime);
	LIB_FUNC("XC9wM+xULz8", AvPlayer::AvPlayerJumpToTime);
	LIB_FUNC("NkJwDzKmIlw", AvPlayer::AvPlayerClose);
	LIB_FUNC("eBTreZ84JFY", AvPlayer::AvPlayerSetLogCallback);
}

} // namespace LibAvPlayer

namespace LibAudiodec {

LIB_VERSION("Audiodec", 1, "Audiodec", 1, 1);

static constexpr int32_t AUDIODEC_ERROR_INVALID_TYPE             = -2139160575; // 0x807F0001
static constexpr int32_t AUDIODEC_ERROR_ARG                      = -2139160574; // 0x807F0002
static constexpr int32_t AUDIODEC_ERROR_INVALID_PARAM_SIZE       = -2139160572; // 0x807F0004
static constexpr int32_t AUDIODEC_ERROR_INVALID_BSI_INFO_SIZE    = -2139160571; // 0x807F0005
static constexpr int32_t AUDIODEC_ERROR_INVALID_AU_INFO_SIZE     = -2139160570; // 0x807F0006
static constexpr int32_t AUDIODEC_ERROR_INVALID_PCM_ITEM_SIZE    = -2139160569; // 0x807F0007
static constexpr int32_t AUDIODEC_ERROR_INVALID_CTRL_POINTER     = -2139160568; // 0x807F0008
static constexpr int32_t AUDIODEC_ERROR_INVALID_PARAM_POINTER    = -2139160567; // 0x807F0009
static constexpr int32_t AUDIODEC_ERROR_INVALID_BSI_INFO_POINTER = -2139160566; // 0x807F000A
static constexpr int32_t AUDIODEC_ERROR_INVALID_AU_INFO_POINTER  = -2139160565; // 0x807F000B
static constexpr int32_t AUDIODEC_ERROR_INVALID_PCM_ITEM_POINTER = -2139160564; // 0x807F000C
static constexpr int32_t AUDIODEC_ERROR_INVALID_AU_POINTER       = -2139160563; // 0x807F000D
static constexpr int32_t AUDIODEC_ERROR_INVALID_PCM_POINTER      = -2139160562; // 0x807F000E
static constexpr int32_t AUDIODEC_ERROR_INVALID_HANDLE           = -2139160561; // 0x807F000F
static constexpr int32_t AUDIODEC_ERROR_INVALID_WORD_LENGTH      = -2139160560; // 0x807F0010
static constexpr int32_t AUDIODEC_ERROR_INVALID_AU_SIZE          = -2139160559; // 0x807F0011
static constexpr int32_t AUDIODEC_ERROR_INVALID_PCM_SIZE         = -2139160558; // 0x807F0012

static constexpr uint32_t AUDIODEC_TYPE_AT9   = 0x0001;
static constexpr uint32_t AUDIODEC_TYPE_MP3   = 0x0002;
static constexpr uint32_t AUDIODEC_TYPE_M4AAC = 0x0003;

static constexpr int32_t AUDIODEC_WORD_SZ_24BIT = 0;
static constexpr int32_t AUDIODEC_WORD_SZ_16BIT = 1;
static constexpr int32_t AUDIODEC_WORD_SZ_FLOAT = 2;

static constexpr uint32_t AUDIODEC_AT9_MAX_FRAME_SIZE      = 2048;
static constexpr uint32_t AUDIODEC_AT9_MAX_FRAME_SAMPLES   = 256;
static constexpr uint32_t AUDIODEC_MP3_MAX_FRAME_SIZE      = 1441;
static constexpr uint32_t AUDIODEC_MP3_MAX_FRAME_SAMPLES   = 1152;
static constexpr uint32_t AUDIODEC_M4AAC_MAX_FRAME_SIZE    = 4608;
static constexpr uint32_t AUDIODEC_M4AAC_MAX_FRAME_SAMPLES = 2048;

struct AudiodecAuInfo {
	uint32_t uiSize;
	void*    pAuAddr;
	uint32_t uiAuSize;
};

struct AudiodecPcmItem {
	uint32_t uiSize;
	void*    pPcmAddr;
	uint32_t uiPcmSize;
};

struct AudiodecCtrl {
	void*            pParam;
	void*            pBsiInfo;
	AudiodecAuInfo*  pAuInfo;
	AudiodecPcmItem* pPcmItem;
};

struct AudiodecParamAt9 {
	uint32_t uiSize;
	int32_t  iBwPcm;
	uint8_t  uiConfigData[4];
};

struct AudiodecAt9Info {
	uint32_t uiSize;
	uint32_t uiChannel;
	uint32_t uiBitrate;
	uint32_t uiSamplingRate;
	uint32_t uiSuperFrameSize;
	uint32_t uiFramesInSuperFrame;
	uint32_t uiNextFrameSize;
	uint32_t uiFrameSamples;
	int32_t  iResult;
};

struct AudiodecParamMp3 {
	uint32_t uiSize;
	int32_t  iBwPcm;
};

struct AudiodecMp3Info {
	uint32_t uiSize;
	uint32_t uiHeader;
	uint8_t  ucCrc;
	uint8_t  ucMode;
	uint8_t  ucModeExtension;
	uint8_t  ucCopyright;
	uint8_t  ucOriginal;
	uint8_t  ucEmphasis;
	uint8_t  ucReserved[2];
	int32_t  iResult;
};

struct AudiodecParamM4aac {
	uint32_t uiSize;
	int32_t  iBwPcm;
	uint32_t uiConfigNumber;
	uint32_t uiSamplingFreqIndex;
	uint32_t uiMaxChannels;
	uint32_t uiEnableHeaac;
};

struct AudiodecM4aacInfo {
	uint32_t uiSize;
	uint32_t uiSamplingFreq;
	uint32_t uiNumberOfChannels;
	uint32_t uiHeaac;
	int32_t  iResult;
};

struct AudiodecDecoder {
	bool     used          = false;
	uint32_t codec_type    = 0;
	int32_t  word_size     = AUDIODEC_WORD_SZ_16BIT;
	uint32_t channels      = 2;
	uint32_t sample_rate   = 48000;
	uint32_t frame_bytes   = AUDIODEC_MP3_MAX_FRAME_SIZE;
	uint32_t frame_samples = AUDIODEC_MP3_MAX_FRAME_SAMPLES;
};

static Common::Mutex                   g_audiodec_mutex;
static std::array<uint32_t, 4>         g_audiodec_init_count {};
static std::array<AudiodecDecoder, 64> g_audiodec_decoders {};

static bool audiodec_is_valid_type(uint32_t codec_type) {
	switch (codec_type) {
		case AUDIODEC_TYPE_AT9:
		case AUDIODEC_TYPE_MP3:
		case AUDIODEC_TYPE_M4AAC: return true;
		default: return false;
	}
}

static uint32_t audiodec_sample_rate_from_index(uint32_t index) {
	static constexpr std::array<uint32_t, 12> rates = {96000, 88200, 64000, 48000, 44100, 32000,
	                                                   24000, 22050, 16000, 12000, 11025, 8000};
	return index < rates.size() ? rates[index] : 48000;
}

static int32_t audiodec_word_size_bytes(int32_t word_size) {
	switch (word_size) {
		case AUDIODEC_WORD_SZ_24BIT: return 4;
		case AUDIODEC_WORD_SZ_16BIT: return 2;
		case AUDIODEC_WORD_SZ_FLOAT: return 4;
		default: return 0;
	}
}

static int32_t audiodec_validate_ctrl(const AudiodecCtrl* ctrl, uint32_t codec_type, bool decode) {
	if (ctrl == nullptr) {
		return AUDIODEC_ERROR_INVALID_CTRL_POINTER;
	}
	if (ctrl->pParam == nullptr) {
		return AUDIODEC_ERROR_INVALID_PARAM_POINTER;
	}
	if (ctrl->pBsiInfo == nullptr) {
		return AUDIODEC_ERROR_INVALID_BSI_INFO_POINTER;
	}
	if (ctrl->pAuInfo == nullptr) {
		return AUDIODEC_ERROR_INVALID_AU_INFO_POINTER;
	}
	if (ctrl->pPcmItem == nullptr) {
		return AUDIODEC_ERROR_INVALID_PCM_ITEM_POINTER;
	}
	if (ctrl->pAuInfo->uiSize != sizeof(AudiodecAuInfo)) {
		return AUDIODEC_ERROR_INVALID_AU_INFO_SIZE;
	}
	if (ctrl->pPcmItem->uiSize != sizeof(AudiodecPcmItem)) {
		return AUDIODEC_ERROR_INVALID_PCM_ITEM_SIZE;
	}
	if (decode && ctrl->pAuInfo->pAuAddr == nullptr) {
		return AUDIODEC_ERROR_INVALID_AU_POINTER;
	}
	if (decode && ctrl->pPcmItem->pPcmAddr == nullptr) {
		return AUDIODEC_ERROR_INVALID_PCM_POINTER;
	}

	switch (codec_type) {
		case AUDIODEC_TYPE_AT9: {
			auto* param = static_cast<const AudiodecParamAt9*>(ctrl->pParam);
			auto* info  = static_cast<const AudiodecAt9Info*>(ctrl->pBsiInfo);
			if (param->uiSize != sizeof(AudiodecParamAt9)) {
				return AUDIODEC_ERROR_INVALID_PARAM_SIZE;
			}
			if (info->uiSize != sizeof(AudiodecAt9Info)) {
				return AUDIODEC_ERROR_INVALID_BSI_INFO_SIZE;
			}
			if (audiodec_word_size_bytes(param->iBwPcm) == 0) {
				return AUDIODEC_ERROR_INVALID_WORD_LENGTH;
			}
			break;
		}
		case AUDIODEC_TYPE_MP3: {
			auto* param = static_cast<const AudiodecParamMp3*>(ctrl->pParam);
			auto* info  = static_cast<const AudiodecMp3Info*>(ctrl->pBsiInfo);
			if (param->uiSize != sizeof(AudiodecParamMp3)) {
				return AUDIODEC_ERROR_INVALID_PARAM_SIZE;
			}
			if (info->uiSize != sizeof(AudiodecMp3Info)) {
				return AUDIODEC_ERROR_INVALID_BSI_INFO_SIZE;
			}
			if (audiodec_word_size_bytes(param->iBwPcm) == 0) {
				return AUDIODEC_ERROR_INVALID_WORD_LENGTH;
			}
			break;
		}
		case AUDIODEC_TYPE_M4AAC: {
			auto* param = static_cast<const AudiodecParamM4aac*>(ctrl->pParam);
			auto* info  = static_cast<const AudiodecM4aacInfo*>(ctrl->pBsiInfo);
			if (param->uiSize < sizeof(AudiodecParamM4aac)) {
				return AUDIODEC_ERROR_INVALID_PARAM_SIZE;
			}
			if (info->uiSize != sizeof(AudiodecM4aacInfo)) {
				return AUDIODEC_ERROR_INVALID_BSI_INFO_SIZE;
			}
			if (audiodec_word_size_bytes(param->iBwPcm) == 0) {
				return AUDIODEC_ERROR_INVALID_WORD_LENGTH;
			}
			break;
		}
		default: return AUDIODEC_ERROR_INVALID_TYPE;
	}

	if (decode && ctrl->pAuInfo->uiAuSize == 0) {
		return AUDIODEC_ERROR_INVALID_AU_SIZE;
	}
	if (decode && ctrl->pPcmItem->uiPcmSize == 0) {
		return AUDIODEC_ERROR_INVALID_PCM_SIZE;
	}

	return 0;
}

static void audiodec_fill_info(AudiodecCtrl* ctrl, const AudiodecDecoder& decoder) {
	switch (decoder.codec_type) {
		case AUDIODEC_TYPE_AT9: {
			auto* info                 = static_cast<AudiodecAt9Info*>(ctrl->pBsiInfo);
			info->uiChannel            = decoder.channels;
			info->uiBitrate            = 0;
			info->uiSamplingRate       = decoder.sample_rate;
			info->uiSuperFrameSize     = decoder.frame_bytes;
			info->uiFramesInSuperFrame = 1;
			info->uiNextFrameSize      = decoder.frame_bytes;
			info->uiFrameSamples       = decoder.frame_samples;
			info->iResult              = 0;
			break;
		}
		case AUDIODEC_TYPE_MP3: {
			auto* info            = static_cast<AudiodecMp3Info*>(ctrl->pBsiInfo);
			info->uiHeader        = 0;
			info->ucCrc           = 0;
			info->ucMode          = decoder.channels == 1 ? 3 : 0;
			info->ucModeExtension = 0;
			info->ucCopyright     = 0;
			info->ucOriginal      = 0;
			info->ucEmphasis      = 0;
			info->ucReserved[0]   = 0;
			info->ucReserved[1]   = 0;
			info->iResult         = 0;
			break;
		}
		case AUDIODEC_TYPE_M4AAC: {
			auto* info               = static_cast<AudiodecM4aacInfo*>(ctrl->pBsiInfo);
			info->uiSamplingFreq     = decoder.sample_rate;
			info->uiNumberOfChannels = decoder.channels;
			info->uiHeaac            = 0;
			info->iResult            = 0;
			break;
		}
		default: break;
	}
}

static int32_t KYTY_SYSV_ABI AudiodecInitLibrary(uint32_t codec_type) {
	PRINT_NAME();

	if (!audiodec_is_valid_type(codec_type)) {
		return AUDIODEC_ERROR_INVALID_TYPE;
	}

	Common::LockGuard lock(g_audiodec_mutex);
	g_audiodec_init_count[codec_type]++;

	return 0;
}

static int32_t KYTY_SYSV_ABI AudiodecTermLibrary(uint32_t codec_type) {
	PRINT_NAME();

	if (!audiodec_is_valid_type(codec_type)) {
		return AUDIODEC_ERROR_INVALID_TYPE;
	}

	Common::LockGuard lock(g_audiodec_mutex);
	if (g_audiodec_init_count[codec_type] > 0) {
		g_audiodec_init_count[codec_type]--;
	}

	return 0;
}

static int32_t KYTY_SYSV_ABI AudiodecCreateDecoder(AudiodecCtrl* ctrl, uint32_t codec_type) {
	PRINT_NAME();

	if (!audiodec_is_valid_type(codec_type)) {
		return AUDIODEC_ERROR_INVALID_TYPE;
	}

	int32_t ret = audiodec_validate_ctrl(ctrl, codec_type, false);
	if (ret < 0) {
		return ret;
	}

	Common::LockGuard lock(g_audiodec_mutex);
	if (g_audiodec_init_count[codec_type] == 0) {
		return AUDIODEC_ERROR_ARG;
	}

	for (uint32_t i = 0; i < g_audiodec_decoders.size(); i++) {
		if (!g_audiodec_decoders[i].used) {
			auto& decoder       = g_audiodec_decoders[i];
			decoder             = {};
			decoder.used        = true;
			decoder.codec_type  = codec_type;
			decoder.sample_rate = 48000;

			switch (codec_type) {
				case AUDIODEC_TYPE_AT9: {
					auto* param           = static_cast<const AudiodecParamAt9*>(ctrl->pParam);
					decoder.word_size     = param->iBwPcm;
					decoder.channels      = 2;
					decoder.frame_bytes   = AUDIODEC_AT9_MAX_FRAME_SIZE;
					decoder.frame_samples = AUDIODEC_AT9_MAX_FRAME_SAMPLES;
					break;
				}
				case AUDIODEC_TYPE_MP3: {
					auto* param           = static_cast<const AudiodecParamMp3*>(ctrl->pParam);
					decoder.word_size     = param->iBwPcm;
					decoder.channels      = 2;
					decoder.frame_bytes   = AUDIODEC_MP3_MAX_FRAME_SIZE;
					decoder.frame_samples = AUDIODEC_MP3_MAX_FRAME_SAMPLES;
					break;
				}
				case AUDIODEC_TYPE_M4AAC: {
					auto* param       = static_cast<const AudiodecParamM4aac*>(ctrl->pParam);
					decoder.word_size = param->iBwPcm;
					decoder.channels  = param->uiMaxChannels == 0 ? 2 : param->uiMaxChannels;
					if (decoder.channels > 8) {
						decoder.channels = 8;
					}
					decoder.sample_rate =
					    audiodec_sample_rate_from_index(param->uiSamplingFreqIndex);
					decoder.frame_bytes   = AUDIODEC_M4AAC_MAX_FRAME_SIZE;
					decoder.frame_samples = AUDIODEC_M4AAC_MAX_FRAME_SAMPLES;
					break;
				}
			}

			audiodec_fill_info(ctrl, decoder);

			return static_cast<int32_t>(i + 1);
		}
	}

	return AUDIODEC_ERROR_ARG;
}

static int32_t KYTY_SYSV_ABI AudiodecDeleteDecoder(int32_t handle) {
	PRINT_NAME();

	if (handle <= 0 || static_cast<uint32_t>(handle) > g_audiodec_decoders.size()) {
		return AUDIODEC_ERROR_INVALID_HANDLE;
	}

	Common::LockGuard lock(g_audiodec_mutex);
	auto&             decoder = g_audiodec_decoders[static_cast<uint32_t>(handle - 1)];
	if (!decoder.used) {
		return AUDIODEC_ERROR_INVALID_HANDLE;
	}

	decoder = {};

	return 0;
}

static int32_t KYTY_SYSV_ABI AudiodecDecode(int32_t handle, AudiodecCtrl* ctrl) {
	PRINT_NAME();

	if (handle <= 0 || static_cast<uint32_t>(handle) > g_audiodec_decoders.size()) {
		return AUDIODEC_ERROR_INVALID_HANDLE;
	}

	Common::LockGuard lock(g_audiodec_mutex);
	auto&             decoder = g_audiodec_decoders[static_cast<uint32_t>(handle - 1)];
	if (!decoder.used) {
		return AUDIODEC_ERROR_INVALID_HANDLE;
	}

	int32_t ret = audiodec_validate_ctrl(ctrl, decoder.codec_type, true);
	if (ret < 0) {
		return ret;
	}

	const uint32_t word_bytes = static_cast<uint32_t>(audiodec_word_size_bytes(decoder.word_size));
	const uint32_t wanted_pcm = decoder.frame_samples * decoder.channels * word_bytes;
	const uint32_t produced_pcm =
	    ctrl->pPcmItem->uiPcmSize < wanted_pcm ? ctrl->pPcmItem->uiPcmSize : wanted_pcm;
	const uint32_t consumed_data = ctrl->pAuInfo->uiAuSize < decoder.frame_bytes
	                                   ? ctrl->pAuInfo->uiAuSize
	                                   : decoder.frame_bytes;

	std::memset(ctrl->pPcmItem->pPcmAddr, 0, produced_pcm);
	ctrl->pPcmItem->uiPcmSize = produced_pcm;
	ctrl->pAuInfo->uiAuSize   = consumed_data;
	audiodec_fill_info(ctrl, decoder);

	return 0;
}

static int32_t KYTY_SYSV_ABI AudiodecClearContext(int32_t handle) {
	PRINT_NAME();

	if (handle <= 0 || static_cast<uint32_t>(handle) > g_audiodec_decoders.size()) {
		return AUDIODEC_ERROR_INVALID_HANDLE;
	}

	Common::LockGuard lock(g_audiodec_mutex);
	if (!g_audiodec_decoders[static_cast<uint32_t>(handle - 1)].used) {
		return AUDIODEC_ERROR_INVALID_HANDLE;
	}

	return 0;
}

LIB_DEFINE(InitAudio_1_Audiodec) {
	LIB_FUNC("VjhsmxpcezI", AudiodecInitLibrary);
	LIB_FUNC("h5jSB2QIDV0", AudiodecTermLibrary);
	LIB_FUNC("O3f1sLMWRvs", AudiodecCreateDecoder);
	LIB_FUNC("Tp+ZEy69mLk", AudiodecDeleteDecoder);
	LIB_FUNC("KHXHMDLkILw", AudiodecDecode);
	LIB_FUNC("6Vf9WTLDoss", AudiodecClearContext);
}

} // namespace LibAudiodec

namespace LibAudio3d {

LIB_VERSION("Audio3d", 1, "Audio3d", 1, 1);

namespace Audio3d = Audio::Audio3d;

LIB_DEFINE(InitAudio_1_Audio3d) {
	LIB_FUNC("UmCvjSmuZIw", Audio3d::Audio3dInitialize);
	LIB_FUNC("Im+jOoa5WAI", Audio3d::Audio3dGetDefaultOpenParameters);
	LIB_FUNC("XeDDK0xJWQA", Audio3d::Audio3dPortOpen);
	LIB_FUNC("Yq9bfUQ0uJg", Audio3d::Audio3dPortSetAttribute);
	LIB_FUNC("YaaDbDwKpFM", Audio3d::Audio3dPortGetQueueLevel);
	LIB_FUNC("lw0qrdSjZt8", Audio3d::Audio3dPortAdvance);
	LIB_FUNC("VEVhZ9qd4ZY", Audio3d::Audio3dPortPush);
}

} // namespace LibAudio3d

namespace LibNgs2 {

LIB_VERSION("Ngs2", 1, "Ngs2", 1, 1);

namespace Ngs2 = Audio::Ngs2;

LIB_DEFINE(InitAudio_1_Ngs2) {
	LIB_FUNC("AQkj7C0f3PY", Ngs2::Ngs2SystemResetOption);
	LIB_FUNC("pgFAiLR5qT4", Ngs2::Ngs2SystemQueryBufferSize);
	LIB_FUNC("koBbCMvOKWw", Ngs2::Ngs2SystemCreate);
	LIB_FUNC("mPYgU4oYpuY", Ngs2::Ngs2SystemCreateWithAllocator);
	LIB_FUNC("u-WrYDaJA3k", Ngs2::Ngs2SystemDestroy);
	LIB_FUNC("U546k6orxQo", Ngs2::Ngs2RackCreateWithAllocator);
	LIB_FUNC("cLV4aiT9JpA", Ngs2::Ngs2RackCreate);
	LIB_FUNC("0eFLVCfWVds", Ngs2::Ngs2RackQueryBufferSize);
	LIB_FUNC("lCqD7oycmIM", Ngs2::Ngs2RackDestroy);
	LIB_FUNC("MwmHz8pAdAo", Ngs2::Ngs2RackGetVoiceHandle);
	LIB_FUNC("MzTa7VLjogY", Ngs2::Ngs2RackLock);
	LIB_FUNC("++YZ7P9e87U", Ngs2::Ngs2RackUnlock);
	LIB_FUNC("uu94irFOGpA", Ngs2::Ngs2VoiceControl);
	LIB_FUNC("AbYvTOZ8Pts", Ngs2::Ngs2VoiceRunCommands);
	LIB_FUNC("-TOuuAQ-buE", Ngs2::Ngs2VoiceGetState);
	LIB_FUNC("rEh728kXk3w", Ngs2::Ngs2VoiceGetStateFlags);
	LIB_FUNC("i0VnXM-C9fc", Ngs2::Ngs2SystemRender);
	LIB_FUNC("l4Q2dWEH6UM", Ngs2::Ngs2SystemSetGrainSamples);
	LIB_FUNC("hyVLT2VlOYk", Ngs2::Ngs2ParseWaveformData);
	LIB_FUNC("3pCNbVM11UA", Ngs2::Ngs2CalcWaveformBlock);
	LIB_FUNC("xa8oL9dmXkM", Ngs2::Ngs2PanInit);
	LIB_FUNC("gbMKV+8Enuo", Ngs2::Ngs2PanGetVolumeMatrix);
	LIB_FUNC("7Lcfo8SmpsU", Ngs2::Ngs2GeomResetListenerParam);
	LIB_FUNC("0lbbayqDNoE", Ngs2::Ngs2GeomResetSourceParam);
	LIB_FUNC("1WsleK-MTkE", Ngs2::Ngs2GeomCalcListener);
	LIB_FUNC("eF8yRCC6W64", Ngs2::Ngs2GeomApply);
}

} // namespace LibNgs2

LIB_DEFINE(InitAudio_1) {
	LibAudioOut::InitAudio_1_AudioOut(s);
	LibAudioOut2::InitAudio_1_AudioOut2(s);
	LibAudioIn::InitAudio_1_AudioIn(s);
	LibVoiceQoS::InitAudio_1_VoiceQoS(s);
	LibVoice::InitAudio_1_Voice(s);
	LibAjm::InitAudio_1_Ajm(s);
	LibAcm::InitAudio_1_Acm(s);
	LibAvPlayer::InitAudio_1_AvPlayer(s);
	LibAudiodec::InitAudio_1_Audiodec(s);
	LibAudio3d::InitAudio_1_Audio3d(s);
	LibNgs2::InitAudio_1_Ngs2(s);
	LibAudioPropagation::InitAudio_1_AudioPropagation(s);
}

} // namespace Libs
