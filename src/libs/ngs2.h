#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Audio {

namespace Ngs2 {

struct Ngs2SystemOption;
struct Ngs2SystemInfo;
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
int KYTY_SYSV_ABI Ngs2SystemGetInfo(uintptr_t system_handle, Ngs2SystemInfo* info,
                                    size_t info_size);
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

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_ */
