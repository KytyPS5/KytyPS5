#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#ifdef s_addr
#undef s_addr
#endif
#else
#include <arpa/inet.h>
#endif

#include "common/assert.h"
#include "common/byteBuffer.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Network {

class Network {
public:
	class Id {
	public:
		static constexpr int MAX_ID = 65536;

		enum class Type : uint32_t {
			Invalid    = 0,
			Http       = 1,
			Ssl        = 2,
			Template   = 3,
			Connection = 4,
			Request    = 5,
		};

		explicit Id(int id)
		    : m_id(static_cast<uint32_t>(id) & 0xffffu), m_type(static_cast<uint32_t>(id) >> 16u) {}
		[[nodiscard]] int ToInt() const {
			return static_cast<int>(m_id + (static_cast<uint32_t>(m_type) << 16u));
		}
		[[nodiscard]] bool IsValid() const { return GetType() != Type::Invalid; }
		[[nodiscard]] Type GetType() const {
			switch (m_type) {
				case static_cast<uint32_t>(Type::Http): return Type::Http;
				case static_cast<uint32_t>(Type::Ssl): return Type::Ssl;
				case static_cast<uint32_t>(Type::Template): return Type::Template;
				case static_cast<uint32_t>(Type::Connection): return Type::Connection;
				case static_cast<uint32_t>(Type::Request): return Type::Request;
				default: return Type::Invalid;
			}
		}

		friend class Network;

	private:
		Id() = default;
		static Id Invalid() { return {}; }
		static Id Create(int net_id, Type type) {
			Id r;
			r.m_id   = net_id;
			r.m_type = static_cast<uint32_t>(type);
			return r;
		}
		[[nodiscard]] int GetId() const { return static_cast<int>(m_id); }

		uint32_t m_id   = 0;
		uint32_t m_type = static_cast<uint32_t>(Type::Invalid);
	};

	using HttpsCallback = KYTY_SYSV_ABI int (*)(int, unsigned int, void* const*, int, void*);

	Network()          = default;
	virtual ~Network() = default;

	KYTY_CLASS_NO_COPY(Network);

	int  PoolCreate(const char* name, int size);
	bool PoolDestroy(int memid);
	int  ResolverCreate(const char* name, int memid);
	bool ResolverValid(int rid);

	Id   SslInit(uint64_t pool_size);
	bool SslTerm(Id ssl_ctx_id);
	bool SslValid(Id ssl_ctx_id);

	Id   HttpInit(int memid, Id ssl_ctx_id, uint64_t pool_size);
	bool HttpTerm(Id http_ctx_id);
	Id   HttpCreateTemplate(Id http_ctx_id, const char* user_agent, int http_ver,
	                        bool is_auto_proxy_conf);
	bool HttpDeleteTemplate(Id tmpl_id);
	bool HttpSetNonblock(Id id, bool enable);
	bool HttpsSetSslCallback(Id id, HttpsCallback cbfunc, void* user_arg);
	bool HttpsSetMinSslVersion(Id id, uint32_t ssl_version);
	bool HttpsDisableOption(Id id, uint32_t ssl_flags);
	bool HttpAddRequestHeader(Id id, const char* name, const char* value, bool add);
	bool HttpValid(Id http_ctx_id);
	bool HttpValidTemplate(Id tmpl_id);
	bool HttpValidConnection(Id conn_id);
	bool HttpValidRequest(Id req_id);
	Id HttpCreateConnection(Id tmpl_id, const char* server_name, const char* scheme, uint16_t port,
	                        bool enable_keep_alive);
	Id HttpCreateConnectionWithURL(Id tmpl_id, const char* url, bool enable_keep_alive);
	bool HttpDeleteConnection(Id conn_id);
	Id   HttpCreateRequestWithURL2(Id conn_id, const char* method, const char* url,
	                               uint64_t content_length);
	bool HttpSetRequestContentLength(Id req_id, uint64_t content_length);
	bool HttpDeleteRequest(Id req_id);
	bool HttpSetResolveTimeOut(Id id, uint32_t usec);
	bool HttpSetResolveRetry(Id id, int32_t retry);
	bool HttpSetConnectTimeOut(Id id, uint32_t usec);
	bool HttpSetSendTimeOut(Id id, uint32_t usec);
	bool HttpSetRecvTimeOut(Id id, uint32_t usec);
	bool HttpSetAutoRedirect(Id id, int enable);
	bool HttpSetAuthEnabled(Id id, int enable);
	bool HttpMarkRequestSent(Id req_id, int result);
	bool HttpGetRequestResponse(Id req_id, int* send_result, int* status_code, const char** headers,
	                            size_t* headers_size, uint64_t* content_length);

private:
	struct Pool {
		bool        used = false;
		std::string name;
		int         size = 0;
	};

	struct Ssl {
		bool     used = false;
		uint64_t size = 0;
	};

	struct Resolver {
		bool        used = false;
		std::string name;
		int         memid = 0;
	};

	struct Http {
		bool     used       = false;
		uint64_t size       = 0;
		int      memid      = 0;
		int      ssl_ctx_id = 0;
	};

	struct HttpHeader {
		std::string name;
		std::string value;
	};

	struct HttpBase {
		std::vector<HttpHeader> headers;
		bool                    used            = false;
		bool                    nonblock        = false;
		bool                    auto_redirect   = true;
		bool                    auth_enabled    = true;
		HttpsCallback           ssl_cbfunc      = nullptr;
		void*                   ssl_user_arg    = nullptr;
		uint32_t                ssl_flags       = 0xA7;
		uint32_t                min_ssl_version = 0;
		int                     http_ctx_id     = 0;
		uint32_t                resolve_timeout = 1'000000;
		int32_t                 resolve_retry   = 4;
		uint32_t                connect_timeout = 30'000000;
		uint32_t                send_timeout    = 120'000000;
		uint32_t                recv_timeout    = 120'000000;
	};

	struct HttpTemplate: public HttpBase {
		std::string user_agent;
		int         http_ver           = 0;
		bool        is_auto_proxy_conf = true;
	};

	struct HttpConnection: public HttpTemplate {
		explicit HttpConnection(const HttpTemplate& tmpl): HttpTemplate(tmpl) {}
		// int    tmpl_id = 0;
		std::string url;
		bool        enable_keep_alive = false;
	};

	struct HttpRequest: public HttpConnection {
		explicit HttpRequest(HttpConnection& conn): HttpConnection(conn) {}
		// int      conn_id = 0;
		std::string method;
		std::string url;
		uint64_t    content_length = 0;
		bool        send_attempted = false;
		int         send_result    = HTTP_ERROR_BEFORE_SEND;
		int         status_code    = 0;
		std::string response_headers;
	};

	HttpBase* FindHttpBase(Id id, bool include_request);

	static constexpr int POOLS_MAX     = 32;
	static constexpr int RESOLVERS_MAX = 32;
	static constexpr int SSL_MAX       = 32;
	static constexpr int HTTP_MAX      = 32;

	Common::Mutex               m_mutex;
	Pool                        m_pools[POOLS_MAX];
	Resolver                    m_resolvers[RESOLVERS_MAX];
	Ssl                         m_ssl[SSL_MAX];
	Http                        m_http[HTTP_MAX];
	std::vector<HttpTemplate>   m_templates;
	std::vector<HttpConnection> m_connections;
	std::vector<HttpRequest>    m_requests;
};

static Network* g_net = nullptr;

KYTY_SUBSYSTEM_INIT(Network) {
	EXIT_IF(g_net != nullptr);

	g_net = new Network;
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Network) {}

KYTY_SUBSYSTEM_DESTROY(Network) {}

int Network::PoolCreate(const char* name, int size) {
	Common::LockGuard lock(m_mutex);

	for (int id = 0; id < POOLS_MAX; id++) {
		if (!m_pools[id].used) {
			m_pools[id].used = true;
			m_pools[id].size = size;
			m_pools[id].name = std::string(name);

			return id;
		}
	}

	return -1;
}

bool Network::PoolDestroy(int memid) {
	Common::LockGuard lock(m_mutex);

	if (memid >= 0 && memid < POOLS_MAX && m_pools[memid].used) {
		m_pools[memid].used = false;

		return true;
	}

	return false;
}

int Network::ResolverCreate(const char* name, int memid) {
	Common::LockGuard lock(m_mutex);

	if (memid < 0 || memid >= POOLS_MAX || !m_pools[memid].used) {
		return -1;
	}

	for (int id = 0; id < RESOLVERS_MAX; id++) {
		if (!m_resolvers[id].used) {
			m_resolvers[id].used  = true;
			m_resolvers[id].name  = std::string(name);
			m_resolvers[id].memid = memid;

			return id;
		}
	}

	return -1;
}

bool Network::ResolverValid(int rid) {
	Common::LockGuard lock(m_mutex);

	return rid >= 0 && rid < RESOLVERS_MAX && m_resolvers[rid].used;
}

Network::Id Network::SslInit(uint64_t pool_size) {
	Common::LockGuard lock(m_mutex);

	for (int id = 0; id < SSL_MAX; id++) {
		if (!m_ssl[id].used) {
			m_ssl[id].used = true;
			m_ssl[id].size = pool_size;

			return Id::Create(id, Id::Type::Ssl);
		}
	}

	return Id::Invalid();
}

bool Network::SslTerm(Id ssl_ctx_id) {
	Common::LockGuard lock(m_mutex);

	if (ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 &&
	    ssl_ctx_id.GetId() < SSL_MAX && m_ssl[ssl_ctx_id.GetId()].used) {
		m_ssl[ssl_ctx_id.GetId()].used = false;

		return true;
	}

	return false;
}

bool Network::SslValid(Id ssl_ctx_id) {
	Common::LockGuard lock(m_mutex);

	return ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 &&
	       ssl_ctx_id.GetId() < SSL_MAX && m_ssl[ssl_ctx_id.GetId()].used;
}

Network::Id Network::HttpInit(int memid, Id ssl_ctx_id, uint64_t pool_size) {
	Common::LockGuard lock(m_mutex);

	if (ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 &&
	    ssl_ctx_id.GetId() < SSL_MAX && m_ssl[ssl_ctx_id.GetId()].used && memid >= 0 &&
	    memid < POOLS_MAX && m_pools[memid].used) {
		for (int id = 0; id < HTTP_MAX; id++) {
			if (!m_http[id].used) {
				m_http[id].used       = true;
				m_http[id].size       = pool_size;
				m_http[id].ssl_ctx_id = ssl_ctx_id.GetId();
				m_http[id].memid      = memid;

				return Id::Create(id, Id::Type::Http);
			}
		}
	}

	return Id::Invalid();
}

bool Network::HttpValid(Id http_ctx_id) {
	Common::LockGuard lock(m_mutex);

	return (http_ctx_id.GetType() == Id::Type::Http && http_ctx_id.GetId() >= 0 &&
	        http_ctx_id.GetId() < HTTP_MAX && m_http[http_ctx_id.GetId()].used);
}

bool Network::HttpValidTemplate(Id tmpl_id) {
	Common::LockGuard lock(m_mutex);

	return (tmpl_id.GetType() == Id::Type::Template && tmpl_id.GetId() >= 0 &&
	        static_cast<size_t>(tmpl_id.GetId()) < m_templates.size() &&
	        m_templates[tmpl_id.GetId()].used);
}

bool Network::HttpValidConnection(Id conn_id) {
	Common::LockGuard lock(m_mutex);

	return (conn_id.GetType() == Id::Type::Connection && conn_id.GetId() >= 0 &&
	        static_cast<size_t>(conn_id.GetId()) < m_connections.size() &&
	        m_connections[conn_id.GetId()].used);
}

bool Network::HttpValidRequest(Id req_id) {
	Common::LockGuard lock(m_mutex);

	return (req_id.GetType() == Id::Type::Request && req_id.GetId() >= 0 &&
	        static_cast<size_t>(req_id.GetId()) < m_requests.size() &&
	        m_requests[req_id.GetId()].used);
}

Network::HttpBase* Network::FindHttpBase(Id id, bool include_request) {
	const auto index = id.GetId();

	switch (id.GetType()) {
		case Id::Type::Template:
			if (index >= 0 && static_cast<size_t>(index) < m_templates.size() &&
			    m_templates[index].used) {
				return &m_templates[index];
			}
			break;
		case Id::Type::Connection:
			if (index >= 0 && static_cast<size_t>(index) < m_connections.size() &&
			    m_connections[index].used) {
				return &m_connections[index];
			}
			break;
		case Id::Type::Request:
			if (include_request && index >= 0 && static_cast<size_t>(index) < m_requests.size() &&
			    m_requests[index].used) {
				return &m_requests[index];
			}
			break;
		default: break;
	}

	return nullptr;
}

bool Network::HttpTerm(Id http_ctx_id) {
	Common::LockGuard lock(m_mutex);

	if (HttpValid(http_ctx_id)) {
		m_http[http_ctx_id.GetId()].used = false;

		return true;
	}

	return false;
}

Network::Id Network::HttpCreateTemplate(Id http_ctx_id, const char* user_agent, int http_ver,
                                        bool is_auto_proxy_conf) {
	Common::LockGuard lock(m_mutex);

	if (HttpValid(http_ctx_id)) {
		HttpTemplate tn {};
		tn.used               = true;
		tn.http_ver           = http_ver;
		tn.user_agent         = std::string(user_agent);
		tn.is_auto_proxy_conf = is_auto_proxy_conf;
		tn.http_ctx_id        = http_ctx_id.GetId();
		tn.nonblock           = false;

		int index = 0;
		for (auto& t: m_templates) {
			if (!t.used) {
				t = tn;
				return Id::Create(index, Id::Type::Template);
			}
			index++;
		}

		if (index < Id::MAX_ID) {
			m_templates.push_back(tn);
			return Id::Create(index, Id::Type::Template);
		}
	}

	return Id::Invalid();
}

Network::Id Network::HttpCreateConnectionWithURL(Id tmpl_id, const char* url,
                                                 bool enable_keep_alive) {
	Common::LockGuard lock(m_mutex);

	if (url != nullptr && url[0] != '\0' && HttpValidTemplate(tmpl_id)) {
		HttpConnection cn(m_templates[tmpl_id.GetId()]);
		cn.used              = true;
		cn.enable_keep_alive = enable_keep_alive;
		cn.url               = std::string(url);
		// cn.tmpl_id           = tmpl_id.ToInt();

		int index = 0;
		for (auto& t: m_connections) {
			if (!t.used) {
				t = cn;
				return Id::Create(index, Id::Type::Connection);
			}
			index++;
		}

		if (index < Id::MAX_ID) {
			m_connections.push_back(cn);
			return Id::Create(index, Id::Type::Connection);
		}
	}

	return Id::Invalid();
}

Network::Id Network::HttpCreateConnection(Id tmpl_id, const char* server_name, const char* scheme,
                                          uint16_t port, bool enable_keep_alive) {
	Common::LockGuard lock(m_mutex);

	if (server_name != nullptr && scheme != nullptr && HttpValidTemplate(tmpl_id)) {
		HttpConnection cn(m_templates[tmpl_id.GetId()]);
		cn.used              = true;
		cn.enable_keep_alive = enable_keep_alive;
		cn.url               = std::string(scheme) + "://" + std::string(server_name);
		if (port != 0) {
			cn.url = cn.url + fmt::format(":{}", static_cast<unsigned>(port));
		}

		int index = 0;
		for (auto& t: m_connections) {
			if (!t.used) {
				t = cn;
				return Id::Create(index, Id::Type::Connection);
			}
			index++;
		}

		if (index < Id::MAX_ID) {
			m_connections.push_back(cn);
			return Id::Create(index, Id::Type::Connection);
		}
	}

	return Id::Invalid();
}

bool Network::HttpDeleteConnection(Id conn_id) {
	Common::LockGuard lock(m_mutex);

	if (HttpValidConnection(conn_id)) {
		m_connections[conn_id.GetId()].used = false;

		return true;
	}

	return false;
}

Network::Id Network::HttpCreateRequestWithURL2(Id conn_id, const char* method, const char* url,
                                               uint64_t content_length) {
	Common::LockGuard lock(m_mutex);

	if (method != nullptr && url != nullptr && url[0] != '\0' && HttpValidConnection(conn_id)) {
		HttpRequest cn(m_connections[conn_id.GetId()]);
		cn.used   = true;
		cn.method = std::string(method);
		cn.url    = std::string(url);
		// cn.conn_id        = conn_id.ToInt();
		cn.content_length = content_length;

		int index = 0;
		for (auto& t: m_requests) {
			if (!t.used) {
				t = cn;
				return Id::Create(index, Id::Type::Request);
			}
			index++;
		}

		if (index < Id::MAX_ID) {
			m_requests.push_back(cn);
			return Id::Create(index, Id::Type::Request);
		}
	}

	return Id::Invalid();
}

bool Network::HttpDeleteRequest(Id req_id) {
	Common::LockGuard lock(m_mutex);

	if (HttpValidRequest(req_id)) {
		m_requests[req_id.GetId()].used = false;

		return true;
	}

	return false;
}

bool Network::HttpSetRequestContentLength(Id req_id, uint64_t content_length) {
	Common::LockGuard lock(m_mutex);

	if (HttpValidRequest(req_id)) {
		m_requests[req_id.GetId()].content_length = content_length;
		return true;
	}

	return false;
}

bool Network::HttpMarkRequestSent(Id req_id, int result) {
	Common::LockGuard lock(m_mutex);

	if (req_id.GetType() == Id::Type::Request && req_id.GetId() >= 0 &&
	    static_cast<size_t>(req_id.GetId()) < m_requests.size() &&
	    m_requests[req_id.GetId()].used) {
		auto& request          = m_requests[req_id.GetId()];
		request.send_attempted = true;
		request.send_result    = result;
		request.status_code    = (result == OK ? 200 : 0);
		request.response_headers.clear();
		return true;
	}

	return false;
}

bool Network::HttpGetRequestResponse(Id req_id, int* send_result, int* status_code,
                                     const char** headers, size_t* headers_size,
                                     uint64_t* content_length) {
	Common::LockGuard lock(m_mutex);

	if (req_id.GetType() != Id::Type::Request || req_id.GetId() < 0 ||
	    static_cast<size_t>(req_id.GetId()) >= m_requests.size() ||
	    !m_requests[req_id.GetId()].used) {
		return false;
	}

	const auto& request = m_requests[req_id.GetId()];

	if (send_result != nullptr) {
		*send_result = (request.send_attempted ? request.send_result : HTTP_ERROR_BEFORE_SEND);
	}
	if (status_code != nullptr) {
		*status_code = request.status_code;
	}
	if (headers != nullptr) {
		*headers = request.response_headers.c_str();
	}
	if (headers_size != nullptr) {
		*headers_size = request.response_headers.size();
	}
	if (content_length != nullptr) {
		*content_length = 0;
	}

	return true;
}

bool Network::HttpDeleteTemplate(Id tmpl_id) {
	Common::LockGuard lock(m_mutex);

	if (HttpValidTemplate(tmpl_id)) {
		m_templates[tmpl_id.GetId()].used = false;

		return true;
	}

	return false;
}

bool Network::HttpSetNonblock(Id id, bool enable) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->nonblock = enable;
		return true;
	}

	return false;
}

bool Network::HttpsSetSslCallback(Id id, HttpsCallback cbfunc, void* user_arg) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->ssl_cbfunc   = cbfunc;
		base->ssl_user_arg = user_arg;
		return true;
	}

	return false;
}

bool Network::HttpsSetMinSslVersion(Id id, uint32_t ssl_version) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->min_ssl_version = ssl_version;
		return true;
	}

	return false;
}

bool Network::HttpsDisableOption(Id id, uint32_t ssl_flags) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->ssl_flags &= ~ssl_flags;
		return true;
	}

	return false;
}

bool Network::HttpAddRequestHeader(Id id, const char* name, const char* value, bool add) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		HttpHeader nh({std::string(name), std::string(value)});
		if (add) {
			base->headers.push_back(nh);
		} else {
			for (auto& h: base->headers) {
				if (h.name == nh.name) {
					h.value = nh.value;
				}
			}
		}
		return true;
	}

	return false;
}

bool Network::HttpSetResolveTimeOut(Id id, uint32_t usec) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, false);

	if (base != nullptr) {
		base->resolve_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetResolveRetry(Id id, int32_t retry) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, false);

	if (base != nullptr) {
		base->resolve_retry = retry;
		return true;
	}

	return false;
}

bool Network::HttpSetConnectTimeOut(Id id, uint32_t usec) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->connect_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetSendTimeOut(Id id, uint32_t usec) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->send_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetRecvTimeOut(Id id, uint32_t usec) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->recv_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetAutoRedirect(Id id, int enable) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->auto_redirect = (enable != 0);
		return true;
	}

	return false;
}

bool Network::HttpSetAuthEnabled(Id id, int enable) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->auth_enabled = (enable != 0);
		return true;
	}

	return false;
}

namespace Net {

LIB_NAME("Net", "Net");

struct NetEtherAddr {
	uint8_t data[6] = {0};
};

struct SocketSlot {
	bool used = false;
#if defined(_WIN32)
	SOCKET socket = INVALID_SOCKET;
#else
	int socket = -1;
#endif
};

struct NetTimeval {
	int64_t tv_sec;
	int64_t tv_usec;
};

static constexpr int                         SOCKET_FD_MIN = 128;
static constexpr int                         SOCKET_FD_MAX = 1024;
static Common::Mutex                         g_socket_mutex;
static std::array<SocketSlot, SOCKET_FD_MAX> g_sockets;
static std::atomic_bool                      g_winsock_initialized = false;

constexpr int      EPOLL_ID_BASE = SOCKET_FD_MAX;
constexpr uint32_t EPOLL_IN      = 0x00000001;
constexpr uint32_t EPOLL_OUT     = 0x00000002;
constexpr uint32_t EPOLL_ERR     = 0x00000008;

struct EpollRegistration {
	int           id = -1;
	NetEpollEvent event {};
};

struct EpollSlot {
	bool                           used = false;
	std::string                    name;
	std::vector<EpollRegistration> registrations;
};

static std::mutex                g_epoll_mutex;
static std::condition_variable   g_epoll_changed;
static std::array<EpollSlot, 32> g_epolls;

static EpollSlot* GetEpollSlot(int eid) {
	const auto index = eid - EPOLL_ID_BASE;
	if (index < 0 || index >= static_cast<int>(g_epolls.size())) {
		return nullptr;
	}
	auto& slot = g_epolls[static_cast<size_t>(index)];
	return slot.used ? &slot : nullptr;
}

static void RemoveSocketFromEpolls(int id) {
	std::lock_guard lock(g_epoll_mutex);
	for (auto& epoll: g_epolls) {
		if (!epoll.used) {
			continue;
		}
		std::erase_if(epoll.registrations,
		              [id](const auto& registration) { return registration.id == id; });
	}
	g_epoll_changed.notify_all();
}

static bool EnsureSocketBackend() {
#if defined(_WIN32)
	if (!g_winsock_initialized.load()) {
		WSADATA data {};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
			return false;
		}
		g_winsock_initialized = true;
	}
#endif
	return true;
}

static int SetPosixSocketError(int error) {
	int posix_error = Posix::POSIX_EIO;
#if defined(_WIN32)
	switch (error) {
		case WSAEACCES: posix_error = Posix::POSIX_EACCES; break;
		case WSAEADDRINUSE: posix_error = Posix::POSIX_EADDRINUSE; break;
		case WSAEADDRNOTAVAIL: posix_error = Posix::POSIX_EADDRNOTAVAIL; break;
		case WSAEAFNOSUPPORT: posix_error = Posix::POSIX_EAFNOSUPPORT; break;
		case WSAEFAULT: posix_error = Posix::POSIX_EFAULT; break;
		case WSAEINTR: posix_error = Posix::POSIX_EINTR; break;
		case WSAEINVAL: posix_error = Posix::POSIX_EINVAL; break;
		case WSAEISCONN: posix_error = Posix::POSIX_EISCONN; break;
		case WSAEMFILE: posix_error = Posix::POSIX_EMFILE; break;
		case WSAEMSGSIZE: posix_error = Posix::POSIX_EMSGSIZE; break;
		case WSAENOBUFS: posix_error = Posix::POSIX_ENOBUFS; break;
		case WSAENETDOWN: posix_error = Posix::POSIX_ENETDOWN; break;
		case WSAENETRESET: posix_error = Posix::POSIX_ENETRESET; break;
		case WSAENETUNREACH: posix_error = Posix::POSIX_ENETUNREACH; break;
		case WSAENOTCONN: posix_error = Posix::POSIX_ENOTCONN; break;
		case WSAENOTSOCK: posix_error = Posix::POSIX_ENOTSOCK; break;
		case WSAEOPNOTSUPP: posix_error = Posix::POSIX_EOPNOTSUPP; break;
		case WSAEPROTONOSUPPORT: posix_error = Posix::POSIX_EPROTONOSUPPORT; break;
		case WSAESHUTDOWN: posix_error = Posix::POSIX_ESHUTDOWN; break;
		case WSAETIMEDOUT: posix_error = Posix::POSIX_ETIMEDOUT; break;
		case WSAEWOULDBLOCK: posix_error = Posix::POSIX_EWOULDBLOCK; break;
		case WSAECONNABORTED: posix_error = Posix::POSIX_ECONNABORTED; break;
		case WSAECONNREFUSED: posix_error = Posix::POSIX_ECONNREFUSED; break;
		case WSAECONNRESET: posix_error = Posix::POSIX_ECONNRESET; break;
		case WSAEDESTADDRREQ: posix_error = Posix::POSIX_EDESTADDRREQ; break;
		case WSAEHOSTUNREACH: posix_error = Posix::POSIX_EHOSTUNREACH; break;
		case WSAEINPROGRESS: posix_error = Posix::POSIX_EINPROGRESS; break;
		case WSAEALREADY: posix_error = Posix::POSIX_EALREADY; break;
		default: break;
	}
#else
	posix_error = error;
#endif
	*Posix::GetErrorAddr() = posix_error;
	return -1;
}

static int SetPosixSocketError() {
#if defined(_WIN32)
	return SetPosixSocketError(WSAGetLastError());
#else
	return SetPosixSocketError(errno);
#endif
}

static int ConvertFamily(int family) {
	switch (family) {
		case 2: return AF_INET;
		case 28: return AF_INET6;
		default: return -1;
	}
}

static int ConvertSocketOptionLevel(int level) {
	return (level == 0xffff ? SOL_SOCKET : level);
}

static int ConvertMessageFlags(int flags) {
#if defined(_WIN32)
	constexpr int guest_msg_peek      = 0x00000002;
	constexpr int guest_msg_dontroute = 0x00000004;
	constexpr int guest_msg_waitall   = 0x00000040;
	constexpr int guest_msg_dontwait  = 0x00000080;
	constexpr int guest_msg_nosignal  = 0x00020000;

	int host_flags = 0;
	if ((flags & guest_msg_peek) != 0) {
		host_flags |= MSG_PEEK;
	}
	if ((flags & guest_msg_dontroute) != 0) {
		host_flags |= MSG_DONTROUTE;
	}
	if ((flags & guest_msg_waitall) != 0) {
		host_flags |= MSG_WAITALL;
	}

	flags &= ~(guest_msg_peek | guest_msg_dontroute | guest_msg_waitall | guest_msg_dontwait |
	           guest_msg_nosignal);
	if (flags != 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP;
		return -1;
	}

	return host_flags;
#else
	return flags;
#endif
}

#if defined(_WIN32)
static int ConvertGuestSockaddr(const void* addr, uint32_t addrlen, sockaddr_storage* out,
                                int* out_len) {
	EXIT_IF(out == nullptr);
	EXIT_IF(out_len == nullptr);

	if (addr == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}

	const auto* bytes = static_cast<const uint8_t*>(addr);
	if (addrlen < 2) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	const int family = ConvertFamily(bytes[1]);
	if (family != AF_INET) {
		*Posix::GetErrorAddr() = (family < 0 ? Posix::POSIX_EAFNOSUPPORT : Posix::POSIX_EOPNOTSUPP);
		return -1;
	}

	if (addrlen < 8) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	std::memset(out, 0, sizeof(*out));
	auto* in       = reinterpret_cast<sockaddr_in*>(out);
	in->sin_family = AF_INET;
	std::memcpy(&in->sin_port, bytes + 2, sizeof(in->sin_port));
	std::memcpy(&in->sin_addr, bytes + 4, sizeof(in->sin_addr));
	*out_len = sizeof(sockaddr_in);
	return 0;
}

static int ConvertHostSockaddr(const sockaddr_storage* addr, int addrlen, void* out,
                               uint32_t* out_len) {
	EXIT_IF(addr == nullptr);
	EXIT_IF(out_len == nullptr);

	if (out == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}

	if (addr->ss_family != AF_INET || addrlen < static_cast<int>(sizeof(sockaddr_in))) {
		*Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP;
		return -1;
	}

	static constexpr uint32_t guest_addrlen = 16;
	if (*out_len < guest_addrlen) {
		*out_len               = guest_addrlen;
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	auto*       bytes = static_cast<uint8_t*>(out);
	const auto* in    = reinterpret_cast<const sockaddr_in*>(addr);
	std::memset(bytes, 0, guest_addrlen);
	bytes[0] = static_cast<uint8_t>(guest_addrlen);
	bytes[1] = 2;
	std::memcpy(bytes + 2, &in->sin_port, sizeof(in->sin_port));
	std::memcpy(bytes + 4, &in->sin_addr, sizeof(in->sin_addr));
	*out_len = guest_addrlen;
	return 0;
}
#endif

static bool GetSocketBackend(int guest_fd, SOCKET* out) {
	EXIT_IF(out == nullptr);

	if (guest_fd < 0 || guest_fd >= SOCKET_FD_MAX) {
		return false;
	}

	Common::LockGuard lock(g_socket_mutex);
	if (!g_sockets[static_cast<size_t>(guest_fd)].used) {
		return false;
	}

	*out = g_sockets[static_cast<size_t>(guest_fd)].socket;
	return true;
}

bool KYTY_SYSV_ABI IsSocket(int s) {
	if (s < 0 || s >= SOCKET_FD_MAX) {
		return false;
	}

	Common::LockGuard lock(g_socket_mutex);
	return g_sockets[static_cast<size_t>(s)].used;
}

static bool TakeSocketBackend(int guest_fd, SOCKET* out) {
	EXIT_IF(out == nullptr);

	if (guest_fd < 0 || guest_fd >= SOCKET_FD_MAX) {
		return false;
	}

	Common::LockGuard lock(g_socket_mutex);
	auto&             slot = g_sockets[static_cast<size_t>(guest_fd)];
	if (!slot.used) {
		return false;
	}

	*out = slot.socket;
	slot = {};
	return true;
}

static int AllocSocketFd(SOCKET socket) {
	Common::LockGuard lock(g_socket_mutex);
	for (int fd = SOCKET_FD_MIN; fd < SOCKET_FD_MAX; fd++) {
		auto& slot = g_sockets[static_cast<size_t>(fd)];
		if (!slot.used) {
			slot.used   = true;
			slot.socket = socket;
			return fd;
		}
	}

	return -1;
}

static bool GuestFdIsSet(const void* fds, int fd) {
	if (fds == nullptr || fd < 0 || fd >= SOCKET_FD_MAX) {
		return false;
	}

	const auto* words = static_cast<const uint64_t*>(fds);
	return (words[fd / 64] & (uint64_t {1} << (fd % 64))) != 0;
}

static void GuestFdZero(void* fds, int nfds) {
	if (fds == nullptr || nfds <= 0) {
		return;
	}

	std::memset(fds, 0, static_cast<size_t>((nfds + 63) / 64) * sizeof(uint64_t));
}

static void GuestFdSet(void* fds, int fd) {
	if (fds == nullptr || fd < 0 || fd >= SOCKET_FD_MAX) {
		return;
	}

	auto* words = static_cast<uint64_t*>(fds);
	words[fd / 64] |= (uint64_t {1} << (fd % 64));
}

int KYTY_SYSV_ABI NetInit() {
	PRINT_NAME();

	EnsureSocketBackend();

	return OK;
}

int KYTY_SYSV_ABI NetTerm() {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NetPoolCreate(const char* name, int size, int flags) {
	PRINT_NAME();

	LOGF("\t name = %s\n"
	     "\t size = %d\n"
	     "\t flags = %d\n",
	     name, size, flags);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(size == 0);

	int id = g_net->PoolCreate(name, size);

	if (id < 0) {
		return NET_ERROR_ENFILE;
	}

	return id;
}

int KYTY_SYSV_ABI NetPoolDestroy(int memid) {
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->PoolDestroy(memid)) {
		return NET_ERROR_EBADF;
	}

	return OK;
}

int KYTY_SYSV_ABI NetResolverCreate(const char* name, int memid, int flags) {
	PRINT_NAME();

	LOGF("\t name  = %s\n"
	     "\t memid = %d\n"
	     "\t flags = %d\n",
	     name != nullptr ? name : "<null>", memid, flags);

	if (name == nullptr || flags != 0) {
		return NET_ERROR_EINVAL;
	}

	EXIT_IF(g_net == nullptr);

	const int id = g_net->ResolverCreate(name, memid);
	if (id < 0) {
		return NET_ERROR_RESOLVER_ENOSPACE;
	}

	return id;
}

int KYTY_SYSV_ABI NetResolverStartNtoa(int rid, const char* hostname, void* addr, int timeout,
                                       int retry, int flags) {
	PRINT_NAME();

	LOGF("\t rid      = %d\n"
	     "\t hostname = %s\n"
	     "\t timeout  = %d\n"
	     "\t retry    = %d\n"
	     "\t flags    = 0x%08x\n",
	     rid, hostname != nullptr ? hostname : "<null>", timeout, retry, flags);

	(void)timeout;
	(void)retry;

	if (hostname == nullptr || addr == nullptr || (flags & ~0x00010000) != 0) {
		return NET_ERROR_EINVAL;
	}

	EXIT_IF(g_net == nullptr);
	if (!g_net->ResolverValid(rid)) {
		return NET_ERROR_EBADF;
	}

	if ((flags & 0x00010000) == 0 && NetInetPton(2, hostname, addr) == 1) {
		return OK;
	}

#if defined(_WIN32)
	if (!EnsureSocketBackend()) {
		return NET_ERROR_ENETDOWN;
	}

	addrinfo hints {};
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo* result = nullptr;
	const int ret    = getaddrinfo(hostname, nullptr, &hints, &result);
	if (ret != 0 || result == nullptr) {
		if (result != nullptr) {
			freeaddrinfo(result);
		}
		return ret == EAI_NONAME ? NET_ERROR_RESOLVER_ENOHOST : NET_ERROR_RESOLVER_EINTERNAL;
	}

	for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
		if (ai->ai_family == AF_INET && ai->ai_addr != nullptr &&
		    ai->ai_addrlen >= sizeof(sockaddr_in)) {
			const auto* in = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
			std::memcpy(addr, &in->sin_addr, sizeof(in->sin_addr));
			freeaddrinfo(result);
			return OK;
		}
	}

	freeaddrinfo(result);
	return NET_ERROR_RESOLVER_ENORECORD;
#else
	return NET_ERROR_RESOLVER_ENOTIMPLEMENTED;
#endif
}

int KYTY_SYSV_ABI NetInetPton(int af, const char* src, void* dst) {
	PRINT_NAME();

	if (af != 2) {
		return NET_ERROR_EAFNOSUPPORT;
	}
	if (src == nullptr || dst == nullptr) {
		return NET_ERROR_EINVAL;
	}

	LOGF("\t src = %.16s\n", src);

	uint32_t octets[4] = {};
	char     extra     = '\0';
	if (sscanf(src, "%u.%u.%u.%u%c", &octets[0], &octets[1], &octets[2], &octets[3], &extra) != 4) {
		return 0;
	}
	for (auto octet: octets) {
		if (octet > 255) {
			return 0;
		}
	}

	auto* out = static_cast<uint8_t*>(dst);
	out[0]    = static_cast<uint8_t>(octets[0]);
	out[1]    = static_cast<uint8_t>(octets[1]);
	out[2]    = static_cast<uint8_t>(octets[2]);
	out[3]    = static_cast<uint8_t>(octets[3]);

	return 1;
}

const char* KYTY_SYSV_ABI NetInetNtop(int af, const void* src, char* dst, uint32_t size) {
	PRINT_NAME();

	if (src == nullptr || dst == nullptr || size == 0) {
		return nullptr;
	}

	if (af != 2 && af != 28) {
		return nullptr;
	}

#if defined(_WIN32)
	const int win_af = (af == 28 ? AF_INET6 : AF_INET);
	if (::InetNtopA(win_af, const_cast<void*>(src), dst, size) == nullptr) {
		return nullptr;
	}
#else
	if (::inet_ntop(af, src, dst, size) == nullptr) {
		return nullptr;
	}
#endif

	return dst;
}

int KYTY_SYSV_ABI NetEtherNtostr(const NetEtherAddr* n, char* str, size_t len) {
	PRINT_NAME();

	NetEtherAddr zero {};

	EXIT_NOT_IMPLEMENTED(len != 18);
	EXIT_NOT_IMPLEMENTED(n == nullptr);
	EXIT_NOT_IMPLEMENTED(str == nullptr);
	EXIT_NOT_IMPLEMENTED(memcmp(n->data, zero.data, sizeof(zero.data)) != 0);

	strcpy(str, "00:00:00:00:00:00"); // NOLINT

	return OK;
}

int KYTY_SYSV_ABI NetGetMacAddress(NetEtherAddr* addr, int flags) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	EXIT_NOT_IMPLEMENTED(flags != 0);

	memset(addr->data, 0, sizeof(addr->data));

	return OK;
}

int KYTY_SYSV_ABI NetGetSockInfo(int s, void* info, int n, int flags) {
	PRINT_NAME();

	LOGF("\t s     = %d\n"
	     "\t info  = 0x%016" PRIx64 "\n"
	     "\t n     = %d\n"
	     "\t flags = %d\n",
	     s, reinterpret_cast<uint64_t>(info), n, flags);

	return OK;
}

int KYTY_SYSV_ABI EpollCreate(const char* name, int flags) {
	if (name == nullptr || flags != 0) {
		return SetPosixSocketError(Posix::POSIX_EINVAL);
	}

	std::lock_guard lock(g_epoll_mutex);
	for (size_t index = 0; index < g_epolls.size(); index++) {
		auto& slot = g_epolls[index];
		if (!slot.used) {
			slot.used = true;
			slot.name = name;
			slot.registrations.clear();
			return EPOLL_ID_BASE + static_cast<int>(index);
		}
	}

	return SetPosixSocketError(Posix::POSIX_EMFILE);
}

int KYTY_SYSV_ABI EpollControl(int eid, int op, int id, const NetEpollEvent* event) {
	constexpr int EPOLL_CTL_ADD = 1;
	constexpr int EPOLL_CTL_MOD = 2;
	constexpr int EPOLL_CTL_DEL = 3;

	if ((op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) && event == nullptr) {
		return SetPosixSocketError(Posix::POSIX_EINVAL);
	}
	if (op == EPOLL_CTL_DEL && event != nullptr) {
		return SetPosixSocketError(Posix::POSIX_EINVAL);
	}
	if (op < EPOLL_CTL_ADD || op > EPOLL_CTL_DEL) {
		return SetPosixSocketError(Posix::POSIX_EINVAL);
	}
	if (!IsSocket(id)) {
		return SetPosixSocketError(Posix::POSIX_EBADF);
	}

	std::lock_guard lock(g_epoll_mutex);
	auto*           slot = GetEpollSlot(eid);
	if (slot == nullptr) {
		return SetPosixSocketError(Posix::POSIX_EBADF);
	}

	auto registration = std::find_if(slot->registrations.begin(), slot->registrations.end(),
	                                 [id](const auto& value) { return value.id == id; });
	switch (op) {
		case EPOLL_CTL_ADD:
			if (registration != slot->registrations.end()) {
				return SetPosixSocketError(Posix::POSIX_EEXIST);
			}
			slot->registrations.push_back({id, *event});
			break;
		case EPOLL_CTL_MOD:
			if (registration == slot->registrations.end()) {
				return SetPosixSocketError(Posix::POSIX_ENOENT);
			}
			registration->event = *event;
			break;
		case EPOLL_CTL_DEL:
			if (registration == slot->registrations.end()) {
				return SetPosixSocketError(Posix::POSIX_ENOENT);
			}
			slot->registrations.erase(registration);
			break;
		default: break;
	}

	g_epoll_changed.notify_all();
	return 0;
}

int KYTY_SYSV_ABI EpollWait(int eid, NetEpollEvent* events, int maxevents, int timeout) {
	if (events == nullptr) {
		return SetPosixSocketError(Posix::POSIX_EFAULT);
	}
	if (maxevents <= 0 || timeout < -1) {
		return SetPosixSocketError(Posix::POSIX_EINVAL);
	}

	std::vector<EpollRegistration> registrations;
	{
		std::unique_lock lock(g_epoll_mutex);
		auto*            slot = GetEpollSlot(eid);
		if (slot == nullptr) {
			return SetPosixSocketError(Posix::POSIX_EBADF);
		}

		if (slot->registrations.empty() && timeout != 0) {
			auto ready = [eid] {
				auto* current = GetEpollSlot(eid);
				return current == nullptr || !current->registrations.empty();
			};
			if (timeout < 0) {
				g_epoll_changed.wait(lock, ready);
			} else {
				g_epoll_changed.wait_for(lock, std::chrono::microseconds(timeout), ready);
			}
			slot = GetEpollSlot(eid);
			if (slot == nullptr) {
				return SetPosixSocketError(Posix::POSIX_EBADF);
			}
		}
		registrations = slot->registrations;
	}

	if (registrations.empty()) {
		return 0;
	}

#if defined(_WIN32)
	struct HostRegistration {
		EpollRegistration guest;
		SOCKET            socket = INVALID_SOCKET;
	};

	fd_set host_read {};
	fd_set host_write {};
	fd_set host_except {};
	FD_ZERO(&host_read);
	FD_ZERO(&host_write);
	FD_ZERO(&host_except);

	std::vector<HostRegistration> host_registrations;
	host_registrations.reserve(registrations.size());
	for (const auto& registration: registrations) {
		SOCKET socket = INVALID_SOCKET;
		if (!GetSocketBackend(registration.id, &socket)) {
			continue;
		}
		if (host_registrations.size() >= FD_SETSIZE) {
			return SetPosixSocketError(Posix::POSIX_EINVAL);
		}
		if ((registration.event.events & EPOLL_IN) != 0) {
			FD_SET(socket, &host_read);
		}
		if ((registration.event.events & EPOLL_OUT) != 0) {
			FD_SET(socket, &host_write);
		}
		FD_SET(socket, &host_except);
		host_registrations.push_back({registration, socket});
	}

	if (host_registrations.empty()) {
		return 0;
	}

	timeval  host_timeout {};
	timeval* host_timeout_ptr = nullptr;
	if (timeout >= 0) {
		host_timeout.tv_sec  = timeout / 1000000;
		host_timeout.tv_usec = timeout % 1000000;
		host_timeout_ptr     = &host_timeout;
	}

	const int result = ::select(0, &host_read, &host_write, &host_except, host_timeout_ptr);
	if (result == SOCKET_ERROR) {
		return SetPosixSocketError();
	}
	if (result == 0) {
		return 0;
	}

	int count = 0;
	for (const auto& registration: host_registrations) {
		uint32_t ready = 0;
		if (FD_ISSET(registration.socket, &host_read)) {
			ready |= EPOLL_IN;
		}
		if (FD_ISSET(registration.socket, &host_write)) {
			ready |= EPOLL_OUT;
		}
		if (FD_ISSET(registration.socket, &host_except)) {
			ready |= EPOLL_ERR;
		}
		if (ready == 0) {
			continue;
		}

		auto& output    = events[count++];
		output          = registration.guest.event;
		output.events   = ready;
		output.reserved = 0;
		output.ident    = static_cast<uint64_t>(registration.guest.id);
		if (count == maxevents) {
			break;
		}
	}
	return count;
#else
	(void)timeout;
	return SetPosixSocketError(Posix::POSIX_ENOSYS);
#endif
}

int KYTY_SYSV_ABI EpollDestroy(int eid) {
	std::lock_guard lock(g_epoll_mutex);
	auto*           slot = GetEpollSlot(eid);
	if (slot == nullptr) {
		return SetPosixSocketError(Posix::POSIX_EBADF);
	}

	slot->used = false;
	slot->name.clear();
	slot->registrations.clear();
	g_epoll_changed.notify_all();
	return 0;
}

int KYTY_SYSV_ABI SocketClose(int s) {
	PRINT_NAME();

	LOGF("\t s = %d\n", s);

	SOCKET socket = INVALID_SOCKET;
	if (!TakeSocketBackend(s, &socket)) {
		return NET_ERROR_EBADF;
	}
	RemoveSocketFromEpolls(s);

#if defined(_WIN32)
	if (closesocket(socket) == SOCKET_ERROR) {
		return NET_ERROR_EBADF;
	}

	return OK;
#else
	return NET_ERROR_ENOSYS;
#endif
}

int KYTY_SYSV_ABI Socket(int family, int type, int protocol) {
	PRINT_NAME();

	LOGF("\t family   = %d\n"
	     "\t type     = %d\n"
	     "\t protocol = %d\n",
	     family, type, protocol);

	if (!EnsureSocketBackend()) {
		return SetPosixSocketError(Posix::POSIX_ENETDOWN);
	}

	const int host_family = ConvertFamily(family);
	if (host_family < 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EAFNOSUPPORT;
		return -1;
	}

#if defined(_WIN32)
	SOCKET socket = ::socket(host_family, type, protocol);
	if (socket == INVALID_SOCKET) {
		return SetPosixSocketError();
	}

	const int fd = AllocSocketFd(socket);
	if (fd < 0) {
		closesocket(socket);
		*Posix::GetErrorAddr() = Posix::POSIX_EMFILE;
		return -1;
	}

	LOGF("\t fd = %d\n", fd);
	return fd;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Bind(int s, const void* addr, uint32_t addrlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t addr    = 0x%016" PRIx64 "\n"
	     "\t addrlen = %" PRIu32 "\n",
	     s, reinterpret_cast<uint64_t>(addr), addrlen);

	SOCKET socket = INVALID_SOCKET;
	if (addr == nullptr || !GetSocketBackend(s, &socket)) {
		*Posix::GetErrorAddr() = (addr == nullptr ? Posix::POSIX_EFAULT : Posix::POSIX_EBADF);
		return -1;
	}

#if defined(_WIN32)
	sockaddr_storage host_addr {};
	int              host_addrlen = 0;
	if (ConvertGuestSockaddr(addr, addrlen, &host_addr, &host_addrlen) != 0) {
		return -1;
	}

	if (::bind(socket, reinterpret_cast<const sockaddr*>(&host_addr), host_addrlen) ==
	    SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	return 0;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Connect(int s, const void* addr, uint32_t addrlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t addr    = 0x%016" PRIx64 "\n"
	     "\t addrlen = %" PRIu32 "\n",
	     s, reinterpret_cast<uint64_t>(addr), addrlen);

	SOCKET socket = INVALID_SOCKET;
	if (addr == nullptr || !GetSocketBackend(s, &socket)) {
		*Posix::GetErrorAddr() = (addr == nullptr ? Posix::POSIX_EFAULT : Posix::POSIX_EBADF);
		return -1;
	}

#if defined(_WIN32)
	sockaddr_storage host_addr {};
	int              host_addrlen = 0;
	if (ConvertGuestSockaddr(addr, addrlen, &host_addr, &host_addrlen) != 0) {
		return -1;
	}

	if (::connect(socket, reinterpret_cast<const sockaddr*>(&host_addr), host_addrlen) ==
	    SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	return 0;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Listen(int s, int backlog) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t backlog = %d\n",
	     s, backlog);

	SOCKET socket = INVALID_SOCKET;
	if (!GetSocketBackend(s, &socket)) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

#if defined(_WIN32)
	if (::listen(socket, backlog) == SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	return 0;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Accept(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t addr    = 0x%016" PRIx64 "\n"
	     "\t addrlen = 0x%016" PRIx64 "\n",
	     s, reinterpret_cast<uint64_t>(addr), reinterpret_cast<uint64_t>(addrlen));

	SOCKET socket = INVALID_SOCKET;
	if (!GetSocketBackend(s, &socket)) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

#if defined(_WIN32)
	sockaddr_storage host_addr {};
	int              host_addrlen = sizeof(host_addr);
	SOCKET accepted = ::accept(socket, reinterpret_cast<sockaddr*>(&host_addr), &host_addrlen);
	if (accepted == INVALID_SOCKET) {
		return SetPosixSocketError();
	}

	const int fd = AllocSocketFd(accepted);
	if (fd < 0) {
		closesocket(accepted);
		*Posix::GetErrorAddr() = Posix::POSIX_EMFILE;
		return -1;
	}

	if (addr != nullptr || addrlen != nullptr) {
		if (addr == nullptr || addrlen == nullptr) {
			SocketClose(fd);
			*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
			return -1;
		}
		if (ConvertHostSockaddr(&host_addr, host_addrlen, addr, addrlen) != 0) {
			SocketClose(fd);
			return -1;
		}
	}

	return fd;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Shutdown(int s, int how) {
	PRINT_NAME();

	LOGF("\t s   = %d\n"
	     "\t how = %d\n",
	     s, how);

	SOCKET socket = INVALID_SOCKET;
	if (!GetSocketBackend(s, &socket)) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	if (how < 0 || how > 2) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

#if defined(_WIN32)
	if (::shutdown(socket, how) == SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	return 0;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Getsockname(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t addr    = 0x%016" PRIx64 "\n"
	     "\t addrlen = 0x%016" PRIx64 "\n",
	     s, reinterpret_cast<uint64_t>(addr), reinterpret_cast<uint64_t>(addrlen));

	SOCKET     socket    = INVALID_SOCKET;
	const bool socket_ok = GetSocketBackend(s, &socket);
	if (addr == nullptr || addrlen == nullptr || !socket_ok) {
		*Posix::GetErrorAddr() = (!socket_ok ? Posix::POSIX_EBADF : Posix::POSIX_EFAULT);
		return -1;
	}

#if defined(_WIN32)
	sockaddr_storage host_addr {};
	int              host_addrlen = sizeof(host_addr);
	if (::getsockname(socket, reinterpret_cast<sockaddr*>(&host_addr), &host_addrlen) ==
	    SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	return ConvertHostSockaddr(&host_addr, host_addrlen, addr, addrlen);
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Getsockopt(int s, int level, int optname, void* optval, uint32_t* optlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t level   = 0x%08" PRIx32 "\n"
	     "\t optname = 0x%08" PRIx32 "\n",
	     s, static_cast<uint32_t>(level), static_cast<uint32_t>(optname));

	SOCKET     socket    = INVALID_SOCKET;
	const bool socket_ok = GetSocketBackend(s, &socket);
	if (optval == nullptr || optlen == nullptr || !socket_ok) {
		*Posix::GetErrorAddr() = (!socket_ok ? Posix::POSIX_EBADF : Posix::POSIX_EFAULT);
		return -1;
	}

#if defined(_WIN32)
	int len = static_cast<int>(*optlen);
	if (::getsockopt(socket, ConvertSocketOptionLevel(level), optname, static_cast<char*>(optval),
	                 &len) == SOCKET_ERROR) {
		return SetPosixSocketError();
	}
	*optlen = static_cast<uint32_t>(len);
	return 0;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Setsockopt(int s, int level, int optname, const void* optval, uint32_t optlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t level   = 0x%08" PRIx32 "\n"
	     "\t optname = 0x%08" PRIx32 "\n"
	     "\t optlen  = %" PRIu32 "\n",
	     s, static_cast<uint32_t>(level), static_cast<uint32_t>(optname), optlen);

	SOCKET socket = INVALID_SOCKET;
	if (optval == nullptr || !GetSocketBackend(s, &socket)) {
		*Posix::GetErrorAddr() = (optval == nullptr ? Posix::POSIX_EFAULT : Posix::POSIX_EBADF);
		return -1;
	}

#if defined(_WIN32)
	constexpr int ORBIS_SO_NBIO = 0x1200;
	if (ConvertSocketOptionLevel(level) == SOL_SOCKET && optname == ORBIS_SO_NBIO &&
	    optlen >= sizeof(int)) {
		u_long enabled = (*static_cast<const int*>(optval) != 0 ? 1 : 0);
		if (ioctlsocket(socket, FIONBIO, &enabled) == SOCKET_ERROR) {
			return SetPosixSocketError();
		}
		return 0;
	}

	if (::setsockopt(socket, ConvertSocketOptionLevel(level), optname,
	                 static_cast<const char*>(optval), static_cast<int>(optlen)) == SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	return 0;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int64_t KYTY_SYSV_ABI Send(int s, const void* buf, uint64_t len, int flags) {
	PRINT_NAME();

	LOGF("\t s     = %d\n"
	     "\t buf   = 0x%016" PRIx64 "\n"
	     "\t len   = %" PRIu64 "\n"
	     "\t flags = 0x%08" PRIx32 "\n",
	     s, reinterpret_cast<uint64_t>(buf), len, static_cast<uint32_t>(flags));

	SOCKET socket = INVALID_SOCKET;
	if (buf == nullptr || !GetSocketBackend(s, &socket)) {
		*Posix::GetErrorAddr() = (buf == nullptr ? Posix::POSIX_EFAULT : Posix::POSIX_EBADF);
		return -1;
	}

#if defined(_WIN32)
	const int host_flags = ConvertMessageFlags(flags);
	if (host_flags < 0) {
		return -1;
	}

	const int host_len = static_cast<int>(len > 0x7fffffffu ? 0x7fffffffu : len);
	const int result   = ::send(socket, static_cast<const char*>(buf), host_len, host_flags);
	if (result == SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	return result;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int64_t KYTY_SYSV_ABI Recv(int s, void* buf, uint64_t len, int flags) {
	PRINT_NAME();

	LOGF("\t s     = %d\n"
	     "\t buf   = 0x%016" PRIx64 "\n"
	     "\t len   = %" PRIu64 "\n"
	     "\t flags = 0x%08" PRIx32 "\n",
	     s, reinterpret_cast<uint64_t>(buf), len, static_cast<uint32_t>(flags));

	SOCKET socket = INVALID_SOCKET;
	if (buf == nullptr || !GetSocketBackend(s, &socket)) {
		*Posix::GetErrorAddr() = (buf == nullptr ? Posix::POSIX_EFAULT : Posix::POSIX_EBADF);
		return -1;
	}

#if defined(_WIN32)
	const int host_flags = ConvertMessageFlags(flags);
	if (host_flags < 0) {
		return -1;
	}

	const int host_len = static_cast<int>(len > 0x7fffffffu ? 0x7fffffffu : len);
	const int result   = ::recv(socket, static_cast<char*>(buf), host_len, host_flags);
	if (result == SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	return result;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

int KYTY_SYSV_ABI Select(int nfds, void* readfds, void* writefds, void* exceptfds,
                         const void* timeout) {
	PRINT_NAME();

	if (nfds < 0 || nfds > SOCKET_FD_MAX) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	static std::atomic_uint32_t select_log_count = 0;
	const bool log_select = select_log_count.fetch_add(1, std::memory_order_relaxed) < 64;

	if (log_select) {
		const auto  read0   = (readfds != nullptr ? *static_cast<const uint64_t*>(readfds) : 0);
		const auto  write0  = (writefds != nullptr ? *static_cast<const uint64_t*>(writefds) : 0);
		const auto  except0 = (exceptfds != nullptr ? *static_cast<const uint64_t*>(exceptfds) : 0);
		const auto* guest_timeout = static_cast<const NetTimeval*>(timeout);
		LOGF("\t nfds       = %d\n"
		     "\t read       = 0x%016" PRIx64 " bits0=0x%016" PRIx64 "\n"
		     "\t write      = 0x%016" PRIx64 " bits0=0x%016" PRIx64 "\n"
		     "\t except     = 0x%016" PRIx64 " bits0=0x%016" PRIx64 "\n"
		     "\t timeout    = 0x%016" PRIx64,
		     nfds, reinterpret_cast<uint64_t>(readfds), read0, reinterpret_cast<uint64_t>(writefds),
		     write0, reinterpret_cast<uint64_t>(exceptfds), except0,
		     reinterpret_cast<uint64_t>(timeout));
		if (guest_timeout != nullptr) {
			LOGF(" sec=%" PRId64 " usec=%" PRId64, guest_timeout->tv_sec, guest_timeout->tv_usec);
		}
		LOGF("\n");
	}

#if defined(_WIN32)
	fd_set host_read {};
	fd_set host_write {};
	fd_set host_except {};
	FD_ZERO(&host_read);
	FD_ZERO(&host_write);
	FD_ZERO(&host_except);

	std::array<int, FD_SETSIZE> read_map {};
	std::array<int, FD_SETSIZE> write_map {};
	std::array<int, FD_SETSIZE> except_map {};
	int                         read_count   = 0;
	int                         write_count  = 0;
	int                         except_count = 0;

	for (int fd = 0; fd < nfds; fd++) {
		SOCKET socket = INVALID_SOCKET;
		if (!GetSocketBackend(fd, &socket)) {
			continue;
		}
		if (GuestFdIsSet(readfds, fd) && read_count < FD_SETSIZE) {
			FD_SET(socket, &host_read);
			read_map[static_cast<size_t>(read_count++)] = fd;
		}
		if (GuestFdIsSet(writefds, fd) && write_count < FD_SETSIZE) {
			FD_SET(socket, &host_write);
			write_map[static_cast<size_t>(write_count++)] = fd;
		}
		if (GuestFdIsSet(exceptfds, fd) && except_count < FD_SETSIZE) {
			FD_SET(socket, &host_except);
			except_map[static_cast<size_t>(except_count++)] = fd;
		}
	}

	timeval  host_timeout {};
	timeval* host_timeout_ptr = nullptr;
	if (timeout != nullptr) {
		const auto* guest_timeout = static_cast<const NetTimeval*>(timeout);
		host_timeout.tv_sec       = static_cast<long>(guest_timeout->tv_sec);
		host_timeout.tv_usec      = static_cast<long>(guest_timeout->tv_usec);
		host_timeout_ptr          = &host_timeout;
	}

	fd_set* read_ptr   = (read_count != 0 ? &host_read : nullptr);
	fd_set* write_ptr  = (write_count != 0 ? &host_write : nullptr);
	fd_set* except_ptr = (except_count != 0 ? &host_except : nullptr);
	if (read_ptr == nullptr && write_ptr == nullptr && except_ptr == nullptr) {
		if (host_timeout_ptr != nullptr) {
			const auto sleep_ms =
			    static_cast<DWORD>(host_timeout.tv_sec * 1000 + host_timeout.tv_usec / 1000);
			Sleep(sleep_ms);
		}
		GuestFdZero(readfds, nfds);
		GuestFdZero(writefds, nfds);
		GuestFdZero(exceptfds, nfds);
		return 0;
	}

	const int result = ::select(0, read_ptr, write_ptr, except_ptr, host_timeout_ptr);
	if (result == SOCKET_ERROR) {
		return SetPosixSocketError();
	}

	GuestFdZero(readfds, nfds);
	GuestFdZero(writefds, nfds);
	GuestFdZero(exceptfds, nfds);
	for (int i = 0; i < read_count; i++) {
		SOCKET socket = INVALID_SOCKET;
		if (GetSocketBackend(read_map[static_cast<size_t>(i)], &socket) &&
		    FD_ISSET(socket, &host_read)) {
			GuestFdSet(readfds, read_map[static_cast<size_t>(i)]);
		}
	}
	for (int i = 0; i < write_count; i++) {
		SOCKET socket = INVALID_SOCKET;
		if (GetSocketBackend(write_map[static_cast<size_t>(i)], &socket) &&
		    FD_ISSET(socket, &host_write)) {
			GuestFdSet(writefds, write_map[static_cast<size_t>(i)]);
		}
	}
	for (int i = 0; i < except_count; i++) {
		SOCKET socket = INVALID_SOCKET;
		if (GetSocketBackend(except_map[static_cast<size_t>(i)], &socket) &&
		    FD_ISSET(socket, &host_except)) {
			GuestFdSet(exceptfds, except_map[static_cast<size_t>(i)]);
		}
	}

	return result;
#else
	*Posix::GetErrorAddr() = Posix::POSIX_ENOSYS;
	return -1;
#endif
}

} // namespace Net

namespace Ssl {

LIB_NAME("Ssl", "Ssl");

struct SslData {
	char*  ptr  = nullptr;
	size_t size = 0;
};

struct SslCaCerts {
	SslData* cert_data     = nullptr;
	size_t   cert_data_num = 0;
	void*    pool          = nullptr;
};

int KYTY_SYSV_ABI SslInit(uint64_t pool_size) {
	PRINT_NAME();

	LOGF("\t size = %" PRIu64 "\n", pool_size);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(pool_size == 0);

	auto id = g_net->SslInit(pool_size);

	if (!id.IsValid()) {
		return SSL_ERROR_OUT_OF_SIZE;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI SslTerm(int ssl_ctx_id) {
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->SslTerm(Network::Id(ssl_ctx_id))) {
		return SSL_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI SslGetCaCerts(int ssl_ctx_id, void* ca_certs) {
	PRINT_NAME();

	LOGF("\t ssl_ctx_id = %d\n", ssl_ctx_id);

	if (ca_certs == nullptr) {
		return SSL_ERROR_INVALID_ARG;
	}

	EXIT_IF(g_net == nullptr);

	if (!g_net->SslValid(Network::Id(ssl_ctx_id))) {
		return SSL_ERROR_INVALID_ID;
	}

	auto* certs          = static_cast<SslCaCerts*>(ca_certs);
	certs->cert_data     = nullptr;
	certs->cert_data_num = 0;
	certs->pool          = nullptr;

	return SSL_ERROR_NOT_FOUND;
}

int KYTY_SYSV_ABI SslFreeCaCerts(int ssl_ctx_id, void* ca_certs) {
	PRINT_NAME();

	LOGF("\t ssl_ctx_id = %d\n", ssl_ctx_id);

	if (ca_certs == nullptr) {
		return SSL_ERROR_INVALID_ARG;
	}

	EXIT_IF(g_net == nullptr);

	if (!g_net->SslValid(Network::Id(ssl_ctx_id))) {
		return SSL_ERROR_INVALID_ID;
	}

	auto* certs          = static_cast<SslCaCerts*>(ca_certs);
	certs->cert_data     = nullptr;
	certs->cert_data_num = 0;
	certs->pool          = nullptr;

	return OK;
}

} // namespace Ssl

namespace Http {

struct HttpEpoll {
	Network::Id http_ctx_id = Network::Id(0);
	Network::Id request_id  = Network::Id(0);
	void*       user_arg    = nullptr;
};

struct HttpNBEvent {
	uint32_t events       = 0;
	uint32_t event_detail = 0;
	int      id           = 0;
	void*    user_arg     = nullptr;
};

LIB_NAME("Http", "Http");

static const char* HttpMethodToString(int method) {
	switch (method) {
		case 0: return "GET";
		case 1: return "POST";
		case 2: return "HEAD";
		case 3: return "OPTIONS";
		case 4: return "PUT";
		case 5: return "DELETE";
		case 6: return "TRACE";
		case 7: return "CONNECT";
		default: return nullptr;
	}
}

int KYTY_SYSV_ABI HttpInit(int memid, int ssl_ctx_id, uint64_t pool_size) {
	PRINT_NAME();

	LOGF("\t memid      = %d\n"
	     "\t ssl_ctx_id = %d\n"
	     "\t size       = %" PRIu64 "\n",
	     memid, ssl_ctx_id, pool_size);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(pool_size == 0);

	auto id = g_net->HttpInit(memid, Network::Id(ssl_ctx_id), pool_size);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpTerm(int http_ctx_id) {
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpTerm(Network::Id(http_ctx_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpCreateTemplate(int http_ctx_id, const char* user_agent, int http_ver,
                                     int is_auto_proxy_conf) {
	PRINT_NAME();

	LOGF("\t http_ctx_id        = %d\n"
	     "\t user_agent         = %s\n"
	     "\t http_ver           = %d\n"
	     "\t is_auto_proxy_conf = %d\n",
	     http_ctx_id, user_agent, http_ver, is_auto_proxy_conf);

	EXIT_IF(g_net == nullptr);

	auto id = g_net->HttpCreateTemplate(Network::Id(http_ctx_id), user_agent, http_ver,
	                                    is_auto_proxy_conf != 0);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpDeleteTemplate(int tmpl_id) {
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteTemplate(Network::Id(tmpl_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetNonblock(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n"
	     "\t enable = %d\n",
	     id, enable);

	if (!g_net->HttpSetNonblock(Network::Id(id), enable != 0)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpsSetSslCallback(int id, HttpsCallback cbfunc, void* user_arg) {
	PRINT_NAME();

	LOGF("\t id     = %d\n", id);

	if (!g_net->HttpsSetSslCallback(Network::Id(id), cbfunc, user_arg)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpsSetMinSslVersion(int id, uint32_t ssl_version) {
	PRINT_NAME();

	LOGF("\t id          = %d\n"
	     "\t ssl_version = %u\n",
	     id, ssl_version);

	if (!g_net->HttpsSetMinSslVersion(Network::Id(id), ssl_version)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpsDisableOption(int id, uint32_t ssl_flags) {
	PRINT_NAME();

	LOGF("\t id        = %d\n"
	     "\t ssl_flags = %u\n",
	     id, ssl_flags);

	if (!g_net->HttpsDisableOption(Network::Id(id), ssl_flags)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetResolveTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n"
	     "\t usec = %u\n",
	     id, usec);

	if (!g_net->HttpSetResolveTimeOut(Network::Id(id), usec)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetResolveRetry(int id, int32_t retry) {
	PRINT_NAME();

	LOGF("\t id    = %d\n"
	     "\t retry = %d\n",
	     id, retry);

	if (!g_net->HttpSetResolveRetry(Network::Id(id), retry)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetConnectTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n"
	     "\t usec = %u\n",
	     id, usec);

	if (!g_net->HttpSetConnectTimeOut(Network::Id(id), usec)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetSendTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n"
	     "\t usec = %u\n",
	     id, usec);

	if (!g_net->HttpSetSendTimeOut(Network::Id(id), usec)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetRecvTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n"
	     "\t usec = %u\n",
	     id, usec);

	if (!g_net->HttpSetRecvTimeOut(Network::Id(id), usec)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetAutoRedirect(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n"
	     "\t enable = %d\n",
	     id, enable);

	if (!g_net->HttpSetAutoRedirect(Network::Id(id), enable)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetAuthEnabled(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n"
	     "\t enable = %d\n",
	     id, enable);

	if (!g_net->HttpSetAuthEnabled(Network::Id(id), enable)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpAddRequestHeader(int id, const char* name, const char* value, uint32_t mode) {
	PRINT_NAME();

	LOGF("\t id    = %d\n"
	     "\t name  = %s\n"
	     "\t value = %s\n"
	     "\t mode  = %u\n",
	     id, name, value, mode);

	EXIT_NOT_IMPLEMENTED(mode != 0 && mode != 1);

	if (!g_net->HttpAddRequestHeader(Network::Id(id), name, value, mode == 1)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpCreateEpoll(int http_ctx_id, HttpEpollHandle* eh) {
	PRINT_NAME();

	LOGF("\t http_ctx_id = %d\n", http_ctx_id);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValid(Network::Id(http_ctx_id)));

	*eh = new HttpEpoll;

	(*eh)->http_ctx_id = Network::Id(http_ctx_id);

	return OK;
}

int KYTY_SYSV_ABI HttpDestroyEpoll(int http_ctx_id, HttpEpollHandle eh) {
	PRINT_NAME();

	LOGF("\t http_ctx_id = %d\n", http_ctx_id);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValid(Network::Id(http_ctx_id)));

	delete eh;

	return OK;
}

int KYTY_SYSV_ABI HttpSetEpoll(int id, HttpEpollHandle eh, void* user_arg) {
	PRINT_NAME();

	LOGF("\t id = %d\n", id);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValidRequest(Network::Id(id)));

	eh->request_id = Network::Id(id);
	eh->user_arg   = user_arg;

	return OK;
}

int KYTY_SYSV_ABI HttpUnsetEpoll(int id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", id);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValidRequest(Network::Id(id)));

	return OK;
}

int KYTY_SYSV_ABI HttpSendRequest(int request_id, const void* /*post_data*/, size_t /*size*/) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n", request_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpMarkRequestSent(Network::Id(request_id), HTTP_ERROR_TIMEOUT)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return HTTP_ERROR_TIMEOUT;
}

int KYTY_SYSV_ABI HttpAbortRequest(int request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n", request_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpValidRequest(Network::Id(request_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpWaitRequest(HttpEpollHandle eh, HttpNBEvent* nbev, int maxevents,
                                  int timeout) {
	PRINT_NAME();

	LOGF("\t eh        = 0x%016" PRIx64 "\n"
	     "\t nbev      = 0x%016" PRIx64 "\n"
	     "\t maxevents = %d\n"
	     "\t timeout   = %d\n",
	     reinterpret_cast<uint64_t>(eh), reinterpret_cast<uint64_t>(nbev), maxevents, timeout);

	EXIT_IF(g_net == nullptr);

	if (eh == nullptr || maxevents < 0 || (maxevents > 0 && nbev == nullptr)) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	return 0;
}

int KYTY_SYSV_ABI HttpGetStatusCode(int request_id, int* status_code) {
	PRINT_NAME();

	LOGF("\t request_id  = %d\n"
	     "\t status_code = 0x%016" PRIx64 "\n",
	     request_id, reinterpret_cast<uint64_t>(status_code));

	if (status_code == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	*status_code = 0;

	EXIT_IF(g_net == nullptr);

	int send_result = HTTP_ERROR_BEFORE_SEND;
	if (!g_net->HttpGetRequestResponse(Network::Id(request_id), &send_result, status_code, nullptr,
	                                   nullptr, nullptr)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return send_result;
}

int KYTY_SYSV_ABI HttpGetAllResponseHeaders(int request_id, char** header, size_t* header_size) {
	PRINT_NAME();

	LOGF("\t request_id  = %d\n"
	     "\t header      = 0x%016" PRIx64 "\n"
	     "\t header_size = 0x%016" PRIx64 "\n",
	     request_id, reinterpret_cast<uint64_t>(header), reinterpret_cast<uint64_t>(header_size));

	if (header == nullptr || header_size == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	*header      = nullptr;
	*header_size = 0;

	EXIT_IF(g_net == nullptr);

	int         send_result  = HTTP_ERROR_BEFORE_SEND;
	const char* headers      = nullptr;
	size_t      headers_size = 0;
	if (!g_net->HttpGetRequestResponse(Network::Id(request_id), &send_result, nullptr, &headers,
	                                   &headers_size, nullptr)) {
		return HTTP_ERROR_INVALID_ID;
	}

	*header      = const_cast<char*>(headers);
	*header_size = headers_size;

	return send_result;
}

int KYTY_SYSV_ABI HttpGetResponseContentLength(int request_id, int* result,
                                               uint64_t* content_length) {
	PRINT_NAME();

	LOGF("\t request_id     = %d\n"
	     "\t result         = 0x%016" PRIx64 "\n"
	     "\t content_length = 0x%016" PRIx64 "\n",
	     request_id, reinterpret_cast<uint64_t>(result),
	     reinterpret_cast<uint64_t>(content_length));

	if (result == nullptr || content_length == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	*result         = 0;
	*content_length = 0;

	EXIT_IF(g_net == nullptr);

	int send_result = HTTP_ERROR_BEFORE_SEND;
	if (!g_net->HttpGetRequestResponse(Network::Id(request_id), &send_result, nullptr, nullptr,
	                                   nullptr, content_length)) {
		return HTTP_ERROR_INVALID_ID;
	}

	*result = (send_result == OK ? 0 : -1);
	return send_result;
}

int KYTY_SYSV_ABI HttpCreateConnection(int tmpl_id, const char* server_name, const char* scheme,
                                       uint16_t port, int enable_keep_alive) {
	PRINT_NAME();

	LOGF("\t tmpl_id           = %d\n"
	     "\t server_name       = %s\n"
	     "\t scheme            = %s\n"
	     "\t port              = %u\n"
	     "\t enable_keep_alive = %d\n",
	     tmpl_id, server_name, scheme, static_cast<unsigned>(port), enable_keep_alive);

	EXIT_IF(g_net == nullptr);

	if (server_name == nullptr || scheme == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	auto id = g_net->HttpCreateConnection(Network::Id(tmpl_id), server_name, scheme, port,
	                                      enable_keep_alive != 0);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpCreateConnectionWithURL(int tmpl_id, const char* url, int enable_keep_alive) {
	PRINT_NAME();

	LOGF("\t tmpl_id           = %d\n"
	     "\t url               = %s\n"
	     "\t enable_keep_alive = %d\n",
	     tmpl_id, url, enable_keep_alive);

	EXIT_IF(g_net == nullptr);

	if (url == nullptr || url[0] == '\0') {
		return HTTP_ERROR_INVALID_URL;
	}

	auto id = g_net->HttpCreateConnectionWithURL(Network::Id(tmpl_id), url, enable_keep_alive != 0);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpDeleteConnection(int conn_id) {
	PRINT_NAME();

	LOGF("\t conn_id = %d\n", conn_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteConnection(Network::Id(conn_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpCreateRequest(int conn_id, int method, const char* path,
                                    uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t conn_id        = %d\n"
	     "\t method         = %d\n"
	     "\t path           = %s\n"
	     "\t content_length = %" PRIu64 "\n",
	     conn_id, method, path, content_length);

	EXIT_IF(g_net == nullptr);

	auto method_name = HttpMethodToString(method);
	if (method_name == nullptr) {
		return HTTP_ERROR_UNKNOWN_METHOD;
	}
	if (path == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	auto id =
	    g_net->HttpCreateRequestWithURL2(Network::Id(conn_id), method_name, path, content_length);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpCreateRequestWithURL2(int conn_id, const char* method, const char* url,
                                            uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t conn_id        = %d\n"
	     "\t url            = %s\n"
	     "\t method         = %s\n"
	     "\t content_length = %" PRIu64 "\n",
	     conn_id, url, method, content_length);

	EXIT_IF(g_net == nullptr);

	if (method == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}
	if (url == nullptr || url[0] == '\0') {
		return HTTP_ERROR_INVALID_URL;
	}

	auto id = g_net->HttpCreateRequestWithURL2(Network::Id(conn_id), method, url, content_length);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpDeleteRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteRequest(Network::Id(req_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetRequestContentLength(int request_id, uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t request_id     = %d\n"
	     "\t content_length = %" PRIu64 "\n",
	     request_id, content_length);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpSetRequestContentLength(Network::Id(request_id), content_length)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

} // namespace Http

namespace NetCtl {

LIB_NAME("NetCtl", "NetCtl");

constexpr int NET_CTL_STATE_DISCONNECTED  = 0;
constexpr int NET_CTL_STATE_IPOBTAINED    = 3;
constexpr int NET_CTL_DEVICE_WIRED        = 0;
constexpr int NET_CTL_DEVICE_WIRELESS     = 1;
constexpr int NET_CTL_LINK_DISCONNECTED   = 0;
constexpr int NET_CTL_LINK_CONNECTED      = 1;
constexpr int NET_CTL_EVENT_DISCONNECTED  = 1;
constexpr int NET_CTL_EVENT_IPOBTAINED    = 3;
constexpr int NET_CTL_IP_DHCP             = 0;
constexpr int NET_CTL_HTTP_PROXY_OFF      = 0;
constexpr int NET_CTL_CALLBACK_MAX        = 8;
constexpr int NET_CTL_ERROR_CALLBACK_MAX  = -2143215357; /* 0x80412103 */
constexpr int NET_CTL_ERROR_ID_NOT_FOUND  = -2143215356; /* 0x80412104 */
constexpr int NET_CTL_ERROR_INVALID_ID    = -2143215355; /* 0x80412105 */
constexpr int NET_CTL_ERROR_INVALID_CODE  = -2143215354; /* 0x80412106 */
constexpr int NET_CTL_ERROR_INVALID_ADDR  = -2143215353; /* 0x80412107 */
constexpr int NET_CTL_ERROR_NOT_CONNECTED = -2143215352; /* 0x80412108 */
constexpr int NET_CTL_ERROR_INVALID_TYPE  = -2143215345; /* 0x8041210f */

struct NetInAddr {
	uint32_t s_addr = 0;
};

struct NetEtherAddr {
	uint8_t data[6];
};

struct NetCtlNatInfo {
	unsigned int size       = sizeof(NetCtlNatInfo);
	int          stunStatus = 0;
	int          natType    = 0;
	NetInAddr    mappedAddr;
};

union NetCtlInfo {
	uint32_t     device;
	NetEtherAddr ether_addr;
	uint32_t     mtu;
	uint32_t     link;
	NetEtherAddr bssid;
	char         ssid[32 + 1];
	uint32_t     wifi_security;
	int32_t      rssi_dbm;
	uint8_t      rssi_percentage;
	uint8_t      channel;
	uint32_t     ip_config;
	char         dhcp_hostname[255 + 1];
	char         pppoe_auth_name[127 + 1];
	char         ip_address[16];
	char         netmask[16];
	char         default_route[16];
	char         primary_dns[16];
	char         secondary_dns[16];
	uint32_t     http_proxy_config;
	char         http_proxy_server[255 + 1];
	uint16_t     http_proxy_port;
};

struct NetCtlCallbackSlot {
	NetCtlCallback func       = nullptr;
	void*          arg        = nullptr;
	int            last_event = 0;
};

static Common::Mutex                     g_net_ctl_callbacks_mutex;
static std::array<NetCtlCallbackSlot, 8> g_net_ctl_callbacks;
static std::atomic_bool                  g_net_ctl_connected          = false;
static std::atomic_bool                  g_net_ctl_status_initialized = false;

struct HostNetworkInfo {
	bool         connected = false;
	bool         wireless  = false;
	NetEtherAddr ether_addr {};
	char         ip_address[16] {};
	char         netmask[16] {};
	char         default_route[16] {};
	char         primary_dns[16] {};
	char         secondary_dns[16] {};
};

static void CopyIpv4String(char* dst, size_t dst_size, const sockaddr* addr) {
#if defined(_WIN32)
	if (dst == nullptr || dst_size == 0 || addr == nullptr || addr->sa_family != AF_INET) {
		return;
	}

	const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
	inet_ntop(AF_INET, &in->sin_addr, dst, static_cast<socklen_t>(dst_size));
#else
	(void)dst;
	(void)dst_size;
	(void)addr;
#endif
}

static void CopyIpv4Netmask(char* dst, size_t dst_size, uint8_t prefix_len) {
#if defined(_WIN32)
	if (dst == nullptr || dst_size == 0 || prefix_len > 32) {
		return;
	}

	const uint32_t mask = (prefix_len == 0 ? 0 : (0xffffffffu << (32u - prefix_len)));
	in_addr        in {};
	in.S_un.S_addr = htonl(mask);
	inet_ntop(AF_INET, &in, dst, static_cast<socklen_t>(dst_size));
#else
	(void)dst;
	(void)dst_size;
	(void)prefix_len;
#endif
}

static HostNetworkInfo QueryHostNetworkInfo() {
	HostNetworkInfo info;
#if defined(_WIN32)
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
	ULONG size  = 0;
	auto  ret   = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size);
	if (ret != ERROR_BUFFER_OVERFLOW || size == 0) {
		return info;
	}

	std::vector<uint8_t> buffer(size);
	auto*                adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
	ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size);
	if (ret != NO_ERROR) {
		return info;
	}

	for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
		    adapter->IfType == IF_TYPE_TUNNEL) {
			continue;
		}

		for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
		     unicast       = unicast->Next) {
			const auto* addr = unicast->Address.lpSockaddr;
			if (addr == nullptr) {
				continue;
			}
			if (addr->sa_family == AF_INET) {
				const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
				const auto  ip = ntohl(in->sin_addr.S_un.S_addr);
				if ((ip & 0xff000000u) != 0x7f000000u && ip != 0) {
					info.connected = true;
					info.wireless  = (adapter->IfType == IF_TYPE_IEEE80211);
					if (adapter->PhysicalAddressLength >= sizeof(info.ether_addr.data)) {
						std::memcpy(info.ether_addr.data, adapter->PhysicalAddress,
						            sizeof(info.ether_addr.data));
					}
					CopyIpv4String(info.ip_address, sizeof(info.ip_address), addr);
					CopyIpv4Netmask(info.netmask, sizeof(info.netmask),
					                unicast->OnLinkPrefixLength);
					if (adapter->FirstGatewayAddress != nullptr) {
						CopyIpv4String(info.default_route, sizeof(info.default_route),
						               adapter->FirstGatewayAddress->Address.lpSockaddr);
					}
					int dns_index = 0;
					for (auto* dns = adapter->FirstDnsServerAddress; dns != nullptr;
					     dns       = dns->Next) {
						if (dns->Address.lpSockaddr != nullptr &&
						    dns->Address.lpSockaddr->sa_family == AF_INET) {
							CopyIpv4String(dns_index == 0 ? info.primary_dns : info.secondary_dns,
							               dns_index == 0 ? sizeof(info.primary_dns)
							                              : sizeof(info.secondary_dns),
							               dns->Address.lpSockaddr);
							dns_index++;
							if (dns_index >= 2) {
								break;
							}
						}
					}
					return info;
				}
			} else if (addr->sa_family == AF_INET6) {
				const auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
				if (!IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr) &&
				    !IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr)) {
					info.connected = true;
				}
			}
		}
	}

	return info;
#else
	info.connected = true;
	return info;
#endif
}

[[maybe_unused]] static bool HostNetworkConnected() {
	return QueryHostNetworkInfo().connected;
}

static bool NetCtlConnected() {
	if (!g_net_ctl_status_initialized.load()) {
		// g_net_ctl_connected          = HostNetworkConnected();
		g_net_ctl_connected          = false;
		g_net_ctl_status_initialized = true;
		// LOGF("\t host network connected = %s\n", (g_net_ctl_connected.load() ? "true" :
		// "false"));
		LOGF("\t host network connected = false (forced offline)\n");
	}

	return g_net_ctl_connected.load();
}

int KYTY_SYSV_ABI NetCtlInit() {
	PRINT_NAME();

	// g_net_ctl_connected = HostNetworkConnected();
	g_net_ctl_connected          = false;
	g_net_ctl_status_initialized = true;
	// LOGF("\t host network connected = %s\n", (g_net_ctl_connected.load() ? "true" : "false"));
	LOGF("\t host network connected = false (forced offline)\n");

	return OK;
}

void KYTY_SYSV_ABI NetCtlTerm() {
	PRINT_NAME();

	Common::LockGuard lock(g_net_ctl_callbacks_mutex);

	for (auto& cb: g_net_ctl_callbacks) {
		cb = {};
	}
}

int KYTY_SYSV_ABI NetCtlGetNatInfo(NetCtlNatInfo* nat_info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(nat_info == nullptr);
	EXIT_NOT_IMPLEMENTED(nat_info->size != sizeof(NetCtlNatInfo));

	nat_info->stunStatus        = 0;
	nat_info->natType           = 0;
	nat_info->mappedAddr.s_addr = 0;

	return OK;
}

int KYTY_SYSV_ABI NetCtlCheckCallback() {
	PRINT_NAME();

	Common::LockGuard lock(g_net_ctl_callbacks_mutex);
	const int event = (NetCtlConnected() ? NET_CTL_EVENT_IPOBTAINED : NET_CTL_EVENT_DISCONNECTED);
	for (auto& cb: g_net_ctl_callbacks) {
		if (cb.func != nullptr && cb.last_event != event) {
			cb.func(event, cb.arg);
			cb.last_event = event;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI NetCtlGetState(int* state) {
	PRINT_NAME();

	if (state == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}

	*state = (NetCtlConnected() ? NET_CTL_STATE_IPOBTAINED : NET_CTL_STATE_DISCONNECTED);

	return OK;
}

int KYTY_SYSV_ABI NetCtlGetStateV6(int* state) {
	PRINT_NAME();

	if (state == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}

	*state = (NetCtlConnected() ? NET_CTL_STATE_IPOBTAINED : NET_CTL_STATE_DISCONNECTED);

	return OK;
}

int KYTY_SYSV_ABI NetCtlRegisterCallback(NetCtlCallback func, void* arg, int* cid) {
	PRINT_NAME();

	if (func == nullptr || cid == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}

	Common::LockGuard lock(g_net_ctl_callbacks_mutex);

	for (int i = 0; i < NET_CTL_CALLBACK_MAX; i++) {
		if (g_net_ctl_callbacks[static_cast<size_t>(i)].func == nullptr) {
			g_net_ctl_callbacks[static_cast<size_t>(i)].func       = func;
			g_net_ctl_callbacks[static_cast<size_t>(i)].arg        = arg;
			g_net_ctl_callbacks[static_cast<size_t>(i)].last_event = 0;
			*cid                                                   = i;
			return OK;
		}
	}

	return NET_CTL_ERROR_CALLBACK_MAX;
}

int KYTY_SYSV_ABI NetCtlUnregisterCallback(int cid) {
	PRINT_NAME();

	if (cid < 0 || cid >= NET_CTL_CALLBACK_MAX) {
		return NET_CTL_ERROR_INVALID_ID;
	}

	Common::LockGuard lock(g_net_ctl_callbacks_mutex);

	auto& cb = g_net_ctl_callbacks[static_cast<size_t>(cid)];

	if (cb.func == nullptr) {
		return NET_CTL_ERROR_ID_NOT_FOUND;
	}

	cb = {};

	return OK;
}

int KYTY_SYSV_ABI NetCtlGetResult(int event_type, int* error_code) {
	PRINT_NAME();

	if (error_code == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}

	LOGF("\t event_type = %d\n", event_type);

	switch (event_type) {
		case NET_CTL_EVENT_DISCONNECTED:
		case 2:
		case NET_CTL_EVENT_IPOBTAINED: *error_code = OK; break;
		default: return NET_CTL_ERROR_INVALID_TYPE;
	}

	return OK;
}

int KYTY_SYSV_ABI NetCtlGetInfo(int code, NetCtlInfo* info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(info == nullptr);

	memset(info, 0, sizeof(NetCtlInfo));

	// Online/host-backed info responses are left below for testing, but disabled
	// to preserve the disconnected-console behavior.
	return NET_CTL_ERROR_NOT_CONNECTED;

	switch (code) {
		case 1:
			info->device =
			    (QueryHostNetworkInfo().wireless ? NET_CTL_DEVICE_WIRELESS : NET_CTL_DEVICE_WIRED);
			break;
		case 2: info->ether_addr = QueryHostNetworkInfo().ether_addr; break;
		case 3: info->mtu = 1500; break;
		case 4:
			info->link = (NetCtlConnected() ? NET_CTL_LINK_CONNECTED : NET_CTL_LINK_DISCONNECTED);
			break;
		case 5: break;
		case 6: break;
		case 7: info->wifi_security = 0; break;
		case 8: info->rssi_dbm = 0; break;
		case 9: info->rssi_percentage = 0; break;
		case 10: info->channel = 0; break;
		case 11: info->ip_config = NET_CTL_IP_DHCP; break;
		case 12: break;
		case 13: break;
		case 14:
			std::strncpy(info->ip_address, QueryHostNetworkInfo().ip_address,
			             sizeof(info->ip_address) - 1);
			break;
		case 15:
			std::strncpy(info->netmask, QueryHostNetworkInfo().netmask, sizeof(info->netmask) - 1);
			break;
		case 16:
			std::strncpy(info->default_route, QueryHostNetworkInfo().default_route,
			             sizeof(info->default_route) - 1);
			break;
		case 17:
			std::strncpy(info->primary_dns, QueryHostNetworkInfo().primary_dns,
			             sizeof(info->primary_dns) - 1);
			break;
		case 18:
			std::strncpy(info->secondary_dns, QueryHostNetworkInfo().secondary_dns,
			             sizeof(info->secondary_dns) - 1);
			break;
		case 19: info->http_proxy_config = NET_CTL_HTTP_PROXY_OFF; break;
		case 20: break;
		case 21: info->http_proxy_port = 0; break;
		default: LOGF("\t unknown NetCtl info code: %d\n", code); return NET_CTL_ERROR_INVALID_CODE;
	}

	return OK;
}

} // namespace NetCtl

namespace NpManager {

LIB_NAME("NpManager", "NpManager");

struct NpTitleId {
	char    id[12 + 1];
	uint8_t padding[3];
};

struct NpTitleSecret {
	uint8_t data[128];
};

struct NpCountryCode {
	char data[2];
	char term;
	char padding[1];
};

struct NpAgeRestriction {
	NpCountryCode country_code;
	int8_t        age;
	uint8_t       padding[3];
};

struct NpContentRestriction {
	size_t                  size;
	int8_t                  default_age_restriction;
	char                    padding[3];
	int32_t                 age_restriction_count;
	const NpAgeRestriction* age_restriction;
};

struct NpOnlineId {
	char data[16];
	char term;
	char dummy[3];
};

struct NpId {
	NpOnlineId handle;
	uint8_t    opt[8];
	uint8_t    reserved[8];
};

struct NpCreateAsyncRequestParameter {
	size_t                   size;
	LibKernel::KernelCpumask cpu_affinity_mask;
	int                      thread_priority;
	uint8_t                  padding[4];
};

struct NpCheckPremiumParameter {
	size_t   size;
	int      user_id;
	char     padding[4];
	uint64_t features;
	uint8_t  reserved[32];
};

struct NpCheckPremiumResult {
	bool    authorized;
	uint8_t reserved[32];
};

constexpr int np_error_invalid_argument  = -2141913085; /* 0x80550003 */
constexpr int np_error_signed_out        = -2141913082; /* 0x80550006 */
constexpr int np_error_invalid_size      = -2141913071; /* 0x80550011 */
constexpr int np_error_aborted           = -2141913070; /* 0x80550012 */
constexpr int np_error_request_max       = -2141913069; /* 0x80550013 */
constexpr int np_error_request_not_found = -2141913068; /* 0x80550014 */
constexpr int np_error_invalid_id        = -2141913067; /* 0x80550015 */
constexpr int np_request_max             = 128;

enum class NpRequestState {
	Free,
	Ready,
	Aborted,
	Complete,
};

struct NpRequest {
	NpRequestState state  = NpRequestState::Free;
	bool           async  = false;
	int            result = OK;
};

static std::mutex             g_np_request_mutex;
static std::vector<NpRequest> g_np_requests;

static int np_create_request(bool async) {
	std::lock_guard lock(g_np_request_mutex);

	for (size_t i = 0; i < g_np_requests.size(); i++) {
		auto& request = g_np_requests[i];
		if (request.state == NpRequestState::Free) {
			request.state  = NpRequestState::Ready;
			request.async  = async;
			request.result = OK;
			return static_cast<int>(i + 1);
		}
	}

	if (g_np_requests.size() >= np_request_max) {
		return np_error_request_max;
	}

	g_np_requests.push_back({NpRequestState::Ready, async, OK});
	return static_cast<int>(g_np_requests.size());
}

static NpRequest* np_get_request_locked(int req_id) {
	if (req_id <= 0) {
		return nullptr;
	}

	const auto index = static_cast<size_t>(req_id - 1);
	if (index >= g_np_requests.size() || g_np_requests[index].state == NpRequestState::Free) {
		return nullptr;
	}

	return &g_np_requests[index];
}

static int np_complete_signed_out_locked(NpRequest* request) {
	if (request->state == NpRequestState::Complete) {
		request->result = np_error_invalid_argument;
		return np_error_invalid_argument;
	}
	if (request->state == NpRequestState::Aborted) {
		request->result = np_error_aborted;
		return np_error_aborted;
	}

	request->state  = NpRequestState::Complete;
	request->result = np_error_signed_out;

	return (request->async ? OK : np_error_signed_out);
}

int KYTY_SYSV_ABI NpCheckCallback() {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpSetNpTitleId(const NpTitleId* title_id, const NpTitleSecret* title_secret) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(title_id == nullptr);
	EXIT_NOT_IMPLEMENTED(title_secret == nullptr);

	LOGF("\t title_id = %.12s\n"
	     "\t title_secret = %s\n",
	     title_id->id, Common::HexFromBin(Common::ByteBuffer(title_secret->data, 128)).c_str());

	return OK;
}

int KYTY_SYSV_ABI NpSetContentRestriction(const NpContentRestriction* restriction) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(restriction == nullptr);
	EXIT_NOT_IMPLEMENTED(restriction->size != sizeof(NpContentRestriction));

	LOGF("\t default_age_restriction = %" PRIi8 "\n"
	     "\t age_restriction_count   = %" PRIi32 "\n",
	     restriction->default_age_restriction, restriction->age_restriction_count);

	for (int i = 0; i < restriction->age_restriction_count; i++) {
		LOGF("\t age_restriction[%d].age = %" PRIi8 "\n"
		     "\t age_restriction[%d].country_code.data = %.2s\n",
		     i, restriction->age_restriction[i].age, i,
		     restriction->age_restriction[i].country_code.data);
	}

	return OK;
}

int KYTY_SYSV_ABI NpRegisterStateCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpUnregisterStateCallback() {
	PRINT_NAME();

	return OK;
}

void KYTY_SYSV_ABI NpRegisterGamePresenceCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();
}

int KYTY_SYSV_ABI NpRegisterPlusEventCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpRegisterPremiumEventCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpRegisterNpReachabilityStateCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpGetNpId(int user_id, NpId* np_id) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(np_id == nullptr);

	// int s = snprintf(np_id->handle.data, 16, "Kyty");
	// EXIT_NOT_IMPLEMENTED(s >= 16);
	// np_id->handle.term = 0;
	std::memset(np_id, 0, sizeof(*np_id));

	// return OK;
	return np_error_signed_out;
}

int KYTY_SYSV_ABI NpGetOnlineId(int user_id, NpOnlineId* online_id) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(online_id == nullptr);

	// int s = snprintf(online_id->data, 16, "Kyty");
	// EXIT_NOT_IMPLEMENTED(s >= 16);
	// online_id->term = 0;
	std::memset(online_id, 0, sizeof(*online_id));

	// return OK;
	return np_error_signed_out;
}

int KYTY_SYSV_ABI NpGetAccountIdA(int user_id, uint64_t* account_id) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(account_id == nullptr);

	// *account_id = 0x00000000feedfaceull;
	*account_id = 0;

	// return OK;
	return np_error_signed_out;
}

int KYTY_SYSV_ABI NpGetAccountCountryA(int user_id, void* country_code) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(country_code == nullptr);

	auto* code = static_cast<NpCountryCode*>(country_code);
	std::memset(code, 0, sizeof(*code));
	// code->data[0] = '';
	// code->data[1] = 'S';

	// return OK;
	return np_error_signed_out;
}

int KYTY_SYSV_ABI NpGetAccountAge(int req_id, int user_id, uint8_t* age) {
	PRINT_NAME();

	LOGF("\t req_id  = %d\n", req_id);
	LOGF("\t user_id = %d\n", user_id);
	LOGF("\t age     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(age));

	if (req_id <= 0 || age == nullptr) {
		return np_error_invalid_argument;
	}

	// *age = 21;
	*age = 0;

	// return OK;
	return np_error_signed_out;
}

int KYTY_SYSV_ABI NpCreateRequest() {
	PRINT_NAME();

	const auto req_id = np_create_request(false);

	LOGF("\t req_id = %d\n", req_id);

	return req_id;
}

int KYTY_SYSV_ABI NpCreateAsyncRequest(const NpCreateAsyncRequestParameter* param) {
	PRINT_NAME();

	if (param == nullptr) {
		return np_error_invalid_argument;
	}

	if (param->size < sizeof(NpCreateAsyncRequestParameter)) {
		return np_error_invalid_size;
	}

	LOGF("\t size              = %" PRIu64 "\n"
	     "\t cpu_affinity_mask = %" PRIu64 "\n"
	     "\t thread_priority   = %d\n",
	     param->size, param->cpu_affinity_mask, param->thread_priority);

	const auto req_id = np_create_request(true);

	LOGF("\t req_id            = %d\n", req_id);

	return req_id;
}

int KYTY_SYSV_ABI NpDeleteRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	request->state  = NpRequestState::Free;
	request->async  = false;
	request->result = OK;

	return OK;
}

int KYTY_SYSV_ABI NpAbortRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	request->state  = NpRequestState::Aborted;
	request->result = np_error_aborted;

	return OK;
}

int KYTY_SYSV_ABI NpCheckNpAvailability(int req_id, const char* user, void* result) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(req_id <= 0);
	// EXIT_NOT_IMPLEMENTED(user == nullptr);
	// EXIT_NOT_IMPLEMENTED(result != nullptr);

	LOGF("\t req_id = %d\n"
	     "\t user   = %s\n",
	     req_id, user != nullptr ? user : "(null)");

	if (req_id <= 0 || user == nullptr) {
		return np_error_invalid_argument;
	}

	if (result != nullptr) {
		std::memset(result, 0, sizeof(int));
	}

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	// return OK;
	return np_complete_signed_out_locked(request);
}

int KYTY_SYSV_ABI NpCheckNpReachability(int req_id, int user_id) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(req_id <= 0);

	LOGF("\t req_id  = %d\n", req_id);
	LOGF("\t user_id = %d\n", user_id);

	if (req_id <= 0) {
		return np_error_invalid_argument;
	}

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	// return OK;
	return np_complete_signed_out_locked(request);
}

int KYTY_SYSV_ABI NpPollAsync(int req_id, int* result) {
	PRINT_NAME();

	if (result == nullptr) {
		return np_error_invalid_argument;
	}

	LOGF("\t req_id = %d\n", req_id);

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	if (!request->async || request->state == NpRequestState::Ready) {
		return np_error_invalid_id;
	}

	*result = request->result;

	// return request->state == NpRequestState::Aborted ? np_error_aborted : OK;
	return OK;
}

int KYTY_SYSV_ABI NpCheckPremium(int req_id, const NpCheckPremiumParameter* param,
                                 NpCheckPremiumResult* result) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t param  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	if (param == nullptr || result == nullptr) {
		return np_error_invalid_argument;
	}

	if (param->size < sizeof(NpCheckPremiumParameter)) {
		return np_error_invalid_size;
	}

	LOGF("\t size    = %" PRIu64 "\n"
	     "\t user_id = %d\n"
	     "\t features = 0x%016" PRIx64 "\n",
	     param->size, param->user_id, param->features);

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	if (request->state == NpRequestState::Complete) {
		request->result = np_error_invalid_argument;
		return np_error_invalid_argument;
	}

	if (request->state == NpRequestState::Aborted) {
		return np_error_aborted;
	}

	std::memset(result, 0, sizeof(*result));
	// result->authorized = true;

	// request->state  = NpRequestState::Complete;
	// request->result = OK;

	// return OK;
	return np_complete_signed_out_locked(request);
}

int KYTY_SYSV_ABI NpGetState(int user_id, uint32_t* state) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	LOGF("\t user_id = %d\n", user_id);

	*state = 1; // Signed out

	return OK;
}

int KYTY_SYSV_ABI NpGetNpReachabilityState(int user_id, uint32_t* state) {
	PRINT_NAME();

	constexpr int np_error_invalid_argument = -2141913085; /* 0x80550003 */

	if (state == nullptr) {
		return np_error_invalid_argument;
	}

	LOGF("\t user_id = %d\n", user_id);

	// *state = 2; // SCE_NP_REACHABILITY_STATE_REACHABLE
	*state = 0; // SCE_NP_REACHABILITY_STATE_UNAVAILABLE

	return OK;
}

} // namespace NpManager

} // namespace Libs::Network
