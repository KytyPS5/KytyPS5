#include "common/abi.h"
#include "common/common.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("FontFt", 1, "FontFt", 1, 1);

namespace FontFt {

struct SelectionState {
	int value;
	int kind;
};

void* KYTY_SYSV_ABI FontSelectLibraryFt(int value) {
	PRINT_NAME();

	static SelectionState s_selection {};
	s_selection.value = value;
	s_selection.kind  = 0;

	return &s_selection;
}

void* KYTY_SYSV_ABI FontSelectRendererFt(int value) {
	PRINT_NAME();

	static SelectionState s_selection {};
	s_selection.value = value;
	s_selection.kind  = 1;

	return &s_selection;
}

} // namespace FontFt

LIB_DEFINE(InitFontFt_1) {
	LIB_FUNC("oM+XCzVG3oM", FontFt::FontSelectLibraryFt);
	LIB_FUNC("Xx974EW-QFY", FontFt::FontSelectRendererFt);
}

} // namespace Libs
