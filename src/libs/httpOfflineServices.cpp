#include "libs/httpOfflineServices.h"
#include "libs/eaEmulation/eaServer.h"

#include "common/logging/log.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace Libs::Network::OfflineServices {

// What the publisher's service directory answers to a client that has not signed in: the list of
// service addresses. A game reads it before it tries to sign in. Only the addresses every game of
// the publisher shares are here; a game's own services are left out.
static constexpr std::string_view EA_DIRECTORY = R"JSON({"eadp.auth.account":"https://signin.ea.com/","eadp.authentication.sonyDeviceTokenScopes":"psn:s2s openid id_token:psn.basic_claims id_token:duid id_token:device_properties.cldk id_token:streaming_client_duid id_token:user_device_ip","eadp.authentication.useJwtToken":"false","eadp.candi.catalog.service":"https://gateway.grpc.ea.com:443","eadp.candi.catalog.v2.service":"https://gateway.grpc.ea.com:443","eadp.candi.drm.service":"https://gateway.grpc.ea.com:443","eadp.candi.entitlement.v2.service":"https://gateway.grpc.ea.com:443","eadp.candi.firstpartycheckout":"https://gateway.grpc.ea.com:443","eadp.candi.gamecontentpromocodes.service":"https://gateway.grpc.ea.com:443","eadp.candi.offer.service":"https://gateway.grpc.ea.com:443","eadp.candi.offer.v2.service":"https://gateway.grpc.ea.com:443","eadp.candi.partnerproduct.v1.service":"https://gateway.grpc.ea.com","eadp.candi.valuetransfer.service":"https://gateway.grpc.ea.com:443","eadp.candi.valuetransfer.v2.service":"https://gateway.grpc.ea.com:443","eadp.chat":"https://rtm.tnt-ea.com","eadp.chat.tcp":"https://rtm.tnt-ea.com:9000","eadp.contentaccesscontrol.grpc.model.v2":"https://gateway.grpc.ea.com","eadp.crossplaycontentmart.v1":"https://api.k.social.ea.com","eadp.eaid.grpc.model":"https://gateway.grpc.ea.com","eadp.eaid.grpc.model.v1":"https://gateway.grpc.ea.com","eadp.experimentation.grouping.v1":"https://experimentation-grpc.data.ea.com","eadp.experimentation.grouping.v2":"https://experimentation-grpc.data.ea.com","eadp.experimentation.tracking.v1":"https://experimentation-tracking-grpc.data.ea.com","eadp.experimentation.tracking.v2":"https://experimentation-tracking-grpc.data.ea.com","eadp.friends.notifications":"https://api.k.social.ea.com","eadp.friends.v1":"https://api.k.social.ea.com","eadp.friends.v1.FriendListNotifications":"https://api.k.social.ea.com","eadp.friends.v1.InviteListNotifications":"https://api.k.social.ea.com","eadp.identity":"https://accounts.ea.com","eadp.identity.proxy":"https://gateway.ea.com/proxy","eadp.identity.v2":"https://accounts.ea.com","eadp.instrumentation.enabled":"true","eadp.instrumentation.service":"https://freeform-river.data.ea.com","eadp.leaderboards":"https://leaderboards.gameservices.ea.com:11000","eadp.mcr":"https://api.mcr.ea.com:4430","eadp.mcr.query.noncached.service":"https://q-origin-internal.mcr.ea.com","eadp.mcr.query.service":"https://q.mcr.ea.com","eadp.networkReachability.primaryEndpoint":"ping1.tnt-ea.com","eadp.networkReachability.primaryTimeoutInMilliseconds":"5000","eadp.networkReachability.secondaryEndpoint":"google.com","eadp.networkReachability.secondaryTimeoutInMilliseconds":"5000","eadp.nexus.connect.grpc.v1":"https://accounts.grpc.ea.com","eadp.pin":"https://pin-river-grpc.data.ea.com:443","eadp.playercard.v1":"https://gateway.grpc.ea.com","eadp.playerownership.v1.service":"https://gateway.grpc.ea.com","eadp.playersearch.grpc.search.v1.service":"https://gateway.grpc.ea.com","eadp.pushnotification":"https://pn.tnt-ea.com","eadp.realtimemessaging":"https://rtm.tnt-ea.com:9000","eadp.social.gameinvite.v1":"https://api.k.social.ea.com","eadp.social.groups.v1":"https://groups.social.ea.com","eadp.social.groups.v1.GroupNotifications":"https://api.k.social.ea.com","eadp.social.presence.v1":"https://api.k.social.ea.com","eadp.social.privacy.v1":"https://api.k.social.ea.com","eadp.social.privacy.v1.BlockListNotifications":"https://api.k.social.ea.com","eadp.social.privacy.v1.MuteListNotifications":"https://api.k.social.ea.com","eadp.um.v1":"https://pin-em-grpc.data.ea.com:443","kanas.commerce.serviceLabel":"0","shadowbroker.grpc.service.subscription":"https://gateway.grpc.ea.com:443","sony.commerce.serviceLabel":"0"})JSON";

static bool StartsWith(const std::string& text, std::string_view prefix) {
	return text.size() >= prefix.size() && std::string_view(text).substr(0, prefix.size()) == prefix;
}

static void Json(Response& response, int status, std::string_view body) {
	response.status = status;
	response.headers.push_back({"Content-Type", "application/json"});
	response.body = std::string(body);
}

// The Blaze server picker (any of EA's redirector hosts: blazeredirector, gosredirector) answers
// with the server of libs/eaEmulation, on this machine.
static bool EmulatedBlazeRedirector(const Request& request, Response& response) {
	const auto port_number = EaEmulation::Port();
	if (port_number == 0 || request.method != "POST" ||
	    request.host.find("redirector.ea.com") == std::string::npos ||
	    !StartsWith(request.path, "/redirector/getServerInstance")) {
		return false;
	}
	EaEmulation::NoteServerPickerQuestion({request.host, request.body});
	const auto port = std::to_string(port_number);
	// 127.0.0.1 with its bytes swapped: the game connects to the bytes of this integer as stored.
	// (A real server address in this field connected to the reversed address too.)
	response.status = 200;
	response.headers.push_back({"Content-Type", "application/xml"});
	response.body = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<serverinstanceinfo>\n"
	                            "  <address member=\"0\">\n    <valu>\n"
	                            "      <hostname>emulated.blaze.ea.com</hostname>\n      <ip>16777343</ip>\n"
	                            "      <port>") +
	                port +
	                "</port>\n    </valu>\n  </address>\n  <secure>1</secure>\n"
	                "  <trialservicename></trialservicename>\n  <defaultdnsaddress>0</defaultdnsaddress>\n"
	                "</serverinstanceinfo>\n";
	return true;
}

bool Handle(const Request& request, Response& response) {
	if (EmulatedBlazeRedirector(request, response)) {
		return true;
	}

	if (request.host == "gcs.ea.com" && request.method == "POST" &&
	    StartsWith(request.path, "/application_id/")) {
		Json(response, 200, EA_DIRECTORY);
		return true;
	}

	// Usage events: nothing reads the answer.
	if (request.host == "freeform-river.data.ea.com" && request.method == "POST" &&
	    StartsWith(request.path, "/genericEvents")) {
		Json(response, 200, "{}");
		return true;
	}

	return false;
}

} // namespace Libs::Network::OfflineServices
