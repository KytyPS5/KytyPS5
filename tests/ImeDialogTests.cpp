#include "libs/imeDialog.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace Ime = Libs::Dialog::ImeDialog;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

namespace {

constexpr int Error(uint32_t value) { return static_cast<int32_t>(value); }

int g_filter_calls = 0;
int g_keyboard_filter_calls = 0;
bool g_block_keyboard = false;
uint16_t g_remap_keyboard = 0;

int32_t KYTY_SYSV_ABI CopyFilter(char16_t *out_text, uint32_t *out_length,
                                 const char16_t *source,
                                 uint32_t source_length) {
  g_filter_calls++;
  CHECK(*out_length >= source_length);
  std::memcpy(out_text, source, source_length * sizeof(char16_t));
  *out_length = source_length;
  return 0;
}

int32_t KYTY_SYSV_ABI SupplementaryFilter(char16_t *out_text,
                                          uint32_t *out_length,
                                          const char16_t *, uint32_t) {
  CHECK(*out_length >= 2);
  out_text[0] = 0xd83d;
  out_text[1] = 0xde00;
  *out_length = 2;
  return 0;
}

int KYTY_SYSV_ABI KeyboardFilter(const Ime::Keycode *source,
                                 uint16_t *out_keycode, uint32_t *out_status,
                                 void *) {
  g_keyboard_filter_calls++;
  if (g_block_keyboard) {
    *out_keycode = 0;
    *out_status = 0;
    return 0;
  }
  if (g_remap_keyboard != 0) {
    *out_keycode = g_remap_keyboard;
    *out_status = source->status;
    return 0;
  }
  return -1;
}

Ime::Param MakeParam(char16_t *text) {
  Ime::Param param{};
  param.user_id = 1000;
  param.type = Ime::Type::Default;
  param.supported_languages = 1;
  param.enter_label = Ime::EnterLabel::Default;
  param.input_text_buffer = text;
  param.max_text_length = 31;
  return param;
}

void TestLayoutAndPanelSize() {
  static_assert(sizeof(Ime::Param) == 0x60);
  static_assert(sizeof(Ime::ExtendedParam) == 0x88);
  static_assert(sizeof(Ime::Result) == 0x10);
  static_assert(sizeof(Ime::PositionAndForm) == 0x1c);

  std::array<char16_t, 32> text{};
  auto param = MakeParam(text.data());
  uint32_t width = 0;
  uint32_t height = 0;
  CHECK(Ime::ImeDialogGetPanelSize(&param, &width, &height) == 0);
  CHECK(width == 793 && height == 528);
  param.option = Ime::OPTION_MULTILINE;
  CHECK(Ime::ImeDialogGetPanelSize(&param, &width, &height) == 0);
  CHECK(width == 793 && height == 628);
  param.option = 0;
  param.type = Ime::Type::Number;
  CHECK(Ime::ImeDialogGetPanelSize(&param, &width, &height) == 0);
  CHECK(width == 370 && height == 522);

  param = MakeParam(text.data());
  param.option = Ime::OPTION_EXT_KEYBOARD;
  Ime::ExtendedParam extended{};
  extended.option = 0x00000400;
  CHECK(Ime::ImeDialogGetPanelSizeExtended(&param, &extended, &width,
                                           &height) == 0);
  CHECK(width == 793 && height == 168);
  param.type = Ime::Type::BasicLatin;
  CHECK(Ime::ImeDialogGetPanelSizeExtended(&param, &extended, &width,
                                           &height) == 0);
  CHECK(width == 793 && height == 103);
}

void TestAcceptLifecycle() {
  std::array<char16_t, 32> text{u'A', u'l', u'i', u'c', u'e'};
  auto param = MakeParam(text.data());
  CHECK(Ime::ImeDialogInit(&param, nullptr) == 0);
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Running));
  CHECK(Ime::ImeDialogInit(nullptr, nullptr) == Error(0x80bc0001));

  Ime::HostSnapshot snapshot;
  CHECK(Ime::GetHostSnapshot(&snapshot));
  CHECK(snapshot.text == u"Alice");
  CHECK(Ime::HostInsertText(snapshot.generation, u" 2"));
  CHECK(Ime::HostBackspace(snapshot.generation));
  CHECK(Ime::HostAccept(snapshot.generation));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Finished));
  CHECK(std::u16string(text.data()) == u"Alice ");

  Ime::Result result{};
  CHECK(Ime::ImeDialogGetResult(&result) == 0);
  CHECK(result.endstatus == Ime::EndStatus::Ok);
  CHECK(Ime::ImeDialogTerm() == 0);
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::None));
}

void TestCancelAndAbortRestoreText() {
  for (const auto expected :
       {Ime::EndStatus::UserCanceled, Ime::EndStatus::Aborted}) {
    std::array<char16_t, 32> text{u'o', u'l', u'd'};
    auto param = MakeParam(text.data());
    CHECK(Ime::ImeDialogInit(&param, nullptr) == 0);
    Ime::HostSnapshot snapshot;
    CHECK(Ime::GetHostSnapshot(&snapshot));
    CHECK(Ime::HostInsertText(snapshot.generation, u" value"));
    if (expected == Ime::EndStatus::UserCanceled) {
      CHECK(Ime::HostCancel(snapshot.generation));
    } else {
      CHECK(Ime::ImeDialogAbort() == 0);
    }
    Ime::Result result{};
    CHECK(Ime::ImeDialogGetResult(&result) == 0);
    CHECK(result.endstatus == expected);
    CHECK(std::u16string(text.data()) == u"old");
    CHECK(Ime::ImeDialogTerm() == 0);
  }
}

void TestValidation() {
  std::array<char16_t, 32> text{};
  auto param = MakeParam(text.data());
  param.reserved[0] = 1;
  CHECK(Ime::ImeDialogInit(&param, nullptr) == Error(0x80bc0032));
  param.reserved[0] = 0;
  param.max_text_length = Ime::IME_DIALOG_MAX_TEXT_LENGTH + 1;
  CHECK(Ime::ImeDialogInit(&param, nullptr) == Error(0x80bc0016));
  CHECK(Ime::ImeDialogGetResult(nullptr) == Error(0x80bc0107));

  param = MakeParam(text.data());
  Ime::ExtendedParam extended{};
  extended.ext_keyboard_mode = 0x00000004;
  CHECK(Ime::ImeDialogInit(&param, &extended) == Error(0x80bc001c));
  extended = {};
  extended.option = 0x00000200;
  extended.disable_device =
      Ime::DISABLE_DEVICE_CONTROLLER | Ime::DISABLE_DEVICE_EXT_KEYBOARD;
  CHECK(Ime::ImeDialogInit(&param, &extended) == 0);
  Ime::HostSnapshot snapshot;
  CHECK(Ime::GetHostSnapshot(&snapshot));
  CHECK(snapshot.disable_device == 3);
  CHECK(Ime::ImeDialogAbort() == 0);
  Ime::Result result{};
  CHECK(Ime::ImeDialogGetResult(&result) == 0);
  CHECK(Ime::ImeDialogTerm() == 0);
}

void TestFilteringAndInputPolicy() {
  std::array<char16_t, 32> text{u'1'};
  auto param = MakeParam(text.data());
  param.type = Ime::Type::Number;
  param.filter = CopyFilter;
  g_filter_calls = 0;
  CHECK(Ime::ImeDialogInit(&param, nullptr) == 0);
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Running));
  CHECK(g_filter_calls == 0);
  CHECK(Ime::ImeDialogGetResult(nullptr) == Error(0x80bc0031));
  CHECK(Ime::ImeDialogTerm() == Error(0x80bc0106));
  CHECK(g_filter_calls == 0);

  Ime::HostSnapshot snapshot;
  CHECK(Ime::GetHostSnapshot(&snapshot));
  CHECK(Ime::HostInsertText(snapshot.generation, u"a2.\n"));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Running));
  CHECK(g_filter_calls == 1);
  CHECK(std::u16string(text.data()) == u"12.");
  CHECK(Ime::HostCancel(snapshot.generation));
  Ime::Result result{};
  CHECK(Ime::ImeDialogGetResult(&result) == 0);
  CHECK(Ime::ImeDialogTerm() == 0);
}

void TestExternalKeyboardFilter() {
  std::array<char16_t, 32> text{};
  auto param = MakeParam(text.data());
  Ime::ExtendedParam extended{};
  extended.ext_keyboard_filter = KeyboardFilter;
  CHECK(Ime::ImeDialogInit(&param, &extended) == 0);
  Ime::HostSnapshot snapshot;
  CHECK(Ime::GetHostSnapshot(&snapshot));

  Ime::ExternalInput input{};
  input.key.status = 3;
  input.key.keycode = 4;
  input.action = Ime::ExternalAction::Text;
  input.text = u"x";
  g_keyboard_filter_calls = 0;
  g_block_keyboard = true;
  CHECK(Ime::HostQueueExternalInput(snapshot.generation, input));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Running));
  CHECK(g_keyboard_filter_calls == 1);
  CHECK(text[0] == u'\0');

  g_block_keyboard = false;
  CHECK(Ime::HostQueueExternalInput(snapshot.generation, std::move(input)));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Running));
  CHECK(g_keyboard_filter_calls == 2);
  CHECK(std::u16string(text.data()) == u"x");

  input = {};
  input.key.status = 3;
  input.key.keycode = 4;
  input.action = Ime::ExternalAction::Text;
  input.text = u"x";
  g_remap_keyboard = 5;
  CHECK(Ime::HostQueueExternalInput(snapshot.generation, std::move(input)));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Running));
  CHECK(g_keyboard_filter_calls == 3);
  CHECK(std::u16string(text.data()) == u"xb");

  input = {};
  input.key.status = 0x00000201;
  input.key.keycode = 4;
  input.action = Ime::ExternalAction::Text;
  input.text = u"x";
  g_remap_keyboard = 50;
  CHECK(Ime::HostQueueExternalInput(snapshot.generation, std::move(input)));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Running));
  CHECK(g_keyboard_filter_calls == 4);
  CHECK(std::u16string(text.data()) == u"xb~");

  input = {};
  input.key.status = 3;
  input.key.keycode = 4;
  input.action = Ime::ExternalAction::Text;
  input.text = u"x";
  g_remap_keyboard = 88;
  CHECK(Ime::HostQueueExternalInput(snapshot.generation, std::move(input)));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Finished));
  CHECK(g_keyboard_filter_calls == 5);
  Ime::Result result{};
  CHECK(Ime::ImeDialogGetResult(&result) == 0);
  CHECK(result.endstatus == Ime::EndStatus::Ok);
  CHECK(Ime::ImeDialogTerm() == 0);

  g_remap_keyboard = 0;
  CHECK(Ime::ImeDialogInit(&param, &extended) == 0);
  CHECK(Ime::GetHostSnapshot(&snapshot));

  input = {};
  input.key.status = 1;
  input.key.keycode = 41;
  input.action = Ime::ExternalAction::Cancel;
  CHECK(Ime::HostQueueExternalInput(snapshot.generation, std::move(input)));
  input = {};
  input.key.status = 3;
  input.key.keycode = 4;
  input.action = Ime::ExternalAction::Text;
  input.text = u"ignored";
  CHECK(Ime::HostQueueExternalInput(snapshot.generation, std::move(input)));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Finished));
  CHECK(g_keyboard_filter_calls == 6);
  result = {};
  CHECK(Ime::ImeDialogGetResult(&result) == 0);
  CHECK(Ime::ImeDialogTerm() == 0);
}

void TestFilteredCursorBoundary() {
  std::array<char16_t, 8> text{};
  auto param = MakeParam(text.data());
  param.filter = SupplementaryFilter;
  CHECK(Ime::ImeDialogInit(&param, nullptr) == 0);
  Ime::HostSnapshot snapshot;
  CHECK(Ime::GetHostSnapshot(&snapshot));
  CHECK(Ime::HostInsertText(snapshot.generation, u"x"));
  CHECK(Ime::ImeDialogGetStatus() == static_cast<int>(Ime::Status::Running));
  CHECK(Ime::GetHostSnapshot(&snapshot));
  CHECK(snapshot.cursor == 2);
  CHECK(text[0] == 0xd83d && text[1] == 0xde00 && text[2] == 0);
  CHECK(Ime::HostCancel(snapshot.generation));
  Ime::Result result{};
  CHECK(Ime::ImeDialogGetResult(&result) == 0);
  CHECK(Ime::ImeDialogTerm() == 0);
}

} // namespace

int main() {
  TestLayoutAndPanelSize();
  TestAcceptLifecycle();
  TestCancelAndAbortRestoreText();
  TestValidation();
  TestFilteringAndInputPolicy();
  TestExternalKeyboardFilter();
  TestFilteredCursorBoundary();
  return 0;
}
