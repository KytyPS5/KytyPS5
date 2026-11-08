// Tests for the replies of the stand-in for EA's servers to a sign-in: the
// account services (eaNexus) and the Blaze calls of a signed-in session
// (eaAuthentication).

#include "libs/eaEmulation/eaAuthentication.h"
#include "libs/eaEmulation/eaNexus.h"
#include "libs/eaEmulation/eaPreAuth.h"
#include "libs/eaEmulation/eaTdf.h"

#include "common/logging/log.h"

#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// eaNexus logs a call it does not answer; the tests do not keep the log.
namespace Log {
void WriteToConsoleAndLog(std::string_view /*text*/) {}
} // namespace Log

namespace {

using namespace Libs::EaEmulation;

int g_failures = 0;

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    g_failures++;
  }
}

bool Has(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}

constexpr int64_t PLAYSTATION_ACCOUNT = 4123456789;

// An Authentication.login request as a Blaze client sends it, with made-up
// values.
std::vector<uint8_t> LoginRequest() {
  Writer w;
  w.BeginStruct(Tag("AIDS"));
  w.BeginStruct(Tag("EXID"));
  w.Integer(Tag("PSID"), PLAYSTATION_ACCOUNT);
  w.Integer(Tag("XBID"), 0);
  w.EndStruct();
  w.Integer(Tag("PLAT"), 22);
  w.EndStruct();
  w.Text(Tag("PRDT"), "game-2099-ps5");
  w.Text(Tag("TOKN"), "any token");
  return std::move(w.out);
}

// Each member's tag is after the one before, and the payload reads to its end.
bool InTagOrder(const std::vector<uint8_t> &payload) {
  Reader reader{payload.data(), payload.size()};
  uint32_t last = 0;
  while (reader.ok && reader.at < reader.size) {
    const uint32_t tag = reader.TagValue();
    if (tag <= last)
      return false;
    last = tag;
    reader.SkipValue(reader.Byte());
  }
  return reader.ok;
}

bool Decodes(const std::vector<uint8_t> &payload) {
  return !Has(DumpTdf(payload.data(), payload.size()), "undecoded");
}

void TestTheLoginReplyIsTheEmulatorsPersona() {
  const auto reply = LoginReply(LoginRequest(), "Player", 1700000000);
  const auto text = DumpTdf(reply.data(), reply.size());
  Check(Decodes(reply) && InTagOrder(reply),
        "the login reply decodes, in the order of its tags");
  Check(Has(text, "SESS") && Has(text, "PDTL"),
        "the reply has the session and the persona");
  Check(Has(text, "\"Player\""), "the persona has the display name");
  Check(Has(text, "BUID (type 0) " + std::to_string(EMULATED_PERSONA_ID)),
        "the blaze id is the persona's");
  Check(Has(text, "XREF (type 0) " + std::to_string(PLAYSTATION_ACCOUNT)),
        "the external id is the request's PlayStation account");
  Check(Has(text, "PSID (type 0) " + std::to_string(PLAYSTATION_ACCOUNT)),
        "the platform ids come back");
  Check(Has(text, "LLOG (type 0) 1700000000"),
        "the login time is the given time");
}

void TestTheLoginNotificationsAuthenticateThenAddTheUser() {
  const auto notifications =
      LoginNotifications(LoginRequest(), "Player", 1700000000);
  Check(notifications.size() == 2, "two notifications follow a login");
  if (notifications.size() != 2)
    return;
  Check(notifications[0].component == USER_SESSIONS_COMPONENT &&
            notifications[0].command == USER_SESSIONS_USER_AUTHENTICATED,
        "UserAuthenticated comes first");
  Check(notifications[1].component == USER_SESSIONS_COMPONENT &&
            notifications[1].command == USER_SESSIONS_USER_ADDED,
        "UserAdded comes second");
  for (const auto &notification : notifications) {
    Check(Decodes(notification.body) && InTagOrder(notification.body),
          "a notification decodes, in the order of its tags");
  }
  const auto added =
      DumpTdf(notifications[1].body.data(), notifications[1].body.size());
  Check(Has(added, "USER") && Has(added, "\"Player\"") &&
            Has(added, "EXID (type 0) " + std::to_string(PLAYSTATION_ACCOUNT)),
        "the added user is the persona, with the PlayStation account");
}

void TestTheSessionAnswersItsCallsAndNoOthers() {
  std::vector<std::pair<uint16_t, uint16_t>> answered = {
      {AUTHENTICATION_COMPONENT, AUTHENTICATION_LOGIN},
      {AUTHENTICATION_COMPONENT, AUTHENTICATION_GET_ACCOUNT},
      {UTIL_COMPONENT, UTIL_POST_AUTH},
      {UTIL_COMPONENT, UTIL_FETCH_CLIENT_CONFIG},
      {UTIL_COMPONENT, UTIL_FETCH_CLIENT_CONFIG_AUTHENTICATED},
      {CENSUS_DATA_COMPONENT, CENSUS_DATA_SUBSCRIBE},
  };
  for (const auto &call : EMPTY_REPLY_CALLS) {
    answered.emplace_back(call.component, call.command);
    std::vector<uint8_t> reply{1};
    std::vector<Notification> notifications;
    Check(SessionReply(call.component, call.command, {}, "Player", 0, reply,
                       notifications) &&
              reply.empty(),
          "a call of the empty-reply table gets an empty reply");
  }
  // The calls the games were seen to make after a login, each answered.
  for (const auto &[component, command] :
       std::vector<std::pair<uint16_t, uint16_t>>{
           {0x0009, 0x000c}, {0x0009, 0x0017}, {0x0007, 0x0014},
           {0x000a, 0x0005}, {0x000a, 0x0004}, {0x086c, 0x0003},
           {0x086c, 0x0004}, {0x0001, 0x0020}, {0x7802, 0x0032},
           {0x086e, 0x0001}, {0x0009, 0x001c}, {0x000f, 0x0002},
           {0x0019, 0x0006}, {0x000b, 0x0a28}, {0x08c9, 0x0001},
           {0x08c9, 0x0002}, {0x0007, 0x000f}, {0x0007, 0x0003},
           {0x08dd, 0x0001}, {0x08de, 0x0001}, {0x0001, 0x0046},
           {0x0009, 0x0005}, {0x0001, 0x001f}, {0x081c, 0x0003},
           {0x082b, 0x0025}, {0x0009, 0x0016}, {0x7802, 0x0008},
           {0x7802, 0x0014}}) {
    std::vector<uint8_t> reply;
    std::vector<Notification> notifications;
    Check(
        SessionReply(component, command, {}, "Player", 0, reply, notifications),
        "a call seen after login is answered");
  }
  for (const auto &[component, command] : answered) {
    std::vector<uint8_t> reply;
    std::vector<Notification> notifications;
    Check(SessionReply(component, command, LoginRequest(), "Player", 0, reply,
                       notifications),
          "a call of the session is answered");
    Check(Decodes(reply), "its reply decodes");
    Check(notifications.empty() || command == AUTHENTICATION_LOGIN,
          "only a login has notifications");
  }

  std::vector<uint8_t> reply;
  std::vector<Notification> notifications;
  Check(!SessionReply(UTIL_COMPONENT, UTIL_PRE_AUTH, {}, "Player", 0, reply,
                      notifications),
        "preAuth is not a call of the session");
  Check(!SessionReply(0x0007, 0x0001, {}, "Player", 0, reply, notifications),
        "an unknown call is not answered");
}

void TestTheAccountIsActiveAndNotUnderage() {
  const auto reply = GetAccountReply();
  const auto text = DumpTdf(reply.data(), reply.size());
  Check(Decodes(reply) && InTagOrder(reply),
        "the account decodes, in the order of its tags");
  Check(Has(text, "STAS (type 0) 1") && Has(text, "UDU (type 0) 0"),
        "the account is active and not underage");
  Check(Has(text, "UID (type 0) " + std::to_string(EMULATED_ACCOUNT_ID)),
        "the account id is the emulator's");
}

void TestTheCensusSubscriptionHasPeriods() {
  const auto reply = CensusSubscribeReply();
  const auto text = DumpTdf(reply.data(), reply.size());
  Check(Decodes(reply) && InTagOrder(reply),
        "the census reply decodes, in the order of its tags");
  Check(Has(text, "RTMT (type 0) 300000000"),
        "the client resubscribes after minutes, not at once");
}

void TestTheUtilRepliesDecode() {
  const auto post_auth = PostAuthReply();
  Check(Decodes(post_auth) && InTagOrder(post_auth),
        "postAuth decodes, in the order of its tags");
  const auto config = FetchConfigReply();
  Check(Decodes(config) && Has(DumpTdf(config.data(), config.size()), "CONF"),
        "the config reply has CONF");
}

// A protobuf length-delimited field: the key, the length, the bytes.
std::vector<uint8_t> Field(uint32_t field, const std::vector<uint8_t> &bytes) {
  std::vector<uint8_t> out;
  for (uint64_t key = (uint64_t{field} << 3u) | 2u;; key >>= 7) {
    out.push_back(
        static_cast<uint8_t>((key & 0x7fu) | (key >= 0x80 ? 0x80u : 0u)));
    if (key < 0x80)
      break;
  }
  out.push_back(static_cast<uint8_t>(bytes.size()));
  out.insert(out.end(), bytes.begin(), bytes.end());
  return out;
}

std::vector<uint8_t> Text(std::string_view text) {
  return {text.begin(), text.end()};
}

void TestTheAccountServiceValuesAreInWrappers() {
  std::vector<uint8_t> reply;
  const auto request = Field(1, Field(1, Text("GAME99")));

  Check(
      NexusReply("/eadp.nexus.connect.grpc.v1.AuthService/GetAuthForPs5Client",
                 request, reply),
      "GetAuthFor* is answered");
  // GetAuthResponse: field 1 is a StringValue, so the code is in field 1 of
  // field 1.
  Check(reply.size() > 4 && reply[0] == 0x0a && reply[2] == 0x0a,
        "the code is a StringValue in field 1");

  Check(NexusReply("/eadp.nexus.connect.grpc.v1.TokenService/"
                   "GrantTokenByAuthorizationCode",
                   request, reply),
        "GrantTokenByAuthorizationCode is answered");
  const auto grant = DescribeProtobuf(reply);
  Check(Has(grant, "1: {") && Has(grant, "2: {") && Has(grant, "3: {"),
        "the token, its type and lifetime are wrapped");

  Check(NexusReply("/eadp.nexus.connect.grpc.v1.TokenInfoService/GetTokenInfo",
                   request, reply),
        "GetTokenInfo is answered");
  const auto info = DescribeProtobuf(reply);
  Check(Has(info, "\"GAME99\""), "the token's client is the request's");
  for (const char *field : {"401: {", "402: {", "501: {", "2053: {", "3131: {",
                            "4004: {", "8008: {"}) {
    Check(Has(info, field),
          "the token info has the account fields the game reads");
  }
  Check(Has(info, std::string("\"") + EMULATED_ACCOUNT_ID_TEXT + "\""),
        "the account id is the emulator's");

  Check(!NexusReply("/eadp.nexus.connect.grpc.v1.Other/Call", request, reply),
        "an unknown call is not answered");
}

} // namespace

int main() {
  TestTheLoginReplyIsTheEmulatorsPersona();
  TestTheLoginNotificationsAuthenticateThenAddTheUser();
  TestTheSessionAnswersItsCallsAndNoOthers();
  TestTheAccountIsActiveAndNotUnderage();
  TestTheUtilRepliesDecode();
  TestTheCensusSubscriptionHasPeriods();
  TestTheAccountServiceValuesAreInWrappers();
  if (g_failures != 0) {
    std::cerr << g_failures << " EA emulation authentication check(s) failed\n";
    return 1;
  }
  std::cout << "EA emulation authentication tests passed\n";
  return 0;
}
