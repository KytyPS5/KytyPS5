#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NET_CALL(func)                                                                             \
	[&]() {                                                                                        \
		auto result = func;                                                                        \
		if (result < 0) {                                                                          \
			*GetNetErrorAddr() = result;                                                           \
		}                                                                                          \
		return result;                                                                             \
	}()

namespace Libs {

namespace Network::Net {
struct NetEtherAddr;
} // namespace Network::Net

namespace LibNet {

LIB_VERSION("Net", 1, "Net", 1, 1);

static thread_local int g_net_errno = 0;

namespace Net = Network::Net;

KYTY_SYSV_ABI int* GetNetErrorAddr() {
	return &g_net_errno;
}

static int KYTY_SYSV_ABI NetInit() {
	return NET_CALL(Net::NetInit());
}

static int PosixToNetError(int error) {
	if (error >= Posix::POSIX_EPERM && error <= Posix::POSIX_ECANCELED) {
		return Network::NET_ERROR_EPERM + error - Posix::POSIX_EPERM;
	}

	switch (error) {
		case Posix::POSIX_EPROTO: return Network::NET_ERROR_EPROTO;
		case Posix::POSIX_EADHOC: return Network::NET_ERROR_EADHOC;
		case Posix::POSIX_EINACTIVEDISABLED: return Network::NET_ERROR_EINACTIVEDISABLED;
		case Posix::POSIX_ENETNODATA: return Network::NET_ERROR_ENODATA;
		case Posix::POSIX_ENETDESC: return Network::NET_ERROR_EDESC;
		case Posix::POSIX_ENETDESCTIMEDOUT: return Network::NET_ERROR_EDESCTIMEDOUT;
		case Posix::POSIX_ENETINTR: return Network::NET_ERROR_ENETINTR;
		case Posix::POSIX_ERETURN: return Network::NET_ERROR_ERETURN;
		default: return Network::NET_ERROR_EIO;
	}
}

static int FinishSocketCall(int result) {
	if (result >= 0) {
		return result;
	}

	const int net_error = PosixToNetError(*Posix::GetErrorAddr());
	*GetNetErrorAddr()  = net_error;
	return net_error;
}

int KYTY_SYSV_ABI NetSocket(const char* name, int family, int type, int protocol) {
	LOGF("\t name = %s\n", name != nullptr ? name : "<null>");

	return FinishSocketCall(Net::Socket(family, type, protocol));
}

int KYTY_SYSV_ABI NetAccept(int s, void* addr, uint32_t* addrlen) {
	return FinishSocketCall(Net::Accept(s, addr, addrlen));
}

int KYTY_SYSV_ABI NetBind(int s, const void* addr, uint32_t addrlen) {
	return FinishSocketCall(Net::Bind(s, addr, addrlen));
}

int KYTY_SYSV_ABI NetListen(int s, int backlog) {
	return FinishSocketCall(Net::Listen(s, backlog));
}

int KYTY_SYSV_ABI NetShutdown(int s, int how) {
	return FinishSocketCall(Net::Shutdown(s, how));
}

int KYTY_SYSV_ABI NetPoolCreate(const char* name, int size, int flags) {
	return NET_CALL(Net::NetPoolCreate(name, size, flags));
}

int KYTY_SYSV_ABI NetPoolDestroy(int memid) {
	return NET_CALL(Net::NetPoolDestroy(memid));
}

int KYTY_SYSV_ABI NetResolverCreate(const char* name, int memid, int flags) {
	return NET_CALL(Net::NetResolverCreate(name, memid, flags));
}

int KYTY_SYSV_ABI NetResolverStartNtoa(int rid, const char* hostname, void* addr, int timeout,
                                       int retry, int flags) {
	return NET_CALL(Net::NetResolverStartNtoa(rid, hostname, addr, timeout, retry, flags));
}

int KYTY_SYSV_ABI NetInetPton(int af, const char* src, void* dst) {
	return NET_CALL(Net::NetInetPton(af, src, dst));
}

const char* KYTY_SYSV_ABI NetInetNtop(int af, const void* src, char* dst, uint32_t size) {
	const char* result = Net::NetInetNtop(af, src, dst, size);
	if (result == nullptr) {
		*GetNetErrorAddr() = PosixToNetError(*Posix::GetErrorAddr());
	}
	return result;
}

int KYTY_SYSV_ABI NetEtherNtostr(const Net::NetEtherAddr* n, char* str, size_t len) {
	return NET_CALL(Net::NetEtherNtostr(n, str, len));
}

int KYTY_SYSV_ABI NetGetMacAddress(Net::NetEtherAddr* addr, int flags) {
	return NET_CALL(Net::NetGetMacAddress(addr, flags));
}

int KYTY_SYSV_ABI NetGetSockInfo(int s, void* info, int n, int flags) {
	return NET_CALL(Net::NetGetSockInfo(s, info, n, flags));
}

int KYTY_SYSV_ABI NetEpollCreate(const char* name, int flags) {
	return FinishSocketCall(Net::EpollCreate(name, flags));
}

int KYTY_SYSV_ABI NetEpollControl(int eid, int op, int id, const Net::NetEpollEvent* event) {
	return FinishSocketCall(Net::EpollControl(eid, op, id, event));
}

int KYTY_SYSV_ABI NetEpollWait(int eid, Net::NetEpollEvent* events, int maxevents, int timeout) {
	return FinishSocketCall(Net::EpollWait(eid, events, maxevents, timeout));
}

int KYTY_SYSV_ABI NetEpollDestroy(int eid) {
	return FinishSocketCall(Net::EpollDestroy(eid));
}

int KYTY_SYSV_ABI NetSocketClose(int s) {
	return NET_CALL(Net::SocketClose(s));
}

int KYTY_SYSV_ABI NetSetsockopt(int s, int level, int optname, const void* optval,
                                uint32_t optlen) {
	return NET_CALL(Net::Setsockopt(s, level, optname, optval, optlen));
}

uint32_t KYTY_SYSV_ABI NetHtonl(uint32_t host32) {
	return ((host32 & 0x000000ffu) << 24u) | ((host32 & 0x0000ff00u) << 8u) |
	       ((host32 & 0x00ff0000u) >> 8u) | ((host32 & 0xff000000u) >> 24u);
}

uint16_t KYTY_SYSV_ABI NetHtons(uint16_t host16) {
	return static_cast<uint16_t>(((host16 & 0x00ffu) << 8u) | ((host16 & 0xff00u) >> 8u));
}

uint32_t KYTY_SYSV_ABI NetNtohl(uint32_t net32) {
	return NetHtonl(net32);
}

uint16_t KYTY_SYSV_ABI NetNtohs(uint16_t net16) {
	return NetHtons(net16);
}

LIB_DEFINE(InitNet_1_Net) {
	LIB_FUNC("Nlev7Lg8k3A", LibNet::NetInit);
	LIB_FUNC("HQOwnfMGipQ", LibNet::GetNetErrorAddr);
	LIB_FUNC("PIWqhn9oSxc", LibNet::NetAccept);
	LIB_FUNC("bErx49PgxyY", LibNet::NetBind);
	LIB_FUNC("dgJBaeJnGpo", LibNet::NetPoolCreate);
	LIB_FUNC("K7RlrTkI-mw", LibNet::NetPoolDestroy);
	LIB_FUNC("C4UgDHHPvdw", LibNet::NetResolverCreate);
	LIB_FUNC("Nd91WaWmG2w", LibNet::NetResolverStartNtoa);
	LIB_FUNC("8Kcp5d-q1Uo", LibNet::NetInetPton);
	LIB_FUNC("9vA2aW+CHuA", LibNet::NetInetNtop);
	LIB_FUNC("v6M4txecCuo", LibNet::NetEtherNtostr);
	LIB_FUNC("6Oc0bLsIYe0", LibNet::NetGetMacAddress);
	LIB_FUNC("hLuXdjHnhiI", LibNet::NetGetSockInfo);
	LIB_FUNC("SF47kB2MNTo", LibNet::NetEpollCreate);
	LIB_FUNC("ZVw46bsasAk", LibNet::NetEpollControl);
	LIB_FUNC("drjIbDbA7UQ", LibNet::NetEpollWait);
	LIB_FUNC("Inp1lfL+Jdw", LibNet::NetEpollDestroy);
	LIB_FUNC("kOj1HiAGE54", LibNet::NetListen);
	LIB_FUNC("TSM6whtekok", LibNet::NetShutdown);
	LIB_FUNC("Q4qBuN-c0ZM", LibNet::NetSocket);
	LIB_FUNC("45ggEzakPJQ", LibNet::NetSocketClose);
	LIB_FUNC("2mKX2Spso7I", LibNet::NetSetsockopt);
	LIB_FUNC("9T2pDF2Ryqg", LibNet::NetHtonl);
	LIB_FUNC("iWQWrwiSt8A", LibNet::NetHtons);
	LIB_FUNC("pQGpHYopAIY", LibNet::NetNtohl);
	LIB_FUNC("Rbvt+5Y2iEw", LibNet::NetNtohs);
}

} // namespace LibNet

namespace LibSsl {

LIB_VERSION("Ssl", 1, "Ssl", 1, 1);

namespace Ssl = Network::Ssl;

LIB_DEFINE(InitNet_1_Ssl) {
	LIB_FUNC("hdpVEUDFW3s", Ssl::SslInit);
	LIB_FUNC("0K1yQ6Lv-Yc", Ssl::SslTerm);
	LIB_FUNC("TDfQqO-gMbY", Ssl::SslGetCaCerts);
	LIB_FUNC("qIvLs0gYxi0", Ssl::SslFreeCaCerts);
}

} // namespace LibSsl

namespace LibHttp {

LIB_VERSION("Http", 1, "Http", 1, 1);

namespace Http = Network::Http;

constexpr int HTTP_ERROR_INVALID_VALUE = -2143088130; /* 0x804311fe */
constexpr int HTTP_ERROR_OUT_OF_MEMORY = -2143088606; /* 0x80431022 */
constexpr int HTTP_ERROR_INVALID_URL   = -2143080352; /* 0x80433060 */

struct SceHttpUriElement {
	int      opaque   = 0;
	char*    scheme   = nullptr;
	char*    username = nullptr;
	char*    password = nullptr;
	char*    hostname = nullptr;
	char*    path     = nullptr;
	char*    query    = nullptr;
	char*    fragment = nullptr;
	uint16_t port     = 0;
	uint8_t  reserved[10] {};
};

struct UriPart {
	const char* begin = nullptr;
	size_t      len   = 0;
};

static bool IsUriSchemeChar(char c, bool first) {
	const auto ch = static_cast<unsigned char>(c);
	return first ? std::isalpha(ch) != 0
	             : (std::isalnum(ch) != 0 || c == '+' || c == '-' || c == '.');
}

static char* CopyUriPart(char*& dst, const UriPart& part) {
	if (part.begin == nullptr) {
		return nullptr;
	}

	auto* out = dst;
	std::memcpy(out, part.begin, part.len);
	out[part.len] = '\0';
	dst += part.len + 1;
	return out;
}

static int ParseEmptyUri(SceHttpUriElement* out, void* pool, size_t* require, size_t prepare) {
	constexpr size_t needed = 3;

	if (require != nullptr) {
		*require = needed;
	}

	if (out != nullptr) {
		std::memset(out, 0, sizeof(*out));
		out->opaque = 1;
	}

	if (out != nullptr && pool != nullptr) {
		if (prepare != 0 && prepare < needed) {
			return HTTP_ERROR_OUT_OF_MEMORY;
		}

		auto* dst        = static_cast<char*>(pool);
		out->scheme      = dst++;
		out->hostname    = dst++;
		out->path        = dst;
		out->scheme[0]   = '\0';
		out->hostname[0] = '\0';
		out->path[0]     = '\0';
	}

	return 0;
}

static int KYTY_SYSV_ABI HttpUriParse(SceHttpUriElement* out, const char* src_url, void* pool,
                                      size_t* require, size_t prepare) {
	PRINT_NAME();

	LOGF("\t out     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(out));
	LOGF("\t src_url = %s\n", src_url != nullptr ? src_url : "(null)");
	LOGF("\t pool    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(pool));
	LOGF("\t require = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(require));
	LOGF("\t prepare = %" PRIu64 "\n", static_cast<uint64_t>(prepare));

	if (src_url == nullptr) {
		return HTTP_ERROR_INVALID_URL;
	}

	if (out == nullptr && pool == nullptr && require == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	if (src_url[0] == '\0') {
		return ParseEmptyUri(out, pool, require, prepare);
	}

	UriPart  scheme {};
	UriPart  username {};
	UriPart  password {};
	UriPart  hostname {};
	UriPart  path {};
	UriPart  query {};
	UriPart  fragment {};
	auto     opaque = false;
	uint16_t port   = 0;

	const char* cursor = src_url;
	if (!IsUriSchemeChar(*cursor, true)) {
		return HTTP_ERROR_INVALID_URL;
	}

	while (*cursor != '\0' && *cursor != ':') {
		if (!IsUriSchemeChar(*cursor, false)) {
			return HTTP_ERROR_INVALID_URL;
		}
		cursor++;
	}

	if (*cursor != ':') {
		return HTTP_ERROR_INVALID_URL;
	}

	scheme = {src_url, static_cast<size_t>(cursor - src_url)};
	cursor++;

	if (cursor[0] == '/' && cursor[1] == '/') {
		cursor += 2;
		const char* authority     = cursor;
		const char* authority_end = authority;
		while (*authority_end != '\0' && *authority_end != '/' && *authority_end != '?' &&
		       *authority_end != '#') {
			authority_end++;
		}

		const char* host_begin = authority;
		for (const char* p = authority; p < authority_end; p++) {
			if (*p == '@') {
				const char* colon = static_cast<const char*>(
				    std::memchr(authority, ':', static_cast<size_t>(p - authority)));
				if (colon != nullptr) {
					username = {authority, static_cast<size_t>(colon - authority)};
					password = {colon + 1, static_cast<size_t>(p - colon - 1)};
				} else {
					username = {authority, static_cast<size_t>(p - authority)};
				}
				host_begin = p + 1;
			}
		}

		const char* host_end   = authority_end;
		const char* port_begin = nullptr;
		if (host_begin < authority_end && *host_begin == '[') {
			const char* close = static_cast<const char*>(
			    std::memchr(host_begin, ']', static_cast<size_t>(authority_end - host_begin)));
			if (close == nullptr) {
				return HTTP_ERROR_INVALID_URL;
			}
			host_end = close + 1;
			if (host_end < authority_end && *host_end == ':') {
				port_begin = host_end + 1;
			}
		} else {
			for (const char* p = host_begin; p < authority_end; p++) {
				if (*p == ':') {
					host_end   = p;
					port_begin = p + 1;
					break;
				}
			}
		}

		if (host_begin < host_end) {
			hostname = {host_begin, static_cast<size_t>(host_end - host_begin)};
		}

		if (port_begin != nullptr) {
			if (port_begin == authority_end) {
				return HTTP_ERROR_INVALID_URL;
			}

			uint32_t port_value = 0;
			for (const char* p = port_begin; p < authority_end; p++) {
				if (std::isdigit(static_cast<unsigned char>(*p)) == 0) {
					return HTTP_ERROR_INVALID_URL;
				}
				port_value = port_value * 10u + static_cast<uint32_t>(*p - '0');
				if (port_value > 65535u) {
					return HTTP_ERROR_INVALID_URL;
				}
			}
			port = static_cast<uint16_t>(port_value);
		}

		cursor = authority_end;
	} else {
		opaque = true;
	}

	const char* path_begin = cursor;
	while (*cursor != '\0' && *cursor != '?' && *cursor != '#') {
		cursor++;
	}
	if (cursor > path_begin) {
		path = {path_begin, static_cast<size_t>(cursor - path_begin)};
	}

	if (*cursor == '?') {
		const char* query_begin = cursor;
		cursor++;
		while (*cursor != '\0' && *cursor != '#') {
			cursor++;
		}
		query = {query_begin, static_cast<size_t>(cursor - query_begin)};
	}

	if (*cursor == '#') {
		const char* fragment_begin = cursor;
		while (*cursor != '\0') {
			cursor++;
		}
		fragment = {fragment_begin, static_cast<size_t>(cursor - fragment_begin)};
	}

	size_t needed = 0;
	for (const auto& part: {scheme, username, password, hostname, path, query, fragment}) {
		if (part.begin != nullptr) {
			needed += part.len + 1;
		}
	}

	if (require != nullptr) {
		*require = needed;
	}

	if (out != nullptr) {
		std::memset(out, 0, sizeof(*out));
		out->opaque = opaque ? 1 : 0;
		out->port   = port;
	}

	if (out != nullptr && pool != nullptr) {
		if (prepare != 0 && prepare < needed) {
			return HTTP_ERROR_OUT_OF_MEMORY;
		}

		auto* dst     = static_cast<char*>(pool);
		out->scheme   = CopyUriPart(dst, scheme);
		out->username = CopyUriPart(dst, username);
		out->password = CopyUriPart(dst, password);
		out->hostname = CopyUriPart(dst, hostname);
		out->path     = CopyUriPart(dst, path);
		out->query    = CopyUriPart(dst, query);
		out->fragment = CopyUriPart(dst, fragment);
	}

	return 0;
}

static int KYTY_SYSV_ABI HttpUriEscape(char* out, size_t* require, size_t prepare, const char* in) {
	PRINT_NAME();

	LOGF("\t out     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(out));
	LOGF("\t require = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(require));
	LOGF("\t prepare = %" PRIu64 "\n", static_cast<uint64_t>(prepare));
	LOGF("\t in      = %s\n", in != nullptr ? in : "(null)");

	if (in == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	auto is_unreserved = [](unsigned char c) {
		return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~';
	};

	size_t needed = 1;
	for (const auto* p = reinterpret_cast<const unsigned char*>(in); *p != 0; p++) {
		needed += is_unreserved(*p) ? 1 : 3;
	}

	if (require != nullptr) {
		*require = needed;
	}

	if (out == nullptr) {
		return 0;
	}

	if (prepare < needed) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	static constexpr char HEX[] = "0123456789ABCDEF";
	auto*                 dst   = out;
	for (const auto* p = reinterpret_cast<const unsigned char*>(in); *p != 0; p++) {
		if (is_unreserved(*p)) {
			*dst++ = static_cast<char>(*p);
		} else {
			*dst++ = '%';
			*dst++ = HEX[(*p >> 4u) & 0x0fu];
			*dst++ = HEX[*p & 0x0fu];
		}
	}
	*dst = '\0';

	return 0;
}

static void AppendUriPart(std::string* dst, const char* part) {
	EXIT_IF(dst == nullptr);

	if (part != nullptr) {
		dst->append(part);
	}
}

static int KYTY_SYSV_ABI HttpUriBuild(char* out, size_t* require, size_t prepare,
                                      const SceHttpUriElement* src_element, uint32_t option) {
	PRINT_NAME();

	LOGF("\t out         = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(out));
	LOGF("\t require     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(require));
	LOGF("\t prepare     = %" PRIu64 "\n", static_cast<uint64_t>(prepare));
	LOGF("\t src_element = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(src_element));
	LOGF("\t option      = 0x%08" PRIx32 "\n", option);

	if (src_element == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	if (out == nullptr && require == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	std::string uri;
	if (src_element->scheme != nullptr) {
		uri.append(src_element->scheme);
		uri.push_back(':');
	}

	if (src_element->opaque == 0) {
		if (src_element->hostname != nullptr) {
			uri.append("//");
			if (src_element->username != nullptr) {
				uri.append(src_element->username);
				if (src_element->password != nullptr) {
					uri.push_back(':');
					uri.append(src_element->password);
				}
				uri.push_back('@');
			}
			uri.append(src_element->hostname);
			if (src_element->port != 0) {
				uri.push_back(':');
				uri.append(std::to_string(src_element->port));
			}
		}
	}

	AppendUriPart(&uri, src_element->path);
	AppendUriPart(&uri, src_element->query);
	AppendUriPart(&uri, src_element->fragment);

	const auto needed = uri.size() + 1;
	if (require != nullptr) {
		*require = needed;
	}

	if (out != nullptr) {
		if (prepare != 0 && prepare < needed) {
			return HTTP_ERROR_OUT_OF_MEMORY;
		}

		std::memcpy(out, uri.c_str(), needed);
	}

	return 0;
}

LIB_DEFINE(InitNet_1_Http) {
	LIB_FUNC("A9cVMUtEp4Y", Http::HttpInit);
	LIB_FUNC("Ik-KpLTlf7Q", Http::HttpTerm);
	LIB_FUNC("0gYjPTR-6cY", Http::HttpCreateTemplate);
	LIB_FUNC("4I8vEpuEhZ8", Http::HttpDeleteTemplate);
	LIB_FUNC("s2-NPIvz+iA", Http::HttpSetNonblock);
	LIB_FUNC("htyBOoWeS58", Http::HttpsSetSslCallback);
	LIB_FUNC("jUjp+yqMNdQ", Http::HttpsSetMinSslVersion);
	LIB_FUNC("mSQCxzWTwVI", Http::HttpsDisableOption);
	LIB_FUNC("6381dWF+xsQ", Http::HttpCreateEpoll);
	LIB_FUNC("wYhXVfS2Et4", Http::HttpDestroyEpoll);
	LIB_FUNC("-xm7kZQNpHI", Http::HttpSetEpoll);
	LIB_FUNC("59tL1AQBb8U", Http::HttpUnsetEpoll);
	LIB_FUNC("Kiwv9r4IZCc", Http::HttpCreateConnection);
	LIB_FUNC("qgxDBjorUxs", Http::HttpCreateConnectionWithURL);
	LIB_FUNC("P6A3ytpsiYc", Http::HttpDeleteConnection);
	LIB_FUNC("tsGVru3hCe8", Http::HttpCreateRequest);
	LIB_FUNC("Aeu5wVKkF9w", Http::HttpCreateRequest);
	LIB_FUNC("Cnp77podkCU", Http::HttpCreateRequestWithURL2);
	LIB_FUNC("qe7oZ+v4PWA", Http::HttpDeleteRequest);
	LIB_FUNC("PTiFIUxCpJc", Http::HttpSetRequestContentLength);
	LIB_FUNC("EY28T2bkN7k", Http::HttpAddRequestHeader);
	LIB_FUNC("1e2BNwI-XzE", Http::HttpSendRequest);
	LIB_FUNC("hvG6GfBMXg8", Http::HttpAbortRequest);
	LIB_FUNC("qISjDHrxONc", Http::HttpWaitRequest);
	LIB_FUNC("0a2TBNfE3BU", Http::HttpGetStatusCode);
	LIB_FUNC("aCYPMSUIaP8", Http::HttpGetAllResponseHeaders);
	LIB_FUNC("yuO2H2Uvnos", Http::HttpGetResponseContentLength);
	LIB_FUNC("Tc-hAYDKtQc", Http::HttpSetResolveTimeOut);
	LIB_FUNC("K1d1LqZRQHQ", Http::HttpSetResolveRetry);
	LIB_FUNC("0S9tTH0uqTU", Http::HttpSetConnectTimeOut);
	LIB_FUNC("xegFfZKBVlw", Http::HttpSetSendTimeOut);
	LIB_FUNC("yigr4V0-HTM", Http::HttpSetRecvTimeOut);
	LIB_FUNC("T-mGo9f3Pu4", Http::HttpSetAutoRedirect);
	LIB_FUNC("qFg2SuyTJJY", Http::HttpSetAuthEnabled);
	LIB_FUNC("IWalAn-guFs", LibHttp::HttpUriParse);
	LIB_FUNC("YuOW3dDAKYc", LibHttp::HttpUriEscape);
	LIB_FUNC("5LZA+KPISVA", LibHttp::HttpUriBuild);
}

} // namespace LibHttp

namespace LibHttp2 {

LIB_VERSION("Http2", 1, "Http2", 1, 1);

constexpr int HTTP2_ERROR_INVALID_ID   = -2122641152; /* 0x817B1100 */
constexpr int HTTP2_ERROR_NULL_POINTER = -2122640859; /* 0x817B1225 */

struct Http2Options {
	bool     auth_enabled               = false;
	bool     auto_redirect              = false;
	bool     inflate_gzip               = false;
	uint32_t ssl_options                = 0;
	uint32_t resolve_timeout_us         = 0;
	uint32_t connect_timeout_us         = 0;
	uint32_t connection_wait_timeout_us = 0;
	uint32_t send_timeout_us            = 0;
	uint32_t recv_timeout_us            = 0;
	uint32_t timeout_us                 = 0;
	void*    ssl_callback               = nullptr;
	void*    ssl_callback_arg           = nullptr;
	void*    redirect_callback          = nullptr;
	void*    redirect_user_arg          = nullptr;
};

struct Http2Context {
	int    libnet_mem_id           = 0;
	int    libssl_ctx_id           = 0;
	size_t pool_size               = 0;
	int    max_concurrent_requests = 0;
};

struct Http2Template {
	int          context_id = 0;
	std::string  user_agent;
	int          http_ver           = 0;
	int          is_auto_proxy_conf = 0;
	Http2Options options;
};

struct Http2Request {
	int                                              tmpl_id = 0;
	std::string                                      method;
	std::string                                      url;
	uint64_t                                         content_length = 0;
	std::vector<std::pair<std::string, std::string>> headers;
	bool                                             sent        = false;
	int                                              status_code = 204;
	std::string  response_headers                                = "HTTP/2 204 No Content\r\n\r\n";
	std::string  response_body;
	size_t       read_offset  = 0;
	int          async_result = 0;
	int          async_event  = 0;
	Http2Options options;
};

struct Http2AsyncResult {
	int     event_type;
	int     req_id;
	int     result;
	uint8_t padding[4];
	void*   reserved;
};

static int g_http2_next_context_id  = 1;
static int g_http2_next_template_id = 1;
static int g_http2_next_request_id  = 1;

static std::map<int, Http2Context>  g_http2_contexts;
static std::map<int, Http2Template> g_http2_templates;
static std::map<int, Http2Request>  g_http2_requests;

static Http2Options* GetHttp2Options(int id) {
	auto request = g_http2_requests.find(id);
	if (request != g_http2_requests.end()) {
		return &request->second.options;
	}

	auto tmpl = g_http2_templates.find(id);
	if (tmpl != g_http2_templates.end()) {
		return &tmpl->second.options;
	}

	return nullptr;
}

static int KYTY_SYSV_ABI Http2Init(int libnet_mem_id, int libssl_ctx_id, size_t pool_size,
                                   int max_concurrent_request) {
	PRINT_NAME();

	LOGF("\t libnet_mem_id          = %d\n"
	     "\t libssl_ctx_id          = %d\n"
	     "\t pool_size              = %" PRIu64 "\n"
	     "\t max_concurrent_request = %d\n",
	     libnet_mem_id, libssl_ctx_id, pool_size, max_concurrent_request);

	const auto id        = g_http2_next_context_id++;
	g_http2_contexts[id] = {libnet_mem_id, libssl_ctx_id, pool_size, max_concurrent_request};

	return id;
}

static int KYTY_SYSV_ABI Http2CreateTemplate(int lib_http2_ctx_id, const char* user_agent,
                                             int http_ver, int is_auto_proxy_conf) {
	PRINT_NAME();

	LOGF("\t lib_http2_ctx_id   = %d\n", lib_http2_ctx_id);
	LOGF("\t user_agent         = %s\n", user_agent != nullptr ? user_agent : "(null)");
	LOGF("\t http_ver           = %d\n", http_ver);
	LOGF("\t is_auto_proxy_conf = %d\n", is_auto_proxy_conf);

	if (g_http2_contexts.find(lib_http2_ctx_id) == g_http2_contexts.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const auto tmpl_id         = g_http2_next_template_id++;
	g_http2_templates[tmpl_id] = {lib_http2_ctx_id, user_agent != nullptr ? user_agent : "",
	                              http_ver, is_auto_proxy_conf};

	return tmpl_id;
}

static int KYTY_SYSV_ABI Http2DeleteTemplate(int tmpl_id) {
	PRINT_NAME();

	LOGF("\t tmpl_id = %d\n", tmpl_id);

	auto tmpl = g_http2_templates.find(tmpl_id);
	if (tmpl == g_http2_templates.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	for (auto request = g_http2_requests.begin(); request != g_http2_requests.end();) {
		if (request->second.tmpl_id == tmpl_id) {
			request = g_http2_requests.erase(request);
		} else {
			++request;
		}
	}

	g_http2_templates.erase(tmpl);

	return 0;
}

static int KYTY_SYSV_ABI Http2Term(int lib_http2_ctx_id) {
	PRINT_NAME();

	LOGF("\t lib_http2_ctx_id = %d\n", lib_http2_ctx_id);

	auto context = g_http2_contexts.find(lib_http2_ctx_id);
	if (context == g_http2_contexts.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	for (auto tmpl = g_http2_templates.begin(); tmpl != g_http2_templates.end();) {
		if (tmpl->second.context_id == lib_http2_ctx_id) {
			const auto tmpl_id = tmpl->first;
			for (auto request = g_http2_requests.begin(); request != g_http2_requests.end();) {
				if (request->second.tmpl_id == tmpl_id) {
					request = g_http2_requests.erase(request);
				} else {
					++request;
				}
			}
			tmpl = g_http2_templates.erase(tmpl);
		} else {
			++tmpl;
		}
	}

	g_http2_contexts.erase(context);

	return 0;
}

static int KYTY_SYSV_ABI Http2CreateRequestWithURL(int tmpl_id, const char* method, const char* url,
                                                   uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t tmpl_id        = %d\n", tmpl_id);
	LOGF("\t method         = %s\n", method != nullptr ? method : "(null)");
	LOGF("\t url            = %s\n", url != nullptr ? url : "(null)");
	LOGF("\t content_length = %" PRIu64 "\n", content_length);

	if (method == nullptr || url == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	if (g_http2_templates.find(tmpl_id) == g_http2_templates.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const auto req_id        = g_http2_next_request_id++;
	g_http2_requests[req_id] = {tmpl_id, method, url, content_length};

	return req_id;
}

static int KYTY_SYSV_ABI Http2AddRequestHeader(int id, const char* name, const char* value,
                                               uint32_t mode) {
	PRINT_NAME();

	LOGF("\t id    = %d\n", id);
	LOGF("\t name  = %s\n", name != nullptr ? name : "(null)");
	LOGF("\t value = %s\n", value != nullptr ? value : "(null)");
	LOGF("\t mode  = %" PRIu32 "\n", mode);

	if (name == nullptr || value == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.headers.emplace_back(name, value);

	return 0;
}

static int KYTY_SYSV_ABI Http2SetRequestContentLength(int id, uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t id             = %d\n", id);
	LOGF("\t content_length = %" PRIu64 "\n", content_length);

	auto request = g_http2_requests.find(id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.content_length = content_length;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetAuthEnabled(int id, int is_enable) {
	PRINT_NAME();

	LOGF("\t id        = %d\n", id);
	LOGF("\t is_enable = %d\n", is_enable);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->auth_enabled = is_enable != 0;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetAutoRedirect(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n", id);
	LOGF("\t enable = %d\n", enable);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->auto_redirect = enable != 0;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetInflateGZIPEnabled(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n", id);
	LOGF("\t enable = %d\n", enable);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->inflate_gzip = enable != 0;

	return 0;
}

static int KYTY_SYSV_ABI Http2SslEnableOption(int id, uint32_t ssl_flags) {
	PRINT_NAME();

	LOGF("\t id        = %d\n", id);
	LOGF("\t ssl_flags = 0x%08" PRIx32 "\n", ssl_flags);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->ssl_options |= ssl_flags;

	return 0;
}

static int KYTY_SYSV_ABI Http2SslDisableOption(int id, uint32_t ssl_flags) {
	PRINT_NAME();

	LOGF("\t id        = %d\n", id);
	LOGF("\t ssl_flags = 0x%08" PRIx32 "\n", ssl_flags);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->ssl_options &= ~ssl_flags;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetSslCallback(int id, void* cb_func, void* user_arg) {
	PRINT_NAME();

	LOGF("\t id       = %d\n", id);
	LOGF("\t cb_func  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cb_func));
	LOGF("\t user_arg = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_arg));

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->ssl_callback     = cb_func;
	options->ssl_callback_arg = user_arg;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetRedirectCallback(int id, void* cb_func, void* user_arg) {
	PRINT_NAME();

	LOGF("\t id       = %d\n", id);
	LOGF("\t cb_func  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cb_func));
	LOGF("\t user_arg = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_arg));

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->redirect_callback = cb_func;
	options->redirect_user_arg = user_arg;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetConnectTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->connect_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetConnectionWaitTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->connection_wait_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetResolveTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->resolve_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetSendTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->send_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SetRecvTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n", id);
	LOGF("\t usec = %" PRIu32 "\n", usec);

	auto* options = GetHttp2Options(id);
	if (options == nullptr) {
		return HTTP2_ERROR_INVALID_ID;
	}

	options->recv_timeout_us = usec;

	return 0;
}

static int KYTY_SYSV_ABI Http2SendRequest(int req_id, const void* post_data, size_t size) {
	PRINT_NAME();

	LOGF("\t req_id    = %d\n", req_id);
	LOGF("\t post_data = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(post_data));
	LOGF("\t size      = %" PRIu64 "\n", size);

	if (post_data == nullptr && size != 0) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.sent = true;

	return 0;
}

static int KYTY_SYSV_ABI Http2SendRequestAsync(int req_id, const void* post_data, size_t size,
                                               void* /*kqueue_option*/, void* /*option*/) {
	PRINT_NAME();

	LOGF("\t req_id    = %d\n", req_id);
	LOGF("\t post_data = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(post_data));
	LOGF("\t size      = %" PRIu64 "\n", size);

	if (post_data == nullptr && size != 0) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.sent         = true;
	request->second.async_result = 0;
	request->second.async_event  = 0;

	return 0;
}

static int KYTY_SYSV_ABI Http2WaitAsync(int req_id, Http2AsyncResult* result, uint32_t* /*timeout*/,
                                        void* /*option*/) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	if (result == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	request->second.sent = true;
	*result              = {};
	result->event_type   = request->second.async_event;
	result->req_id       = req_id;
	result->result       = request->second.async_result;

	return 0;
}

static int KYTY_SYSV_ABI Http2GetStatusCode(int req_id, int* status_code) {
	PRINT_NAME();

	LOGF("\t req_id      = %d\n", req_id);
	LOGF("\t status_code = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(status_code));

	if (status_code == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	*status_code = request->second.status_code;

	return 0;
}

static int KYTY_SYSV_ABI Http2GetResponseContentLength(int req_id, int* result,
                                                       uint64_t* content_length) {
	PRINT_NAME();

	LOGF("\t req_id         = %d\n", req_id);
	LOGF("\t result         = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));
	LOGF("\t content_length = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(content_length));

	if (result == nullptr || content_length == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	*result         = 0; // SCE_HTTP2_CONTENTLEN_EXIST
	*content_length = request->second.response_body.size();

	return 0;
}

static int KYTY_SYSV_ABI Http2GetAllResponseHeaders(int req_id, char** header,
                                                    size_t* header_size) {
	PRINT_NAME();

	LOGF("\t req_id      = %d\n", req_id);
	LOGF("\t header      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(header));
	LOGF("\t header_size = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(header_size));

	if (header == nullptr || header_size == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	*header      = const_cast<char*>(request->second.response_headers.c_str());
	*header_size = request->second.response_headers.size();

	return 0;
}

static int KYTY_SYSV_ABI Http2ReadData(int req_id, void* data, size_t size) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	LOGF("\t size   = %" PRIu64 "\n", size);

	if (data == nullptr && size != 0) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const auto& body = request->second.response_body;
	const auto  remaining =
	    request->second.read_offset < body.size() ? body.size() - request->second.read_offset : 0;
	const auto to_copy = size < remaining ? size : remaining;

	if (to_copy != 0) {
		std::memcpy(data, body.data() + request->second.read_offset, to_copy);
		request->second.read_offset += to_copy;
	}

	return static_cast<int>(to_copy);
}

static int KYTY_SYSV_ABI Http2ReadDataAsync(int req_id, void* data, size_t size,
                                            void* /*kqueue_option*/, void* /*option*/) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t data   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	LOGF("\t size   = %" PRIu64 "\n", size);

	if (data == nullptr && size != 0) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const auto& body = request->second.response_body;
	const auto  remaining =
	    request->second.read_offset < body.size() ? body.size() - request->second.read_offset : 0;
	const auto to_copy = size < remaining ? size : remaining;

	if (to_copy != 0) {
		std::memcpy(data, body.data() + request->second.read_offset, to_copy);
		request->second.read_offset += to_copy;
	}

	request->second.async_result = static_cast<int>(to_copy);
	request->second.async_event  = 1;

	return 0;
}

static int KYTY_SYSV_ABI Http2DeleteRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	g_http2_requests.erase(request);

	return 0;
}

LIB_DEFINE(InitNet_1_Http2) {
	LIB_FUNC("3JCe3lCbQ8A", LibHttp2::Http2Init);
	LIB_FUNC("YiBUtz-pGkc", LibHttp2::Http2Term);
	LIB_FUNC("+wCt7fCijgk", LibHttp2::Http2CreateTemplate);
	LIB_FUNC("pDom5-078DA", LibHttp2::Http2DeleteTemplate);
	LIB_FUNC("mmyOCxQMVYQ", LibHttp2::Http2CreateRequestWithURL);
	LIB_FUNC("nrPfOE8TQu0", LibHttp2::Http2AddRequestHeader);
	LIB_FUNC("FSAFOzi0FpM", LibHttp2::Http2SetRequestContentLength);
	LIB_FUNC("jjFahkBPCYs", LibHttp2::Http2SetAuthEnabled);
	LIB_FUNC("b9AvoIaOuHI", LibHttp2::Http2SetAutoRedirect);
	LIB_FUNC("uRosf8GQbHQ", LibHttp2::Http2SetInflateGZIPEnabled);
	LIB_FUNC("B37SruheQ5Y", LibHttp2::Http2SslDisableOption);
	LIB_FUNC("EWcwMpbr5F8", LibHttp2::Http2SslEnableOption);
	LIB_FUNC("BJgi0CH7al4", LibHttp2::Http2SetRedirectCallback);
	LIB_FUNC("izvHhqgDt44", LibHttp2::Http2SetRecvTimeOut);
	LIB_FUNC("XPtW45xiLHk", LibHttp2::Http2SetSendTimeOut);
	LIB_FUNC("-HIO4VT87v8", LibHttp2::Http2SetConnectTimeOut);
	LIB_FUNC("n8hMLe31OPA", LibHttp2::Http2SetConnectionWaitTimeOut);
	LIB_FUNC("ACjtE27aErY", LibHttp2::Http2SetResolveTimeOut);
	LIB_FUNC("VYMxTcBqSE0", LibHttp2::Http2SetTimeOut);
	LIB_FUNC("YrWX+DhPHQY", LibHttp2::Http2SetSslCallback);
	LIB_FUNC("rbqZig38AT8", LibHttp2::Http2SendRequest);
	LIB_FUNC("A+NVAFu4eCg", LibHttp2::Http2SendRequestAsync);
	LIB_FUNC("o0DBQpFE13o", LibHttp2::Http2GetResponseContentLength);
	LIB_FUNC("9XYJwCf3lEA", LibHttp2::Http2GetStatusCode);
	LIB_FUNC("-rdXUi2XW90", LibHttp2::Http2GetAllResponseHeaders);
	LIB_FUNC("QygCNNmbGss", LibHttp2::Http2ReadData);
	LIB_FUNC("bGN-6zbo7ms", LibHttp2::Http2ReadDataAsync);
	LIB_FUNC("c8D9qIjo8EY", LibHttp2::Http2DeleteRequest);
	LIB_FUNC("MOp-AUhdfi8", LibHttp2::Http2WaitAsync);
}

} // namespace LibHttp2

namespace LibNetCtl {

LIB_VERSION("NetCtl", 1, "NetCtl", 1, 1);

namespace NetCtl = Network::NetCtl;

LIB_DEFINE(InitNet_1_NetCtl) {
	LIB_FUNC("gky0+oaNM4k", NetCtl::NetCtlInit);
	LIB_FUNC("JO4yuTuMoKI", NetCtl::NetCtlGetNatInfo);
	LIB_FUNC("iQw3iQPhvUQ", NetCtl::NetCtlCheckCallback);
	LIB_FUNC("uBPlr0lbuiI", NetCtl::NetCtlGetState);
	LIB_FUNC("+lxqIKeU9UY", NetCtl::NetCtlGetStateV6);
	LIB_FUNC("UJ+Z7Q+4ck0", NetCtl::NetCtlRegisterCallback);
	LIB_FUNC("Rqm2OnZMCz0", NetCtl::NetCtlUnregisterCallback);
	LIB_FUNC("0cBgduPRR+M", NetCtl::NetCtlGetResult);
	LIB_FUNC("obuxdTiwkF8", NetCtl::NetCtlGetInfo);
	LIB_FUNC("Z4wwCFiBELQ", NetCtl::NetCtlTerm);
}

} // namespace LibNetCtl

namespace LibNpManager {

LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

namespace NpManager = Network::NpManager;

LIB_DEFINE(InitNet_1_NpManager) {
	LIB_FUNC("3Zl8BePTh9Y", NpManager::NpCheckCallback);
	LIB_FUNC("Ec63y59l9tw", NpManager::NpSetNpTitleId);
	LIB_FUNC("A2CQ3kgSopQ", NpManager::NpSetContentRestriction);
	LIB_FUNC("VfRSmPmj8Q8", NpManager::NpRegisterStateCallback);
	LIB_FUNC("qQJfO8HAiaY", NpManager::NpRegisterStateCallback);
	LIB_FUNC("M3wFXbYQtAA", NpManager::NpUnregisterStateCallback);
	LIB_FUNC("uFJpaKNBAj4", NpManager::NpRegisterGamePresenceCallback);
	LIB_FUNC("GImICnh+boA", NpManager::NpRegisterPlusEventCallback);
	LIB_FUNC("+yqjab2fUJA", NpManager::NpRegisterPremiumEventCallback);
	LIB_FUNC("hw5KNqAAels", NpManager::NpRegisterNpReachabilityStateCallback);
	LIB_FUNC("p-o74CnoNzY", NpManager::NpGetNpId);
	LIB_FUNC("XDncXQIJUSk", NpManager::NpGetOnlineId);
	LIB_FUNC("rbknaUjpqWo", NpManager::NpGetAccountIdA);
	LIB_FUNC("JT+t00a3TxA", NpManager::NpGetAccountCountryA);
	LIB_FUNC("+4DegjBqV1g", NpManager::NpGetAccountAge);
	LIB_FUNC("GpLQDNKICac", NpManager::NpCreateRequest);
	LIB_FUNC("eiqMCt9UshI", NpManager::NpCreateAsyncRequest);
	LIB_FUNC("S7QTn72PrDw", NpManager::NpDeleteRequest);
	LIB_FUNC("OzKvTvg3ZYU", NpManager::NpAbortRequest);
	LIB_FUNC("2rsFmlGWleQ", NpManager::NpCheckNpAvailability);
	LIB_FUNC("KfGZg2y73oM", NpManager::NpCheckNpReachability);
	LIB_FUNC("uqcPJLWL08M", NpManager::NpPollAsync);
	LIB_FUNC("O80NrhUOPGY", NpManager::NpCheckPremium);
	LIB_FUNC("eQH7nWPcAgc", NpManager::NpGetState);
	LIB_FUNC("e-ZuhGEoeC4", NpManager::NpGetNpReachabilityState);
}

} // namespace LibNpManager

namespace LibNpSessionSignaling {

LIB_VERSION("NpSessionSignaling", 1, "NpSessionSignaling", 1, 1);

static int KYTY_SYSV_ABI NpSessionSignalingInitialize(void* param) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	return 0;
}

LIB_DEFINE(InitNet_1_NpSessionSignaling) {
	LIB_FUNC("ysmw6J-P8Ak", NpSessionSignalingInitialize);
}

} // namespace LibNpSessionSignaling

namespace LibNpEntitlementAccess {

LIB_VERSION("NpEntitlementAccess", 1, "NpEntitlementAccess", 1, 1);

constexpr int      NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER      = -2122514430; /* 0x817D0002 */
constexpr int      NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT = -2122514425; /* 0x817D0007 */
constexpr uint32_t NP_ENTITLEMENT_ACCESS_SKU_FLAG_FULL        = 3;

struct NpEntitlementAccessInitParam {
	char reserved[32];
};

struct NpEntitlementAccessBootParam {
	char reserved[32];
};

struct NpUnifiedEntitlementLabel {
	char data[17];
	char padding[3];
};

struct NpEntitlementAccessAddcontEntitlementInfo {
	NpUnifiedEntitlementLabel entitlement_label;
	uint32_t                  package_type;
	uint32_t                  download_status;
};

static constexpr NpEntitlementAccessAddcontEntitlementInfo NP_ENTITLEMENT_ACCESS_ADDON_LIST[] = {
    {{{"85y-je"}, {}}, 3, 4}, // GTA V hash 0xf4315381
    {{{"5d5c48"}, {}}, 3, 4}, // GTA V hash 0x961c34b0
    {{{"_mtqu6"}, {}}, 3, 4}, // GTA V hash 0x9cd1bcad
};

static int KYTY_SYSV_ABI NpEntitlementAccessInitialize(
    const NpEntitlementAccessInitParam* init_param, NpEntitlementAccessBootParam* boot_param) {
	PRINT_NAME();

	if (init_param == nullptr || boot_param == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	memset(boot_param, 0, sizeof(*boot_param));

	return 0;
}

static int KYTY_SYSV_ABI NpEntitlementAccessGetSkuFlag(uint32_t* sku_flag) {
	PRINT_NAME();

	if (sku_flag == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	*sku_flag = NP_ENTITLEMENT_ACCESS_SKU_FLAG_FULL;
	return 0;
}

static int KYTY_SYSV_ABI NpEntitlementAccessGetAddcontEntitlementInfoList(
    uint32_t service_label, NpEntitlementAccessAddcontEntitlementInfo* list, uint32_t list_num,
    uint32_t* hit_num) {
	PRINT_NAME();

	LOGF("\t service_label = %" PRIu32 "\n", service_label);
	LOGF("\t list          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(list));
	LOGF("\t list_num      = %" PRIu32 "\n", list_num);
	LOGF("\t hit_num       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(hit_num));

	if (hit_num == nullptr || (list == nullptr && list_num != 0)) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	*hit_num = static_cast<uint32_t>(sizeof(NP_ENTITLEMENT_ACCESS_ADDON_LIST) /
	                                 sizeof(NP_ENTITLEMENT_ACCESS_ADDON_LIST[0]));

	if (list != nullptr && list_num != 0) {
		memset(list, 0, sizeof(*list) * list_num);

		const auto copy_num = (list_num < *hit_num ? list_num : *hit_num);
		for (uint32_t i = 0; i < copy_num; i++) {
			list[i] = NP_ENTITLEMENT_ACCESS_ADDON_LIST[i];
		}
	}

	return 0;
}

static int KYTY_SYSV_ABI NpEntitlementAccessGetAddcontEntitlementInfo(
    uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label,
    NpEntitlementAccessAddcontEntitlementInfo* info) {
	PRINT_NAME();

	LOGF("\t service_label     = %" PRIu32 "\n", service_label);
	LOGF("\t entitlement_label = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(entitlement_label));
	LOGF("\t info              = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));

	if (entitlement_label == nullptr || info == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	memset(info, 0, sizeof(*info));

	for (const auto& entitlement: NP_ENTITLEMENT_ACCESS_ADDON_LIST) {
		if (strncmp(entitlement_label->data, entitlement.entitlement_label.data,
		            sizeof(entitlement_label->data)) == 0) {
			*info = entitlement;
			return 0;
		}
	}

	return NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

LIB_DEFINE(InitNet_1_NpEntitlementAccess) {
	LIB_FUNC("jO8DM8oyego", LibNpEntitlementAccess::NpEntitlementAccessInitialize);
	LIB_FUNC("lPDO62PpJIA", LibNpEntitlementAccess::NpEntitlementAccessGetSkuFlag);
	LIB_FUNC("TFyU+KFBv54",
	         LibNpEntitlementAccess::NpEntitlementAccessGetAddcontEntitlementInfoList);
	LIB_FUNC("xddD23+8TfQ", LibNpEntitlementAccess::NpEntitlementAccessGetAddcontEntitlementInfo);
}

} // namespace LibNpEntitlementAccess

namespace LibNpAuth {

LIB_VERSION("NpAuth", 1, "NpAuth", 1, 1);

constexpr int NP_AUTH_MAX_REQUEST_NUM         = 16;
constexpr int NP_AUTH_REQUEST_ID_OFFSET       = 0x10000000;
constexpr int NP_AUTH_ERROR_INVALID_ARGUMENT  = -2141912319; /* 0x80550301 */
constexpr int NP_AUTH_ERROR_ABORTED           = -2141912316; /* 0x80550304 */
constexpr int NP_AUTH_ERROR_REQUEST_MAX       = -2141912315; /* 0x80550305 */
constexpr int NP_AUTH_ERROR_REQUEST_NOT_FOUND = -2141912314; /* 0x80550306 */
constexpr int NP_AUTH_ERROR_INVALID_ID        = -2141912313; /* 0x80550307 */
constexpr int NP_ERROR_SIGNED_OUT             = -2141913082; /* 0x80550006 */

enum class NpAuthRequestState {
	None = 0,
	Ready,
	Aborted,
	Complete,
};

struct NpAuthRequest {
	NpAuthRequestState state  = NpAuthRequestState::None;
	bool               async  = false;
	int                result = 0;
};

static std::mutex                 g_np_auth_request_mutex;
static int                        g_np_auth_active_requests = 0;
static std::vector<NpAuthRequest> g_np_auth_requests;

static int np_auth_request_index(int req_id) {
	return req_id - NP_AUTH_REQUEST_ID_OFFSET - 1;
}

static NpAuthRequest* np_auth_find_request(int req_id) {
	const int index = np_auth_request_index(req_id);
	if (index < 0 || g_np_auth_active_requests == 0 ||
	    static_cast<size_t>(index) >= g_np_auth_requests.size() ||
	    g_np_auth_requests[index].state == NpAuthRequestState::None) {
		return nullptr;
	}

	return &g_np_auth_requests[index];
}

static int np_auth_create_request(bool async) {
	PRINT_NAME();

	LOGF("\t async = %d\n", async ? 1 : 0);

	std::scoped_lock lock(g_np_auth_request_mutex);

	if (g_np_auth_active_requests >= NP_AUTH_MAX_REQUEST_NUM) {
		return NP_AUTH_ERROR_REQUEST_MAX;
	}

	size_t index = 0;
	for (; index < g_np_auth_requests.size(); index++) {
		if (g_np_auth_requests[index].state == NpAuthRequestState::None) {
			g_np_auth_requests[index] = {NpAuthRequestState::Ready, async, 0};
			break;
		}
	}

	if (index == g_np_auth_requests.size()) {
		g_np_auth_requests.push_back({NpAuthRequestState::Ready, async, 0});
	}

	g_np_auth_active_requests++;

	return static_cast<int>(index) + NP_AUTH_REQUEST_ID_OFFSET + 1;
}

static int KYTY_SYSV_ABI NpAuthCreateRequest() {
	return np_auth_create_request(false);
}

static int KYTY_SYSV_ABI NpAuthCreateAsyncRequest(const void* param) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	return np_auth_create_request(true);
}

static int KYTY_SYSV_ABI NpAuthDeleteRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	std::scoped_lock lock(g_np_auth_request_mutex);

	auto* request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}

	request->state  = NpAuthRequestState::None;
	request->async  = false;
	request->result = 0;
	g_np_auth_active_requests--;

	return 0;
}

static int KYTY_SYSV_ABI NpAuthAbortRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	std::scoped_lock lock(g_np_auth_request_mutex);

	auto* request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}

	if (request->state != NpAuthRequestState::Complete) {
		request->state = NpAuthRequestState::Aborted;
	}

	return 0;
}

static int np_auth_wait_or_poll_async(int req_id, int* result) {
	if (result == nullptr) {
		return NP_AUTH_ERROR_INVALID_ARGUMENT;
	}

	std::scoped_lock lock(g_np_auth_request_mutex);

	auto* request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}

	if (!request->async || request->state == NpAuthRequestState::Ready) {
		return NP_AUTH_ERROR_INVALID_ID;
	}

	*result = request->result;
	return 0;
}

static int np_auth_complete_signed_out(NpAuthRequest* request) {
	if (request->state == NpAuthRequestState::Complete) {
		request->result = NP_AUTH_ERROR_INVALID_ARGUMENT;
		return NP_AUTH_ERROR_INVALID_ARGUMENT;
	}
	if (request->state == NpAuthRequestState::Aborted) {
		request->result = NP_AUTH_ERROR_ABORTED;
		return NP_AUTH_ERROR_ABORTED;
	}

	request->state  = NpAuthRequestState::Complete;
	request->result = NP_ERROR_SIGNED_OUT;

	return request->async ? 0 : NP_ERROR_SIGNED_OUT;
}

static int KYTY_SYSV_ABI NpAuthWaitAsync(int req_id, int* result) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	return np_auth_wait_or_poll_async(req_id, result);
}

static int KYTY_SYSV_ABI NpAuthPollAsync(int req_id, int* result) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	return np_auth_wait_or_poll_async(req_id, result);
}

static int KYTY_SYSV_ABI NpAuthGetAuthorizationCodeV3(int req_id, const void* param,
                                                      void* auth_code, int* issuer_id) {
	PRINT_NAME();

	LOGF("\t req_id    = %d\n", req_id);
	LOGF("\t param     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	LOGF("\t auth_code = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(auth_code));
	LOGF("\t issuer_id = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(issuer_id));

	if (auth_code != nullptr) {
		memset(auth_code, 0, 136);
	}
	if (issuer_id != nullptr) {
		*issuer_id = 0;
	}

	std::scoped_lock lock(g_np_auth_request_mutex);
	auto*            request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}
	if (request->state == NpAuthRequestState::Aborted) {
		// request->result = 0;
		// return 0;
		request->result = NP_AUTH_ERROR_ABORTED;
		return NP_AUTH_ERROR_ABORTED;
	}
	// request->state  = NpAuthRequestState::Complete;
	// request->result = 0;

	// return 0;
	return np_auth_complete_signed_out(request);
}

static int KYTY_SYSV_ABI NpAuthGetIdTokenV3(int req_id, const void* param, void* id_token) {
	PRINT_NAME();

	LOGF("\t req_id   = %d\n", req_id);
	LOGF("\t param    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	LOGF("\t id_token = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(id_token));

	if (id_token != nullptr) {
		memset(id_token, 0, 4104);
	}

	std::scoped_lock lock(g_np_auth_request_mutex);
	auto*            request = np_auth_find_request(req_id);
	if (request == nullptr) {
		return NP_AUTH_ERROR_REQUEST_NOT_FOUND;
	}
	if (request->state == NpAuthRequestState::Aborted) {
		// request->result = 0;
		// return 0;
		request->result = NP_AUTH_ERROR_ABORTED;
		return NP_AUTH_ERROR_ABORTED;
	}
	// request->state  = NpAuthRequestState::Complete;
	// request->result = 0;

	// return 0;
	return np_auth_complete_signed_out(request);
}

LIB_DEFINE(InitNet_1_NpAuth) {
	LIB_FUNC("6bwFkosYRQg", LibNpAuth::NpAuthCreateRequest);
	LIB_FUNC("N+mr7GjTvr8", LibNpAuth::NpAuthCreateAsyncRequest);
	LIB_FUNC("H8wG9Bk-nPc", LibNpAuth::NpAuthDeleteRequest);
	LIB_FUNC("cE7wIsqXdZ8", LibNpAuth::NpAuthAbortRequest);
	LIB_FUNC("SK-S7daqJSE", LibNpAuth::NpAuthWaitAsync);
	LIB_FUNC("gjSyfzSsDcE", LibNpAuth::NpAuthPollAsync);
	LIB_FUNC("KI4dHLlTNl0", LibNpAuth::NpAuthGetAuthorizationCodeV3);
	LIB_FUNC("RdsFVsgSpZY", LibNpAuth::NpAuthGetIdTokenV3);
}

} // namespace LibNpAuth

namespace LibNpTrophy2 {

LIB_VERSION("NpTrophy2", 1, "NpTrophy2", 1, 1);

constexpr int NP_TROPHY2_ERROR_ICON_FILE_NOT_FOUND = -2141898479; /* 0x80553911 */

struct NpTrophy2Progress {
	int32_t  type;
	uint8_t  reserved[4];
	uint64_t value;
};

struct NpTrophy2GameDetails {
	uint32_t num_groups;
	uint32_t num_trophies;
	uint32_t num_platinum;
	uint32_t num_gold;
	uint32_t num_silver;
	uint32_t num_bronze;
	char     title[128];
};

struct NpTrophy2GameData {
	uint32_t unlocked_trophies;
	uint32_t unlocked_platinum;
	uint32_t unlocked_gold;
	uint32_t unlocked_silver;
	uint32_t unlocked_bronze;
	uint32_t progress_percentage;
};

struct NpTrophy2GroupDetails {
	int32_t  group_id;
	uint32_t num_trophies;
	uint32_t num_platinum;
	uint32_t num_gold;
	uint32_t num_silver;
	uint32_t num_bronze;
	char     title[128];
};

struct NpTrophy2GroupData {
	int32_t  group_id;
	uint32_t unlocked_trophies;
	uint32_t unlocked_platinum;
	uint32_t unlocked_gold;
	uint32_t unlocked_silver;
	uint32_t unlocked_bronze;
	uint32_t progress_percentage;
	uint8_t  reserved[4];
};

struct NpTrophy2Details {
	int32_t           trophy_id;
	int32_t           trophy_grade;
	int32_t           group_id;
	bool              hidden;
	bool              has_reward;
	uint8_t           reserved2[2];
	NpTrophy2Progress target;
	char              name[128];
	char              description[1024];
	char              reward[128];
};

struct NpTrophy2Data {
	int32_t           trophy_id;
	bool              unlocked;
	uint8_t           reserved[3];
	NpTrophy2Progress progress;
	uint64_t          timestamp_tick;
};

static_assert(sizeof(NpTrophy2GameDetails) == 152);
static_assert(sizeof(NpTrophy2GameData) == 24);
static_assert(sizeof(NpTrophy2GroupDetails) == 152);
static_assert(sizeof(NpTrophy2GroupData) == 32);
static_assert(sizeof(NpTrophy2Details) == 1312);
static_assert(sizeof(NpTrophy2Data) == 32);

static void NpTrophy2FillTitle(char* dst, size_t dst_size, const char* src) {
	if (dst == nullptr || dst_size == 0) {
		return;
	}

	std::strncpy(dst, src, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static int KYTY_SYSV_ABI NpTrophy2CreateHandle(int* handle) {
	PRINT_NAME();

	if (handle != nullptr) {
		*handle = 1;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2CreateContext(int* context, int user_id, uint32_t service_label,
                                                uint64_t options) {
	PRINT_NAME();

	if (context != nullptr) {
		*context = 1;
	}

	LOGF("\t user_id       = %d\n"
	     "\t service_label = %u\n"
	     "\t options       = 0x%016" PRIx64 "\n",
	     user_id, service_label, options);

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2RegisterContext(int context, int handle, uint64_t options) {
	PRINT_NAME();

	LOGF("\t context = %d\n"
	     "\t handle  = %d\n"
	     "\t options = 0x%016" PRIx64 "\n",
	     context, handle, options);

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetGameInfo(int context, int handle,
                                              NpTrophy2GameDetails* details,
                                              NpTrophy2GameData*    data) {
	PRINT_NAME();

	LOGF("\t context = %d\n"
	     "\t handle  = %d\n"
	     "\t details = 0x%016" PRIx64 "\n"
	     "\t data    = 0x%016" PRIx64 "\n",
	     context, handle, reinterpret_cast<uint64_t>(details), reinterpret_cast<uint64_t>(data));

	if (details != nullptr) {
		std::memset(details, 0, sizeof(*details));
		details->num_trophies = 1;
		details->num_bronze   = 1;
		NpTrophy2FillTitle(details->title, sizeof(details->title), "Kyty");
	}
	if (data != nullptr) {
		std::memset(data, 0, sizeof(*data));
	}

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetGroupInfo(int context, int handle, int group_id,
                                               NpTrophy2GroupDetails* details,
                                               NpTrophy2GroupData*    data) {
	PRINT_NAME();

	LOGF("\t context  = %d\n"
	     "\t handle   = %d\n"
	     "\t group_id = %d\n"
	     "\t details  = 0x%016" PRIx64 "\n"
	     "\t data     = 0x%016" PRIx64 "\n",
	     context, handle, group_id, reinterpret_cast<uint64_t>(details),
	     reinterpret_cast<uint64_t>(data));

	const auto normalized_group_id = (group_id < 0 ? 0 : group_id);

	if (details != nullptr) {
		std::memset(details, 0, sizeof(*details));
		details->group_id     = normalized_group_id;
		details->num_trophies = 1;
		details->num_bronze   = 1;
		NpTrophy2FillTitle(details->title, sizeof(details->title), "Base Game");
	}
	if (data != nullptr) {
		std::memset(data, 0, sizeof(*data));
		data->group_id = normalized_group_id;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetGroupInfoArray(int context, int handle, uint32_t offset,
                                                    uint32_t               limit,
                                                    NpTrophy2GroupDetails* details_array,
                                                    NpTrophy2GroupData*    data_array,
                                                    uint32_t*              count) {
	PRINT_NAME();

	LOGF("\t context       = %d\n"
	     "\t handle        = %d\n"
	     "\t offset        = %" PRIu32 "\n"
	     "\t limit         = %" PRIu32 "\n"
	     "\t details_array = 0x%016" PRIx64 "\n"
	     "\t data_array    = 0x%016" PRIx64 "\n"
	     "\t count         = 0x%016" PRIx64 "\n",
	     context, handle, offset, limit, reinterpret_cast<uint64_t>(details_array),
	     reinterpret_cast<uint64_t>(data_array), reinterpret_cast<uint64_t>(count));

	const uint32_t out_count = (offset == 0 && limit != 0 ? 1u : 0u);

	if (count != nullptr) {
		*count = out_count;
	}
	if (out_count != 0 && details_array != nullptr) {
		std::memset(details_array, 0, sizeof(*details_array));
		details_array->group_id     = 0;
		details_array->num_trophies = 1;
		details_array->num_bronze   = 1;
		NpTrophy2FillTitle(details_array->title, sizeof(details_array->title), "Base Game");
	}
	if (out_count != 0 && data_array != nullptr) {
		std::memset(data_array, 0, sizeof(*data_array));
		data_array->group_id = 0;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetTrophyInfo(int context, int handle, int trophy_id,
                                                NpTrophy2Details* details, NpTrophy2Data* data) {
	PRINT_NAME();

	LOGF("\t context   = %d\n"
	     "\t handle    = %d\n"
	     "\t trophy_id = %d\n"
	     "\t details   = 0x%016" PRIx64 "\n"
	     "\t data      = 0x%016" PRIx64 "\n",
	     context, handle, trophy_id, reinterpret_cast<uint64_t>(details),
	     reinterpret_cast<uint64_t>(data));

	if (details != nullptr) {
		std::memset(details, 0, sizeof(*details));
		details->trophy_id    = trophy_id;
		details->trophy_grade = 4;
		details->group_id     = 0;
		details->target.type  = 0;
		details->target.value = 0;
		NpTrophy2FillTitle(details->name, sizeof(details->name), "Trophy");
		NpTrophy2FillTitle(details->description, sizeof(details->description), "Trophy");
	}
	if (data != nullptr) {
		std::memset(data, 0, sizeof(*data));
		data->trophy_id = trophy_id;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetTrophyInfoArray(int context, int handle, uint32_t offset,
                                                     uint32_t          limit,
                                                     NpTrophy2Details* details_array,
                                                     NpTrophy2Data* data_array, uint32_t* count) {
	PRINT_NAME();

	LOGF("\t context       = %d\n"
	     "\t handle        = %d\n"
	     "\t offset        = %" PRIu32 "\n"
	     "\t limit         = %" PRIu32 "\n"
	     "\t details_array = 0x%016" PRIx64 "\n"
	     "\t data_array    = 0x%016" PRIx64 "\n"
	     "\t count         = 0x%016" PRIx64 "\n",
	     context, handle, offset, limit, reinterpret_cast<uint64_t>(details_array),
	     reinterpret_cast<uint64_t>(data_array), reinterpret_cast<uint64_t>(count));

	const uint32_t out_count = (offset == 0 && limit != 0 ? 1u : 0u);

	if (count != nullptr) {
		*count = out_count;
	}
	if (out_count != 0 && details_array != nullptr) {
		std::memset(details_array, 0, sizeof(*details_array));
		details_array->trophy_id    = 0;
		details_array->trophy_grade = 4;
		details_array->group_id     = 0;
		NpTrophy2FillTitle(details_array->name, sizeof(details_array->name), "Trophy");
		NpTrophy2FillTitle(details_array->description, sizeof(details_array->description),
		                   "Trophy");
	}
	if (out_count != 0 && data_array != nullptr) {
		std::memset(data_array, 0, sizeof(*data_array));
		data_array->trophy_id = 0;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2GetGameIcon(int context, int handle, void* buffer, size_t* size) {
	PRINT_NAME();

	LOGF("\t context = %d\n"
	     "\t handle  = %d\n"
	     "\t buffer  = 0x%016" PRIx64 "\n"
	     "\t size    = 0x%016" PRIx64 "\n",
	     context, handle, reinterpret_cast<uint64_t>(buffer), reinterpret_cast<uint64_t>(size));

	if (size != nullptr) {
		*size = 0;
	}

	return NP_TROPHY2_ERROR_ICON_FILE_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpTrophy2GetGroupIcon(int context, int handle, int group_id, void* buffer,
                                               size_t* size) {
	PRINT_NAME();

	LOGF("\t context  = %d\n"
	     "\t handle   = %d\n"
	     "\t group_id = %d\n"
	     "\t buffer   = 0x%016" PRIx64 "\n"
	     "\t size     = 0x%016" PRIx64 "\n",
	     context, handle, group_id, reinterpret_cast<uint64_t>(buffer),
	     reinterpret_cast<uint64_t>(size));

	if (size != nullptr) {
		*size = 0;
	}

	return NP_TROPHY2_ERROR_ICON_FILE_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpTrophy2GetTrophyIcon(int context, int handle, int trophy_id,
                                                void* buffer, size_t* size) {
	PRINT_NAME();

	LOGF("\t context   = %d\n"
	     "\t handle    = %d\n"
	     "\t trophy_id = %d\n"
	     "\t buffer    = 0x%016" PRIx64 "\n"
	     "\t size      = 0x%016" PRIx64 "\n",
	     context, handle, trophy_id, reinterpret_cast<uint64_t>(buffer),
	     reinterpret_cast<uint64_t>(size));

	if (size != nullptr) {
		*size = 0;
	}

	return NP_TROPHY2_ERROR_ICON_FILE_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpTrophy2RegisterUnlockCallback(void* callback, void* userdata) {
	PRINT_NAME();

	LOGF("\t callback = 0x%016" PRIx64 "\n"
	     "\t userdata = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(callback), reinterpret_cast<uint64_t>(userdata));

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2AbortHandle(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2DestroyHandle(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return 0;
}

static int KYTY_SYSV_ABI NpTrophy2DestroyContext(int context) {
	PRINT_NAME();

	LOGF("\t context = %d\n", context);

	return 0;
}

LIB_DEFINE(InitNet_1_NpTrophy2) {
	LIB_FUNC("Bagshr7OQ6Q", LibNpTrophy2::NpTrophy2CreateContext);
	LIB_FUNC("Gz1rmUZpROM", LibNpTrophy2::NpTrophy2CreateHandle);
	LIB_FUNC("bIDov3wBu5Q", LibNpTrophy2::NpTrophy2RegisterContext);
	LIB_FUNC("4IzqhhUQ3nk", LibNpTrophy2::NpTrophy2GetGameInfo);
	LIB_FUNC("DoZWauG8mu0", LibNpTrophy2::NpTrophy2GetGroupInfo);
	LIB_FUNC("+PDSI6WgPRc", LibNpTrophy2::NpTrophy2GetGroupInfoArray);
	LIB_FUNC("EwNylPdWUTM", LibNpTrophy2::NpTrophy2GetTrophyInfo);
	LIB_FUNC("y3zHpdZO6ME", LibNpTrophy2::NpTrophy2GetTrophyInfoArray);
	LIB_FUNC("2QgUy+xJqS0", LibNpTrophy2::NpTrophy2GetGameIcon);
	LIB_FUNC("6IjXJUy6ZnA", LibNpTrophy2::NpTrophy2GetGroupIcon);
	LIB_FUNC("-9LLVU0uvs8", LibNpTrophy2::NpTrophy2GetTrophyIcon);
	LIB_FUNC("sUXGfNMalIo", LibNpTrophy2::NpTrophy2RegisterUnlockCallback);
	LIB_FUNC("fYapWA9xVmA", LibNpTrophy2::NpTrophy2AbortHandle);
	LIB_FUNC("d8P11CI40KE", LibNpTrophy2::NpTrophy2DestroyHandle);
	LIB_FUNC("sysY2FHYff4", LibNpTrophy2::NpTrophy2DestroyContext);
}

} // namespace LibNpTrophy2

namespace LibNpUniversalDataSystem {

LIB_VERSION("NpUniversalDataSystem", 1, "NpUniversalDataSystem", 1, 1);

constexpr int NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT = -2141900542; /* 0x80553102 */

struct NpUniversalDataSystemInitParam {
	size_t size;
	size_t pool_size;
};

struct NpUniversalDataSystemMemoryStat {
	size_t pool_size;
	size_t max_inuse_size;
	size_t current_inuse_size;
};

struct NpUniversalDataSystemEvent {};

struct NpUniversalDataSystemEventPropertyObject {};

struct NpUniversalDataSystemEventPropertyArray {};

struct NpUniversalDataSystemStorageStat {
	size_t in_events;
	size_t out_events;
	size_t lost_events;
	size_t max_inuse_size;
	size_t current_events;
	size_t current_inuse_size;
	size_t current_free_size;
};

static int KYTY_SYSV_ABI
NpUniversalDataSystemInitialize(const NpUniversalDataSystemInitParam* param) {
	PRINT_NAME();

	if (param == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t size      = %" PRIu64 "\n"
	     "\t pool_size = %" PRIu64 "\n",
	     static_cast<uint64_t>(param->size), static_cast<uint64_t>(param->pool_size));

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemCreateContext(int* context, int user_id,
                                                            uint32_t service_label,
                                                            uint64_t options) {
	PRINT_NAME();

	if (context != nullptr) {
		*context = 1;
	}

	LOGF("\t user_id       = %d\n"
	     "\t service_label = %u\n"
	     "\t options       = 0x%016" PRIx64 "\n",
	     user_id, service_label, options);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemCreateHandle(int* handle) {
	PRINT_NAME();

	if (handle == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	if (handle != nullptr) {
		*handle = 1;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemDestroyHandle(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemAbortHandle(int handle) {
	PRINT_NAME();

	LOGF("\t handle = %d\n", handle);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemCreateEvent(
    const char* event_name, const NpUniversalDataSystemEventPropertyObject* prop,
    NpUniversalDataSystemEvent** new_event, NpUniversalDataSystemEventPropertyObject** prop_ptr) {
	PRINT_NAME();

	if (event_name == nullptr || new_event == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*new_event = new NpUniversalDataSystemEvent;
	if (prop_ptr != nullptr) {
		*prop_ptr = (prop != nullptr ? const_cast<NpUniversalDataSystemEventPropertyObject*>(prop)
		                             : new NpUniversalDataSystemEventPropertyObject);
	}

	LOGF("\t event_name = %s\n"
	     "\t prop       = 0x%016" PRIx64 "\n"
	     "\t new_event  = 0x%016" PRIx64 "\n"
	     "\t prop_ptr   = 0x%016" PRIx64 "\n",
	     event_name != nullptr ? event_name : "<null>", reinterpret_cast<uint64_t>(prop),
	     reinterpret_cast<uint64_t>(*new_event),
	     prop_ptr != nullptr ? reinterpret_cast<uint64_t>(*prop_ptr) : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemPostEvent(int context, int handle, const void* event,
                                                        uint64_t options) {
	PRINT_NAME();

	LOGF("\t context = %d\n"
	     "\t handle  = %d\n"
	     "\t event   = 0x%016" PRIx64 "\n"
	     "\t options = 0x%016" PRIx64 "\n",
	     context, handle, reinterpret_cast<uint64_t>(event), options);

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemEventEstimateSize(const NpUniversalDataSystemEvent* event, size_t* size) {
	PRINT_NAME();

	LOGF("\t event = 0x%016" PRIx64 "\n"
	     "\t size  = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(event), reinterpret_cast<uint64_t>(size));

	if (event == nullptr || size == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*size = 3;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventToString(const NpUniversalDataSystemEvent* event,
                                                            char* buf, size_t buf_size,
                                                            size_t* string_size) {
	PRINT_NAME();

	LOGF("\t event       = 0x%016" PRIx64 "\n"
	     "\t buf         = 0x%016" PRIx64 "\n"
	     "\t buf_size    = %" PRIu64 "\n"
	     "\t string_size = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(event), reinterpret_cast<uint64_t>(buf),
	     static_cast<uint64_t>(buf_size), reinterpret_cast<uint64_t>(string_size));

	if (event == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	const char* json = "{}";
	if (string_size != nullptr) {
		*string_size = std::strlen(json) + 1;
	}
	if (buf != nullptr && buf_size > 0) {
		std::snprintf(buf, buf_size, "%s", json);
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemDestroyEvent(NpUniversalDataSystemEvent* event) {
	PRINT_NAME();

	LOGF("\t event = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(event));

	delete event;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemRegisterContext(int context, int handle,
                                                              uint64_t options) {
	PRINT_NAME();

	LOGF("\t context = %d\n"
	     "\t handle  = %d\n"
	     "\t options = 0x%016" PRIx64 "\n",
	     context, handle, options);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemDestroyContext(int context) {
	PRINT_NAME();

	LOGF("\t context = %d\n", context);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemGetMemoryStat(NpUniversalDataSystemMemoryStat* stat) {
	PRINT_NAME();

	if (stat == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*stat = {};

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemCreateEventPropertyObject(
    NpUniversalDataSystemEventPropertyObject** new_object) {
	PRINT_NAME();

	if (new_object == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*new_object = new NpUniversalDataSystemEventPropertyObject;

	LOGF("\t new_object = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(*new_object));

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemDestroyEventPropertyObject(NpUniversalDataSystemEventPropertyObject* object) {
	PRINT_NAME();

	LOGF("\t object = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(object));

	delete object;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetString(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, const char* value) {
	PRINT_NAME();

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %s\n",
	     reinterpret_cast<uint64_t>(object), key != nullptr ? key : "<null>",
	     value != nullptr ? value : "<null>");

	if (object == nullptr || key == nullptr || value == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetInt32(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, int32_t value) {
	PRINT_NAME();

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %" PRId32 "\n",
	     reinterpret_cast<uint64_t>(object), key != nullptr ? key : "<null>", value);

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetUInt32(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, uint32_t value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(object), key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetInt64(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, int64_t value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %" PRId64 "\n",
	     reinterpret_cast<uint64_t>(object), key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetUInt64(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, uint64_t value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(object), key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetFloat32(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, float value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %f\n",
	     reinterpret_cast<uint64_t>(object), key, static_cast<double>(value));

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetFloat64(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, double value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %f\n",
	     reinterpret_cast<uint64_t>(object), key, value);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetBool(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, bool value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t object = 0x%016" PRIx64 "\n"
	     "\t key    = %s\n"
	     "\t value  = %d\n",
	     reinterpret_cast<uint64_t>(object), key, value ? 1 : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetBinary(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, const void* value,
    size_t value_size) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr || (value == nullptr && value_size != 0)) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	LOGF("\t object     = 0x%016" PRIx64 "\n"
	     "\t key        = %s\n"
	     "\t value      = 0x%016" PRIx64 "\n"
	     "\t value_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(object), key, reinterpret_cast<uint64_t>(value),
	     static_cast<uint64_t>(value_size));

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetObject(
    NpUniversalDataSystemEventPropertyObject* object, const char* key,
    const NpUniversalDataSystemEventPropertyObject* value,
    NpUniversalDataSystemEventPropertyObject**      value_ptr) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	if (value_ptr != nullptr) {
		*value_ptr =
		    (value != nullptr ? const_cast<NpUniversalDataSystemEventPropertyObject*>(value)
		                      : new NpUniversalDataSystemEventPropertyObject);
	}

	LOGF("\t object    = 0x%016" PRIx64 "\n"
	     "\t key       = %s\n"
	     "\t value     = 0x%016" PRIx64 "\n"
	     "\t value_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(object), key, reinterpret_cast<uint64_t>(value),
	     value_ptr != nullptr ? reinterpret_cast<uint64_t>(*value_ptr) : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetArray(
    NpUniversalDataSystemEventPropertyObject* object, const char* key,
    const NpUniversalDataSystemEventPropertyArray* value,
    NpUniversalDataSystemEventPropertyArray**      value_ptr) {
	PRINT_NAME();

	LOGF("\t object    = 0x%016" PRIx64 "\n"
	     "\t key       = %s\n"
	     "\t value     = 0x%016" PRIx64 "\n"
	     "\t value_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(object), key != nullptr ? key : "<null>",
	     reinterpret_cast<uint64_t>(value), reinterpret_cast<uint64_t>(value_ptr));

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	if (value_ptr != nullptr) {
		*value_ptr = (value != nullptr ? const_cast<NpUniversalDataSystemEventPropertyArray*>(value)
		                               : new NpUniversalDataSystemEventPropertyArray);
	}

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemCreateEventPropertyArray(NpUniversalDataSystemEventPropertyArray** new_array) {
	PRINT_NAME();

	if (new_array == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*new_array = new NpUniversalDataSystemEventPropertyArray;

	LOGF("\t new_array = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(*new_array));

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemDestroyEventPropertyArray(NpUniversalDataSystemEventPropertyArray* array) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(array));

	delete array;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetString(
    NpUniversalDataSystemEventPropertyArray* array, const char* value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %s\n",
	     reinterpret_cast<uint64_t>(array), value != nullptr ? value : "<null>");

	if (array == nullptr || value == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetInt32(
    NpUniversalDataSystemEventPropertyArray* array, int32_t value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %" PRId32 "\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetUInt32(
    NpUniversalDataSystemEventPropertyArray* array, uint32_t value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetInt64(
    NpUniversalDataSystemEventPropertyArray* array, int64_t value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %" PRId64 "\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetUInt64(
    NpUniversalDataSystemEventPropertyArray* array, uint64_t value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetFloat32(
    NpUniversalDataSystemEventPropertyArray* array, float value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %f\n",
	     reinterpret_cast<uint64_t>(array), static_cast<double>(value));

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetFloat64(
    NpUniversalDataSystemEventPropertyArray* array, double value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %f\n",
	     reinterpret_cast<uint64_t>(array), value);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetBool(
    NpUniversalDataSystemEventPropertyArray* array, bool value) {
	PRINT_NAME();

	LOGF("\t array = 0x%016" PRIx64 "\n"
	     "\t value = %d\n",
	     reinterpret_cast<uint64_t>(array), value ? 1 : 0);

	return (array != nullptr ? 0 : NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT);
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetBinary(
    NpUniversalDataSystemEventPropertyArray* array, const void* value, size_t value_size) {
	PRINT_NAME();

	LOGF("\t array      = 0x%016" PRIx64 "\n"
	     "\t value      = 0x%016" PRIx64 "\n"
	     "\t value_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(array), reinterpret_cast<uint64_t>(value),
	     static_cast<uint64_t>(value_size));

	if (array == nullptr || (value == nullptr && value_size != 0)) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetObject(
    NpUniversalDataSystemEventPropertyArray*        array,
    const NpUniversalDataSystemEventPropertyObject* value,
    NpUniversalDataSystemEventPropertyObject**      value_ptr) {
	PRINT_NAME();

	if (array == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	if (value_ptr != nullptr) {
		*value_ptr =
		    (value != nullptr ? const_cast<NpUniversalDataSystemEventPropertyObject*>(value)
		                      : new NpUniversalDataSystemEventPropertyObject);
	}

	LOGF("\t array     = 0x%016" PRIx64 "\n"
	     "\t value     = 0x%016" PRIx64 "\n"
	     "\t value_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(array), reinterpret_cast<uint64_t>(value),
	     value_ptr != nullptr ? reinterpret_cast<uint64_t>(*value_ptr) : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyArraySetArray(
    NpUniversalDataSystemEventPropertyArray*       array,
    const NpUniversalDataSystemEventPropertyArray* value,
    NpUniversalDataSystemEventPropertyArray**      value_ptr) {
	PRINT_NAME();

	if (array == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	if (value_ptr != nullptr) {
		*value_ptr = (value != nullptr ? const_cast<NpUniversalDataSystemEventPropertyArray*>(value)
		                               : new NpUniversalDataSystemEventPropertyArray);
	}

	LOGF("\t array     = 0x%016" PRIx64 "\n"
	     "\t value     = 0x%016" PRIx64 "\n"
	     "\t value_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(array), reinterpret_cast<uint64_t>(value),
	     value_ptr != nullptr ? reinterpret_cast<uint64_t>(*value_ptr) : 0);

	return 0;
}

static int KYTY_SYSV_ABI
NpUniversalDataSystemGetStorageStat(int context, NpUniversalDataSystemStorageStat* stat) {
	PRINT_NAME();

	LOGF("\t context = %d\n"
	     "\t stat    = 0x%016" PRIx64 "\n",
	     context, reinterpret_cast<uint64_t>(stat));

	if (stat == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}

	*stat = {};

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemTerminate() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitNet_1_NpUniversalDataSystem) {
	LIB_FUNC("sjaobBgqeB4", LibNpUniversalDataSystem::NpUniversalDataSystemInitialize);
	LIB_FUNC("5zBnau1uIEo", LibNpUniversalDataSystem::NpUniversalDataSystemCreateContext);
	LIB_FUNC("hT0IAEvN+M0", LibNpUniversalDataSystem::NpUniversalDataSystemCreateHandle);
	LIB_FUNC("p+GcLqwpL9M", LibNpUniversalDataSystem::NpUniversalDataSystemCreateEvent);
	LIB_FUNC("CzkKf7ahIyU", LibNpUniversalDataSystem::NpUniversalDataSystemPostEvent);
	LIB_FUNC("AUIHb7jUX3I", LibNpUniversalDataSystem::NpUniversalDataSystemDestroyHandle);
	LIB_FUNC("jZCqWFgMehE", LibNpUniversalDataSystem::NpUniversalDataSystemAbortHandle);
	LIB_FUNC("wB7IWzGp2v0", LibNpUniversalDataSystem::NpUniversalDataSystemDestroyContext);
	LIB_FUNC("su7jW3VDDb4", LibNpUniversalDataSystem::NpUniversalDataSystemGetMemoryStat);
	LIB_FUNC("+s14jq-KGYw", LibNpUniversalDataSystem::NpUniversalDataSystemEventEstimateSize);
	LIB_FUNC("vj6CQGWtEBg", LibNpUniversalDataSystem::NpUniversalDataSystemEventToString);
	LIB_FUNC("wG+84pnNIuo", LibNpUniversalDataSystem::NpUniversalDataSystemDestroyEvent);
	LIB_FUNC("tpFJ8LIKvPw", LibNpUniversalDataSystem::NpUniversalDataSystemRegisterContext);
	LIB_FUNC("s6W4Zl4Slgk",
	         LibNpUniversalDataSystem::NpUniversalDataSystemCreateEventPropertyObject);
	LIB_FUNC("kKUH0Viib3c",
	         LibNpUniversalDataSystem::NpUniversalDataSystemDestroyEventPropertyObject);
	LIB_FUNC("MfDb+4Nln64",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetString);
	LIB_FUNC("YE4dbtbz6OE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetInt32);
	LIB_FUNC("AzD4irAcKE4",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetUInt32);
	LIB_FUNC("56QLTqx911s",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetInt64);
	LIB_FUNC("xvsP5Yz6FmY",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetUInt64);
	LIB_FUNC("lbPlT4+QVcE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetFloat32);
	LIB_FUNC("4Fu8tHW+u-k",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetFloat64);
	LIB_FUNC("Fidd8vWgyVE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetBool);
	LIB_FUNC("wAcxBDLHj1M",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetBinary);
	LIB_FUNC("74ASEqxSnkM",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetObject);
	LIB_FUNC("Wxbg5x3pTXA",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyObjectSetArray);
	LIB_FUNC("Hm7qubT3b70",
	         LibNpUniversalDataSystem::NpUniversalDataSystemCreateEventPropertyArray);
	LIB_FUNC("W-0xwY0ZMjw",
	         LibNpUniversalDataSystem::NpUniversalDataSystemDestroyEventPropertyArray);
	LIB_FUNC("4llLk7YJRTE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetString);
	LIB_FUNC("BypQuF113-k",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetInt32);
	LIB_FUNC("yMi0xAOpmXM",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetUInt32);
	LIB_FUNC("viVXAwmmYrY",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetInt64);
	LIB_FUNC("Qo9qR7v5zO4",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetUInt64);
	LIB_FUNC("JmgwKm96Lq4",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetFloat32);
	LIB_FUNC("sbSYZLR5AiE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetFloat64);
	LIB_FUNC("0+l4QSWCM4E",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetBool);
	LIB_FUNC("IEdUCV9j2Cw",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetBinary);
	LIB_FUNC("XY14n3jNIpE",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetObject);
	LIB_FUNC("rdi9BAfDLq8",
	         LibNpUniversalDataSystem::NpUniversalDataSystemEventPropertyArraySetArray);
	LIB_FUNC("KmN62tT4U8A", LibNpUniversalDataSystem::NpUniversalDataSystemGetStorageStat);
	LIB_FUNC("47UAEuQl+iI", LibNpUniversalDataSystem::NpUniversalDataSystemTerminate);
}

} // namespace LibNpUniversalDataSystem

namespace LibCes {

LIB_VERSION("Ces", 1, "Ces", 1, 1);

static const uint8_t* KYTY_SYSV_ABI CesRefersUcsProfileCp1252() {
	PRINT_NAME();

	static const uint8_t profile = 0;
	return &profile;
}

static uint32_t CesDecodeUtf8(const uint8_t* utf8, uint32_t utf8max, uint32_t* utf8_len) {
	if (utf8 == nullptr || utf8max == 0) {
		if (utf8_len != nullptr) {
			*utf8_len = 0;
		}
		return 0xfffd;
	}

	const uint8_t c0 = utf8[0];
	if (c0 < 0x80) {
		if (utf8_len != nullptr) {
			*utf8_len = 1;
		}
		return c0;
	}
	if ((c0 & 0xe0) == 0xc0 && utf8max >= 2 && (utf8[1] & 0xc0) == 0x80) {
		if (utf8_len != nullptr) {
			*utf8_len = 2;
		}
		return ((c0 & 0x1f) << 6u) | (utf8[1] & 0x3fu);
	}
	if ((c0 & 0xf0) == 0xe0 && utf8max >= 3 && (utf8[1] & 0xc0) == 0x80 &&
	    (utf8[2] & 0xc0) == 0x80) {
		if (utf8_len != nullptr) {
			*utf8_len = 3;
		}
		return ((c0 & 0x0f) << 12u) | ((utf8[1] & 0x3fu) << 6u) | (utf8[2] & 0x3fu);
	}
	if ((c0 & 0xf8) == 0xf0 && utf8max >= 4 && (utf8[1] & 0xc0) == 0x80 &&
	    (utf8[2] & 0xc0) == 0x80 && (utf8[3] & 0xc0) == 0x80) {
		if (utf8_len != nullptr) {
			*utf8_len = 4;
		}
		return ((c0 & 0x07) << 18u) | ((utf8[1] & 0x3fu) << 12u) | ((utf8[2] & 0x3fu) << 6u) |
		       (utf8[3] & 0x3fu);
	}

	if (utf8_len != nullptr) {
		*utf8_len = 1;
	}
	return 0xfffd;
}

struct Cp1252Mapping {
	uint8_t  cp1252;
	uint32_t unicode;
};

static constexpr Cp1252Mapping CP1252_EXTENDED_MAP[] = {
    {0x80, 0x20ac}, {0x82, 0x201a}, {0x83, 0x0192}, {0x84, 0x201e}, {0x85, 0x2026}, {0x86, 0x2020},
    {0x87, 0x2021}, {0x88, 0x02c6}, {0x89, 0x2030}, {0x8a, 0x0160}, {0x8b, 0x2039}, {0x8c, 0x0152},
    {0x8e, 0x017d}, {0x91, 0x2018}, {0x92, 0x2019}, {0x93, 0x201c}, {0x94, 0x201d}, {0x95, 0x2022},
    {0x96, 0x2013}, {0x97, 0x2014}, {0x98, 0x02dc}, {0x99, 0x2122}, {0x9a, 0x0161}, {0x9b, 0x203a},
    {0x9c, 0x0153}, {0x9e, 0x017e}, {0x9f, 0x0178},
};

static uint8_t CesUnicodeToCp1252(uint32_t code) {
	if (code <= 0x7f || (code >= 0xa0 && code <= 0xff)) {
		return static_cast<uint8_t>(code);
	}

	for (const auto& map: CP1252_EXTENDED_MAP) {
		if (map.unicode == code) {
			return map.cp1252;
		}
	}
	return '?';
}

static uint32_t CesCp1252ToUnicode(uint8_t sbc) {
	if (sbc <= 0x7f || sbc >= 0xa0) {
		return sbc;
	}

	for (const auto& map: CP1252_EXTENDED_MAP) {
		if (map.cp1252 == sbc) {
			return map.unicode;
		}
	}
	return sbc;
}

static uint32_t CesEncodeUtf8(uint32_t code, uint8_t* utf8, uint32_t utf8max) {
	uint8_t  tmp[4] {};
	uint32_t len = 0;

	if (code <= 0x7f) {
		tmp[0] = static_cast<uint8_t>(code);
		len    = 1;
	} else if (code <= 0x7ff) {
		tmp[0] = static_cast<uint8_t>(0xc0u | ((code >> 6u) & 0x1fu));
		tmp[1] = static_cast<uint8_t>(0x80u | (code & 0x3fu));
		len    = 2;
	} else if (code <= 0xffff) {
		tmp[0] = static_cast<uint8_t>(0xe0u | ((code >> 12u) & 0x0fu));
		tmp[1] = static_cast<uint8_t>(0x80u | ((code >> 6u) & 0x3fu));
		tmp[2] = static_cast<uint8_t>(0x80u | (code & 0x3fu));
		len    = 3;
	} else {
		tmp[0] = static_cast<uint8_t>(0xf0u | ((code >> 18u) & 0x07u));
		tmp[1] = static_cast<uint8_t>(0x80u | ((code >> 12u) & 0x3fu));
		tmp[2] = static_cast<uint8_t>(0x80u | ((code >> 6u) & 0x3fu));
		tmp[3] = static_cast<uint8_t>(0x80u | (code & 0x3fu));
		len    = 4;
	}

	if (utf8 != nullptr && utf8max >= len) {
		std::memcpy(utf8, tmp, len);
	}

	return len;
}

static int KYTY_SYSV_ABI CesUtf8ToSbc(const uint8_t* utf8, uint32_t utf8max, uint32_t* utf8_len,
                                      const uint8_t* profile, uint8_t* sbc) {
	PRINT_NAME();

	if (sbc == nullptr || profile == nullptr) {
		return -1;
	}

	uint32_t   local_len = 0;
	const auto code      = CesDecodeUtf8(utf8, utf8max, &local_len);
	if (utf8_len != nullptr) {
		*utf8_len = local_len;
	}
	*sbc = CesUnicodeToCp1252(code);

	return 0;
}

static int KYTY_SYSV_ABI CesSbcToUtf8(const uint8_t* profile, uint8_t sbc, uint8_t* utf8,
                                      uint32_t utf8max, uint32_t* utf8_len) {
	PRINT_NAME();

	if (profile == nullptr || utf8 == nullptr) {
		return -1;
	}

	const auto code = CesCp1252ToUnicode(sbc);
	const auto len  = CesEncodeUtf8(code, utf8, utf8max);
	if (utf8_len != nullptr) {
		*utf8_len = len;
	}

	return utf8max >= len ? 0 : -1;
}

static int KYTY_SYSV_ABI CesStub(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
	PRINT_NAME();

	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;

	return 0;
}

LIB_DEFINE(InitNet_1_Ces) {
	LIB_FUNC("ZiDCxUUGbec", LibCes::CesStub);
	LIB_FUNC("538bRGc6Zo8", LibCes::CesStub);
	LIB_FUNC("LPzYZ+FR0BI", LibCes::CesRefersUcsProfileCp1252);
	LIB_FUNC("3Q1gOWWarcw", LibCes::CesUtf8ToSbc);
	LIB_FUNC("xTd54EEL1Ao", LibCes::CesSbcToUtf8);
}

} // namespace LibCes

namespace LibNpGameIntent {

LIB_VERSION("NpGameIntent", 1, "NpGameIntent", 1, 1);

constexpr int    NP_GAME_INTENT_ERROR_INVALID_ARGUMENT = -2141898748; /* 0x80553804 */
constexpr int    NP_GAME_INTENT_ERROR_INTENT_NOT_FOUND = -2141898746; /* 0x80553806 */
constexpr int    NP_GAME_INTENT_ERROR_VALUE_NOT_FOUND  = -2141898745; /* 0x80553807 */
constexpr int    NP_GAME_INTENT_USER_ID_INVALID        = -1;
constexpr size_t NP_GAME_INTENT_TYPE_MAX_SIZE          = 33;
constexpr size_t NP_GAME_INTENT_DATA_MAX_SIZE          = 16 * 1024 + 1;

struct NpGameIntentData {
	uint8_t data[NP_GAME_INTENT_DATA_MAX_SIZE];
	uint8_t padding[7];
};

struct NpGameIntentInfo {
	size_t           size;
	int32_t          user_id;
	char             intent_type[NP_GAME_INTENT_TYPE_MAX_SIZE];
	uint8_t          padding[7];
	uint8_t          reserved[256];
	NpGameIntentData intent_data;
};

static_assert(sizeof(NpGameIntentData) == 16392);
static_assert(sizeof(NpGameIntentInfo) == 16704);

static int KYTY_SYSV_ABI NpGameIntentInitialize(const void* init_param) {
	PRINT_NAME();

	LOGF("\t init_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));

	return 0;
}

static int KYTY_SYSV_ABI NpGameIntentTerminate() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI NpGameIntentReceiveIntent(NpGameIntentInfo* intent_info) {
	PRINT_NAME();

	LOGF("\t intent_info = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(intent_info));

	if (intent_info == nullptr) {
		return NP_GAME_INTENT_ERROR_INVALID_ARGUMENT;
	}

	intent_info->user_id = NP_GAME_INTENT_USER_ID_INVALID;
	std::memset(intent_info->intent_type, 0, sizeof(intent_info->intent_type));
	std::memset(&intent_info->intent_data, 0, sizeof(intent_info->intent_data));

	return NP_GAME_INTENT_ERROR_INTENT_NOT_FOUND;
}

static int KYTY_SYSV_ABI NpGameIntentGetPropertyValueString(const NpGameIntentData* intent_data,
                                                            const char* key, char* value_buf,
                                                            size_t buf_size) {
	PRINT_NAME();

	LOGF("\t intent_data = 0x%016" PRIx64 "\n"
	     "\t key         = %s\n"
	     "\t value_buf   = 0x%016" PRIx64 "\n"
	     "\t buf_size    = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(intent_data), key != nullptr ? key : "(null)",
	     reinterpret_cast<uint64_t>(value_buf), buf_size);

	if (intent_data == nullptr || key == nullptr || value_buf == nullptr || buf_size == 0) {
		return NP_GAME_INTENT_ERROR_INVALID_ARGUMENT;
	}

	value_buf[0] = '\0';
	return NP_GAME_INTENT_ERROR_VALUE_NOT_FOUND;
}

LIB_DEFINE(InitNet_1_NpGameIntent) {
	LIB_FUNC("m87BHxt-H60", LibNpGameIntent::NpGameIntentInitialize);
	LIB_FUNC("0HBYxYAjmf0", LibNpGameIntent::NpGameIntentTerminate);
	LIB_FUNC("jEIXUAr9XE8", LibNpGameIntent::NpGameIntentReceiveIntent);
	LIB_FUNC("rPl0INNc-M8", LibNpGameIntent::NpGameIntentGetPropertyValueString);
}

} // namespace LibNpGameIntent

namespace LibNpWebApi2 {

LIB_VERSION("NpWebApi2", 1, "NpWebApi2", 1, 1);

constexpr int NP_WEBAPI2_ERROR_INVALID_ARGUMENT  = -2141899774; /* 0x80553402 */
constexpr int NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND = -2141899770; /* 0x80553406 */
constexpr int NP_WEBAPI2_ERROR_NOT_SIGNED_IN     = -2141899769; /* 0x80553407 */

struct NpWebApi2ResponseInformationOption {
	int32_t http_status;
	char*   error_object;
	size_t  error_object_size;
	size_t  response_data_size;
};

struct NpWebApi2Request {
	std::string api_group;
	std::string path;
	std::string method;
	std::string response;
	size_t      read_offset = 0;
};

static std::map<int64_t, NpWebApi2Request>& NpWebApi2Requests() {
	static std::map<int64_t, NpWebApi2Request> requests;
	return requests;
}

[[maybe_unused]] static std::string NpWebApi2MakeResponse(const NpWebApi2Request& request) {
	if (request.api_group.find("sessionManager") != std::string::npos ||
	    request.path.find("sessions") != std::string::npos ||
	    request.path.find("Sessions") != std::string::npos) {
		return R"({"gameSessions":[],"playerSessions":[]})";
	}

	return "{}";
}

static int KYTY_SYSV_ABI NpWebApi2Initialize(int lib_http_ctx_id, size_t pool_size) {
	PRINT_NAME();

	LOGF("\t lib_http_ctx_id = %d\n"
	     "\t pool_size       = %" PRIu64 "\n",
	     lib_http_ctx_id, pool_size);

	static int id = 0;

	return ++id;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventCreateHandle(int lib_ctx_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n", lib_ctx_id);

	static int handle = 0;

	return ++handle;
}

static int KYTY_SYSV_ABI NpWebApi2CreateUserContext(int lib_ctx_id, int user_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n"
	     "\t user_id    = %d\n",
	     lib_ctx_id, user_id);

	static int user_context = 1000;

	return ++user_context;
}

static int KYTY_SYSV_ABI NpWebApi2DeleteUserContext(int user_context_id) {
	PRINT_NAME();

	LOGF("\t user_context_id = %d\n", user_context_id);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2CreateRequest(int user_context_id, const char* api_group,
                                                const char* path, const char* method,
                                                const void* content_parameter,
                                                int64_t*    request_id) {
	PRINT_NAME();

	LOGF("\t user_context_id   = %d\n", user_context_id);
	LOGF("\t api_group         = %s\n", api_group != nullptr ? api_group : "(null)");
	LOGF("\t path              = %s\n", path != nullptr ? path : "(null)");
	LOGF("\t method            = %s\n", method != nullptr ? method : "(null)");
	LOGF("\t content_parameter = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(content_parameter));
	LOGF("\t request_id        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(request_id));

	static int64_t next_request_id = 1000;

	if (request_id != nullptr) {
		*request_id = next_request_id++;
		NpWebApi2Request request {};
		request.api_group                = (api_group != nullptr ? api_group : "");
		request.path                     = (path != nullptr ? path : "");
		request.method                   = (method != nullptr ? method : "");
		NpWebApi2Requests()[*request_id] = request;
	}

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2AbortRequest(int64_t request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);

	return (NpWebApi2Requests().find(request_id) == NpWebApi2Requests().end()
	            ? NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND
	            : 0);
}

static int KYTY_SYSV_ABI
NpWebApi2SendRequest(int64_t request_id, const void* data, size_t data_size,
                     NpWebApi2ResponseInformationOption* response_info_option) {
	PRINT_NAME();

	LOGF("\t request_id           = %" PRIi64 "\n", request_id);
	LOGF("\t data                 = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	LOGF("\t data_size            = %" PRIu64 "\n", data_size);
	LOGF("\t response_info_option = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(response_info_option));

	auto request = NpWebApi2Requests().find(request_id);
	if (request == NpWebApi2Requests().end()) {
		return NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND;
	}

	// request->second.response    = NpWebApi2MakeResponse(request->second);
	request->second.response.clear();
	request->second.read_offset = 0;

	if (response_info_option != nullptr) {
		// response_info_option->http_status        = 200;
		response_info_option->http_status        = 0;
		response_info_option->response_data_size = request->second.response.size();
		if (response_info_option->error_object != nullptr &&
		    response_info_option->error_object_size > 0) {
			response_info_option->error_object[0] = '\0';
		}
	}

	// return 0;
	return NP_WEBAPI2_ERROR_NOT_SIGNED_IN;
}

static int KYTY_SYSV_ABI NpWebApi2ReadData(int64_t request_id, void* data, size_t size) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);
	LOGF("\t data       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(data));
	LOGF("\t size       = %" PRIu64 "\n", size);

	if (data == nullptr || size == 0) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}

	auto request = NpWebApi2Requests().find(request_id);
	if (request == NpWebApi2Requests().end()) {
		return NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND;
	}

	const auto remaining = request->second.response.size() - request->second.read_offset;
	const auto to_copy   = (remaining < size ? remaining : size);
	if (to_copy > 0) {
		std::memcpy(data, request->second.response.data() + request->second.read_offset, to_copy);
		request->second.read_offset += to_copy;
	}

	return static_cast<int>(to_copy);
}

static int KYTY_SYSV_ABI NpWebApi2DeleteRequest(int64_t request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);

	NpWebApi2Requests().erase(request_id);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2AddHttpRequestHeader(int64_t request_id, const char* field_name,
                                                       const char* value) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);
	LOGF("\t field_name = %s\n", field_name != nullptr ? field_name : "(null)");
	LOGF("\t value      = %s\n", value != nullptr ? value : "(null)");

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2GetHttpResponseHeaderValueLength(int64_t     request_id,
                                                                   const char* field_name,
                                                                   size_t*     value_length) {
	PRINT_NAME();

	LOGF("\t request_id   = %" PRIi64 "\n", request_id);
	LOGF("\t field_name   = %s\n", field_name != nullptr ? field_name : "(null)");
	LOGF("\t value_length = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(value_length));

	if (field_name == nullptr || value_length == nullptr) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}
	if (NpWebApi2Requests().find(request_id) == NpWebApi2Requests().end()) {
		return NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND;
	}

	*value_length = (std::strcmp(field_name, "Content-Type") == 0 ||
	                         std::strcmp(field_name, "content-type") == 0
	                     ? std::strlen("application/json; charset=utf-8") + 1
	                     : 0);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2GetHttpResponseHeaderValue(int64_t     request_id,
                                                             const char* field_name, char* value,
                                                             size_t value_size) {
	PRINT_NAME();

	LOGF("\t request_id = %" PRIi64 "\n", request_id);
	LOGF("\t field_name = %s\n", field_name != nullptr ? field_name : "(null)");
	LOGF("\t value      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(value));
	LOGF("\t value_size = %" PRIu64 "\n", value_size);

	if (field_name == nullptr || value == nullptr || value_size == 0) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}
	if (NpWebApi2Requests().find(request_id) == NpWebApi2Requests().end()) {
		return NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND;
	}

	const char* header = "";
	if (std::strcmp(field_name, "Content-Type") == 0 ||
	    std::strcmp(field_name, "content-type") == 0) {
		header = "application/json; charset=utf-8";
	}

	const auto copy_size =
	    std::strlen(header) < value_size - 1 ? std::strlen(header) : value_size - 1;
	std::memcpy(value, header, copy_size);
	value[copy_size] = '\0';

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventDeleteHandle(int lib_ctx_id, int handle_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n", lib_ctx_id);
	LOGF("\t handle_id  = %d\n", handle_id);

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventDeletePushContext(int         user_context_id,
                                                             const void* push_context_id) {
	PRINT_NAME();

	LOGF("\t user_context_id = %d\n", user_context_id);
	LOGF("\t push_context_id = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(push_context_id));

	return 0;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventCreateFilter(int lib_ctx_id, int handle_id,
                                                        const char* np_service_name,
                                                        uint32_t    np_service_label,
                                                        const void* filter_param,
                                                        size_t      filter_param_num) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id       = %d\n", lib_ctx_id);
	LOGF("\t handle_id        = %d\n", handle_id);
	LOGF("\t np_service_name  = %s\n", np_service_name != nullptr ? np_service_name : "(null)");
	LOGF("\t np_service_label = %" PRIu32 "\n", np_service_label);
	LOGF("\t filter_param     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(filter_param));
	LOGF("\t filter_param_num = %" PRIu64 "\n", filter_param_num);

	static int filter_id = 1;

	return filter_id++;
}

static int KYTY_SYSV_ABI NpWebApi2PushEventRegisterCallback(int user_context_id, int filter_id,
                                                            void* callback, void* user_arg) {
	PRINT_NAME();

	LOGF("\t user_context_id = %d\n", user_context_id);
	LOGF("\t filter_id       = %d\n", filter_id);
	LOGF("\t callback        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(callback));
	LOGF("\t user_arg        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user_arg));

	static int callback_id = 1;

	return callback_id++;
}

static void KYTY_SYSV_ABI NpWebApi2CheckTimeout() {
	// Timeout processing is an internal maintenance tick in Prospero. Requests
	// complete synchronously in this implementation, so there is no pending
	// timeout state to advance.
}

static int KYTY_SYSV_ABI NpWebApi2Terminate(int lib_ctx_id) {
	PRINT_NAME();

	LOGF("\t lib_ctx_id = %d\n", lib_ctx_id);

	return 0;
}

LIB_DEFINE(InitNet_1_NpWebApi2) {
	LIB_FUNC("+o9816YQhqQ", LibNpWebApi2::NpWebApi2Initialize);
	LIB_FUNC("WV1GwM32NgY", LibNpWebApi2::NpWebApi2PushEventCreateHandle);
	LIB_FUNC("sk54bi6FtYM", LibNpWebApi2::NpWebApi2CreateUserContext);
	LIB_FUNC("9X9+cneTGUU", LibNpWebApi2::NpWebApi2DeleteUserContext);
	LIB_FUNC("3EI-OSJ65Xc", LibNpWebApi2::NpWebApi2CreateRequest);
	LIB_FUNC("lQOCF84lvzw", LibNpWebApi2::NpWebApi2SendRequest);
	LIB_FUNC("OOY9+ObfKec", LibNpWebApi2::NpWebApi2ReadData);
	LIB_FUNC("zpiPsH7dbFQ", LibNpWebApi2::NpWebApi2AbortRequest);
	LIB_FUNC("vvzWO-DvG1s", LibNpWebApi2::NpWebApi2DeleteRequest);
	LIB_FUNC("egOOvrnF6mI", LibNpWebApi2::NpWebApi2AddHttpRequestHeader);
	LIB_FUNC("HwP3aM+c85c", LibNpWebApi2::NpWebApi2GetHttpResponseHeaderValueLength);
	LIB_FUNC("hksbskNToEA", LibNpWebApi2::NpWebApi2GetHttpResponseHeaderValue);
	LIB_FUNC("fIATVMo4Y1w", LibNpWebApi2::NpWebApi2PushEventDeleteHandle);
	LIB_FUNC("QafxeZM3WK4", LibNpWebApi2::NpWebApi2PushEventDeletePushContext);
	LIB_FUNC("MsaFhR+lPE4", LibNpWebApi2::NpWebApi2PushEventCreateFilter);
	LIB_FUNC("fY3QqeNkF8k", LibNpWebApi2::NpWebApi2PushEventRegisterCallback);
	LIB_FUNC("3Tt9zL3tkoc", LibNpWebApi2::NpWebApi2CheckTimeout);
	LIB_FUNC("bEvXpcEk200", LibNpWebApi2::NpWebApi2Terminate);
}

} // namespace LibNpWebApi2

namespace LibGameUpdate {

LIB_VERSION("GameUpdate", 1, "GameUpdate", 1, 1);

constexpr int GAME_UPDATE_ERROR_NOT_INITIALIZED   = static_cast<int>(0x80412801);
constexpr int GAME_UPDATE_ERROR_INVALID_ARG       = static_cast<int>(0x80412803);
constexpr int GAME_UPDATE_ERROR_INVALID_SIZE      = static_cast<int>(0x80412804);
constexpr int GAME_UPDATE_ERROR_REQUEST_NOT_FOUND = static_cast<int>(0x80412805);

struct GameUpdateCheckParam {
	size_t   size;
	uint32_t option;
	uint32_t reserved[9];
};

struct GameUpdateCheckResult {
	size_t   size;
	bool     found;
	bool     addcont_found;
	char     padding[2];
	char     content_version[11];
	char     padding2[1];
	uint32_t reserved[6];
};

struct GameUpdateAddcontVersionInfo {
	size_t   size;
	bool     found;
	char     content_version[11];
	uint32_t reserved[6];
};

static bool                g_game_update_initialized  = false;
static int                 g_game_update_next_request = 1;
static std::map<int, bool> g_game_update_requests;

static int KYTY_SYSV_ABI GameUpdateInitialize() {
	PRINT_NAME();

	g_game_update_initialized = true;

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateTerminate() {
	PRINT_NAME();

	g_game_update_initialized = false;
	g_game_update_requests.clear();

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateCreateRequest() {
	PRINT_NAME();

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}

	const int request_id               = g_game_update_next_request++;
	g_game_update_requests[request_id] = true;

	return request_id;
}

static int KYTY_SYSV_ABI GameUpdateCheck(int request_id, const GameUpdateCheckParam* param,
                                         GameUpdateCheckResult* result) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n"
	     "\t param      = 0x%016" PRIx64 "\n"
	     "\t result     = 0x%016" PRIx64 "\n",
	     request_id, reinterpret_cast<uint64_t>(param), reinterpret_cast<uint64_t>(result));

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (g_game_update_requests.find(request_id) == g_game_update_requests.end()) {
		return GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
	}
	if (param == nullptr || result == nullptr) {
		return GAME_UPDATE_ERROR_INVALID_ARG;
	}
	if (param->size < sizeof(GameUpdateCheckParam) ||
	    result->size < sizeof(GameUpdateCheckResult)) {
		return GAME_UPDATE_ERROR_INVALID_SIZE;
	}

	const auto result_size = result->size;
	*result                = {};
	result->size           = result_size;
	result->found          = false;
	result->addcont_found  = false;

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateAbortRequest(int request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n", request_id);

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (g_game_update_requests.find(request_id) == g_game_update_requests.end()) {
		return GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
	}

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateDeleteRequest(int request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n", request_id);

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (g_game_update_requests.erase(request_id) == 0) {
		return GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
	}

	return 0;
}

static int KYTY_SYSV_ABI GameUpdateGetAddcontLatestVersion(uint32_t    service_label,
                                                           const void* entitlement_label,
                                                           GameUpdateAddcontVersionInfo* info) {
	PRINT_NAME();

	LOGF("\t service_label     = %" PRIu32 "\n"
	     "\t entitlement_label = 0x%016" PRIx64 "\n"
	     "\t info              = 0x%016" PRIx64 "\n",
	     service_label, reinterpret_cast<uint64_t>(entitlement_label),
	     reinterpret_cast<uint64_t>(info));

	if (!g_game_update_initialized) {
		return GAME_UPDATE_ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr) {
		return GAME_UPDATE_ERROR_INVALID_ARG;
	}
	if (info->size < sizeof(GameUpdateAddcontVersionInfo)) {
		return GAME_UPDATE_ERROR_INVALID_SIZE;
	}

	const auto info_size = info->size;
	*info                = {};
	info->size           = info_size;
	info->found          = false;

	return 0;
}

LIB_DEFINE(InitNet_1_GameUpdate) {
	LIB_FUNC("YJtKLttI9fM", LibGameUpdate::GameUpdateInitialize);
	LIB_FUNC("NSH-C-OmoNI", LibGameUpdate::GameUpdateTerminate);
	LIB_FUNC("UvcvKaFvupA", LibGameUpdate::GameUpdateCreateRequest);
	LIB_FUNC("LYVV9z8+owM", LibGameUpdate::GameUpdateCheck);
	LIB_FUNC("d1CNGEOaK28", LibGameUpdate::GameUpdateAbortRequest);
	LIB_FUNC("bcCyjHN5sn0", LibGameUpdate::GameUpdateDeleteRequest);
	LIB_FUNC("0g0+Oq9xcI0", LibGameUpdate::GameUpdateGetAddcontLatestVersion);
}

} // namespace LibGameUpdate

namespace LibShare {

LIB_VERSION("Share", 1, "Share", 1, 1);

static int KYTY_SYSV_ABI ShareInitialize(size_t heap_size, int thread_priority,
                                         uint64_t affinity_mask) {
	PRINT_NAME();

	LOGF("\t heap_size       = %" PRIu64 "\n"
	     "\t thread_priority = %d\n"
	     "\t affinity_mask   = 0x%016" PRIx64 "\n",
	     heap_size, thread_priority, affinity_mask);

	return 0;
}

static int KYTY_SYSV_ABI ShareFeaturePermit(uint32_t feature_flags) {
	PRINT_NAME();

	LOGF("\t feature_flags = 0x%08" PRIx32 "\n", feature_flags);

	return 0;
}

static int KYTY_SYSV_ABI ShareTerminate() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitNet_1_Share) {
	LIB_FUNC("nBDD66kiFW8", LibShare::ShareInitialize);
	LIB_FUNC("YBiIdcDPrxs", LibShare::ShareFeaturePermit);
	LIB_FUNC("0IL1keINExQ", LibShare::ShareTerminate);
}

} // namespace LibShare

namespace LibJson2 {
LIB_DEFINE(InitNet_1_Json2);
} // namespace LibJson2

namespace LibRandom {

LIB_VERSION("Random", 1, "Random", 1, 1);

constexpr int  RANDOM_ERROR_INVALID = 0x817c0016;
constexpr auto RANDOM_MAX_SIZE      = 64u;

static int KYTY_SYSV_ABI RandomGetRandomNumber(void* buf, size_t size) {
	PRINT_NAME();

	if ((buf == nullptr && size != 0) || size > RANDOM_MAX_SIZE) {
		return RANDOM_ERROR_INVALID;
	}

	static uint32_t seed = 0x9e3779b9u;
	auto*           out  = static_cast<uint8_t*>(buf);
	for (size_t i = 0; i < size; i++) {
		seed   = seed * 1664525u + 1013904223u;
		out[i] = static_cast<uint8_t>(seed >> 24u);
	}

	return 0;
}

LIB_DEFINE(InitPlatform_1_Random) {
	LIB_FUNC("PI7jIZj4pcE", LibRandom::RandomGetRandomNumber);
}

} // namespace LibRandom

namespace LibIme {

LIB_VERSION("Ime", 1, "Ime", 1, 1);

static int KYTY_SYSV_ABI ImeKeyboardOpen(int user_id, const void* param) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t param   = 0x%016" PRIx64 "\n",
	     user_id, reinterpret_cast<uint64_t>(param));

	return 0;
}

static int KYTY_SYSV_ABI ImeKeyboardClose(int user_id) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	return 0;
}

static int KYTY_SYSV_ABI ImeKeyboardGetResourceId(int user_id, void* resource_id_array) {
	PRINT_NAME();

	LOGF("\t user_id           = %d\n"
	     "\t resource_id_array = 0x%016" PRIx64 "\n",
	     user_id, reinterpret_cast<uint64_t>(resource_id_array));

	return 0;
}

static int KYTY_SYSV_ABI ImeKeyboardGetInfo(uint32_t resource_id, void* info) {
	PRINT_NAME();

	LOGF("\t resource_id = 0x%08" PRIx32 "\n"
	     "\t info        = 0x%016" PRIx64 "\n",
	     resource_id, reinterpret_cast<uint64_t>(info));

	return 0;
}

static int KYTY_SYSV_ABI ImeKeyboardSetMode(int user_id, uint32_t mode) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n"
	     "\t mode    = 0x%08" PRIx32 "\n",
	     user_id, mode);

	return 0;
}

static int KYTY_SYSV_ABI ImeUpdate(void* handler) {
	PRINT_NAME();

	// LOGF("\t handler = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(handler));

	return 0;
}

LIB_DEFINE(InitPlatform_1_Ime) {
	LIB_FUNC("eaFXjfJv3xs", LibIme::ImeKeyboardOpen);
	LIB_FUNC("PMVehSlfZ94", LibIme::ImeKeyboardClose);
	LIB_FUNC("dKadqZFgKKQ", LibIme::ImeKeyboardGetResourceId);
	LIB_FUNC("VkqLPArfFdc", LibIme::ImeKeyboardGetInfo);
	LIB_FUNC("ua+13Hk9kKs", LibIme::ImeKeyboardSetMode);
	LIB_FUNC("-4GCfYdNF1s", LibIme::ImeUpdate);
}

} // namespace LibIme

namespace LibRemoteplay {

LIB_VERSION("Remoteplay", 1, "Remoteplay", 1, 1);

static int KYTY_SYSV_ABI RemoteplayInitialize(void* heap, size_t heap_size) {
	PRINT_NAME();

	LOGF("\t heap      = 0x%016" PRIx64 "\n"
	     "\t heap_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(heap), static_cast<uint64_t>(heap_size));

	return 0;
}

static int KYTY_SYSV_ABI RemoteplayTerminate() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI RemoteplayGetConnectionStatus(int user_id, int* status) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	if (status == nullptr) {
		return -1;
	}

	*status = 0;

	return 0;
}

LIB_DEFINE(InitPlatform_1_Remoteplay) {
	LIB_FUNC("k1SwgkMSOM8", LibRemoteplay::RemoteplayInitialize);
	LIB_FUNC("BOwybKVa3Do", LibRemoteplay::RemoteplayTerminate);
	LIB_FUNC("g3PNjYKWqnQ", LibRemoteplay::RemoteplayGetConnectionStatus);
}

} // namespace LibRemoteplay

namespace LibWebBrowserDialog {

LIB_VERSION("WebBrowserDialog", 1, "WebBrowserDialog", 1, 1);

static int KYTY_SYSV_ABI WebBrowserDialogInitialize() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI WebBrowserDialogTerminate() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitPlatform_1_WebBrowserDialog) {
	LIB_FUNC("jqb7HntFQFc", LibWebBrowserDialog::WebBrowserDialogInitialize);
	LIB_FUNC("ocHtyBwHfys", LibWebBrowserDialog::WebBrowserDialogTerminate);
}

} // namespace LibWebBrowserDialog

namespace LibGameLiveStreaming {

LIB_VERSION("GameLiveStreaming", 1, "GameLiveStreaming", 1, 1);

static int KYTY_SYSV_ABI GameLiveStreamingInitialize(size_t heap_size) {
	PRINT_NAME();

	LOGF("\t heap_size = %" PRIu64 "\n", static_cast<uint64_t>(heap_size));

	return 0;
}

static int KYTY_SYSV_ABI GameLiveStreamingTerminate() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitPlatform_1_GameLiveStreaming) {
	LIB_FUNC("kvYEw2lBndk", LibGameLiveStreaming::GameLiveStreamingInitialize);
	LIB_FUNC("9yK6Fk8mKOQ", LibGameLiveStreaming::GameLiveStreamingTerminate);
}

} // namespace LibGameLiveStreaming

namespace LibSharePlay {

LIB_VERSION("SharePlay", 1, "SharePlay", 1, 1);

static int KYTY_SYSV_ABI SharePlayInitialize(void* heap, size_t heap_size) {
	PRINT_NAME();

	LOGF("\t heap      = 0x%016" PRIx64 "\n"
	     "\t heap_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(heap), static_cast<uint64_t>(heap_size));

	return 0;
}

static int KYTY_SYSV_ABI SharePlayTerminate() {
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitPlatform_1_SharePlay) {
	LIB_FUNC("isruqthpYcw", LibSharePlay::SharePlayInitialize);
	LIB_FUNC("UaLjloJinow", LibSharePlay::SharePlayTerminate);
}

} // namespace LibSharePlay

LIB_DEFINE(InitPlatform_1) {
	LibRandom::InitPlatform_1_Random(s);
	LibIme::InitPlatform_1_Ime(s);
	LibRemoteplay::InitPlatform_1_Remoteplay(s);
	LibWebBrowserDialog::InitPlatform_1_WebBrowserDialog(s);
	LibGameLiveStreaming::InitPlatform_1_GameLiveStreaming(s);
	LibSharePlay::InitPlatform_1_SharePlay(s);
}

LIB_DEFINE(InitNet_1) {
	LibNet::InitNet_1_Net(s);
	LibSsl::InitNet_1_Ssl(s);
	LibHttp::InitNet_1_Http(s);
	LibHttp2::InitNet_1_Http2(s);
	LibNetCtl::InitNet_1_NetCtl(s);
	LibNpManager::InitNet_1_NpManager(s);
	LibNpSessionSignaling::InitNet_1_NpSessionSignaling(s);
	LibNpEntitlementAccess::InitNet_1_NpEntitlementAccess(s);
	LibNpAuth::InitNet_1_NpAuth(s);
	LibNpTrophy2::InitNet_1_NpTrophy2(s);
	LibNpUniversalDataSystem::InitNet_1_NpUniversalDataSystem(s);
	LibCes::InitNet_1_Ces(s);
	LibNpGameIntent::InitNet_1_NpGameIntent(s);
	LibNpWebApi2::InitNet_1_NpWebApi2(s);
	LibGameUpdate::InitNet_1_GameUpdate(s);
	LibShare::InitNet_1_Share(s);
	LibJson2::InitNet_1_Json2(s);
}

} // namespace Libs
