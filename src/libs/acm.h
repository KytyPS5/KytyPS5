#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_ACM_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_ACM_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Audio {

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

} // namespace Libs::Audio

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_ACM_H_ */
