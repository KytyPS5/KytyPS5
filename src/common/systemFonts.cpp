#include "common/systemFonts.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <system_error>

namespace Common {

namespace {

void AppendIfFile(std::vector<std::filesystem::path>& out, const std::filesystem::path& path) {
	if (path.empty() || std::find(out.begin(), out.end(), path) != out.end()) {
		return;
	}
	std::error_code ec;
	if (std::filesystem::is_regular_file(path, ec) && !ec) {
		out.push_back(path);
	}
}

void AppendAll(std::vector<std::filesystem::path>& out, const std::filesystem::path& dir,
               std::initializer_list<const char*> names) {
	for (const char* name: names) {
		AppendIfFile(out, dir / name);
	}
}

} // namespace

std::vector<std::filesystem::path> FindCjkFontFiles(std::string_view locale) {
	std::vector<std::filesystem::path> result;

	if (const char* env = std::getenv("KYTY_CJK_FONT"); env != nullptr && *env != '\0') {
		AppendIfFile(result, std::filesystem::path(env));
	}

#if defined(_WIN32)
	std::filesystem::path dir = "C:\\Windows\\Fonts";
	if (const char* windir = std::getenv("WINDIR"); windir != nullptr && *windir != '\0') {
		dir = std::filesystem::path(windir) / "Fonts";
	}
	if (locale == "zh-Hant") {
		AppendAll(result, dir, {"msjh.ttc", "msjh.ttf", "mingliu.ttc"});
	} else if (locale == "ja-JP") {
		AppendAll(result, dir, {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"});
	} else if (locale == "ko-KR") {
		AppendAll(result, dir, {"malgun.ttf", "gulim.ttc"});
	}
	AppendAll(result, dir,
	          {"msyh.ttc", "msyh.ttf", "Deng.ttf", "simhei.ttf", "simsun.ttc", "msjh.ttc",
	           "YuGothM.ttc", "meiryo.ttc", "malgun.ttf"});
#elif defined(__APPLE__)
	const std::filesystem::path dir = "/System/Library/Fonts";
	if (locale == "ja-JP") {
		AppendAll(result, dir, {"ヒラギノ角ゴシック W3.ttc", "Hiragino Sans GB.ttc"});
	} else if (locale == "ko-KR") {
		AppendAll(result, dir, {"AppleSDGothicNeo.ttc"});
	}
	AppendAll(result, dir,
	          {"PingFang.ttc", "Hiragino Sans GB.ttc", "STHeiti Medium.ttc", "STHeiti Light.ttc"});
	AppendIfFile(result, "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
	AppendIfFile(result, "/Library/Fonts/Arial Unicode.ttf");
#else
	for (const char* path: {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
	                        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
	                        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
	                        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
	                        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
	                        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
	                        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
	                        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"}) {
		AppendIfFile(result, path);
	}
	(void)locale;
#endif

	return result;
}

} // namespace Common
