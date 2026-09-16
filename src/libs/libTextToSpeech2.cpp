#include "common/abi.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("TextToSpeech2", 1, "TextToSpeech2", 1, 1);

namespace TextToSpeech2 {

static int KYTY_SYSV_ABI TextToSpeech2GetSpeechStatus() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2Cancel() {
	PRINT_NAME();

	return OK;
}

// PPSA17221 (Minecraft) imports these but Kyty only had GetSpeechStatus/Cancel.
// NID-only resolution (SymbolDatabase::FindByNid) means registering the NID here
// silences "Unresolved import stub" regardless of library/module version strings.
// Plain return-OK preserves current behavior (unresolved stubs return 0) without
// guessing output-buffer layouts. Narrator stays silent, game continues.
static int KYTY_SYSV_ABI TextToSpeech2Stub_UOjiprYwVNw() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2Stub_X0HZNbSiqyg() {
	PRINT_NAME();

	return OK;
}

} // namespace TextToSpeech2

LIB_DEFINE(InitTextToSpeech2_1) {
	LIB_FUNC("08JSg9p6bgQ", TextToSpeech2::TextToSpeech2GetSpeechStatus);
	LIB_FUNC("2jiIxUmcsGo", TextToSpeech2::TextToSpeech2Cancel);
	LIB_FUNC("UOjiprYwVNw", TextToSpeech2::TextToSpeech2Stub_UOjiprYwVNw);
	LIB_FUNC("X0HZNbSiqyg", TextToSpeech2::TextToSpeech2Stub_X0HZNbSiqyg);
}

} // namespace Libs
