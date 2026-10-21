#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Audio {

namespace Ngs2 {

struct Ngs2SystemOption;
struct Ngs2SystemInfo;
struct Ngs2RackOption;
struct Ngs2RackInfo;
struct Ngs2BufferAllocator;
struct Ngs2VoiceParamHeader;
struct Ngs2RenderBufferInfo;
struct Ngs2ContextBufferInfo;
struct Ngs2VoiceState;
struct Ngs2VoicePortInfo;
struct Ngs2VoiceChannelsInfo;
struct Ngs2WaveformFormat;
struct Ngs2WaveformBlock;
struct Ngs2WaveformInfo;
struct Ngs2PanWork;
struct Ngs2PanParam;
struct Ngs2GeomListenerParam;
struct Ngs2GeomListenerWork;
struct Ngs2GeomSourceParam;
struct Ngs2GeomAttribute;

/// Initializes a system option structure with default values.
int KYTY_SYSV_ABI Ngs2SystemResetOption(Ngs2SystemOption* option);
/// Reports the context-buffer size and alignment required by a system option.
int KYTY_SYSV_ABI Ngs2SystemQueryBufferSize(const Ngs2SystemOption* option,
                                            Ngs2ContextBufferInfo*  buffer_info);
/// Creates a system using caller-provided context memory.
int KYTY_SYSV_ABI Ngs2SystemCreate(const Ngs2SystemOption*      option,
                                   const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
/// Retrieves information about a system.
int KYTY_SYSV_ABI Ngs2SystemGetInfo(uintptr_t system_handle, Ngs2SystemInfo* info,
                                    size_t info_size);
int KYTY_SYSV_ABI Ngs2SystemLock(uintptr_t system_handle);
int KYTY_SYSV_ABI Ngs2SystemUnlock(uintptr_t system_handle);
/// Sets the number of samples processed in each render grain.
int KYTY_SYSV_ABI Ngs2SystemSetGrainSamples(uintptr_t system_handle, uint32_t num_samples);
/// Reports the context-buffer size and alignment required by a rack option.
int KYTY_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option,
                                          Ngs2ContextBufferInfo* buffer_info);
/// Creates a rack using caller-provided context memory.
int KYTY_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id,
                                 const Ngs2RackOption*        option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
/// Creates a system using the supplied allocation callbacks.
int KYTY_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption*    option,
                                                const Ngs2BufferAllocator* allocator,
                                                uintptr_t*                 handle);
/// Destroys a system and returns its context buffer when applicable.
int KYTY_SYSV_ABI Ngs2SystemDestroy(uintptr_t system_handle, Ngs2ContextBufferInfo* buffer_info);
/// Creates a rack using the supplied allocation callbacks.
int KYTY_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id,
                                              const Ngs2RackOption*      option,
                                              const Ngs2BufferAllocator* allocator,
                                              uintptr_t*                 handle);
/// Destroys a rack and returns its context buffer when applicable.
int KYTY_SYSV_ABI Ngs2RackDestroy(uintptr_t rack_handle, Ngs2ContextBufferInfo* buffer_info);
/// Retrieves information about a rack.
int KYTY_SYSV_ABI Ngs2RackGetInfo(uintptr_t rack_handle, Ngs2RackInfo* info, size_t info_size);
/// Locks a rack for synchronized access.
int KYTY_SYSV_ABI Ngs2RackLock(uintptr_t rack_handle);
/// Releases a rack lock.
int KYTY_SYSV_ABI Ngs2RackUnlock(uintptr_t rack_handle);
/// Retrieves a voice handle by its rack-local identifier.
int KYTY_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id,
                                         uintptr_t* handle);
/// Applies one or more parameter blocks to a voice.
int KYTY_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list);
/// Executes a sequence of commands on a voice.
int KYTY_SYSV_ABI Ngs2VoiceRunCommands(uintptr_t voice_handle, const void* commands,
                                       size_t num_commands);
/// Retrieves the current state of a voice.
int KYTY_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state,
                                    size_t state_size);
/// Retrieves the current state flags for a voice.
int KYTY_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags);
/// Retrieves routing and delay information for a voice port.
int KYTY_SYSV_ABI Ngs2VoiceGetPortInfo(uintptr_t voice_handle, uint32_t port,
                                       Ngs2VoicePortInfo* info, size_t info_size);
/// Retrieves voice-specific information identified by info_id.
int KYTY_SYSV_ABI Ngs2VoiceQueryInfo(uintptr_t voice_handle, uint32_t info_id, void* info,
                                     size_t info_size);
/// Renders the requested output buffers for a system.
int KYTY_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info,
                                   uint32_t num_buffer_info);
/// Parses waveform metadata from a data buffer.
int KYTY_SYSV_ABI Ngs2ParseWaveformData(const void* data, size_t data_size, Ngs2WaveformInfo* info);
/// Calculates the waveform block containing a sample range.
int KYTY_SYSV_ABI Ngs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos,
                                        uint32_t num_samples, Ngs2WaveformBlock* block);
/// Initializes speaker-panning work data.
int KYTY_SYSV_ABI Ngs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle,
                              uint32_t num_speakers);
/// Calculates output gains for a set of panning parameters.
int KYTY_SYSV_ABI Ngs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params,
                                         uint32_t num_params, uint32_t matrix_format,
                                         float* out_volume_matrix);
/// Resets listener parameters to their default values.
int KYTY_SYSV_ABI Ngs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param);
/// Resets source parameters to their default values.
int KYTY_SYSV_ABI Ngs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param);
/// Calculates listener geometry work data from listener parameters.
int KYTY_SYSV_ABI Ngs2GeomCalcListener(const Ngs2GeomListenerParam* param,
                                       Ngs2GeomListenerWork* out_work, uint32_t flags);
/// Applies source and listener geometry to produce output attributes.
int KYTY_SYSV_ABI Ngs2GeomApply(const Ngs2GeomListenerWork* listener,
                                const Ngs2GeomSourceParam* source, Ngs2GeomAttribute* out_attrib,
                                uint32_t flags);

} // namespace Ngs2

} // namespace Libs::Audio

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_NGS2_H_ */
