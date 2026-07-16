#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/subsystems.h"

namespace Libs::Audio {

KYTY_SUBSYSTEM_DEFINE(Audio);

namespace AudioOut {

struct AudioOutOutputParam;
struct AudioOutPortState;

int KYTY_SYSV_ABI AudioOutInit();
int KYTY_SYSV_ABI AudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq,
                               uint32_t param);
int KYTY_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol);
int KYTY_SYSV_ABI AudioOutOutputs(AudioOutOutputParam* param, uint32_t num);
int KYTY_SYSV_ABI AudioOutOutput(int handle, const void* ptr);
int KYTY_SYSV_ABI AudioOutClose(int handle);
int KYTY_SYSV_ABI AudioOutGetPortState(int handle, AudioOutPortState* state);

} // namespace AudioOut

namespace AudioOut2 {

struct AudioOut2ContextParam;
struct AudioOut2PortParam;
struct AudioOut2Attribute;
struct AudioOut2PortState;
struct AudioOut2SystemState;
struct AudioOut2Position;
struct AudioOut2SpeakerInfo;
struct AudioOut2SystemDebugStateParam;
struct AudioOut2MasteringParamsHeader;
struct AudioOut2MasteringStatesHeader;

using AudioOut2ContextHandle      = uint64_t;
using AudioOut2PortHandle         = uint64_t;
using AudioOut2UserHandle         = uintptr_t;
using AudioOut2SpeakerArrayHandle = void*;

int KYTY_SYSV_ABI AudioOut2Initialize();
int KYTY_SYSV_ABI AudioOut2ContextQueryMemory(const AudioOut2ContextParam* params,
                                              size_t*                      memory_size);
int KYTY_SYSV_ABI AudioOut2ContextResetParam(AudioOut2ContextParam* params);
int KYTY_SYSV_ABI AudioOut2ContextCreate(const AudioOut2ContextParam* params, void* buffer,
                                         size_t buffer_size, AudioOut2ContextHandle* ctx);
int KYTY_SYSV_ABI AudioOut2ContextDestroy(AudioOut2ContextHandle ctx);
int KYTY_SYSV_ABI AudioOut2ContextSetAttributes(AudioOut2ContextHandle    ctx,
                                                const AudioOut2Attribute* attributes, uint32_t num);
int KYTY_SYSV_ABI AudioOut2ContextAdvance(AudioOut2ContextHandle ctx);
int KYTY_SYSV_ABI AudioOut2ContextPush(AudioOut2ContextHandle ctx, uint32_t blocking);
int KYTY_SYSV_ABI AudioOut2ContextGetQueueLevel(AudioOut2ContextHandle ctx, uint32_t* queue_level,
                                                uint32_t* available_queues);
int KYTY_SYSV_ABI AudioOut2PortCreate(AudioOut2ContextHandle ctx, const AudioOut2PortParam* params,
                                      AudioOut2PortHandle* port);
int KYTY_SYSV_ABI AudioOut2PortDestroy(AudioOut2PortHandle port);
int KYTY_SYSV_ABI AudioOut2PortSetAttributes(AudioOut2PortHandle       port,
                                             const AudioOut2Attribute* attributes, uint32_t num);
int KYTY_SYSV_ABI AudioOut2PortGetState(AudioOut2PortHandle port, AudioOut2PortState* state);
int KYTY_SYSV_ABI AudioOut2GetSystemState(AudioOut2SystemState* state);
int KYTY_SYSV_ABI AudioOut2UserCreate(uint32_t user_id, AudioOut2UserHandle* handle);
int KYTY_SYSV_ABI AudioOut2UserDestroy(AudioOut2UserHandle handle);
size_t KYTY_SYSV_ABI AudioOut2GetSpeakerArrayMemorySize(uint32_t num_speakers, uint8_t is_3d,
                                                        uint8_t is_ambisonics);
int KYTY_SYSV_ABI    AudioOut2SpeakerArrayCreate(AudioOut2SpeakerArrayHandle* handle,
                                                 const void* vbap_params, const void* ambi_params);
int KYTY_SYSV_ABI    AudioOut2SpeakerArrayDestroy(AudioOut2SpeakerArrayHandle handle);
int KYTY_SYSV_ABI    AudioOut2GetSpeakerArrayCoefficients(
    AudioOut2SpeakerArrayHandle handle, AudioOut2Position pos, float spread, float* coefficients,
    uint32_t num_coefficients, uint8_t height_aware, float downmix_spread_radius);
int KYTY_SYSV_ABI AudioOut2GetSpeakerArrayAmbisonicsCoefficients(AudioOut2SpeakerArrayHandle handle,
                                                                 uint32_t ambisonics_channel,
                                                                 float*   coefficients,
                                                                 uint32_t num_coefficients);
int KYTY_SYSV_ABI AudioOut2GetSpeakerInfo(AudioOut2SpeakerInfo* info, uint32_t flags);
int KYTY_SYSV_ABI AudioOut2SetSystemDebugState(const AudioOut2SystemDebugStateParam* param);
int KYTY_SYSV_ABI AudioOut2Set3DLatency(uint32_t user_id, uint32_t output, uint32_t latency_us);
int KYTY_SYSV_ABI AudioOut2MasteringInit(uint32_t flags);
int KYTY_SYSV_ABI AudioOut2MasteringSetParam(const AudioOut2MasteringParamsHeader* param,
                                             uint32_t output, uint32_t flags);
int KYTY_SYSV_ABI AudioOut2MasteringGetState(AudioOut2MasteringStatesHeader* state, uint32_t output,
                                             AudioOut2UserHandle user);
int KYTY_SYSV_ABI AudioOut2MasteringTerm();

} // namespace AudioOut2

namespace AudioIn {

int KYTY_SYSV_ABI AudioInOpen(int user_id, uint32_t type, uint32_t index, uint32_t len,
                              uint32_t freq, uint32_t param);
int KYTY_SYSV_ABI AudioInInput(int handle, void* dest);

} // namespace AudioIn

namespace VoiceQoS {

int KYTY_SYSV_ABI VoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type);

} // namespace VoiceQoS

namespace Ajm {

struct AjmBatchInfo;
struct AjmBatchError;
struct AjmBuffer;

struct AjmDecAt9ConfigDataInfo {
	uint32_t channels;
	uint32_t sample_rate;
	uint32_t frame_samples_per_channel;
	uint32_t superframe_samples_per_channel;
	uint32_t superframe_size;
};

int KYTY_SYSV_ABI AjmInitialize(int64_t reserved, uint32_t* context);
int KYTY_SYSV_ABI AjmModuleRegister(uint32_t context, uint32_t codec, int64_t reserved);
int KYTY_SYSV_ABI AjmModuleUnregister(uint32_t context, uint32_t codec);
int KYTY_SYSV_ABI AjmFinalize(uint32_t context);
int KYTY_SYSV_ABI AjmInstanceCreate(uint32_t context, uint32_t codec, uint64_t flags,
                                    uint32_t* instance);
int KYTY_SYSV_ABI AjmInstanceDestroy(uint32_t context, uint32_t instance);
int KYTY_SYSV_ABI AjmMemoryRegister(uint32_t context, void* ptr, size_t pages);
int KYTY_SYSV_ABI AjmMemoryUnregister(uint32_t context, void* ptr);
int KYTY_SYSV_ABI AjmBatchInitialize(void* buffer, size_t size, AjmBatchInfo* info);
int KYTY_SYSV_ABI AjmBatchStart(uint32_t context, const AjmBatchInfo* info, int priority,
                                AjmBatchError* error, uint32_t* batch);
int KYTY_SYSV_ABI AjmBatchWait(uint32_t context, uint32_t batch, uint32_t timeout,
                               AjmBatchError* error);
int KYTY_SYSV_ABI AjmBatchCancel(uint32_t context, uint32_t batch);
int KYTY_SYSV_ABI AjmBatchErrorDump(const AjmBatchInfo* info, AjmBatchError* error);
int KYTY_SYSV_ABI AjmBatchJobInitialize(AjmBatchInfo* info, uint32_t instance,
                                        const void* codec_parameters, size_t codec_parameters_size,
                                        void* result);
int KYTY_SYSV_ABI AjmBatchJobClearContext(AjmBatchInfo* info, uint32_t instance, void* result);
int KYTY_SYSV_ABI AjmBatchJobDecode(AjmBatchInfo* info, uint32_t instance,
                                    const void* bitstream_input, size_t bitstream_input_size,
                                    void* pcm_output, size_t pcm_output_size, void* result);
int KYTY_SYSV_ABI AjmBatchJobDecodeSingle(AjmBatchInfo* info, uint32_t instance,
                                          const void* bitstream_input, size_t bitstream_input_size,
                                          void* pcm_output, size_t pcm_output_size, void* result);
int KYTY_SYSV_ABI AjmBatchJobDecodeSplit(AjmBatchInfo* info, uint32_t instance,
                                         const AjmBuffer* input_buffers, size_t input_buffers_num,
                                         const AjmBuffer* output_buffers, size_t output_buffers_num,
                                         void* result);
int KYTY_SYSV_ABI AjmBatchJobEncode(AjmBatchInfo* info, uint32_t instance, const void* pcm_input,
                                    size_t pcm_input_size, void* bitstream_output,
                                    size_t bitstream_output_size, void* result);
int KYTY_SYSV_ABI AjmBatchJobGetInfo(AjmBatchInfo* info, uint32_t instance, void* result);
int KYTY_SYSV_ABI AjmBatchJobGetCodecInfo(AjmBatchInfo* info, uint32_t instance, void* result,
                                          size_t result_size);
int KYTY_SYSV_ABI AjmBatchJobSetGaplessDecode(AjmBatchInfo* info, uint32_t instance,
                                              const void* gapless_decode, int reset, void* result);
int KYTY_SYSV_ABI AjmBatchJobGetGaplessDecode(AjmBatchInfo* info, uint32_t instance, void* result);
int KYTY_SYSV_ABI AjmBatchJobSetResampleParameters(AjmBatchInfo* info, uint32_t instance,
                                                   float ratio, uint32_t flags, void* result);
int KYTY_SYSV_ABI AjmBatchJobSetResampleParametersEx(AjmBatchInfo* info, uint32_t instance,
                                                     float ratio_start,
                                                     float ratio_change_per_sample, uint32_t flags,
                                                     void* result);
int KYTY_SYSV_ABI AjmBatchJobGetResampleInfo(AjmBatchInfo* info, uint32_t instance, void* result);
int KYTY_SYSV_ABI AjmBatchJobGetStatistics(AjmBatchInfo* info, float interval, void* result);
int KYTY_SYSV_ABI AjmBatchJobControl(AjmBatchInfo* info, uint32_t instance, uint64_t flags,
                                     const void* sideband_input, size_t sideband_input_size,
                                     void* sideband_output, size_t sideband_output_size);
int KYTY_SYSV_ABI AjmBatchJobRun(AjmBatchInfo* info, uint32_t instance, uint64_t flags,
                                 const void* data_input, size_t data_input_size, void* data_output,
                                 size_t data_output_size, void* sideband_output,
                                 size_t sideband_output_size);
int KYTY_SYSV_ABI AjmBatchJobRunSplit(AjmBatchInfo* info, uint32_t instance, uint64_t flags,
                                      const AjmBuffer* input_buffers, size_t input_buffers_num,
                                      const AjmBuffer* output_buffers, size_t output_buffers_num,
                                      void* sideband_output, size_t sideband_output_size);
int KYTY_SYSV_ABI AjmDecAt9ParseConfigData(const void*              config_data,
                                           AjmDecAt9ConfigDataInfo* config_info);
const char* KYTY_SYSV_ABI AjmStrError(int error);

} // namespace Ajm

namespace Acm {

struct AcmBatchInfo;
struct AcmBatchError;

using AcmContextId = uint32_t;
using AcmBatchId   = uint32_t;

int KYTY_SYSV_ABI AcmContextCreate(AcmContextId* context);
int KYTY_SYSV_ABI AcmContextDestroy(AcmContextId context);
int KYTY_SYSV_ABI AcmBatchStartBuffer(AcmContextId context, const void* batch_commands,
                                      size_t batch_size, AcmBatchError* batch_error,
                                      AcmBatchId* batch);
int KYTY_SYSV_ABI AcmBatchStartBuffers(AcmContextId context, uint32_t batch_info_count,
                                       const AcmBatchInfo* const batch_info[],
                                       AcmBatchError* batch_error, AcmBatchId* batch);
int KYTY_SYSV_ABI AcmBatchWait(AcmContextId context, AcmBatchId batch, uint32_t timeout);
int KYTY_SYSV_ABI AcmBatchJobNotification(AcmBatchInfo* batch_info);
int KYTY_SYSV_ABI AcmConvReverbSharedInput(AcmBatchInfo* batch_info, uint32_t block_count, void* in,
                                           uint32_t count, const void* const ir[],
                                           const float* gain, void* const out[]);
int KYTY_SYSV_ABI AcmConvReverbSharedIr(AcmBatchInfo* batch_info, uint32_t block_count,
                                        const void* ir, uint32_t count, void* const in[],
                                        const float* gain, void* const out[]);
int KYTY_SYSV_ABI AcmFft(AcmBatchInfo* batch_info, int size, int count, int input_format,
                         const void* const input[], int output_format, void* const output[],
                         uint32_t flags);
int KYTY_SYSV_ABI AcmIfft(AcmBatchInfo* batch_info, int size, int count, int input_format,
                          const void* const input[], int output_format, void* const output[],
                          uint32_t flags);
int KYTY_SYSV_ABI AcmPanner(AcmBatchInfo* batch_info, uint32_t in_count, const float* const in[],
                            uint32_t biquad_count, uint32_t biquad_update_count, uint32_t out_count,
                            const void* const parameter[], void* const state[],
                            const float* const out_init[], float* const out[]);

} // namespace Acm

namespace AvPlayer {

struct AvPlayerInitData;
struct AvPlayerFrameInfoEx;
struct AvPlayerFrameInfo;
struct AvPlayerInternal;

using Bool = uint8_t;

AvPlayerInternal* KYTY_SYSV_ABI AvPlayerInit(AvPlayerInitData* init);
int KYTY_SYSV_ABI               AvPlayerInitEx(const void* init_ex, AvPlayerInternal** handle);
int KYTY_SYSV_ABI               AvPlayerPostInit(AvPlayerInternal* h, const void* post_init);
int KYTY_SYSV_ABI               AvPlayerAddSource(AvPlayerInternal* h, const char* filename);
int KYTY_SYSV_ABI               AvPlayerAddSourceEx(AvPlayerInternal* h, uint32_t uri_type,
                                                    const void* source_details);
int KYTY_SYSV_ABI               AvPlayerStreamCount(AvPlayerInternal* h);
int KYTY_SYSV_ABI      AvPlayerGetStreamInfo(AvPlayerInternal* h, uint32_t stream_id, void* info);
int KYTY_SYSV_ABI      AvPlayerGetStreamInfoEx(AvPlayerInternal* h, uint32_t stream_id, void* info);
int KYTY_SYSV_ABI      AvPlayerEnableStream(AvPlayerInternal* h, uint32_t stream_id);
int KYTY_SYSV_ABI      AvPlayerDisableStream(AvPlayerInternal* h, uint32_t stream_id);
int KYTY_SYSV_ABI      AvPlayerChangeStream(AvPlayerInternal* h, uint32_t old_stream_id,
                                            uint32_t new_stream_id);
int KYTY_SYSV_ABI      AvPlayerStart(AvPlayerInternal* h);
int KYTY_SYSV_ABI      AvPlayerStartEx(AvPlayerInternal* h, const void* start_info_ex);
int KYTY_SYSV_ABI      AvPlayerStop(AvPlayerInternal* h);
int KYTY_SYSV_ABI      AvPlayerPause(AvPlayerInternal* h);
int KYTY_SYSV_ABI      AvPlayerResume(AvPlayerInternal* h);
int KYTY_SYSV_ABI      AvPlayerSetLooping(AvPlayerInternal* h, Bool loop);
int KYTY_SYSV_ABI      AvPlayerSetTrickSpeed(AvPlayerInternal* h, int32_t trick_speed);
int KYTY_SYSV_ABI      AvPlayerSetAvSyncMode(AvPlayerInternal* h, uint32_t sync_mode);
int KYTY_SYSV_ABI      AvPlayerSetAvailableBandwidth(AvPlayerInternal* h, uint32_t start_bandwidth,
                                                     uint32_t minimum_bandwidth,
                                                     uint32_t maximum_bandwidth);
Bool KYTY_SYSV_ABI     AvPlayerGetVideoData(AvPlayerInternal* h, AvPlayerFrameInfo* video_info);
Bool KYTY_SYSV_ABI     AvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info);
Bool KYTY_SYSV_ABI     AvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info);
Bool KYTY_SYSV_ABI     AvPlayerIsActive(AvPlayerInternal* h);
uint64_t KYTY_SYSV_ABI AvPlayerCurrentTime(AvPlayerInternal* h);
int KYTY_SYSV_ABI      AvPlayerJumpToTime(AvPlayerInternal* h, uint64_t time_ms);
int KYTY_SYSV_ABI      AvPlayerClose(AvPlayerInternal* h);
int KYTY_SYSV_ABI      AvPlayerSetLogCallback(void* callback, void* user_data);

} // namespace AvPlayer

namespace Audio3d {

struct Audio3dOpenParameters;

int KYTY_SYSV_ABI  Audio3dInitialize(int64_t reserved);
void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p);
int KYTY_SYSV_ABI  Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters,
                                   uint32_t* id);
int KYTY_SYSV_ABI  Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id,
                                           const void* attribute, size_t attribute_size);
int KYTY_SYSV_ABI  Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level,
                                            uint32_t* queue_available);
int KYTY_SYSV_ABI  Audio3dPortAdvance(uint32_t port_id);
int KYTY_SYSV_ABI  Audio3dPortPush(uint32_t port_id, uint32_t blocking);

} // namespace Audio3d

namespace Ngs2 {

struct Ngs2SystemOption;
struct Ngs2RackOption;
struct Ngs2BufferAllocator;
struct Ngs2VoiceParamHeader;
struct Ngs2RenderBufferInfo;
struct Ngs2ContextBufferInfo;
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

int KYTY_SYSV_ABI Ngs2SystemResetOption(Ngs2SystemOption* option);
int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option,
                                            Ngs2ContextBufferInfo*  buffer_info);
int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption*      option,
                                   const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
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
                                       uint32_t num_commands, uint32_t flags);
int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state,
                                    size_t state_size);
int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags);
int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info,
                                   uint32_t num_buffer_info);
int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size, Ngs2WaveformInfo* info);
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

} // namespace Ngs2

} // namespace Libs::Audio

#endif /* EMULATOR_INCLUDE_EMULATOR_AUDIO_H_ */
