#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>

namespace Libs {

namespace LibHttp {

LIB_NAME("Http", "Http");

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

	constexpr uint32_t BUILD_WITH_SCHEME   = 0x01;
	constexpr uint32_t BUILD_WITH_HOSTNAME = 0x02;
	constexpr uint32_t BUILD_WITH_PORT     = 0x04;
	constexpr uint32_t BUILD_WITH_PATH     = 0x08;
	constexpr uint32_t BUILD_WITH_USERNAME = 0x10;
	constexpr uint32_t BUILD_WITH_PASSWORD = 0x20;
	constexpr uint32_t BUILD_WITH_QUERY    = 0x40;
	constexpr uint32_t BUILD_WITH_FRAGMENT = 0x80;
	constexpr uint32_t BUILD_WITH_ALL      = 0xff;

	// No component bits preserves the legacy full-URI build.
	const uint32_t parts         = (option & BUILD_WITH_ALL) == 0 ? BUILD_WITH_ALL : option;
	const bool     hierarchical  = src_element->opaque == 0 && src_element->hostname != nullptr;
	const bool     with_hostname = (parts & BUILD_WITH_HOSTNAME) != 0 && hierarchical;

	std::string uri;
	if ((parts & BUILD_WITH_SCHEME) != 0) {
		if (src_element->scheme != nullptr) {
			uri.append(src_element->scheme);
			uri.push_back(':');
		}
		// Scheme-only builds include the authority marker.
		if (hierarchical) {
			uri.append("//");
		}
	}

	const bool with_username = (parts & BUILD_WITH_USERNAME) != 0 && src_element->username != nullptr;
	const bool with_password = (parts & BUILD_WITH_PASSWORD) != 0 && src_element->password != nullptr;
	if (with_username) {
		uri.append(src_element->username);
	}
	if (with_password) {
		if (with_username) {
			uri.push_back(':');
		}
		uri.append(src_element->password);
	}
	if (with_hostname) {
		if (with_username || with_password) {
			uri.push_back('@');
		}
		uri.append(src_element->hostname);
	}
	if ((parts & BUILD_WITH_PORT) != 0 && hierarchical && src_element->port != 0) {
		if (with_hostname) {
			uri.push_back(':');
		}
		uri.append(std::to_string(src_element->port));
	}

	if ((parts & BUILD_WITH_PATH) != 0) {
		AppendUriPart(&uri, src_element->path);
	}
	if ((parts & BUILD_WITH_QUERY) != 0) {
		AppendUriPart(&uri, src_element->query);
	}
	if ((parts & BUILD_WITH_FRAGMENT) != 0) {
		AppendUriPart(&uri, src_element->fragment);
	}

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
	LIB_FUNC("h9wmFZX4i-4", Http::HttpSetRedirectCallback);
	LIB_FUNC("Kh6bS2HQKbo", Http::HttpSetCookieRecvCallback);
	LIB_FUNC("vO4B-42ef-k", Http::HttpSetRequestStatusCallback);
	LIB_FUNC("i9mhafzkEi8", Http::HttpSetInflateGZIPEnabled);
	LIB_FUNC("PDxS48xGQLs", Http::HttpSetChunkedTransferEnabled);
	LIB_FUNC("DK+GoXCNT04", Http::HttpsLoadCert);
	LIB_FUNC("zXqcE0fizz0", Http::HttpsUnloadCert);
	LIB_FUNC("JBN6N-EY+3M", Http::HttpsGetSslError);
	LIB_FUNC("0onIrKx9NIE", Http::HttpGetLastErrno);
	LIB_FUNC("IWalAn-guFs", LibHttp::HttpUriParse);
	LIB_FUNC("YuOW3dDAKYc", LibHttp::HttpUriEscape);
	LIB_FUNC("5LZA+KPISVA", LibHttp::HttpUriBuild);
}

} // namespace LibHttp

} // namespace Libs
