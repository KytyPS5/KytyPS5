#include "common/abi.h"
#include "graphics/presentation/videoOut.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

namespace LibGen5 {

LIB_VERSION("VideoOut", 1, "VideoOut", 1, 1);

LIB_DEFINE(InitVideoOut_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("Up36PTk687E", VideoOut::VideoOutOpen);
	LIB_FUNC("uquVH4-Du78", VideoOut::VideoOutClose);
	LIB_FUNC("PjS5uASwcV8", VideoOut::VideoOutSetBufferAttribute2);
	LIB_FUNC("rKBUtgRrtbk", VideoOut::VideoOutRegisterBuffers2);
	LIB_FUNC("HuViW4HnrOw", VideoOut::VideoOutSubmitChangeBufferAttribute2);
	LIB_FUNC("N5KDtkIjjJ4", VideoOut::VideoOutUnregisterBuffers);
	LIB_FUNC("CBiu4mCE1DA", VideoOut::VideoOutSetFlipRate);
	LIB_FUNC("HXzjK9yI30k", VideoOut::VideoOutAddFlipEvent);
	LIB_FUNC("Xru92wHJRmg", VideoOut::VideoOutAddVblankEvent);
	LIB_FUNC("keipklF0pMY", VideoOut::VideoOutAddPreVblankStartEvent);
	LIB_FUNC("-Ozn0F1AFRg", VideoOut::VideoOutDeleteFlipEvent);
	LIB_FUNC("oNOQn3knW6s", VideoOut::VideoOutDeleteVblankEvent);
	LIB_FUNC("elWQ9vERF-Q", VideoOut::VideoOutDeletePreVblankStartEvent);
	LIB_FUNC("U46NwOiJpys", VideoOut::VideoOutSubmitFlip);
	LIB_FUNC("SbU3dwp80lQ", VideoOut::VideoOutGetFlipStatus);
	LIB_FUNC("zgXifHT9ErY", VideoOut::VideoOutIsFlipPending);
	LIB_FUNC("1FZBKy8HeNU", VideoOut::VideoOutGetVblankStatus);
	LIB_FUNC("MTxxrOCeSig", VideoOut::VideoOutSetWindowModeMargins);
	LIB_FUNC("kmSe30JTs+E", VideoOut::VideoOutAddOutputModeEvent);
	LIB_FUNC("U2JJtSqNKZI", VideoOut::VideoOutGetEventId);
	LIB_FUNC("rWUTcKdkUzQ", VideoOut::VideoOutGetEventData);
	LIB_FUNC("Mt4QHHkxkOc", VideoOut::VideoOutGetEventCount);
	LIB_FUNC("j6RaAUlaLv0", VideoOut::VideoOutWaitVblank);
	LIB_FUNC("utPrVdxio-8", VideoOut::VideoOutGetOutputStatus);
	LIB_FUNC("+I4K03i3EL0", VideoOut::VideoOutInitializeOutputOptions);
	LIB_FUNC("Nv8c-Kb+DUM", VideoOut::VideoOutIsOutputSupported);
	LIB_FUNC("w0hLuNarQxY", VideoOut::VideoOutConfigureOutput);
	LIB_FUNC("eb-gvTYQcoY", VideoOut::VideoOutLatencyControlWaitBeforeInput);
	LIB_FUNC("MCJ8SkzsQxY", VideoOut::VideoOutLatencyMeasureSetStartPoint);
	LIB_FUNC("DYhhWbJSeRg", VideoOut::VideoOutColorSettingsSetGamma);
	LIB_FUNC("pv9CI5VC+R0", VideoOut::VideoOutAdjustColor);
}

} // namespace LibGen5

LIB_DEFINE(InitVideoOut_1) {
	LibGen5::InitVideoOut_1(s);
}

} // namespace Libs
