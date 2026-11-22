#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Audio {

namespace Ngs2 {

struct Ngs2SystemOption;
struct Ngs2SystemInfo;
struct Ngs2RackOption;
struct Ngs2RenderBufferInfo;
struct Ngs2ContextBufferInfo {
	void*     host_buffer      = nullptr;
	size_t    host_buffer_size = 0;
	uintptr_t reserved[5]      = {};
	uintptr_t user_data        = 0;
};

using Ngs2BufferAllocHandler = int32_t KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);
using Ngs2BufferFreeHandler  = int32_t  KYTY_SYSV_ABI (*)(Ngs2ContextBufferInfo*);

struct Ngs2BufferAllocator {
	Ngs2BufferAllocHandler alloc_handler = nullptr;
	Ngs2BufferFreeHandler  free_handler  = nullptr;
	uintptr_t              user_data     = 0;
};
struct Ngs2VoiceState;
struct Ngs2WaveformFormat;
struct Ngs2WaveformBlock;
struct Ngs2WaveformInfo;
struct Ngs2PanWork;
struct Ngs2PanParam;
struct Ngs2GeomListenerParam;
struct Ngs2GeomListenerWork;
struct Ngs2GeomSourceParam;
struct Ngs2GeomAttribute;
struct Ngs2RackInfo;
struct Ngs2CustomModuleInfo;
struct Ngs2VoiceMatrixInfo;

struct Ngs2VoiceParamHeader {
	uint16_t size;
	int16_t  next;
	uint32_t id;
};
static_assert(sizeof(Ngs2VoiceParamHeader) == 8);

// --- Mastering Rack Parameters ---
constexpr uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_SETUP      = 0x30000000;
constexpr uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_LFE        = 0x30000002;
constexpr uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_LIMITER    = 0x30000003;
constexpr uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_GAIN       = 0x30000004;
constexpr uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_OUTPUT     = 0x30000005;
constexpr uint32_t SCE_NGS2_MASTERING_VOICE_PARAM_PEAK_METER = 0x30000006;

struct Ngs2MasteringVoiceEffectParam {
	Ngs2VoiceParamHeader header;
	uint32_t             enable_flag;
	uint32_t             value;
};
static_assert(sizeof(Ngs2MasteringVoiceEffectParam) == 16);

// --- Constants, Modules, and Custom Parameters / Waveform ---
constexpr uint32_t SCE_NGS2_WAVEFORM_TYPE_VAG                = 0x1c;
constexpr uint32_t SCE_NGS2_CUSTOM_MODULE_ID_FILTER          = 0x13;
constexpr uint32_t SCE_NGS2_CUSTOM_MODULE_ID_GAIN            = 0x15;
constexpr uint32_t SCE_NGS2_CUSTOM_MODULE_ID_CHANNEL_MIXER   = 0x17;
constexpr uint32_t SCE_NGS2_CUSTOM_MODULE_ID_PITCH_SHIFT     = 0x20;
constexpr uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_FILTER        = 0x40001300;
constexpr uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_GAIN          = 0x40001500;
constexpr uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_CHANNEL_MIXER = 0x40001700;
constexpr uint32_t SCE_NGS2_CUSTOM_VOICE_PARAM_PITCH_SHIFT   = 0x40002000;

struct Ngs2CustomVoiceFilterParam {
	Ngs2VoiceParamHeader header;
	uint64_t             channel_mask;
	uint32_t             type;
	union {
		struct {
			float i0;
			float i1;
			float i2;
			float o1;
			float o2;
		} direct;
		struct {
			float    frequency;
			float    q;
			float    level;
			uint32_t reserved;
			uint32_t reserved2;
		} fcq;
	} param;
	uint32_t reserved3;
};
static_assert(sizeof(Ngs2CustomVoiceFilterParam) == 48);

struct Ngs2CustomVoiceGainParam {
	Ngs2VoiceParamHeader header;
	float                gain[37];
};
static_assert(sizeof(Ngs2CustomVoiceGainParam) == 156);

struct Ngs2CustomVoiceChannelMixerParam {
	Ngs2VoiceParamHeader header;
	float                level[37][37];
};
static_assert(sizeof(Ngs2CustomVoiceChannelMixerParam) == 5484);

struct Ngs2CustomVoicePitchShiftParam {
	Ngs2VoiceParamHeader header;
	int32_t              cents;
	uint32_t             reserved;
	uint32_t             bypass_mask;
	uint32_t             reserved2;
};
static_assert(sizeof(Ngs2CustomVoicePitchShiftParam) == 24);

int KYTY_SYSV_ABI Ngs2SystemResetOption(Ngs2SystemOption* option);
int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option,
                                            Ngs2ContextBufferInfo*  buffer_info);
int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption*      option,
                                   const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2SystemGetInfo(uintptr_t system_handle, Ngs2SystemInfo* info,
                                    size_t info_size);
int KYTY_SYSV_ABI Ngs2SystemLock(uintptr_t system_handle);
int KYTY_SYSV_ABI Ngs2SystemUnlock(uintptr_t system_handle);
int KYTY_SYSV_ABI Ngs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t num_samples);
int KYTY_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option,
                                          Ngs2ContextBufferInfo* buffer_info);
int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id,
                                 const Ngs2RackOption*        option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption*    option,
                                                const Ngs2BufferAllocator* allocator,
                                                uintptr_t*                 handle);
int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle, Ngs2ContextBufferInfo* buffer_info);
int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id,
                                              const Ngs2RackOption*      option,
                                              const Ngs2BufferAllocator* allocator,
                                              uintptr_t*                 handle);
int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info);
int KYTY_SYSV_ABI Ngs2RackLock(uintptr_t rack_handle);
int KYTY_SYSV_ABI Ngs2RackUnlock(uintptr_t rack_handle);
int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id,
                                         uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list);
int KYTY_SYSV_ABI Ngs2VoiceRunCommands(uintptr_t voice_handle, const void* commands,
                                       size_t num_commands);
int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state,
                                    size_t state_size);
int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags);
int KYTY_SYSV_ABI Ngs2VoiceQueryInfo(uintptr_t voice_handle, uint32_t query_id, void* info,
                                     size_t info_size);
int KYTY_SYSV_ABI Ngs2VoiceGetPortInfo(uintptr_t voice_handle, uint32_t port, void* info,
                                       size_t info_size);
int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info,
                                   uint32_t num_buffer_info);
int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size, Ngs2WaveformInfo* info);
int KYTY_SYSV_ABI Ngs2ParseWaveformFile(const char* path, uint32_t offset, Ngs2WaveformInfo* info);
int KYTY_SYSV_ABI Ngs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos,
                                        uint32_t num_samples, Ngs2WaveformBlock* block);
int KYTY_SYSV_ABI Ngs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle,
                              uint32_t num_speakers);
int KYTY_SYSV_ABI Ngs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params,
                                         uint32_t num_params, uint32_t matrix_format,
                                         float* out_volume_matrix);
int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param);
int KYTY_SYSV_ABI Ngs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param);
int KYTY_SYSV_ABI Ngs2GeomCalcListener(const Ngs2GeomListenerParam* param,
                                       Ngs2GeomListenerWork* out_work, uint32_t flags);
int KYTY_SYSV_ABI Ngs2GeomApply(const Ngs2GeomListenerWork* listener,
                                const Ngs2GeomSourceParam* source, Ngs2GeomAttribute* out_attrib,
                                uint32_t flags);

int KYTY_SYSV_ABI Ngs2SystemRunCommands(uintptr_t system_handle, const void* commands,
                                        size_t count);
int KYTY_SYSV_ABI Ngs2SystemQueryInfo(uintptr_t system_handle, uint32_t info_id, void* info,
                                      size_t info_size);
int KYTY_SYSV_ABI Ngs2SystemSetUserData(uintptr_t system_handle, uintptr_t user_data);
int KYTY_SYSV_ABI Ngs2SystemGetUserData(uintptr_t system_handle, uintptr_t* user_data);
int KYTY_SYSV_ABI Ngs2SystemSetSampleRate(uintptr_t system_handle, uint32_t sample_rate);
int KYTY_SYSV_ABI Ngs2SystemEnumHandles(uintptr_t* out_handles, uint32_t max_handles);
int KYTY_SYSV_ABI Ngs2SystemEnumRackHandles(uintptr_t system_handle, uintptr_t* out_handles,
                                            uint32_t max_handles);

int KYTY_SYSV_ABI Ngs2RackRunCommands(uintptr_t rack_handle, const void* commands, size_t count);
int KYTY_SYSV_ABI Ngs2RackQueryInfo(uintptr_t rack_handle, uint32_t info_id, void* info,
                                    size_t info_size);
int KYTY_SYSV_ABI Ngs2RackGetInfo(uintptr_t rack_handle, Ngs2RackInfo* info, size_t info_size);
int KYTY_SYSV_ABI Ngs2RackSetUserData(uintptr_t rack_handle, uintptr_t user_data);
int KYTY_SYSV_ABI Ngs2RackGetUserData(uintptr_t rack_handle, uintptr_t* user_data);
int KYTY_SYSV_ABI Ngs2CustomRackGetModuleInfo(uintptr_t rack_handle, uint32_t module_index,
                                              Ngs2CustomModuleInfo* info, size_t info_size);

int KYTY_SYSV_ABI Ngs2VoiceGetMatrixInfo(uintptr_t voice_handle, uint32_t matrix_id,
                                         Ngs2VoiceMatrixInfo* info, size_t info_size);
int KYTY_SYSV_ABI Ngs2VoiceGetOwner(uintptr_t voice_handle, uintptr_t* owner_rack,
                                    uint32_t* voice_index);

// --- DSP FFT & Waveform Frame Info ---
int KYTY_SYSV_ABI Ngs2FftQuerySize(uint32_t length, uint32_t flags, uint64_t* out_size);
int KYTY_SYSV_ABI Ngs2FftInit(void* work, size_t size, int length, uint32_t flags,
                              void** out_handle);
int KYTY_SYSV_ABI Ngs2FftProcess(void* handle, const float* in, float* out, float* work,
                                 uintptr_t unused, uint32_t mode);
int KYTY_SYSV_ABI Ngs2GetWaveformFrameInfo(const Ngs2WaveformFormat* format,
                                           uint32_t*                 out_frame_size,
                                           uint32_t*                 out_num_frame_samples,
                                           uint32_t*                 out_units_per_frame,
                                           uint32_t*                 out_num_delay_samples);
int KYTY_SYSV_ABI Ngs2ParseWaveformUser(uintptr_t handler, uintptr_t user_data,
                                        Ngs2WaveformInfo* out_info);

// --- Stream & Module Error Constants ---
constexpr int32_t SCE_NGS2_ERROR_INVALID_OPTION_SIZE          = static_cast<int32_t>(0x804a8013u);
constexpr int32_t SCE_NGS2_ERROR_INVALID_BUFFER_INFO          = static_cast<int32_t>(0x804a8020u);
constexpr int32_t SCE_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR     = static_cast<int32_t>(0x804a8023u);
constexpr int32_t SCE_NGS2_ERROR_INVALID_STREAM_COMMANDS      = static_cast<int32_t>(0x804a8030u);
constexpr int32_t SCE_NGS2_ERROR_INVALID_STREAM_QUERY_ID      = static_cast<int32_t>(0x804a8031u);
constexpr int32_t SCE_NGS2_ERROR_INVALID_NUM_BLOCKS           = static_cast<int32_t>(0x804a8053u);
constexpr int32_t SCE_NGS2_ERROR_NULL_STREAM_BLOCK_PTR        = static_cast<int32_t>(0x804a8055u);
constexpr int32_t SCE_NGS2_ERROR_INVALID_STREAM_HANDLE        = static_cast<int32_t>(0x804a8204u);
constexpr int32_t SCE_NGS2_ERROR_INVALID_STREAM_STATE_COMMAND = static_cast<int32_t>(0x804a8400u);
constexpr int32_t SCE_NGS2_ERROR_INVALID_STREAM_COMMAND_ID    = static_cast<int32_t>(0x804a8401u);
constexpr int32_t SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_3_ZERO    = static_cast<int32_t>(0x804a8402u);
constexpr int32_t SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_1_ZERO    = static_cast<int32_t>(0x804a8403u);
constexpr int32_t SCE_NGS2_ERROR_STREAM_BUFFER_SIZE_2_ZERO    = static_cast<int32_t>(0x804a8404u);

// --- Stream API & Scheduler / Modules ---
struct Ngs2StreamOption {
	uint64_t size              = sizeof(Ngs2StreamOption);
	char     name[64]          = {};
	uint32_t flags             = 0;
	uint32_t num_blocks        = 4;
	uint32_t buffer_size_1     = 0x100;
	uint32_t reserved1         = 0;
	uint32_t buffer_size_2     = 0x10;
	uint32_t reserved2         = 0;
	uint32_t buffer_size_3     = 0x10000;
	uint32_t reserved3         = 0;
	float    default_param     = 0.5f;
	uint32_t reserved4         = 0;
	uint8_t  reserved_tail[24] = {};
};
static_assert(sizeof(Ngs2StreamOption) == 0x88);

struct Ngs2JobSchedulerOption {
	uint64_t size            = sizeof(Ngs2JobSchedulerOption);
	char     name[64]        = {};
	uint32_t flags           = 0;
	uint32_t num_threads     = 1;
	uint32_t thread_affinity = 0;
	uint32_t thread_priority = 0;
	uint8_t  reserved[40]    = {};
};

int KYTY_SYSV_ABI Ngs2StreamResetOption(void* option);
int KYTY_SYSV_ABI Ngs2StreamQueryBufferSize(const void* option, void* buffer_info);
int KYTY_SYSV_ABI Ngs2StreamCreate(uintptr_t system_handle, const void* option,
                                   const void* buffer_info, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2StreamCreateWithAllocator(uintptr_t system_handle, const void* option,
                                                const void* allocator, uintptr_t* handle);
int KYTY_SYSV_ABI Ngs2StreamDestroy(uintptr_t stream_handle, void* buffer_info);
int KYTY_SYSV_ABI Ngs2StreamRunCommands(uintptr_t stream_handle, const void* commands,
                                        size_t count);
int KYTY_SYSV_ABI Ngs2StreamQueryInfo(uintptr_t stream_handle, uint32_t info_id, void* info,
                                      size_t info_size);

int KYTY_SYSV_ABI Ngs2JobSchedulerResetOption(void* option);
int KYTY_SYSV_ABI Ngs2ModuleEnumConfigs(void** out, uint32_t max);
int KYTY_SYSV_ABI Ngs2ModuleArrayEnumItems(const void* array, void* out, uint32_t max);
int KYTY_SYSV_ABI Ngs2ModuleQueueEnumItems(const void* queue, void* out, uint32_t max);
int KYTY_SYSV_ABI Ngs2ReportRegisterHandler(const void* handler, void* user_data);
int KYTY_SYSV_ABI Ngs2ReportUnregisterHandler(const void* handler);

} // namespace Ngs2

} // namespace Libs::Audio

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_ */
