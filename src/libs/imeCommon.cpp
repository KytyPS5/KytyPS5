#include "libs/imeCommon.h"

#include <algorithm>
#include <vector>

namespace Libs::ImeCommon {
namespace {

constexpr bool IsHighSurrogate(char16_t value) {
	return value >= 0xd800 && value <= 0xdbff;
}

constexpr bool IsLowSurrogate(char16_t value) {
	return value >= 0xdc00 && value <= 0xdfff;
}

char16_t HidCharacter(uint16_t keycode, uint32_t status) {
	const bool shift = (status & 0x00002200) != 0;
	const bool caps  = (status & 0x00020000) != 0;
	if (keycode >= 4 && keycode <= 29) {
		return static_cast<char16_t>(((shift != caps) ? u'A' : u'a') + keycode - 4);
	}
	if (keycode >= 30 && keycode <= 39) {
		static constexpr char16_t plain[]   = u"1234567890";
		static constexpr char16_t shifted[] = u"!@#$%^&*()";
		return (shift ? shifted : plain)[keycode - 30];
	}
	if (keycode == 44) {
		return u' ';
	}
	if (keycode >= 45 && keycode <= 56) {
		static constexpr char16_t plain[]   = u"-=[]\\#;'`,./";
		static constexpr char16_t shifted[] = u"_+{}|~:\"~<>?";
		return (shift ? shifted : plain)[keycode - 45];
	}
	if (keycode >= 89 && keycode <= 98) {
		static constexpr char16_t keypad[] = u"1234567890";
		return keypad[keycode - 89];
	}
	return keycode == 99 ? u'.' : u'\0';
}

} // namespace

bool IsValidUtf16(std::u16string_view text) {
	for (size_t i = 0; i < text.size(); i++) {
		if (IsHighSurrogate(text[i])) {
			if (++i >= text.size() || !IsLowSurrogate(text[i])) {
				return false;
			}
		} else if (IsLowSurrogate(text[i])) {
			return false;
		}
	}
	return true;
}

bool IsValidInputText(std::u16string_view text, bool multiline) {
	if (!IsValidUtf16(text)) {
		return false;
	}
	return std::none_of(text.begin(), text.end(), [multiline](char16_t value) {
		return value == u'\0' || (!multiline && (value == u'\n' || value == u'\r'));
	});
}

bool IsAllowedInput(char16_t value, Type type, uint32_t option) {
	if (value == u'\r' || value == u'\0') {
		return false;
	}
	if (value == u'\n') {
		return (option & OPTION_MULTILINE) != 0;
	}
	if (type == Type::Number) {
		return (value >= u'0' && value <= u'9') || value == u',' || value == u'-' || value == u'.';
	}
	if (type == Type::BasicLatin) {
		return value >= u' ' && value <= u'~';
	}
	return true;
}

uint32_t NormalizeCursor(std::u16string_view text, uint32_t cursor) {
	cursor = std::min<uint32_t>(cursor, static_cast<uint32_t>(text.size()));
	if (cursor > 0 && cursor < text.size() && IsHighSurrogate(text[cursor - 1]) &&
	    IsLowSurrogate(text[cursor])) {
		cursor++;
	}
	return cursor;
}

void ClampText(std::u16string* text, uint32_t limit) {
	if (text->size() <= limit) {
		return;
	}
	text->resize(limit);
	if (!text->empty() && IsHighSurrogate(text->back())) {
		text->pop_back();
	}
}

EditDelta ComputeEditDelta(std::u16string_view before, std::u16string_view after) {
	size_t prefix = 0;
	while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) {
		prefix++;
	}
	size_t before_tail = before.size();
	size_t after_tail  = after.size();
	while (before_tail > prefix && after_tail > prefix &&
	       before[before_tail - 1] == after[after_tail - 1]) {
		before_tail--;
		after_tail--;
	}
	return {static_cast<uint32_t>(prefix),
	        static_cast<int32_t>(after_tail - prefix) - static_cast<int32_t>(before_tail - prefix)};
}

bool RunTextFilter(TextFilter filter, const std::u16string& source, uint32_t output_capacity,
                   std::u16string* output) {
	if (filter == nullptr || output == nullptr) {
		return false;
	}
	std::vector<char16_t> buffer(static_cast<size_t>(output_capacity) + 1, u'\0');
	uint32_t              output_length = output_capacity;
	if (filter(buffer.data(), &output_length, source.c_str(),
	           static_cast<uint32_t>(source.size())) != 0 ||
	    output_length > output_capacity ||
	    !IsValidUtf16(std::u16string_view(buffer.data(), output_length))) {
		return false;
	}
	output->assign(buffer.data(), buffer.data() + output_length);
	return true;
}

bool ApplyKeyboardFilterOutput(ExternalInput* input, uint16_t keycode, uint32_t status,
                               bool multiline) {
	constexpr uint32_t KEYCODE_VALID   = 0x00000001;
	constexpr uint32_t CHARACTER_VALID = 0x00000002;
	if (keycode == input->key.keycode && status == input->key.status) {
		return true;
	}
	if ((status & KEYCODE_VALID) == 0 || keycode == 0) {
		return input->action == ExternalAction::Text && (status & CHARACTER_VALID) != 0;
	}
	input->key.keycode = keycode;
	input->key.status  = status;
	input->text.clear();
	switch (keycode) {
		case 40:
		case 88:
		case 158:
			input->action = multiline ? ExternalAction::Newline : ExternalAction::Accept;
			break;
		case 41: input->action = ExternalAction::Cancel; break;
		case 42:
		case 187: input->action = ExternalAction::Backspace; break;
		case 43:
		case 186: input->action = ExternalAction::None; break;
		case 79: input->action = ExternalAction::MoveRight; break;
		case 80: input->action = ExternalAction::MoveLeft; break;
		default: {
			const char16_t character = HidCharacter(keycode, status);
			if (character == u'\0') {
				return false;
			}
			input->action        = ExternalAction::Text;
			input->key.character = character;
			input->text.push_back(character);
			break;
		}
	}
	return true;
}

void TextEditEngine::Reset(Type type, uint32_t option, uint32_t max_length, std::u16string text) {
	m_type       = type;
	m_option     = option;
	m_max_length = max_length;
	ClampText(&text, max_length);
	m_text   = std::move(text);
	m_cursor = static_cast<uint32_t>(m_text.size());
}

bool TextEditEngine::Insert(std::u16string_view text, EditDelta* edit) {
	if (text.empty() || !IsValidUtf16(text)) {
		return false;
	}
	std::u16string allowed;
	allowed.reserve(text.size());
	for (const char16_t value: text) {
		if (IsAllowedInput(value, m_type, m_option)) {
			allowed.push_back(value);
		}
	}
	const size_t available = m_max_length - m_text.size();
	if (allowed.empty() || available == 0) {
		return false;
	}
	allowed.resize(std::min(allowed.size(), available));
	if (!allowed.empty() && IsHighSurrogate(allowed.back())) {
		allowed.pop_back();
	}
	if (allowed.empty()) {
		return false;
	}
	if (edit != nullptr) {
		*edit = {m_cursor, static_cast<int32_t>(allowed.size())};
	}
	m_text.insert(m_cursor, allowed);
	m_cursor += static_cast<uint32_t>(allowed.size());
	return true;
}

bool TextEditEngine::Backspace(EditDelta* edit) {
	if (m_cursor == 0) {
		return false;
	}
	uint32_t first = m_cursor - 1;
	if (first > 0 && IsLowSurrogate(m_text[first]) && IsHighSurrogate(m_text[first - 1])) {
		first--;
	}
	if (edit != nullptr) {
		*edit = {first, -static_cast<int32_t>(m_cursor - first)};
	}
	m_text.erase(first, m_cursor - first);
	m_cursor = first;
	return true;
}

bool TextEditEngine::MoveCursor(int delta) {
	if (delta == 0) {
		return false;
	}
	int next = std::clamp(static_cast<int>(m_cursor) + delta, 0, static_cast<int>(m_text.size()));
	if (delta < 0 && next > 0 && next < static_cast<int>(m_text.size()) &&
	    IsLowSurrogate(m_text[next]) && IsHighSurrogate(m_text[next - 1])) {
		next--;
	} else if (delta > 0 && next > 0 && next < static_cast<int>(m_text.size()) &&
	           IsHighSurrogate(m_text[next - 1]) && IsLowSurrogate(m_text[next])) {
		next++;
	}
	if (next == static_cast<int>(m_cursor)) {
		return false;
	}
	m_cursor = static_cast<uint32_t>(next);
	return true;
}

void TextEditEngine::ReplaceText(std::u16string text, uint32_t cursor) {
	ClampText(&text, m_max_length);
	m_text   = std::move(text);
	m_cursor = NormalizeCursor(m_text, cursor);
}

void TextEditEngine::SetCursor(uint32_t cursor) {
	m_cursor = NormalizeCursor(m_text, cursor);
}

} // namespace Libs::ImeCommon
