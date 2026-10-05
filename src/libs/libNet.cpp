#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"
#include "graphics/presentation/systemOverlay.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs {

namespace Network::Net {
struct NetEtherAddr;
} // namespace Network::Net

namespace LibNet {

LIB_VERSION("Net", 1, "Net", 1, 1);

static thread_local int   g_net_errno = 0;
static constexpr uint32_t g_in6addr_any[4] {};

namespace Net = Network::Net;

KYTY_SYSV_ABI int* GetNetErrorAddr() {
	return &g_net_errno;
}

static int FinishNetCall(int result) {
	if (result < 0) {
		g_net_errno = result - Network::NET_ERROR_EPERM + 1;
	}
	return result;
}

static int KYTY_SYSV_ABI NetInit() {
	return FinishNetCall(Net::NetInit());
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
	return FinishNetCall(result < 0 ? PosixToNetError(*Posix::GetErrorAddr()) : result);
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

int KYTY_SYSV_ABI NetGetsockname(int s, void* addr, uint32_t* addrlen) {
	return FinishSocketCall(Net::Getsockname(s, addr, addrlen));
}

int KYTY_SYSV_ABI NetPoolCreate(const char* name, int size, int flags) {
	return FinishNetCall(Net::NetPoolCreate(name, size, flags));
}

int KYTY_SYSV_ABI NetPoolDestroy(int memid) {
	return FinishNetCall(Net::NetPoolDestroy(memid));
}

int KYTY_SYSV_ABI NetResolverCreate(const char* name, int memid, int flags) {
	return FinishNetCall(Net::NetResolverCreate(name, memid, flags));
}

int KYTY_SYSV_ABI NetResolverDestroy(int rid) {
	return FinishNetCall(Net::NetResolverDestroy(rid));
}

int KYTY_SYSV_ABI NetResolverAbort(int rid, int flags) {
	return FinishNetCall(Net::NetResolverAbort(rid, flags));
}

int KYTY_SYSV_ABI NetResolverStartNtoa(int rid, const char* hostname, void* addr, int timeout,
                                       int retry, int flags) {
	return FinishNetCall(Net::NetResolverStartNtoa(rid, hostname, addr, timeout, retry, flags));
}

int KYTY_SYSV_ABI NetInetPton(int af, const char* src, void* dst) {
	return FinishNetCall(Net::NetInetPton(af, src, dst));
}

const char* KYTY_SYSV_ABI NetInetNtop(int af, const void* src, char* dst, uint32_t size) {
	const char* result = Net::NetInetNtop(af, src, dst, size);
	if (result == nullptr) {
		FinishNetCall(PosixToNetError(*Posix::GetErrorAddr()));
	}
	return result;
}

int KYTY_SYSV_ABI NetEtherNtostr(const Net::NetEtherAddr* n, char* str, size_t len) {
	return FinishNetCall(Net::NetEtherNtostr(n, str, len));
}

int KYTY_SYSV_ABI NetGetMacAddress(Net::NetEtherAddr* addr, int flags) {
	return FinishNetCall(Net::NetGetMacAddress(addr, flags));
}

int KYTY_SYSV_ABI NetGetSockInfo(int s, void* info, int n, int flags) {
	return FinishNetCall(Net::NetGetSockInfo(s, info, n, flags));
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
	return FinishNetCall(Net::SocketClose(s));
}

int KYTY_SYSV_ABI NetSetsockopt(int s, int level, int optname, const void* optval,
                                uint32_t optlen) {
	return FinishSocketCall(Net::Setsockopt(s, level, optname, optval, optlen));
}

int KYTY_SYSV_ABI NetSend(int s, const void* buf, size_t len, int flags) {
	const auto size = std::min<size_t>(len, std::numeric_limits<int>::max());
	return FinishSocketCall(static_cast<int>(Net::Send(s, buf, size, flags | 0x20000)));
}

int KYTY_SYSV_ABI NetRecv(int s, void* buf, size_t len, int flags) {
	const auto size = std::min<size_t>(len, std::numeric_limits<int>::max());
	return FinishSocketCall(static_cast<int>(Net::Recv(s, buf, size, flags)));
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
	LIB_OBJECT("ZRAJo-A-ukc", &LibNet::g_in6addr_any);

	LIB_FUNC("Nlev7Lg8k3A", LibNet::NetInit);
	LIB_FUNC("HQOwnfMGipQ", LibNet::GetNetErrorAddr);
	LIB_FUNC("PIWqhn9oSxc", LibNet::NetAccept);
	LIB_FUNC("bErx49PgxyY", LibNet::NetBind);
	LIB_FUNC("dgJBaeJnGpo", LibNet::NetPoolCreate);
	LIB_FUNC("K7RlrTkI-mw", LibNet::NetPoolDestroy);
	LIB_FUNC("C4UgDHHPvdw", LibNet::NetResolverCreate);
	LIB_FUNC("kJlYH5uMAWI", LibNet::NetResolverDestroy);
	LIB_FUNC("AzqoBha7js4", LibNet::NetResolverAbort);
	LIB_FUNC("Nd91WaWmG2w", LibNet::NetResolverStartNtoa);
	LIB_FUNC("8Kcp5d-q1Uo", LibNet::NetInetPton);
	LIB_FUNC("9vA2aW+CHuA", LibNet::NetInetNtop);
	LIB_FUNC("v6M4txecCuo", LibNet::NetEtherNtostr);
	LIB_FUNC("6Oc0bLsIYe0", LibNet::NetGetMacAddress);
	LIB_FUNC("hLuXdjHnhiI", LibNet::NetGetSockInfo);
	LIB_FUNC("hoOAofhhRvE", LibNet::NetGetsockname);
	LIB_FUNC("SF47kB2MNTo", LibNet::NetEpollCreate);
	LIB_FUNC("ZVw46bsasAk", LibNet::NetEpollControl);
	LIB_FUNC("drjIbDbA7UQ", LibNet::NetEpollWait);
	LIB_FUNC("Inp1lfL+Jdw", LibNet::NetEpollDestroy);
	LIB_FUNC("kOj1HiAGE54", LibNet::NetListen);
	LIB_FUNC("TSM6whtekok", LibNet::NetShutdown);
	LIB_FUNC("Q4qBuN-c0ZM", LibNet::NetSocket);
	LIB_FUNC("45ggEzakPJQ", LibNet::NetSocketClose);
	LIB_FUNC("2mKX2Spso7I", LibNet::NetSetsockopt);
	LIB_FUNC("beRjXBn-z+o", LibNet::NetSend);
	LIB_FUNC("9wO9XrMsNhc", LibNet::NetRecv);
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
	constexpr size_t needed = 4;

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
		out->path        = dst++;
		out->query       = dst;
		out->scheme[0]   = '\0';
		out->hostname[0] = '\0';
		out->path[0]     = '\0';
		out->query[0]    = '\0';
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
	if (query.begin == nullptr) {
		needed += 1;
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
		if (query.begin == nullptr) {
			out->query    = dst++;
			out->query[0] = '\0';
		} else {
			out->query = CopyUriPart(dst, query);
		}
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
	LIB_FUNC("rGNm+FjIXKk", Http::HttpCreateRequest2);
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
constexpr int HTTP2_ERROR_BEFORE_SEND  = -2122641307; /* 0x817B1065 */
constexpr int HTTP2_ERROR_TIMEOUT      = -2122641304; /* 0x817B1068 */
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
	int                                              send_result = HTTP2_ERROR_BEFORE_SEND;
	int                                              status_code = 0;
	std::string                                      response_headers;
	std::string                                      response_body;
	size_t                                           read_offset  = 0;
	int                                              async_result = HTTP2_ERROR_BEFORE_SEND;
	int                                              async_event  = 0;
	Http2Options                                     options;
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

	request->second.send_result = HTTP2_ERROR_TIMEOUT;

	return request->second.send_result;
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

	request->second.send_result  = HTTP2_ERROR_TIMEOUT;
	request->second.async_result = request->second.send_result;
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

	*result            = {};
	result->event_type = request->second.async_event;
	result->req_id     = req_id;
	result->result     = request->second.async_result;

	return 0;
}

static int KYTY_SYSV_ABI Http2GetStatusCode(int req_id, int* status_code) {
	PRINT_NAME();

	LOGF("\t req_id      = %d\n", req_id);
	LOGF("\t status_code = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(status_code));

	if (status_code == nullptr) {
		return HTTP2_ERROR_NULL_POINTER;
	}

	*status_code = 0;

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		return send_result;
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

	*result         = 0;
	*content_length = 0;

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		*result = -1;
		return send_result;
	}

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

	*header      = nullptr;
	*header_size = 0;

	auto request = g_http2_requests.find(req_id);
	if (request == g_http2_requests.end()) {
		return HTTP2_ERROR_INVALID_ID;
	}

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		return send_result;
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

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		return send_result;
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

	const int send_result = request->second.send_result;
	if (send_result != 0) {
		request->second.async_result = send_result;
		request->second.async_event  = 1;
		return 0;
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

namespace LibNpCommerce {

LIB_VERSION("NpCommerce", 1, "NpCommerce", 1, 1);

constexpr int COMMERCE_STATUS_NONE        = 0;
constexpr int COMMERCE_STATUS_INITIALIZED = 1;

constexpr int COMMERCE_ERROR_NOT_INITIALIZED     = static_cast<int>(0x80B80003u);
constexpr int COMMERCE_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80B80004u);

static int g_commerce_status = COMMERCE_STATUS_NONE;

static int KYTY_SYSV_ABI NpCommerceDialogInitialize() {
	PRINT_NAME();
	if (g_commerce_status != COMMERCE_STATUS_NONE) {
		return COMMERCE_ERROR_ALREADY_INITIALIZED;
	}
	g_commerce_status = COMMERCE_STATUS_INITIALIZED;
	return OK;
}

static int KYTY_SYSV_ABI NpCommerceDialogTerminate() {
	PRINT_NAME();
	if (g_commerce_status == COMMERCE_STATUS_NONE) {
		return COMMERCE_ERROR_NOT_INITIALIZED;
	}
	g_commerce_status = COMMERCE_STATUS_NONE;
	return OK;
}

static int KYTY_SYSV_ABI NpCommerceDialogUpdateStatus() {
	PRINT_NAME();

	return g_commerce_status;
}

LIB_DEFINE(InitNet_1_NpCommerce) {
	LIB_FUNC("0aR2aWmQal4", NpCommerceDialogInitialize);
	LIB_FUNC("m-I92Ab50W8", NpCommerceDialogTerminate);
	LIB_FUNC("LR5cwFMMCVE", NpCommerceDialogUpdateStatus);
}

} // namespace LibNpCommerce

namespace LibNpManager {

LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

namespace NpManager = Network::NpManager;

LIB_DEFINE(InitNet_1_NpManager) {
	LIB_FUNC("3Zl8BePTh9Y", NpManager::NpCheckCallback);
	LIB_FUNC("Ec63y59l9tw", NpManager::NpSetNpTitleId);
	LIB_FUNC("A2CQ3kgSopQ", NpManager::NpSetContentRestriction);
	LIB_FUNC("VfRSmPmj8Q8", NpManager::NpRegisterStateCallback);
	LIB_FUNC("qQJfO8HAiaY", NpManager::NpRegisterStateCallbackA);
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
	LIB_FUNC("Oad3rvY-NJQ", NpManager::NpHasSignedUp);
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

struct NpEntitlementAccessEntitlementKey {
	uint8_t data[16];
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

static int KYTY_SYSV_ABI NpEntitlementAccessGetEntitlementKey(
    uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label,
    NpEntitlementAccessEntitlementKey* key) {
	PRINT_NAME();

	LOGF("\t service_label     = %" PRIu32 "\n"
	     "\t entitlement_label = 0x%016" PRIx64 "\n"
	     "\t key               = 0x%016" PRIx64 "\n",
	     service_label, reinterpret_cast<uint64_t>(entitlement_label),
	     reinterpret_cast<uint64_t>(key));

	if (entitlement_label == nullptr || key == nullptr) {
		return NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
	}

	return NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

LIB_DEFINE(InitNet_1_NpEntitlementAccess) {
	LIB_FUNC("jO8DM8oyego", LibNpEntitlementAccess::NpEntitlementAccessInitialize);
	LIB_FUNC("lPDO62PpJIA", LibNpEntitlementAccess::NpEntitlementAccessGetSkuFlag);
	LIB_FUNC("TFyU+KFBv54",
	         LibNpEntitlementAccess::NpEntitlementAccessGetAddcontEntitlementInfoList);
	LIB_FUNC("xddD23+8TfQ", LibNpEntitlementAccess::NpEntitlementAccessGetAddcontEntitlementInfo);
	LIB_FUNC("5LiMEPuW0DQ", LibNpEntitlementAccess::NpEntitlementAccessGetEntitlementKey);
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

struct NpTrophyMetadata {
	int32_t     id = 0;
	int32_t     grade = 4;
	std::string grade_name = "Bronze";
	std::string name;
	std::string description;
	std::string reward;
	std::vector<std::byte> icon_png;
	bool        hidden = false;
	bool        has_reward = false;
};

struct TrophyLocalization {
	std::string_view locale;
	std::string_view earned;
	std::array<std::string_view, 5> grades;
};

static constexpr std::array<TrophyLocalization, 30> TROPHY_LOCALIZATIONS = {{
    {"ja-JP", "????????", {"", "????", "????", "????", "????"}},
    {"en-US", "TROPHY EARNED", {"", "Platinum", "Gold", "Silver", "Bronze"}},
    {"fr-FR", "TROPH?E OBTENU", {"", "Platine", "Or", "Argent", "Bronze"}},
    {"es-ES", "TROFEO OBTENIDO", {"", "Platino", "Oro", "Plata", "Bronce"}},
    {"de-DE", "TROPH?E ERHALTEN", {"", "Platin", "Gold", "Silber", "Bronze"}},
    {"it-IT", "TROFEO OTTENUTO", {"", "Platino", "Oro", "Argento", "Bronzo"}},
    {"nl-NL", "TROFEE BEHAALD", {"", "Platina", "Goud", "Zilver", "Brons"}},
    {"pt-PT", "TROF?U CONQUISTADO", {"", "Platina", "Ouro", "Prata", "Bronze"}},
    {"ru-RU", "?????? ???????", {"", "???????", "??????", "???????", "??????"}},
    {"ko-KR", "??? ??", {"", "????", "??", "??", "???"}},
    {"zh-Hant", "????", {"", "??", "?", "?", "?"}},
    {"zh-Hans", "????", {"", "??", "?", "?", "?"}},
    {"fi-FI", "TROFEE SAATU", {"", "Platina", "Kulta", "Hopea", "Pronssi"}},
    {"sv-SE", "TROF? ERH?LLEN", {"", "Platina", "Guld", "Silver", "Brons"}},
    {"da-DK", "TROF? OPN?ET", {"", "Platin", "Guld", "S?lv", "Bronze"}},
    {"no-NO", "TROFE MOTTATT", {"", "Platina", "Gull", "S?lv", "Bronse"}},
    {"pl-PL", "ZDOBYTO TROFEUM", {"", "Platyna", "Z?oto", "Srebro", "Br?z"}},
    {"pt-BR", "TROF?U CONQUISTADO", {"", "Platina", "Ouro", "Prata", "Bronze"}},
    {"en-GB", "TROPHY EARNED", {"", "Platinum", "Gold", "Silver", "Bronze"}},
    {"tr-TR", "KUPA KAZANILDI", {"", "Platin", "Alt?n", "G?m??", "Bronz"}},
    {"es-419", "TROFEO OBTENIDO", {"", "Platino", "Oro", "Plata", "Bronce"}},
    {"ar-AE", "?? ?????? ??? ?????", {"", "???????", "????", "???", "??????"}},
    {"fr-CA", "TROPH?E OBTENU", {"", "Platine", "Or", "Argent", "Bronze"}},
    {"cs-CZ", "TROFEJ Z?SK?NA", {"", "Platina", "Zlato", "St??bro", "Bronz"}},
    {"hu-HU", "TR?FEA MEGSZEREZVE", {"", "Platina", "Arany", "Ez?st", "Bronz"}},
    {"el-GR", "??????? ?????????", {"", "??????????", "?????", "????????", "???????"}},
    {"ro-RO", "TROFEU OB?INUT", {"", "Platin?", "Aur", "Argint", "Bronz"}},
    {"th-TH", "????????????????", {"", "????????", "???", "????", "??????"}},
    {"vi-VN", "?? NH?N C?P", {"", "B?ch kim", "V?ng", "B?c", "??ng"}},
    {"id-ID", "TROFI DIDAPATKAN", {"", "Platina", "Emas", "Perak", "Perunggu"}},
}};

static std::mutex                             g_trophy_mutex;
static std::unordered_set<int>                g_unlocked_trophies;
static std::map<int, NpTrophyMetadata>         g_trophy_metadata;
static bool                                    g_trophy_metadata_loaded = false;
static bool                                    g_trophy_metadata_scanned = false;
static bool                                    g_trophy_metadata_missing_logged = false;
static bool                                    g_trophy_unlocks_loaded = false;

using TrophyJson = nlohmann::json;

static constexpr uint64_t TROPHY_PACKAGE_MAX_SIZE = uint64_t {128} << 20u;
static constexpr uint32_t TROPHY_UCP_MAGIC        = 0xb228c60a;
static constexpr uint32_t TROPHY_UCP_VERSION      = 1;
static constexpr uint64_t TROPHY_UCP_HEADER_SIZE  = 0x40;
static constexpr uint64_t TROPHY_UCP_TOC_SKIP     = 0x20;
static constexpr uint64_t TROPHY_UCP_ENTRY_SIZE   = 0x40;
static constexpr uint64_t TROPHY_UCP_NAME_SIZE    = 0x20;
static constexpr uint64_t TROPHY_JSON_MAX_SIZE    = uint64_t {1} << 20u;
static constexpr uint64_t TROPHY_METADATA_MAX_SIZE = uint64_t {64} << 20u;
static constexpr uint64_t TROPHY_ICON_MAX_SIZE    = uint64_t {32} << 20u;
static constexpr uint64_t TROPHY_UNLOCKS_MAX_SIZE = uint64_t {1} << 20u;

static const TrophyLocalization& GetTrophyLocalization() {
	const auto language = Config::GetConsoleLanguage();
	return TROPHY_LOCALIZATIONS[language < TROPHY_LOCALIZATIONS.size()
	                                ? language
	                                : Config::DEFAULT_CONSOLE_LANGUAGE];
}

static uint32_t ReadTrophyU32BE(const std::byte* data) {
	return (static_cast<uint32_t>(std::to_integer<uint8_t>(data[0])) << 24u) |
	       (static_cast<uint32_t>(std::to_integer<uint8_t>(data[1])) << 16u) |
	       (static_cast<uint32_t>(std::to_integer<uint8_t>(data[2])) << 8u) |
	       static_cast<uint32_t>(std::to_integer<uint8_t>(data[3]));
}

static uint64_t ReadTrophyU64BE(const std::byte* data) {
	uint64_t value = 0;
	for (int i = 0; i < 8; i++) {
		value = (value << 8u) | std::to_integer<uint8_t>(data[i]);
	}
	return value;
}

static std::string TrophyJsonString(const TrophyJson& value) {
	if (value.is_string()) {
		return value.get<std::string>();
	}
	if (value.is_number_integer() || value.is_number_unsigned()) {
		return value.dump();
	}
	return {};
}

static bool ParseTrophyId(const TrophyJson& value, int* id) {
	const auto text = TrophyJsonString(value);
	if (text.empty()) {
		return false;
	}
	int parsed = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
	if (error != std::errc {} || end != text.data() + text.size() || parsed < 0) {
		return false;
	}
	*id = parsed;
	return true;
}

static int32_t TrophyGradeValue(std::string_view grade) {
	if (grade == "P") {
		return 1;
	}
	if (grade == "G") {
		return 2;
	}
	if (grade == "S") {
		return 3;
	}
	if (grade == "B") {
		return 4;
	}
	return 0;
}

static std::string TrophyGradeName(std::string_view grade) {
	const auto value = TrophyGradeValue(grade);
	return std::string(GetTrophyLocalization().grades[static_cast<size_t>(
	    value >= 1 && value <= 4 ? value : 4)]);
}

static std::string TrophyPackageEntryName(const std::byte* data, size_t size) {
	size_t length = 0;
	while (length < size && data[length] != std::byte {}) {
		length++;
	}
	std::string name(reinterpret_cast<const char*>(data), length);
	std::transform(name.begin(), name.end(), name.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return name;
}

static std::filesystem::path TrophyUnlocksPath() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty() ||
	    title_id.size() > 64 ||
	    !std::all_of(title_id.begin(), title_id.end(), [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '_' || c == '-';
	    })) {
		return {};
	}
	const auto user_id = Config::GetUserId();
	return std::filesystem::path("_SaveData") / title_id /
	       ("trophies_" + std::to_string(user_id) + ".json");
}

static void EnsureTrophyUnlocksLoaded() {
	std::scoped_lock lock(g_trophy_mutex);
	if (g_trophy_unlocks_loaded) {
		return;
	}
	const auto path = TrophyUnlocksPath();
	if (path.empty()) {
		LOGF("[Trophy] could not determine title ID for trophy persistence\n");
		g_trophy_unlocks_loaded = true;
		return;
	}

	Common::File file(path, Common::File::Mode::Read);
	if (!file.IsInvalid() && file.Size() <= TROPHY_UNLOCKS_MAX_SIZE) {
		const auto data = file.ReadWholeBuffer();
		if (!data.empty()) {
			const auto* begin = reinterpret_cast<const char*>(data.data());
			const auto* end   = begin + data.size();
			const auto  json  = TrophyJson::parse(begin, end, nullptr, false);
			if (!json.is_discarded() && json.is_object()) {
				const auto unlocked = json.find("unlockedTrophies");
				if (unlocked != json.end() && unlocked->is_array()) {
					for (const auto& value: *unlocked) {
						int trophy_id = 0;
						if (ParseTrophyId(value, &trophy_id)) {
							g_unlocked_trophies.insert(trophy_id);
						}
					}
				}
				LOGF("[Trophy] loaded %zu saved unlock(s)\n", g_unlocked_trophies.size());
			} else {
				LOGF("[Trophy] saved unlock file is invalid: %s\n",
				     Common::PathToString(path).c_str());
			}
		}
	} else if (!file.IsInvalid()) {
		LOGF("[Trophy] saved unlock file exceeds the 1 MiB limit: %s\n",
		     Common::PathToString(path).c_str());
	}
	g_trophy_unlocks_loaded = true;
}

static void SaveTrophyUnlocks() {
	const auto path = TrophyUnlocksPath();
	if (path.empty()) {
		LOGF("[Trophy] could not determine title ID for saving trophy unlocks\n");
		return;
	}

	TrophyJson json;
	json["unlockedTrophies"] = TrophyJson::array();
	size_t saved_count = 0;
	{
		std::scoped_lock lock(g_trophy_mutex);
		for (const auto trophy_id: g_unlocked_trophies) {
			json["unlockedTrophies"].push_back(trophy_id);
		}
		saved_count = g_unlocked_trophies.size();
	}
	const auto encoded = json.dump();
	if (encoded.size() > TROPHY_UNLOCKS_MAX_SIZE ||
	    !Common::File::CreateDirectories(path.parent_path())) {
		LOGF("[Trophy] could not create trophy unlock directory: %s\n",
		     Common::PathToString(path.parent_path()).c_str());
		return;
	}

	Common::File file;
	if (!file.Create(path)) {
		LOGF("[Trophy] could not open trophy unlock file for writing: %s\n",
		     Common::PathToString(path).c_str());
		return;
	}
	uint32_t bytes_written = 0;
	file.Write(encoded.data(), static_cast<uint32_t>(encoded.size()), &bytes_written);
	if (bytes_written != encoded.size() || !file.Flush()) {
		LOGF("[Trophy] failed writing trophy unlock file: %s\n",
		     Common::PathToString(path).c_str());
		return;
	}
	LOGF("[Trophy] saved %zu unlock(s) to %s\n", saved_count,
	     Common::PathToString(path).c_str());
}

static bool LoadTrophyPackage(const std::filesystem::path& path,
                              std::map<int, NpTrophyMetadata>& metadata) {
	Common::File file(path, Common::File::Mode::Read);
	if (file.IsInvalid() || file.Size() < TROPHY_UCP_HEADER_SIZE ||
	    file.Size() > TROPHY_PACKAGE_MAX_SIZE) {
		return false;
	}
	const auto data = file.ReadWholeBuffer();
	if (data.size() < TROPHY_UCP_HEADER_SIZE ||
	    ReadTrophyU32BE(data.data()) != TROPHY_UCP_MAGIC ||
	    ReadTrophyU32BE(data.data() + 4) != TROPHY_UCP_VERSION) {
		return false;
	}

	const uint64_t declared_size = ReadTrophyU64BE(data.data() + 8);
	const uint32_t file_count    = ReadTrophyU32BE(data.data() + 0x10);
	const uint64_t toc_offset    = ReadTrophyU32BE(data.data() + 0x14);
	if (declared_size < TROPHY_UCP_HEADER_SIZE || declared_size > data.size() ||
	    file_count > 4096) {
		return false;
	}
	const uint64_t toc_size = TROPHY_UCP_TOC_SKIP + static_cast<uint64_t>(file_count) *
	                                                   TROPHY_UCP_ENTRY_SIZE;
	if (toc_offset > declared_size || toc_size > declared_size - toc_offset) {
		return false;
	}

	std::map<std::string, std::string> package_files;
	uint64_t                          metadata_size = 0;
	uint64_t                          icon_size     = 0;
	for (uint32_t i = 0; i < file_count; i++) {
		const uint64_t entry_offset =
		    toc_offset + TROPHY_UCP_TOC_SKIP + static_cast<uint64_t>(i) * TROPHY_UCP_ENTRY_SIZE;
		const auto* entry = data.data() + entry_offset;
		const auto  name  = TrophyPackageEntryName(entry, TROPHY_UCP_NAME_SIZE);
		const bool is_metadata =
		    name == "tropconf.json" || name == "tropmeta.json" ||
		    (name.starts_with("tropmeta_") && name.ends_with(".json"));
		const bool is_icon = name.starts_with("trop") && name.ends_with(".png");
		if (!is_metadata && !is_icon) {
			continue;
		}

		const uint64_t offset = ReadTrophyU64BE(entry + 0x20);
		const uint64_t size   = ReadTrophyU64BE(entry + 0x28);
		if (offset > declared_size || size > declared_size - offset ||
		    (is_metadata &&
		     (size > TROPHY_JSON_MAX_SIZE || size > TROPHY_METADATA_MAX_SIZE - metadata_size)) ||
		    (is_icon && (size > TROPHY_ICON_MAX_SIZE ||
		                 size > TROPHY_METADATA_MAX_SIZE - icon_size))) {
			return false;
		}
		package_files[name] = std::string(reinterpret_cast<const char*>(data.data() + offset),
		                                  static_cast<size_t>(size));
		if (is_metadata) {
			metadata_size += size;
		} else {
			icon_size += size;
		}
	}

	const auto conf_file = package_files.find("tropconf.json");
	if (conf_file == package_files.end()) {
		return false;
	}
	const auto conf = TrophyJson::parse(conf_file->second, nullptr, false);
	if (conf.is_discarded() || !conf.is_object()) {
		return false;
	}

	const auto default_language = TrophyJsonString(conf.value("defaultLanguage", TrophyJson {}));
	std::string metadata_filename;
	const auto find_localized_metadata = [&package_files](std::string_view language) {
		std::string localized = "tropmeta_" + std::string(language) + ".json";
		std::transform(localized.begin(), localized.end(), localized.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return package_files.contains(localized) ? localized : std::string {};
	};
	metadata_filename = find_localized_metadata(GetTrophyLocalization().locale);
	if (metadata_filename.empty()) {
		const auto locale = GetTrophyLocalization().locale;
		const auto separator = locale.find('-');
		const auto language = locale.substr(0, separator);
		const auto prefix = "tropmeta_" + std::string(language);
		for (const auto& [name, contents]: package_files) {
			(void)contents;
			if (name == prefix + ".json" ||
			    (name.starts_with(prefix + "-") && name.ends_with(".json"))) {
				metadata_filename = name;
				break;
			}
		}
	}
	if (metadata_filename.empty()) {
		metadata_filename = find_localized_metadata(default_language);
	}
	if (metadata_filename.empty()) {
		metadata_filename = find_localized_metadata("en-US");
	}
	if (metadata_filename.empty()) {
		for (const auto& [name, contents]: package_files) {
			(void)contents;
			if (name.starts_with("tropmeta_") && name.ends_with(".json")) {
				metadata_filename = name;
				break;
			}
		}
	}
	if (metadata_filename.empty() && package_files.contains("tropmeta.json")) {
		metadata_filename = "tropmeta.json";
	}
	if (metadata_filename.empty()) {
		return false;
	}

	const auto trophy_metadata =
	    TrophyJson::parse(package_files.at(metadata_filename), nullptr, false);
	if (trophy_metadata.is_discarded() || !trophy_metadata.is_object()) {
		return false;
	}
	const auto definitions = conf.find("trophies");
	if (definitions == conf.end() || !definitions->is_array()) {
		return false;
	}
	const auto metadata_root = trophy_metadata.find("metadata");
	if (metadata_root == trophy_metadata.end() || !metadata_root->is_object()) {
		return false;
	}
	const auto text_entries = metadata_root->find("trophyMetadata");

	std::map<int, NpTrophyMetadata> package_metadata;
	for (const auto& definition: *definitions) {
		if (!definition.is_object()) {
			continue;
		}
		int id = 0;
		if (!ParseTrophyId(definition.value("id", TrophyJson {}), &id)) {
			continue;
		}
		auto& item      = package_metadata[id];
		item.id         = id;
		const auto grade = TrophyJsonString(definition.value("grade", TrophyJson {}));
		item.grade       = TrophyGradeValue(grade);
		item.grade_name  = TrophyGradeName(grade);
		const auto hidden = definition.find("hidden");
		item.hidden      = hidden != definition.end() && hidden->is_boolean() && hidden->get<bool>();
		const auto has_reward = definition.find("hasReward");
		item.has_reward = has_reward != definition.end() && has_reward->is_boolean() &&
		                  has_reward->get<bool>();
	}

	if (text_entries != metadata_root->end() && text_entries->is_array()) {
		for (const auto& text: *text_entries) {
			if (!text.is_object()) {
				continue;
			}
			int id = 0;
			if (!ParseTrophyId(text.value("id", TrophyJson {}), &id)) {
				continue;
			}
			const auto found = package_metadata.find(id);
			if (found == package_metadata.end()) {
				continue;
			}
			found->second.name        = TrophyJsonString(text.value("name", TrophyJson {}));
			found->second.description = TrophyJsonString(text.value("detail", TrophyJson {}));
			found->second.reward      = TrophyJsonString(text.value("reward", TrophyJson {}));
		}
	}

	for (auto& [id, item]: package_metadata) {
		if (item.name.empty()) {
			item.name = item.hidden ? "Hidden Trophy" : "Trophy " + std::to_string(id);
		}
		std::string icon_name = "trop" + std::to_string(id) + ".png";
		const auto unpadded_icon = package_files.find(icon_name);
		if (unpadded_icon != package_files.end()) {
			item.icon_png.resize(unpadded_icon->second.size());
			std::memcpy(item.icon_png.data(), unpadded_icon->second.data(),
			            unpadded_icon->second.size());
		} else {
			const auto id_text = std::to_string(id);
			if (id_text.size() < 4) {
				const auto padded_name = "trop" + std::string(4 - id_text.size(), '0') + id_text +
				                         ".png";
				const auto padded_icon = package_files.find(padded_name);
				if (padded_icon != package_files.end()) {
					item.icon_png.resize(padded_icon->second.size());
					std::memcpy(item.icon_png.data(), padded_icon->second.data(),
					            padded_icon->second.size());
				}
			}
		}
		metadata.try_emplace(id, std::move(item));
	}
	return !package_metadata.empty();
}

static void EnsureTrophyMetadataLoaded() {
	std::scoped_lock lock(g_trophy_mutex);
	if (g_trophy_metadata_loaded || g_trophy_metadata_scanned) {
		return;
	}

	const auto directory =
	    Libs::LibKernel::FileSystem::GetRealFilename("/app0/sce_sys/trophy2");
	if (directory.empty()) {
		if (!g_trophy_metadata_missing_logged) {
			LOGF("[Trophy] game trophy package directory is not mounted yet\n");
			g_trophy_metadata_missing_logged = true;
		}
		return;
	}
	g_trophy_metadata_scanned = true;

	std::vector<std::filesystem::path> packages;
	for (const auto& entry: Common::File::GetDirEntries(directory)) {
		std::string name = entry.name;
		std::transform(name.begin(), name.end(), name.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (entry.is_file && name.starts_with("trophy") && name.ends_with(".ucp")) {
			packages.push_back(directory / Common::PathFromUtf8(entry.name));
		}
	}
	std::sort(packages.begin(), packages.end());

	for (const auto& package: packages) {
		if (LoadTrophyPackage(package, g_trophy_metadata)) {
			g_trophy_metadata_loaded = true;
		}
	}
	if (g_trophy_metadata_loaded) {
		LOGF("[Trophy] loaded %zu trophy definitions from game package(s)\n",
		     g_trophy_metadata.size());
	} else if (!g_trophy_metadata_missing_logged) {
		LOGF("[Trophy] could not load trophy metadata from %zu game package(s)\n",
		     packages.size());
		g_trophy_metadata_missing_logged = true;
	}
}

static void NpTrophy2FillTitle(char* dst, size_t dst_size, const char* src);

static NpTrophyMetadata GetTrophyMetadata(int trophy_id) {
	EnsureTrophyMetadataLoaded();
	std::scoped_lock lock(g_trophy_mutex);
	const auto       it = g_trophy_metadata.find(trophy_id);
	if (it != g_trophy_metadata.end()) {
		return it->second;
	}
	NpTrophyMetadata fallback;
	fallback.id = trophy_id;
	fallback.name = "Trophy " + std::to_string(trophy_id);
	return fallback;
}

static std::array<uint32_t, 5> GetTrophyGradeCounts() {
	std::array<uint32_t, 5> counts {};
	EnsureTrophyMetadataLoaded();
	std::scoped_lock lock(g_trophy_mutex);
	for (const auto& [id, item]: g_trophy_metadata) {
		(void)id;
		switch (item.grade) {
			case 1: counts[1]++; break;
			case 2: counts[2]++; break;
			case 3: counts[3]++; break;
			case 4: counts[4]++; break;
			default: break;
		}
	}
	return counts;
}

static std::vector<NpTrophyMetadata> GetTrophyMetadataList() {
	EnsureTrophyMetadataLoaded();
	std::scoped_lock             lock(g_trophy_mutex);
	std::vector<NpTrophyMetadata> trophies;
	trophies.reserve(g_trophy_metadata.size());
	for (const auto& [id, metadata]: g_trophy_metadata) {
		(void)id;
		trophies.push_back(metadata);
	}
	return trophies;
}

static std::array<uint32_t, 5> GetUnlockedTrophyGradeCounts() {
	std::array<uint32_t, 5> counts {};
	EnsureTrophyMetadataLoaded();
	EnsureTrophyUnlocksLoaded();
	std::scoped_lock         lock(g_trophy_mutex);
	for (const auto trophy_id: g_unlocked_trophies) {
		const auto it = g_trophy_metadata.find(trophy_id);
		if (it != g_trophy_metadata.end() && it->second.grade >= 1 && it->second.grade <= 4) {
			counts[static_cast<size_t>(it->second.grade)]++;
		}
	}
	return counts;
}

static void FillTrophyDetails(NpTrophy2Details* details, const NpTrophyMetadata& metadata) {
	if (details == nullptr) {
		return;
	}
	std::memset(details, 0, sizeof(*details));
	details->trophy_id    = metadata.id;
	details->trophy_grade = metadata.grade;
	details->group_id     = 0;
	details->hidden       = metadata.hidden;
	details->has_reward   = metadata.has_reward;
	NpTrophy2FillTitle(details->name, sizeof(details->name), metadata.name.c_str());
	NpTrophy2FillTitle(details->description, sizeof(details->description),
	                   metadata.description.c_str());
	NpTrophy2FillTitle(details->reward, sizeof(details->reward), metadata.reward.c_str());
}

static void FillTrophyData(NpTrophy2Data* data, int trophy_id) {
	if (data == nullptr) {
		return;
	}
	EnsureTrophyUnlocksLoaded();
	std::memset(data, 0, sizeof(*data));
	data->trophy_id = trophy_id;
	std::scoped_lock lock(g_trophy_mutex);
	data->unlocked = g_unlocked_trophies.contains(trophy_id);
}

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

static void RecordTrophyUnlock(int trophy_id) {
	EnsureTrophyMetadataLoaded();
	EnsureTrophyUnlocksLoaded();
	bool newly_unlocked = false;
	{
		std::scoped_lock lock(g_trophy_mutex);
		newly_unlocked = g_unlocked_trophies.insert(trophy_id).second;
	}
	if (newly_unlocked) {
		SaveTrophyUnlocks();
		if (Config::TrophyNotificationsEnabled()) {
			const auto metadata = GetTrophyMetadata(trophy_id);
			Libs::Graphics::NotifyTrophyUnlocked(GetTrophyLocalization().earned, metadata.name,
			                                     metadata.grade_name,
			                                     metadata.grade,
			                                     metadata.icon_png);
		}
	}
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
		const auto counts = GetTrophyGradeCounts();
		std::memset(details, 0, sizeof(*details));
		details->num_groups   = 1;
		details->num_trophies = counts[1] + counts[2] + counts[3] + counts[4];
		details->num_platinum = counts[1];
		details->num_gold     = counts[2];
		details->num_silver   = counts[3];
		details->num_bronze   = counts[4];
		NpTrophy2FillTitle(details->title, sizeof(details->title), "Kyty");
	}
	if (data != nullptr) {
		const auto unlocked = GetUnlockedTrophyGradeCounts();
		std::memset(data, 0, sizeof(*data));
		data->unlocked_platinum = unlocked[1];
		data->unlocked_gold     = unlocked[2];
		data->unlocked_silver   = unlocked[3];
		data->unlocked_bronze   = unlocked[4];
		data->unlocked_trophies = unlocked[1] + unlocked[2] + unlocked[3] + unlocked[4];
		const auto total = GetTrophyGradeCounts();
		const auto total_count = total[1] + total[2] + total[3] + total[4];
		data->progress_percentage =
		    total_count == 0 ? 0 : static_cast<uint32_t>(
		                                static_cast<uint64_t>(data->unlocked_trophies) * 100 /
		                                total_count);
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
	const auto counts             = GetTrophyGradeCounts();

	if (details != nullptr) {
		std::memset(details, 0, sizeof(*details));
		details->group_id     = normalized_group_id;
		details->num_trophies = counts[1] + counts[2] + counts[3] + counts[4];
		details->num_platinum = counts[1];
		details->num_gold     = counts[2];
		details->num_silver   = counts[3];
		details->num_bronze   = counts[4];
		NpTrophy2FillTitle(details->title, sizeof(details->title), "Base Game");
	}
	if (data != nullptr) {
		const auto unlocked = GetUnlockedTrophyGradeCounts();
		std::memset(data, 0, sizeof(*data));
		data->group_id = normalized_group_id;
		data->unlocked_platinum = unlocked[1];
		data->unlocked_gold     = unlocked[2];
		data->unlocked_silver   = unlocked[3];
		data->unlocked_bronze   = unlocked[4];
		data->unlocked_trophies = unlocked[1] + unlocked[2] + unlocked[3] + unlocked[4];
		const auto total = counts[1] + counts[2] + counts[3] + counts[4];
		data->progress_percentage =
		    total == 0 ? 0 : static_cast<uint32_t>(
		                         static_cast<uint64_t>(data->unlocked_trophies) * 100 / total);
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
	const auto     counts    = GetTrophyGradeCounts();

	if (count != nullptr) {
		*count = out_count;
	}
	if (out_count != 0 && details_array != nullptr) {
		std::memset(details_array, 0, sizeof(*details_array));
		details_array->group_id     = 0;
		details_array->num_trophies = counts[1] + counts[2] + counts[3] + counts[4];
		details_array->num_platinum = counts[1];
		details_array->num_gold     = counts[2];
		details_array->num_silver   = counts[3];
		details_array->num_bronze   = counts[4];
		NpTrophy2FillTitle(details_array->title, sizeof(details_array->title), "Base Game");
	}
	if (out_count != 0 && data_array != nullptr) {
		const auto unlocked = GetUnlockedTrophyGradeCounts();
		std::memset(data_array, 0, sizeof(*data_array));
		data_array->group_id = 0;
		data_array->unlocked_platinum = unlocked[1];
		data_array->unlocked_gold     = unlocked[2];
		data_array->unlocked_silver   = unlocked[3];
		data_array->unlocked_bronze   = unlocked[4];
		data_array->unlocked_trophies = unlocked[1] + unlocked[2] + unlocked[3] + unlocked[4];
		const auto total = counts[1] + counts[2] + counts[3] + counts[4];
		data_array->progress_percentage =
		    total == 0 ? 0 : static_cast<uint32_t>(
		                         static_cast<uint64_t>(data_array->unlocked_trophies) * 100 /
		                         total);
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
		FillTrophyDetails(details, GetTrophyMetadata(trophy_id));
	}
	if (data != nullptr) {
		FillTrophyData(data, trophy_id);
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

	const auto trophies = GetTrophyMetadataList();
	const uint32_t out_count =
	    offset < trophies.size() ? static_cast<uint32_t>(
	                                   std::min<uint64_t>(limit, trophies.size() - offset))
	                             : 0u;

	if (count != nullptr) {
		*count = out_count;
	}
	for (uint32_t i = 0; i < out_count; i++) {
		const auto& trophy = trophies[offset + i];
		FillTrophyDetails(details_array != nullptr ? &details_array[i] : nullptr, trophy);
		FillTrophyData(data_array != nullptr ? &data_array[i] : nullptr, trophy.id);
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

struct NpUniversalDataSystemEventPropertyObject {
	std::unordered_map<std::string, int64_t> integers;
};

struct NpUniversalDataSystemEvent {
	std::string                                      name;
	NpUniversalDataSystemEventPropertyObject*        properties = nullptr;
	std::unique_ptr<NpUniversalDataSystemEventPropertyObject> owned_properties;
};

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

	auto event  = std::make_unique<NpUniversalDataSystemEvent>();
	event->name = event_name;
	if (prop != nullptr) {
		event->properties = const_cast<NpUniversalDataSystemEventPropertyObject*>(prop);
	} else if (prop_ptr != nullptr) {
		event->properties = new NpUniversalDataSystemEventPropertyObject;
	} else {
		event->owned_properties =
		    std::make_unique<NpUniversalDataSystemEventPropertyObject>();
		event->properties = event->owned_properties.get();
	}
	*new_event = event.release();
	if (prop_ptr != nullptr) {
		*prop_ptr = (*new_event)->properties;
	}

	LOGF("\t event_name = %s\n"
	     "\t prop       = 0x%016" PRIx64 "\n"
	     "\t new_event  = 0x%016" PRIx64 "\n"
	     "\t prop_ptr   = 0x%016" PRIx64 "\n",
	     event_name != nullptr ? event_name : "<null>", reinterpret_cast<uint64_t>(prop),
	     reinterpret_cast<uint64_t>(*new_event),
	     prop_ptr != nullptr ? reinterpret_cast<uint64_t>(*prop_ptr) : 0);
	LOGF("[UDS] created event: %s\n", event_name);

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemPostEvent(int context, int handle, const void* event,
                                                        uint64_t options) {
	PRINT_NAME();

	if (event == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	const auto* uds_event = static_cast<const NpUniversalDataSystemEvent*>(event);
	LOGF("\t context = %d\n"
	     "\t handle  = %d\n"
	     "\t event_name = %s\n"
	     "\t event   = 0x%016" PRIx64 "\n"
	     "\t options = 0x%016" PRIx64 "\n",
	     context, handle, uds_event->name.c_str(), reinterpret_cast<uint64_t>(event), options);
	LOGF("[UDS] posted event: %s\n", uds_event->name.c_str());

	if (uds_event->name == "_UnlockTrophy" && uds_event->properties != nullptr) {
		const auto trophy_id = uds_event->properties->integers.find("_trophy_id");
		if (trophy_id == uds_event->properties->integers.end() || trophy_id->second < 0 ||
		    trophy_id->second > std::numeric_limits<int32_t>::max()) {
			LOGF("[Trophy] ignored _UnlockTrophy event without a valid _trophy_id\n");
			return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
		}
		LOGF("[Trophy] received _UnlockTrophy event: id=%" PRId64 "\n", trophy_id->second);
		LibNpTrophy2::RecordTrophyUnlock(static_cast<int>(trophy_id->second));
	} else if (uds_event->name == "_UnlockTrophy") {
		LOGF("[Trophy] ignored _UnlockTrophy event without properties\n");
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	} else if (uds_event->name == "COOLING__DIVED_FROM_DIVING_BOARD" ||
	           uds_event->name == "COOLING_DIVED_FROM_DIVING_BOARD") {
		LOGF("[Trophy] mapped diving-board event to trophy 30\n");
		LibNpTrophy2::RecordTrophyUnlock(30);
	}

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
	object->integers[key] = value;

	return 0;
}

static int KYTY_SYSV_ABI NpUniversalDataSystemEventPropertyObjectSetUInt32(
    NpUniversalDataSystemEventPropertyObject* object, const char* key, uint32_t value) {
	PRINT_NAME();

	if (object == nullptr || key == nullptr) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->integers[key] = static_cast<int64_t>(value);

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
	object->integers[key] = value;

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
	if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
		return NP_UNIVERSAL_DATA_SYSTEM_ERROR_INVALID_ARGUMENT;
	}
	object->integers[key] = static_cast<int64_t>(value);

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

constexpr int NP_WEBAPI2_ERROR_INVALID_ARGUMENT       = -2141899774; /* 0x80553402 */
constexpr int NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID = -2141899773; /* 0x80553403 */
constexpr int NP_WEBAPI2_ERROR_LIB_CONTEXT_NOT_FOUND  = -2141899772; /* 0x80553404 */
constexpr int NP_WEBAPI2_ERROR_REQUEST_NOT_FOUND      = -2141899770; /* 0x80553406 */
constexpr int NP_WEBAPI2_ERROR_NOT_SIGNED_IN          = -2141899769; /* 0x80553407 */

struct NpWebApi2MemoryPoolStats {
	size_t  pool_size;
	size_t  max_inuse_size;
	size_t  current_inuse_size;
	int32_t reserved;
};

static std::mutex            g_np_webapi2_context_mutex;
static std::map<int, size_t> g_np_webapi2_contexts;

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

static int KYTY_SYSV_ABI NpWebApi2Initialize(int lib_http_ctx_id, size_t pool_size) {
	PRINT_NAME();

	LOGF("\t lib_http_ctx_id = %d\n"
	     "\t pool_size       = %" PRIu64 "\n",
	     lib_http_ctx_id, pool_size);

	static int id = 0;

	if (pool_size > std::numeric_limits<size_t>::max() - 0x3fff) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}
	std::lock_guard lock(g_np_webapi2_context_mutex);
	g_np_webapi2_contexts[++id] = (pool_size + 0x3fff) & ~size_t {0x3fff};
	return id;
}

static int KYTY_SYSV_ABI NpWebApi2GetMemoryPoolStats(int lib_ctx_id,
                                                   NpWebApi2MemoryPoolStats* stats) {
	if (stats == nullptr) {
		return NP_WEBAPI2_ERROR_INVALID_ARGUMENT;
	}
	if (lib_ctx_id <= 0) {
		return NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID;
	}
	std::lock_guard lock(g_np_webapi2_context_mutex);
	const auto context = g_np_webapi2_contexts.find(lib_ctx_id);
	if (context == g_np_webapi2_contexts.end()) {
		return NP_WEBAPI2_ERROR_LIB_CONTEXT_NOT_FOUND;
	}
	// Requests currently use host storage, not allocations from the library pool.
	*stats = {context->second, 0, 0, 0};
	return OK;
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

	request->second.response.clear();
	request->second.read_offset = 0;

	if (response_info_option != nullptr) {
		response_info_option->http_status        = 0;
		response_info_option->response_data_size = request->second.response.size();
		if (response_info_option->error_object != nullptr &&
		    response_info_option->error_object_size > 0) {
			response_info_option->error_object[0] = '\0';
		}
	}

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

	if (lib_ctx_id <= 0) {
		return NP_WEBAPI2_ERROR_INVALID_LIB_CONTEXT_ID;
	}
	std::lock_guard lock(g_np_webapi2_context_mutex);
	return g_np_webapi2_contexts.erase(lib_ctx_id) != 0 ? OK : NP_WEBAPI2_ERROR_LIB_CONTEXT_NOT_FOUND;
}

LIB_DEFINE(InitNet_1_NpWebApi2) {
	LIB_FUNC("+o9816YQhqQ", LibNpWebApi2::NpWebApi2Initialize);
	LIB_FUNC("Xweb+naPZ8Y", LibNpWebApi2::NpWebApi2GetMemoryPoolStats);
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

constexpr int GAME_LIVE_STREAMING_ERROR_INVALID_PARAM = -2136997886;

struct GameLiveStreamingStatus2 {
	int32_t  user_id;
	bool     is_on_air;
	uint32_t spectator_count;
	uint32_t text_message_count;
	uint32_t command_message_count;
	uint8_t  reserved[52];
};

static_assert(sizeof(GameLiveStreamingStatus2) == 72);
static_assert(offsetof(GameLiveStreamingStatus2, is_on_air) == 4);
static_assert(offsetof(GameLiveStreamingStatus2, spectator_count) == 8);

static int KYTY_SYSV_ABI GameLiveStreamingInitialize(size_t heap_size) {
	PRINT_NAME();

	LOGF("\t heap_size = %" PRIu64 "\n", static_cast<uint64_t>(heap_size));

	return 0;
}

static int KYTY_SYSV_ABI GameLiveStreamingTerminate() {
	PRINT_NAME();

	return 0;
}

static int KYTY_SYSV_ABI GameLiveStreamingGetCurrentStatus2(GameLiveStreamingStatus2* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return GAME_LIVE_STREAMING_ERROR_INVALID_PARAM;
	}

	std::memset(status, 0, sizeof(*status));
	status->user_id = -1;
	return OK;
}

static int KYTY_SYSV_ABI GameLiveStreamingGetSocialFeedbackMessagesCount(int type, uint32_t* count) {
	PRINT_NAME();

	if (type < 1 || type > 3 || count == nullptr) {
		return GAME_LIVE_STREAMING_ERROR_INVALID_PARAM;
	}

	*count = 0;
	return OK;
}

LIB_DEFINE(InitPlatform_1_GameLiveStreaming) {
	LIB_FUNC("kvYEw2lBndk", LibGameLiveStreaming::GameLiveStreamingInitialize);
	LIB_FUNC("9yK6Fk8mKOQ", LibGameLiveStreaming::GameLiveStreamingTerminate);
	LIB_FUNC("lK8dLBNp9OE", LibGameLiveStreaming::GameLiveStreamingGetCurrentStatus2);
	LIB_FUNC("yeQKjHETi40", LibGameLiveStreaming::GameLiveStreamingGetSocialFeedbackMessagesCount);
}

} // namespace LibGameLiveStreaming

namespace LibSharePlay {

LIB_VERSION("SharePlay", 1, "SharePlay", 1, 1);

constexpr int SHARE_PLAY_ERROR_INVALID_ARGS        = -2129788927;
constexpr int SHARE_PLAY_ERROR_ALREADY_INITIALIZED = -2129788925;
constexpr int SHARE_PLAY_ERROR_NOT_INITIALIZED     = -2129788924;

struct SharePlayConnectionInfoA {
	int32_t  status;
	int32_t  mode;
	char     host_online_id[20];
	char     visitor_online_id[20];
	uint64_t host_account_id;
	uint64_t visitor_account_id;
	int32_t  host_user_id;
	int32_t  visitor_user_id;
};

static_assert(sizeof(SharePlayConnectionInfoA) == 72);
static_assert(offsetof(SharePlayConnectionInfoA, host_account_id) == 48);
static_assert(offsetof(SharePlayConnectionInfoA, host_user_id) == 64);

static bool g_share_play_initialized = false;

static int KYTY_SYSV_ABI SharePlayInitialize(void* heap, size_t heap_size) {
	PRINT_NAME();

	LOGF("\t heap      = 0x%016" PRIx64 "\n"
	     "\t heap_size = %" PRIu64 "\n",
	     reinterpret_cast<uint64_t>(heap), static_cast<uint64_t>(heap_size));

	if (g_share_play_initialized) {
		return SHARE_PLAY_ERROR_ALREADY_INITIALIZED;
	}
	if (heap != nullptr && heap_size < 6u * 1024u) {
		return SHARE_PLAY_ERROR_INVALID_ARGS;
	}
	g_share_play_initialized = true;
	return 0;
}

static int KYTY_SYSV_ABI SharePlayTerminate() {
	PRINT_NAME();

	if (!g_share_play_initialized) {
		return SHARE_PLAY_ERROR_NOT_INITIALIZED;
	}
	g_share_play_initialized = false;
	return 0;
}

static int KYTY_SYSV_ABI SharePlayGetCurrentConnectionInfoA(SharePlayConnectionInfoA* info) {
	if (!g_share_play_initialized) {
		return SHARE_PLAY_ERROR_NOT_INITIALIZED;
	}
	if (info == nullptr) {
		return SHARE_PLAY_ERROR_INVALID_ARGS;
	}
	*info = {.status = 0, .host_user_id = -1, .visitor_user_id = -1};
	return 0;
}

LIB_DEFINE(InitPlatform_1_SharePlay) {
	LIB_FUNC("isruqthpYcw", LibSharePlay::SharePlayInitialize);
	LIB_FUNC("UaLjloJinow", LibSharePlay::SharePlayTerminate);
	LIB_FUNC("+MCXJlWdi+s", LibSharePlay::SharePlayGetCurrentConnectionInfoA);
}

} // namespace LibSharePlay

LIB_DEFINE(InitPlatform_1) {
	LibRandom::InitPlatform_1_Random(s);
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
	LibNpCommerce::InitNet_1_NpCommerce(s);
	LibNpManager::InitNet_1_NpManager(s);
	LibNpSessionSignaling::InitNet_1_NpSessionSignaling(s);
	LibNpEntitlementAccess::InitNet_1_NpEntitlementAccess(s);
	LibNpAuth::InitNet_1_NpAuth(s);
	LibNpTrophy2::InitNet_1_NpTrophy2(s);
	LibNpUniversalDataSystem::InitNet_1_NpUniversalDataSystem(s);
	LibNpGameIntent::InitNet_1_NpGameIntent(s);
	LibNpWebApi2::InitNet_1_NpWebApi2(s);
	LibGameUpdate::InitNet_1_GameUpdate(s);
	LibShare::InitNet_1_Share(s);
	LibJson2::InitNet_1_Json2(s);
}

} // namespace Libs
