#include "libs/eaEmulation/eaAuthentication.h"

#include "libs/eaEmulation/eaPreAuth.h"
#include "libs/eaEmulation/eaTdf.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace Libs::EaEmulation {

namespace {

// A member of a struct, or of the top level of a payload: where its tag starts and where its
// value starts and ends.
struct MemberAt {
	uint8_t type        = 0;
	size_t  tag_begin   = 0;
	size_t  value_begin = 0;
	size_t  value_end   = 0;
};

// Finds `tag` among the members in [from, to) of `payload`. The members end at `to` or at the zero
// byte that ends a struct.
bool FindMember(const std::vector<uint8_t>& payload, size_t from, size_t to, uint32_t tag,
                MemberAt& found) {
	Reader reader {payload.data(), to, from};
	while (reader.ok && reader.at < to && payload[reader.at] != 0) {
		MemberAt member;
		member.tag_begin   = reader.at;
		const auto name    = reader.TagValue();
		member.type        = reader.Byte();
		member.value_begin = reader.at;
		reader.SkipValue(member.type);
		member.value_end = reader.at;
		if (reader.ok && name == tag) {
			found = member;
			return true;
		}
	}
	return false;
}

// Writes `member` of `request` (its tag, type and value) as it is.
void CopyMember(Writer& w, const std::vector<uint8_t>& request, const MemberAt& member) {
	w.out.insert(w.out.end(), request.begin() + static_cast<std::ptrdiff_t>(member.tag_begin),
	             request.begin() + static_cast<std::ptrdiff_t>(member.value_end));
}

// The PlayStation account id in AIDS.EXID.PSID of a login request, or 0.
int64_t PlayStationAccountId(const std::vector<uint8_t>& request, const MemberAt& aids) {
	MemberAt exid;
	MemberAt psid;
	if (aids.type != Struct ||
	    !FindMember(request, aids.value_begin, aids.value_end, Tag("EXID"), exid) ||
	    exid.type != Struct ||
	    !FindMember(request, exid.value_begin, exid.value_end, Tag("PSID"), psid) ||
	    psid.type != Int) {
		return 0;
	}
	Reader     reader {request.data(), psid.value_end, psid.value_begin};
	const auto value = reader.Var();
	return reader.ok ? value : 0;
}

// PersonaStatus.ACTIVE, as the game's enum has it (UNKNOWN 0, PENDING 1, ACTIVE 2, ...).
constexpr int64_t PERSONA_STATUS_ACTIVE = 2;

} // namespace

std::vector<uint8_t> LoginReply(const std::vector<uint8_t>& request,
                                const std::string& display_name, int64_t now) {
	MemberAt   aids;
	const bool has_aids = FindMember(request, 0, request.size(), Tag("AIDS"), aids);

	// The members in the order of their tags, as Blaze writes them.
	Writer w;
	w.Integer(Tag("ANON"), 0);                   // isAnonymous
	w.BeginStruct(Tag("SESS"));                  // userLoginInfo
	if (has_aids) CopyMember(w, request, aids);  // platformInfo: the game's own
	w.Integer(Tag("BUID"), EMULATED_PERSONA_ID); // blazeId
	w.Integer(Tag("FRST"), 0);                   // isFirstLogin
	w.Text(Tag("KEY "), "kyty-emulated-session-key");
	w.Integer(Tag("LLOG"), now); // lastLoginDateTime
	w.BeginStruct(Tag("PDTL"));  // personaDetails
	w.Text(Tag("DSNM"), display_name);
	w.Integer(Tag("LAST"), now); // lastAuthenticated
	w.Integer(Tag("PID "), EMULATED_PERSONA_ID);
	w.Integer(Tag("STAS"), PERSONA_STATUS_ACTIVE);
	w.Integer(Tag("XREF"), has_aids ? PlayStationAccountId(request, aids) : 0); // extId
	w.EndStruct();
	w.Integer(Tag("UID "), EMULATED_ACCOUNT_ID); // accountId
	w.EndStruct();
	w.Integer(Tag("SPAM"), 1); // isOfLegalContactAge
	w.Integer(Tag("UNDR"), 0); // isUnderage
	return std::move(w.out);
}

std::vector<uint8_t> GetAccountReply() {
	// AccountInfo, in the order of its tags. The status is Blaze::Nucleus::AccountStatus ACTIVE:
	// the game's enum names are UNKNOWN, ACTIVE, BANNED, ... and ACTIVE is taken to be 1.
	Writer w;
	w.Integer(Tag("AMU "), 0); // anonymousUser
	w.Integer(Tag("STAS"), 1); // status
	w.Integer(Tag("UDU "), 0); // underageUser
	w.Integer(Tag("UID "), EMULATED_ACCOUNT_ID);
	return std::move(w.out);
}

std::vector<Notification> LoginNotifications(const std::vector<uint8_t>& request,
                                             const std::string& display_name, int64_t now) {
	MemberAt      aids;
	const bool    has_aids    = FindMember(request, 0, request.size(), Tag("AIDS"), aids);
	const int64_t external_id = has_aids ? PlayStationAccountId(request, aids) : 0;
	const auto    copy_aids   = [&](Writer& w) {
		if (has_aids) CopyMember(w, request, aids);
	};

	std::vector<Notification> notifications;

	// UserAuthenticated: the UserSessionLoginInfo of the session.
	{
		Writer w;
		copy_aids(w);              // platformInfo
		w.Integer(Tag("ANON"), 0); // isAnonymous
		w.Integer(Tag("BUID"), EMULATED_PERSONA_ID);
		w.Text(Tag("DSNM"), display_name);
		w.Integer(Tag("FRST"), 0); // isFirstLogin
		w.Text(Tag("KEY "), "kyty-emulated-session-key");
		w.Integer(Tag("LAST"), now); // lastAuthenticated
		w.Integer(Tag("LLOG"), now); // lastLoginDateTime
		w.Text(Tag("NASP"), PERSONA_NAMESPACE);
		w.Integer(Tag("PID "), EMULATED_PERSONA_ID);
		w.Integer(Tag("UID "), EMULATED_ACCOUNT_ID);
		w.Integer(Tag("XREF"), external_id);
		notifications.push_back(
		    {USER_SESSIONS_COMPONENT, USER_SESSIONS_USER_AUTHENTICATED, std::move(w.out)});
	}

	// UserAdded: the user of the session (UserIdentification), with no extended data.
	{
		Writer w;
		w.BeginStruct(Tag("DATA")); // extendedData
		w.EndStruct();
		w.BeginStruct(Tag("USER")); // userInfo
		w.Integer(Tag("AID "), EMULATED_ACCOUNT_ID);
		copy_aids(w);
		w.Integer(Tag("EXID"), external_id);
		w.Integer(Tag("ID  "), EMULATED_PERSONA_ID);
		w.Text(Tag("NAME"), display_name);
		w.Text(Tag("NASP"), PERSONA_NAMESPACE);
		w.Integer(Tag("PIDI"), EMULATED_ACCOUNT_ID);
		w.EndStruct();
		notifications.push_back(
		    {USER_SESSIONS_COMPONENT, USER_SESSIONS_USER_ADDED, std::move(w.out)});
	}
	return notifications;
}

std::vector<uint8_t> CensusSubscribeReply() {
	constexpr int64_t MICROSECONDS_PER_SECOND = 1000000;
	Writer            w;
	w.Integer(Tag("CNP "), 60 * MICROSECONDS_PER_SECOND);  // censusNotificationPeriod
	w.Integer(Tag("NTMT"), 60 * MICROSECONDS_PER_SECOND);  // notificationTimeout
	w.Integer(Tag("RTMT"), 300 * MICROSECONDS_PER_SECOND); // resubscribeTimeout
	return std::move(w.out);
}

bool SessionReply(uint16_t component, uint16_t command, const std::vector<uint8_t>& request,
                  const std::string& display_name, int64_t now, std::vector<uint8_t>& reply,
                  std::vector<Notification>& notifications) {
	reply.clear();
	notifications.clear();
	if (component == AUTHENTICATION_COMPONENT && command == AUTHENTICATION_LOGIN) {
		reply         = LoginReply(request, display_name, now);
		notifications = LoginNotifications(request, display_name, now);
		return true;
	}
	if (component == AUTHENTICATION_COMPONENT && command == AUTHENTICATION_GET_ACCOUNT) {
		reply = GetAccountReply();
		return true;
	}
	if (component == CENSUS_DATA_COMPONENT && command == CENSUS_DATA_SUBSCRIBE) {
		reply = CensusSubscribeReply();
		return true;
	}
	if (component == UTIL_COMPONENT && command == UTIL_POST_AUTH) {
		reply = PostAuthReply();
		return true;
	}
	if (component == UTIL_COMPONENT && (command == UTIL_FETCH_CLIENT_CONFIG ||
	                                    command == UTIL_FETCH_CLIENT_CONFIG_AUTHENTICATED)) {
		reply = FetchConfigReply();
		return true;
	}
	return std::any_of(std::begin(EMPTY_REPLY_CALLS), std::end(EMPTY_REPLY_CALLS),
	                   [&](const EmptyReplyCall& call) {
		                   return call.component == component && call.command == command;
	                   });
}

} // namespace Libs::EaEmulation
