#include "libs/eaEmulation/eaPreAuth.h"

#include "libs/eaEmulation/eaTdf.h"

#include <string>
#include <utility>

namespace Libs::EaEmulation {

namespace {

// The components of the Blaze SDK the server says it has (Authentication 1, GameManager 4, Stats 7,
// Util 9, ...). A game's own components (0x7800 and up) are left out: the request does not name them.
const std::vector<int64_t> COMPONENTS = {1, 4, 7, 9, 10, 13, 15, 25, 33, 40, 403};

// The client platforms the server hosts (Blaze ClientPlatformType values).
const std::vector<int64_t> HOST_PLATFORMS = {4, 22, 23, 26};

// The build of the server, in the three version fields of the reply.
constexpr const char* SERVER_BUILD = "324473";

std::vector<std::pair<std::string, std::string>> ClientSettings(const std::string& client,
                                                                 const std::string& environment) {
	return {
	    {"associationListSkipInitialSet", "1"},
	    {"autoReconnectEnabled", "0"},
	    {"blazeSdkClientId", client},
	    {"blazeSdkGuestClientId", client.empty() ? client : client + "_GST"},
	    {"blazeServerClientId", client},
	    {"cachedUserRefreshInterval", "1s"},
	    {"clientUserMetricsUpdateRate", "60000"},
	    {"connIdleTimeout", "40s"},
	    {"defaultRequestTimeout", "20s"},
	    {"Env_override", environment},
	    {"gameManagerPreventMultipleGameMembership", "0"},
	    {"identityRedirectUri", "http://127.0.0.1/login_successful.html"},
	    {"maxReconnectAttempts", "0"},
	    {"nonResumableTimeoutScale", "2.0"},
	    {"nucleusConnect", "https://accounts.grpc.ea.com"},
	    {"nucleusConnectREST", "https://accounts.ea.com"},
	    {"nucleusConnectTrusted", "https://accounts2s.ea.com"},
	    {"nucleusPortal", "https://signin.ea.com"},
	    {"nucleusProxy", "https://gateway.grpc.ea.com"},
	    {"pingPeriod", "20s"},
	    {"userManagerMaxCachedUsers", "0"},
	};
}

} // namespace

std::vector<uint8_t> PreAuthReply(const std::vector<uint8_t>& request) {
	const auto service     = TextMember(request, Tag("CDAT"), Tag("SVCN"), "");
	const auto client      = TextMember(request, Tag("CINF"), Tag("CLNT"), "");
	const auto environment = TextMember(request, Tag("CINF"), Tag("ENV "), "prod");
	const auto sdk         = TextMember(request, Tag("CINF"), Tag("BSDK"), "");

	// The members in the order of their tags, as Blaze writes them.
	Writer w;
	w.Text(Tag("ASRC"), SERVER_BUILD);
	w.IntegerList(Tag("CIDS"), COMPONENTS);
	w.Text(Tag("CLID"), client);
	w.BeginStruct(Tag("CONF"));
	w.TextMap(Tag("CONF"), ClientSettings(client, environment));
	w.EndStruct();
	w.Text(Tag("ESRC"), SERVER_BUILD);
	w.IntegerList(Tag("HPLT"), HOST_PLATFORMS);
	w.Text(Tag("INST"), service);
	w.Integer(Tag("LQTK"), 0);
	w.Integer(Tag("MAID"), 1); // the server's machine: any number
	w.Integer(Tag("MINR"), 1); // as EA's server sends it
	w.Text(Tag("NASP"), "ps3"); // the persona namespace of PlayStation accounts
	w.Text(Tag("PILD"), "");
	w.Text(Tag("PLAT"), "ps5");
	// The QoS settings: the timings are those of EA's server; no ping sites and no coordinator.
	w.BeginStruct(Tag("QOSS"));
	w.Integer(Tag("CQFR"), 300000000);
	w.Integer(Tag("CQRR"), 0);
	w.EmptyStructMap(Tag("LTPS")); // no ping sites
	w.Integer(Tag("QCIS"), 0);
	w.BeginStruct(Tag("QCNF"));
	w.Text(Tag("QCA "), "");
	w.Integer(Tag("QCP "), 0);
	w.Text(Tag("QPR "), "");
	w.EndStruct();
	w.Integer(Tag("SQRR"), 15000000);
	w.Integer(Tag("VERS"), 1);
	w.EndStruct();
	w.Text(Tag("RELT"), environment);
	w.Text(Tag("RSRC"), SERVER_BUILD);
	w.Text(Tag("SVER"), sdk.empty() ? std::string("Blaze") : "Blaze " + sdk);
	return std::move(w.out);
}

} // namespace Libs::EaEmulation
