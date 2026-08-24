#include "common/abi.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <atomic>

namespace Libs {

LIB_VERSION("TextToSpeech2", 1, "TextToSpeech2", 1, 1);

namespace TextToSpeech2 {

constexpr int TEXT_TO_SPEECH2_ERROR_INVALID_ARGUMENT = static_cast<int>(0x81a10001u);

enum class SpeechStatus : int32_t { NotProcessing = 0, Processing = 1 };

static std::atomic<SpeechStatus> g_speech_status {SpeechStatus::NotProcessing};

static int KYTY_SYSV_ABI TextToSpeech2GetSpeechStatus(SpeechStatus* status) {
	PRINT_NAME();
	if (status == nullptr) {
		return TEXT_TO_SPEECH2_ERROR_INVALID_ARGUMENT;
	}
	*status = g_speech_status.load(std::memory_order_relaxed);

	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2Cancel() {
	PRINT_NAME();
	g_speech_status.store(SpeechStatus::NotProcessing, std::memory_order_relaxed);

	return OK;
}

} // namespace TextToSpeech2

LIB_DEFINE(InitTextToSpeech2_1) {
	LIB_FUNC("08JSg9p6bgQ", TextToSpeech2::TextToSpeech2GetSpeechStatus);
	LIB_FUNC("2jiIxUmcsGo", TextToSpeech2::TextToSpeech2Cancel);
}

} // namespace Libs
