#include "libs/acm.h"

#include "common/assert.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace Libs::Audio {

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

static std::atomic_uint32_t g_acm_next_context {1};
static std::atomic_uint32_t g_acm_next_batch {1};

static void acm_advance_batch(AcmBatchInfo* info, size_t bytes) {
	if (info == nullptr || info->buffer == nullptr || info->buffer_size == 0) {
		return;
	}

	info->offset = std::min(info->buffer_size, info->offset + bytes);
}

int KYTY_SYSV_ABI AcmContextCreate(AcmContextId* context) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(context == nullptr);

	*context = g_acm_next_context.fetch_add(1, std::memory_order_relaxed);

	LOGF("\t context = %" PRIu32 "\n", *context);

	return OK;
}

int KYTY_SYSV_ABI AcmContextDestroy(AcmContextId context) {
	PRINT_NAME();
	LOGF("\t context = %" PRIu32 "\n", context);
	return OK;
}

int KYTY_SYSV_ABI AcmBatchStartBuffer(AcmContextId context, const void* batch_commands,
                                      size_t batch_size, AcmBatchError* batch_error,
                                      AcmBatchId* batch) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(batch == nullptr);

	if (batch_error != nullptr) {
		std::memset(batch_error, 0, sizeof(AcmBatchError));
	}

	*batch = g_acm_next_batch.fetch_add(1, std::memory_order_relaxed);

	return OK;
}

int KYTY_SYSV_ABI AcmBatchStartBuffers(AcmContextId context, uint32_t batch_info_count,
                                       const AcmBatchInfo* const batch_info[],
                                       AcmBatchError* batch_error, AcmBatchId* batch) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(batch_info_count != 0 && batch_info == nullptr);
	EXIT_NOT_IMPLEMENTED(batch == nullptr);

	if (batch_error != nullptr) {
		std::memset(batch_error, 0, sizeof(AcmBatchError));
	}

	*batch = g_acm_next_batch.fetch_add(1, std::memory_order_relaxed);

	return OK;
}

int KYTY_SYSV_ABI AcmBatchWait(AcmContextId context, AcmBatchId batch, uint32_t timeout) {
	return OK;
}

int KYTY_SYSV_ABI AcmBatchJobNotification(AcmBatchInfo* batch_info) {
	PRINT_NAME();
	acm_advance_batch(batch_info, 2 * 16);
	return OK;
}

int KYTY_SYSV_ABI AcmConvReverbSharedInput(AcmBatchInfo* batch_info, uint32_t block_count, void* in,
                                           uint32_t count, const void* const ir[],
                                           const float* gain, void* const out[]) {
	PRINT_NAME();
	(void)block_count;
	(void)in;
	(void)count;
	(void)ir;
	(void)gain;
	(void)out;
	acm_advance_batch(batch_info, 1024);
	return OK;
}

int KYTY_SYSV_ABI AcmConvReverbSharedIr(AcmBatchInfo* batch_info, uint32_t block_count,
                                        const void* ir, uint32_t count, void* const in[],
                                        const float* gain, void* const out[]) {
	PRINT_NAME();
	(void)block_count;
	(void)ir;
	(void)count;
	(void)in;
	(void)gain;
	(void)out;
	acm_advance_batch(batch_info, 1024);
	return OK;
}

int KYTY_SYSV_ABI AcmFft(AcmBatchInfo* batch_info, int size, int count, int input_format,
                         const void* const input[], int output_format, void* const output[],
                         uint32_t flags) {
	PRINT_NAME();
	(void)size;
	(void)count;
	(void)input_format;
	(void)input;
	(void)output_format;
	(void)output;
	(void)flags;
	acm_advance_batch(batch_info, 256);
	return OK;
}

int KYTY_SYSV_ABI AcmIfft(AcmBatchInfo* batch_info, int size, int count, int input_format,
                          const void* const input[], int output_format, void* const output[],
                          uint32_t flags) {
	PRINT_NAME();
	(void)size;
	(void)count;
	(void)input_format;
	(void)input;
	(void)output_format;
	(void)output;
	(void)flags;
	acm_advance_batch(batch_info, 256);
	return OK;
}

int KYTY_SYSV_ABI AcmPanner(AcmBatchInfo* batch_info, uint32_t in_count, const float* const in[],
                            uint32_t biquad_count, uint32_t biquad_update_count, uint32_t out_count,
                            const void* const parameter[], void* const state[],
                            const float* const out_init[], float* const out[]) {
	PRINT_NAME();
	(void)in_count;
	(void)in;
	(void)biquad_count;
	(void)biquad_update_count;
	(void)out_count;
	(void)parameter;
	(void)state;
	(void)out_init;
	(void)out;
	acm_advance_batch(batch_info, 512);
	return OK;
}

} // namespace Acm

} // namespace Libs::Audio
