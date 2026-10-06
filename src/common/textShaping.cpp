#include "common/textShaping.h"

#include <cstdint>
#include <vector>

namespace Common::Trophies {
namespace {

struct ArabicForms {
	char32_t letter;
	char32_t isolated;
	char32_t final_form;
	char32_t initial; // 0 for letters that never join to the next one
	char32_t medial;
};

constexpr ArabicForms ARABIC_FORMS[] = {
    {0x0621, 0xFE80, 0, 0, 0},           {0x0622, 0xFE81, 0xFE82, 0, 0},
    {0x0623, 0xFE83, 0xFE84, 0, 0},      {0x0624, 0xFE85, 0xFE86, 0, 0},
    {0x0625, 0xFE87, 0xFE88, 0, 0},      {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},
    {0x0627, 0xFE8D, 0xFE8E, 0, 0},      {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92},
    {0x0629, 0xFE93, 0xFE94, 0, 0},      {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98},
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8},
    {0x062F, 0xFEA9, 0xFEAA, 0, 0},      {0x0630, 0xFEAB, 0xFEAC, 0, 0},
    {0x0631, 0xFEAD, 0xFEAE, 0, 0},      {0x0632, 0xFEAF, 0xFEB0, 0, 0},
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC}, {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0},
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4}, {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8},
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, {0x0648, 0xFEED, 0xFEEE, 0, 0},
    {0x0649, 0xFEEF, 0xFEF0, 0, 0},      {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},
};

const ArabicForms* FindForms(char32_t c) {
	for (const auto& forms: ARABIC_FORMS) {
		if (forms.letter == c) {
			return &forms;
		}
	}
	return nullptr;
}

bool IsMark(char32_t c) {
	return c >= 0x064B && c <= 0x065F;
}

bool IsArabic(char32_t c) {
	return (c >= 0x0600 && c <= 0x06FF) || (c >= 0xFB50 && c <= 0xFDFF) ||
	       (c >= 0xFE70 && c <= 0xFEFF);
}

// Latin letters, digits and other non-Arabic text that must keep its order inside a right-to-left line.
bool IsLtrText(char32_t c) {
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= 0x00C0 && c < 0x0600);
}

std::vector<char32_t> Decode(std::string_view text) {
	std::vector<char32_t> result;
	for (size_t i = 0; i < text.size();) {
		const auto   byte = static_cast<unsigned char>(text[i]);
		const size_t length = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xE ? 3 : 4;
		char32_t     c = length == 1 ? byte : byte & (0xFF >> (length + 1));
		for (size_t k = 1; k < length && i + k < text.size(); ++k) {
			c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
		}
		result.push_back(c);
		i += length;
	}
	return result;
}

void Append(std::string& out, char32_t c) {
	if (c < 0x80) {
		out.push_back(static_cast<char>(c));
	} else if (c < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (c >> 6)));
		out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	} else if (c < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (c >> 12)));
		out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (c >> 18)));
		out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
	}
}

// Lam followed by an alef is drawn as one ligature glyph (isolated form, final form).
bool LamAlef(char32_t alef, char32_t& isolated, char32_t& final_form) {
	switch (alef) {
		case 0x0622: isolated = 0xFEF5; final_form = 0xFEF6; return true;
		case 0x0623: isolated = 0xFEF7; final_form = 0xFEF8; return true;
		case 0x0625: isolated = 0xFEF9; final_form = 0xFEFA; return true;
		case 0x0627: isolated = 0xFEFB; final_form = 0xFEFC; return true;
		default: return false;
	}
}

std::vector<char32_t> Shape(const std::vector<char32_t>& in) {
	std::vector<char32_t> out;
	// Each entry of 'joins_next' tells whether the previous output letter connects to what follows.
	bool prev_joins_next = false;
	for (size_t i = 0; i < in.size(); ++i) {
		const auto c = in[i];
		if (IsMark(c)) {
			out.push_back(c);
			continue;
		}
		const auto* forms = FindForms(c);
		if (forms == nullptr) {
			out.push_back(c);
			prev_joins_next = false;
			continue;
		}
		// Next letter that takes part in joining, skipping marks.
		size_t next = i + 1;
		while (next < in.size() && IsMark(in[next])) {
			++next;
		}
		char32_t lig_iso = 0;
		char32_t lig_fin = 0;
		if (c == 0x0644 && next < in.size() && LamAlef(in[next], lig_iso, lig_fin)) {
			out.push_back(prev_joins_next ? lig_fin : lig_iso);
			for (size_t k = i + 1; k < next; ++k) {
				out.push_back(in[k]);
			}
			i               = next;
			prev_joins_next = false;
			continue;
		}
		const bool joins_next_possible = forms->initial != 0;
		const bool next_joins =
		    joins_next_possible && next < in.size() && FindForms(in[next]) != nullptr &&
		    FindForms(in[next])->final_form != 0;
		char32_t shaped = forms->isolated;
		if (prev_joins_next && forms->final_form != 0) {
			shaped = next_joins ? forms->medial : forms->final_form;
		} else if (next_joins) {
			shaped = forms->initial;
		}
		out.push_back(shaped);
		prev_joins_next = next_joins;
	}
	return out;
}

} // namespace

std::string PrepareDisplayText(std::string_view utf8) {
	auto cps = Decode(utf8);
	bool arabic = false;
	for (const auto c: cps) {
		arabic = arabic || IsArabic(c);
	}
	if (!arabic) {
		return std::string(utf8);
	}
	cps = Shape(cps);

	// Walk backwards so the line reads right to left. Runs of Latin text and digits keep their order.
	std::string out;
	size_t      end = cps.size();
	while (end > 0) {
		size_t begin = end - 1;
		while (begin > 0 && IsMark(cps[begin])) {
			--begin;
		}
		if (IsLtrText(cps[begin])) {
			while (begin > 0 && IsLtrText(cps[begin - 1])) {
				--begin;
			}
		}
		for (size_t k = begin; k < end; ++k) {
			Append(out, cps[k]);
		}
		end = begin;
	}
	return out;
}

} // namespace Common::Trophies
