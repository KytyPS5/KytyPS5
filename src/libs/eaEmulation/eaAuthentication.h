#ifndef EMULATOR_SRC_LIBS_EA_EMULATION_AUTHENTICATION_H_
#define EMULATOR_SRC_LIBS_EA_EMULATION_AUTHENTICATION_H_

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::EaEmulation {

// The EA account of the emulator: one account with one persona, the same for every game. The
// account services (eaNexus.h) and the Blaze server give the same numbers.
inline constexpr int64_t     EMULATED_ACCOUNT_ID      = 1000000000001;
inline constexpr int64_t     EMULATED_PERSONA_ID      = 1000000000001;
inline constexpr const char* EMULATED_ACCOUNT_ID_TEXT = "1000000000001";

// Blaze component 1 (Authentication) and the commands the server answers.
inline constexpr uint16_t AUTHENTICATION_COMPONENT   = 0x0001;
inline constexpr uint16_t AUTHENTICATION_LOGIN       = 0x000a;
inline constexpr uint16_t AUTHENTICATION_GET_ACCOUNT = 0x001e;

// The reply to Authentication.login: the session of the emulator's persona (LoginResponse with its
// UserLoginInfo and PersonaDetails). The server does not check the access token of the request.
// The platform ids of the request (AIDS) come back as they are, and the persona's external id is
// the PlayStation account id of the request. `display_name` names the persona; `now` is the time,
// in seconds since 1970, of the login.
std::vector<uint8_t> LoginReply(const std::vector<uint8_t>& request,
                                const std::string& display_name, int64_t now);

// The reply to Authentication.getAccount: the AccountInfo of the emulator's account, active and not
// underage.
std::vector<uint8_t> GetAccountReply();

// Blaze component 0x7802 (UserSessions) and the notifications a server sends after a login. The
// notification ids are the client's own name table.
inline constexpr uint16_t USER_SESSIONS_COMPONENT          = 0x7802;
inline constexpr uint16_t USER_SESSIONS_USER_ADDED         = 0x0002;
inline constexpr uint16_t USER_SESSIONS_USER_AUTHENTICATED = 0x0008;

// The persona namespace of PlayStation accounts (as in the reply to Util.preAuth).
inline constexpr const char* PERSONA_NAMESPACE = "ps3";

// A message the server sends without a request.
struct Notification {
	uint16_t             component = 0;
	uint16_t             command   = 0;
	std::vector<uint8_t> body;
};

// What the server sends after its reply to Authentication.login, in order: UserAuthenticated (the
// session's UserSessionLoginInfo) and UserAdded (the persona's UserIdentification). The game waits
// for them, and logs out when they do not come.
std::vector<Notification> LoginNotifications(const std::vector<uint8_t>& request,
                                             const std::string& display_name, int64_t now);

// A call whose reply is empty: the response with all its default values. The names are the
// client's own name tables; "-" where the client has no name for the id.
struct EmptyReplyCall {
	uint16_t    component = 0;
	uint16_t    command   = 0;
	const char* name      = "";
};

// The calls a game makes once signed in that a new user's session answers with nothing: no
// telemetry server, no messages, no lists, no settings, no stats, no census data, no partners, no
// users found, no kill switches and no gates.
inline constexpr EmptyReplyCall EMPTY_REPLY_CALLS[] = {
    {0x0001, 0x001f, "Authentication.grantEntitlement"},
    {0x0001, 0x0020, "Authentication -"},
    {0x0001, 0x0046, "Authentication.logout"},
    {0x0007, 0x0003, "Stats -"},
    {0x0007, 0x000f, "Stats -"},
    {0x0007, 0x0014, "Stats -"},
    {0x0009, 0x0005, "Util.getTelemetryServer"},
    {0x0009, 0x000c, "Util.userSettingsLoadAll"}, // no settings: a new user
    {0x0009, 0x0016, "Util.setClientMetrics"},
    {0x0009, 0x0017, "Util.setConnectionState"},
    {0x0009, 0x001c, "Util.setClientState"},
    {0x000a, 0x0004, "CensusData.getLatestCensusData"},
    {0x000b, 0x0a28, "Clubs -"},
    {0x000f, 0x0002, "Messaging.fetchMessages"},
    {0x0019, 0x0006, "AssociationLists.getLists"},
    {0x081c, 0x0003, "SponsoredEvents.getEventsURL"},
    {0x082b, 0x0025, "ProClubsCustom.getFavoriteClub"},
    {0x086c, 0x0003, "ThirdPartyOptin.getPartners"},
    {0x086c, 0x0004, "ThirdPartyOptin -"},
    {0x086e, 0x0001, "Profile.getSeasonDefinition"},
    {0x08c9, 0x0001, "OSDKSettings.fetchSettings"},
    {0x08c9, 0x0002, "OSDKSettings.fetchSettingsGroups"},
    {0x08dd, 0x0001, "GateKeeper.getAll"},
    {0x08de, 0x0001, "KillSwitches.getAll"},
    {0x7802, 0x0008, "UserSessions.updateHardwareFlags"},
    {0x7802, 0x0014, "UserSessions.updateNetworkInfo"},
    {0x7802, 0x0032, "UserSessions.lookupUsersByPersonaNames"},
};

// Blaze component 0x000a (CensusData) and subscribeToCensusDataUpdates. Its reply is not empty:
// with a resubscribe timeout of 0 the client subscribes again at once, without end.
inline constexpr uint16_t CENSUS_DATA_COMPONENT = 0x000a;
inline constexpr uint16_t CENSUS_DATA_SUBSCRIBE = 0x0005;

// SubscribeToCensusDataUpdatesResponse: the notification period, the notification timeout and the
// resubscribe timeout (TimeValue, in microseconds), with the periods of a server that sends no
// census data.
std::vector<uint8_t> CensusSubscribeReply();

// The reply of the server to a call of a signed-in session: Authentication.login and getAccount,
// Util.postAuth, fetchClientConfig and fetchClientConfigAuthenticated, and the calls of
// EMPTY_REPLY_CALLS. Writes the reply body and the notifications that follow it; false for a call
// it does not answer. preAuth and ping are answered elsewhere.
bool SessionReply(uint16_t component, uint16_t command, const std::vector<uint8_t>& request,
                  const std::string& display_name, int64_t now, std::vector<uint8_t>& reply,
                  std::vector<Notification>& notifications);

} // namespace Libs::EaEmulation

#endif // EMULATOR_SRC_LIBS_EA_EMULATION_AUTHENTICATION_H_
