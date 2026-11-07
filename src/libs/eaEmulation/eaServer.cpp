#include "libs/eaEmulation/eaServer.h"

#include "libs/eaEmulation/eaPatchPlan.h"
#include "libs/eaEmulation/eaPreAuth.h"
#include "libs/eaEmulation/eaTdf.h"
#include "libs/eaEmulation/eaUpstream.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <nlohmann/json.hpp>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
// macOS has no MSG_NOSIGNAL (SO_NOSIGPIPE is set on the socket instead) and no SOCK_CLOEXEC.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

namespace Libs::EaEmulation {

namespace {

// The port the game is told. Nothing listens on it: a connect to it gets a pair of sockets.
constexpr uint16_t VIRTUAL_PORT = 10071;

// ---- state ----

struct State {
	std::atomic<uint16_t> port {0};
	std::string           title_id;
	std::vector<uint8_t>  ca_modulus; // of our authority, for the patch
	std::atomic<bool>     signed_in {false};
	std::mutex            picker_mutex;
	ServerPickerQuestion  picker_question; // the game's own, for the forwarding to EA's real server

	mbedtls_entropy_context  entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_pk_context       ca_key;
	mbedtls_pk_context       leaf_key;
	mbedtls_x509_crt         chain; // the leaf, then the authority
	mbedtls_ssl_config       ssl_config;
#if defined(_WIN32)
	mbedtls_net_context      listener;
#endif
};

State* g_state = nullptr;

void Log(const std::string& text) {
	Log::WriteToConsoleAndLog(fmt::format("EA emulation: {}\n", text));
}

std::string TlsError(int code) {
	std::array<char, 160> text {};
	mbedtls_strerror(code, text.data(), text.size());
	return fmt::format("{} (-0x{:04x})", text.data(), static_cast<unsigned>(-code));
}

// ---- certificates ----

// The names are in the order, and the string types, of the certificates of the real servers.
constexpr const char* CA_NAME =
    "CN=GOS 2015 Certificate Authority,C=US,ST=California,L=Redwood City,O=Electronic Arts\\, Inc.,"
    "OU=Global Online Studio,emailAddress=GOSDirtysockSupport@ea.com";
constexpr const char* LEAF_NAME =
    "C=US,ST=California,O=Electronic Arts\\, Inc.,OU=Global Online Studio,CN=*.blaze.ea.com";

bool GenerateKey(State& state, mbedtls_pk_context& key) {
	mbedtls_pk_init(&key);
	int result = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
	if (result == 0) {
		result = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &state.drbg, 2048, 65537);
	}
	if (result != 0) {
		Log("RSA key generation failed: " + TlsError(result));
		return false;
	}
	return true;
}

// Writes the certificate of `subject_key`, signed by `issuer_key`, to `der`.
bool MakeCertificate(State& state, const char* subject_name, mbedtls_pk_context& subject_key,
                     const char* issuer_name, mbedtls_pk_context& issuer_key, bool authority,
                     uint8_t serial, std::vector<uint8_t>& der) {
	mbedtls_x509write_cert cert;
	mbedtls_x509write_crt_init(&cert);
	mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
	mbedtls_x509write_crt_set_subject_key(&cert, &subject_key);
	mbedtls_x509write_crt_set_issuer_key(&cert, &issuer_key);
	const unsigned char serial_bytes[1] = {serial};
	int                 result          = mbedtls_x509write_crt_set_subject_name(&cert, subject_name);
	if (result == 0) result = mbedtls_x509write_crt_set_issuer_name(&cert, issuer_name);
	if (result == 0) result = mbedtls_x509write_crt_set_serial_raw(&cert, const_cast<unsigned char*>(serial_bytes), 1);
	if (result == 0) result = mbedtls_x509write_crt_set_validity(&cert, "20200101000000", "20491231235959");
	if (result == 0) result = mbedtls_x509write_crt_set_basic_constraints(&cert, authority ? 1 : 0, authority ? -1 : 0);
	if (result == 0) result = mbedtls_x509write_crt_set_subject_key_identifier(&cert);
	if (result == 0 && !authority) result = mbedtls_x509write_crt_set_authority_key_identifier(&cert);
	if (result == 0 && authority) {
		result = mbedtls_x509write_crt_set_key_usage(&cert, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN);
	}
	if (result == 0 && !authority) {
		mbedtls_x509_san_list san {};
		san.node.type                          = MBEDTLS_X509_SAN_DNS_NAME;
		san.node.san.unstructured_name.p       = reinterpret_cast<unsigned char*>(const_cast<char*>("*.blaze.ea.com"));
		san.node.san.unstructured_name.len     = std::strlen("*.blaze.ea.com");
		san.next                               = nullptr;
		result = mbedtls_x509write_crt_set_subject_alternative_name(&cert, &san);
	}
	if (result == 0) {
		der.resize(4096);
		result = mbedtls_x509write_crt_der(&cert, der.data(), der.size(), mbedtls_ctr_drbg_random, &state.drbg);
		if (result > 0) {
			// The DER is written at the end of the buffer.
			std::vector<uint8_t> tail(der.end() - result, der.end());
			der = std::move(tail);
			result = 0;
		}
	}
	mbedtls_x509write_crt_free(&cert);
	if (result != 0) {
		Log("certificate failed: " + TlsError(result));
		return false;
	}
	return true;
}

bool SetUpCertificates(State& state) {
	if (!GenerateKey(state, state.ca_key) || !GenerateKey(state, state.leaf_key)) return false;
	std::vector<uint8_t> ca_der;
	std::vector<uint8_t> leaf_der;
	if (!MakeCertificate(state, CA_NAME, state.ca_key, CA_NAME, state.ca_key, true, 1, ca_der) ||
	    !MakeCertificate(state, LEAF_NAME, state.leaf_key, CA_NAME, state.ca_key, false, 2, leaf_der)) {
		return false;
	}
	mbedtls_x509_crt_init(&state.chain);
	if (mbedtls_x509_crt_parse_der(&state.chain, leaf_der.data(), leaf_der.size()) != 0 ||
	    mbedtls_x509_crt_parse_der(&state.chain, ca_der.data(), ca_der.size()) != 0) {
		Log("could not read back the certificates");
		return false;
	}

	// The modulus of the authority, for the patch.
	mbedtls_mpi modulus;
	mbedtls_mpi_init(&modulus);
	std::vector<uint8_t> bytes(256);
	const bool           exported = mbedtls_rsa_export(mbedtls_pk_rsa(state.ca_key), &modulus, nullptr, nullptr,
	                                                   nullptr, nullptr) == 0 &&
	                      mbedtls_mpi_size(&modulus) == bytes.size() &&
	                      mbedtls_mpi_write_binary(&modulus, bytes.data(), bytes.size()) == 0;
	mbedtls_mpi_free(&modulus);
	if (!exported) {
		Log("could not read the modulus of the authority");
		return false;
	}
	state.ca_modulus = std::move(bytes);
	return true;
}

// ---- Blaze ----

constexpr size_t   HEADER_SIZE = 16;
constexpr uint8_t  REPLY       = 0x20;
constexpr uint32_t MAX_FRAME   = 1u << 20;


// The client type in the CDAT.TYPE of a request, or -1.
int64_t ClientType(const std::vector<uint8_t>& payload) {
	return IntegerMember(payload, Tag("CDAT"), Tag("TYPE"), -1);
}


// Util.ping: STIM, the time of the server.
std::vector<uint8_t> PingReply() {
	Writer w;
	w.Integer(Tag("STIM"), std::chrono::duration_cast<std::chrono::seconds>(
	                           std::chrono::system_clock::now().time_since_epoch())
	                           .count());
	return std::move(w.out);
}

// ---- the folder of replies, the trace, the real server ----

// KYTY_EA_TRACE=1 writes every request and reply, decoded, to the log.
bool Tracing() {
	static const bool on = std::getenv("KYTY_EA_TRACE") != nullptr;
	return on;
}

// KYTY_EA_UPSTREAM=1 sends the calls that are not answered here to EA's real server.
// "all" sends Util.preAuth too, instead of the server's own reply (to record the real one).
bool UpstreamEnabled() {
	static const bool on = std::getenv("KYTY_EA_UPSTREAM") != nullptr;
	return on;
}

bool UpstreamForPreAuth() {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_EA_UPSTREAM");
		return value != nullptr && std::strcmp(value, "all") == 0;
	}();
	return on;
}

// KYTY_EA_DIR names a folder of replies, one file for each call and client type
// (0009_0007_type6.bin: component, command, type). A reply in it is used before the server's own.
// A reply that comes from the real server is written to it, with the secret values blanked.
std::filesystem::path ReplyPath(uint16_t component, uint16_t command, int64_t client_type) {
	const char* dir = std::getenv("KYTY_EA_DIR");
	if (dir == nullptr || *dir == 0) return {};
	return std::filesystem::path(dir) / fmt::format("{:04x}_{:04x}_type{}.bin", component, command, client_type);
}

bool ReadReplyFile(const std::filesystem::path& path, std::vector<uint8_t>& body) {
	if (path.empty()) return false;
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;
	body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	return true;
}

void WriteReplyFile(const std::filesystem::path& path, const std::vector<uint8_t>& body) {
	if (path.empty()) return;
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
}

void TraceFrame(const char* what, uint16_t component, uint16_t command, const uint8_t* header,
                const std::vector<uint8_t>& payload) {
	if (!Tracing()) return;
	std::string tail;
	for (size_t i = 10; i < HEADER_SIZE; i++) tail += fmt::format("{:02x}", header[i]);
	Log(fmt::format("{} 0x{:04x}/0x{:04x} tail {} size {}\n{}", what, component, command, tail, payload.size(),
	                DumpTdf(payload.data(), payload.size())));
}

// ---- connections ----

bool ReadAll(mbedtls_ssl_context& ssl, uint8_t* data, size_t size) {
	size_t done = 0;
	while (done < size) {
		const int result = mbedtls_ssl_read(&ssl, data + done, size - done);
		if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		if (result <= 0) return false;
		done += static_cast<size_t>(result);
	}
	return true;
}

bool WriteAll(mbedtls_ssl_context& ssl, const uint8_t* data, size_t size) {
	size_t done = 0;
	while (done < size) {
		const int result = mbedtls_ssl_write(&ssl, data + done, size - done);
		if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		if (result <= 0) return false;
		done += static_cast<size_t>(result);
	}
	return true;
}

// The connection of the game is one end of a pair of connected sockets, so a send to a closed peer
// must not raise SIGPIPE.
#if !defined(_WIN32)
int BioSend(void* context, const unsigned char* data, size_t size) {
	const int  fd     = static_cast<mbedtls_net_context*>(context)->fd;
	const auto result = ::send(fd, data, size, MSG_NOSIGNAL);
	if (result >= 0) return static_cast<int>(result);
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

int BioReceive(void* context, unsigned char* data, size_t size) {
	const int  fd     = static_cast<mbedtls_net_context*>(context)->fd;
	const auto result = ::recv(fd, data, size, 0);
	if (result >= 0) return static_cast<int>(result);
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
	return MBEDTLS_ERR_NET_RECV_FAILED;
}
#else
int BioSend(void* context, const unsigned char* data, size_t size) {
	return mbedtls_net_send(context, data, size);
}

int BioReceive(void* context, unsigned char* data, size_t size) {
	return mbedtls_net_recv(context, data, size);
}
#endif

void ServeClient(State* state, mbedtls_net_context client) {
	mbedtls_ssl_context ssl;
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_set_bio(&ssl, &client, BioSend, BioReceive, nullptr);
	int result = mbedtls_ssl_setup(&ssl, &state->ssl_config);
	while (result == 0) {
		result = mbedtls_ssl_handshake(&ssl);
		if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) break;
		result = 0;
	}
	if (result != 0) {
		Log("TLS handshake failed: " + TlsError(result));
	} else {
		std::unique_ptr<Upstream> upstream;
		for (;;) {
			std::array<uint8_t, HEADER_SIZE> header {};
			if (!ReadAll(ssl, header.data(), header.size())) break;
			const uint32_t size = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) |
			                      (uint32_t(header[2]) << 8) | header[3];
			const uint16_t component = static_cast<uint16_t>((header[6] << 8) | header[7]);
			const uint16_t command   = static_cast<uint16_t>((header[8] << 8) | header[9]);
			if (size > MAX_FRAME) break;
			std::vector<uint8_t> payload(size);
			if (size != 0 && !ReadAll(ssl, payload.data(), payload.size())) break;

			TraceFrame("request", component, command, header.data(), payload);
			std::vector<uint8_t> body;
			bool                 answered = true;
			bool                 forwarded = false;
			const auto           type      = ClientType(payload);
			const auto           file      = ReplyPath(component, command, type);
			if (component == 0x0000 && command == 0x0000) {
				// Keepalive: the reply is the same empty frame.
			} else if (component == 0x0009 && command == 0x0002) {
				body = PingReply();
			} else if (ReadReplyFile(file, body)) {
				// A reply of the folder of replies.
				if (Tracing()) Log(fmt::format("reply from {}", file.string()));
			} else if (component == 0x0009 && command == 0x0007 && !UpstreamForPreAuth()) {
				// Util.preAuth: the configuration of the server, made from the game's own request.
				body = PreAuthReply(payload);
			} else if (UpstreamEnabled()) {
				if (!upstream) {
					ServerPickerQuestion question;
					{
						std::lock_guard lock(state->picker_mutex);
						question = state->picker_question;
					}
					upstream = std::make_unique<Upstream>();
					if (!upstream->Connect(question)) upstream.reset();
				}
				std::vector<uint8_t> request(header.begin(), header.end());
				request.insert(request.end(), payload.begin(), payload.end());
				std::vector<uint8_t> reply;
				if (upstream && upstream->Exchange(request, reply)) {
					// The real server answers; the frame goes to the game as it is.
					std::vector<uint8_t> reply_body(reply.begin() + HEADER_SIZE, reply.end());
					TraceFrame("reply (real server)", component, command, reply.data(), reply_body);
					const auto blanked = BlankSecrets(reply_body);
					if (blanked != 0) Log(fmt::format("{} secret values blanked", blanked));
					WriteReplyFile(file, reply_body);
					std::copy(reply.begin(), reply.begin() + HEADER_SIZE, header.begin());
					body      = std::move(reply_body);
					forwarded = true;
				} else {
					answered = false;
					upstream.reset();
					Log(fmt::format("call 0x{:04x}/0x{:04x}: the real server did not answer", component, command));
				}
			} else {
				answered = false;
				Log(fmt::format("call 0x{:04x}/0x{:04x} is not answered", component, command));
			}
			if (!answered) continue;

			std::vector<uint8_t> frame(HEADER_SIZE + body.size());
			const uint32_t       out_size = static_cast<uint32_t>(body.size());
			frame[0] = static_cast<uint8_t>(out_size >> 24);
			frame[1] = static_cast<uint8_t>(out_size >> 16);
			frame[2] = static_cast<uint8_t>(out_size >> 8);
			frame[3] = static_cast<uint8_t>(out_size);
			std::copy(header.begin() + 4, header.begin() + HEADER_SIZE, frame.begin() + 4);
			// The reply keeps the message number; a keepalive stays a keepalive.
			if (!forwarded) frame[13] = static_cast<uint8_t>(REPLY | (header[13] & 0x80));
			if (!forwarded) TraceFrame("reply", component, command, frame.data(), body);
			std::copy(body.begin(), body.end(), frame.begin() + HEADER_SIZE);
			if (!WriteAll(ssl, frame.data(), frame.size())) break;
		}
	}
	mbedtls_ssl_close_notify(&ssl);
	mbedtls_ssl_free(&ssl);
	mbedtls_net_free(&client);
}

#if defined(_WIN32)
void AcceptLoop(State* state) {
	for (;;) {
		mbedtls_net_context client;
		mbedtls_net_init(&client);
		if (mbedtls_net_accept(&state->listener, &client, nullptr, 0, nullptr) != 0) {
			mbedtls_net_free(&client);
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		std::thread(ServeClient, state, client).detach();
	}
}

bool Listen(State& state) {
	mbedtls_net_init(&state.listener);
	int result = mbedtls_net_bind(&state.listener, "127.0.0.1", "0", MBEDTLS_NET_PROTO_TCP);
	if (result != 0) {
		Log("could not listen: " + TlsError(result));
		return false;
	}
	sockaddr_in address {};
#if defined(_WIN32)
	int length = sizeof(address);
#else
	socklen_t length = sizeof(address);
#endif
	if (getsockname(state.listener.fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
		Log("could not read the port");
		return false;
	}
	state.port = ntohs(address.sin_port);
	return true;
}
#endif

} // namespace

bool StartForGame(const std::string& title_id) {
	if (g_state != nullptr) return false;
	if (!ShouldStartServer(title_id, Config::EaEmulationEnabled())) return false;

	auto* state     = new State();
	state->title_id = title_id;
	mbedtls_entropy_init(&state->entropy);
	mbedtls_ctr_drbg_init(&state->drbg);
	mbedtls_ssl_config_init(&state->ssl_config);
	int result = mbedtls_ctr_drbg_seed(&state->drbg, mbedtls_entropy_func, &state->entropy, nullptr, 0);
	if (result != 0) {
		Log("random numbers failed: " + TlsError(result));
		delete state;
		return false;
	}
	if (!SetUpCertificates(*state)) {
		delete state;
		return false;
	}
	result = mbedtls_ssl_config_defaults(&state->ssl_config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
	                                     MBEDTLS_SSL_PRESET_DEFAULT);
	if (result == 0) {
		// TLS 1.2 only: the TLS 1.3 handshake of this server made the client stop.
		mbedtls_ssl_conf_min_tls_version(&state->ssl_config, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_max_tls_version(&state->ssl_config, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_rng(&state->ssl_config, mbedtls_ctr_drbg_random, &state->drbg);
		mbedtls_ssl_conf_authmode(&state->ssl_config, MBEDTLS_SSL_VERIFY_NONE);
		result = mbedtls_ssl_conf_own_cert(&state->ssl_config, &state->chain, &state->leaf_key);
	}
	if (result != 0) {
		Log("TLS configuration failed: " + TlsError(result));
		delete state;
		return false;
	}
#if defined(_WIN32)
	// No pair of sockets here: the game connects to a port on this machine.
	if (!Listen(*state)) {
		delete state;
		return false;
	}
#else
	state->port = VIRTUAL_PORT;
#endif
	state->signed_in = true;
	g_state          = state;
	// The game asks for a server only when it has a network.
	Config::SetNetworkConnected();
#if defined(_WIN32)
	std::thread(AcceptLoop, state).detach();
	Log(fmt::format("listening on 127.0.0.1:{}", state->port.load()));
#else
	Log(fmt::format("ready, no port: the game connects to 127.0.0.1:{} through a pair of sockets", VIRTUAL_PORT));
#endif
	return true;
}

uint16_t Port() {
	return g_state != nullptr ? g_state->port.load() : 0;
}

int OpenConnection() {
#if defined(_WIN32)
	return -1;
#else
	if (g_state == nullptr) return -1;
	int ends[2] = {-1, -1};
#if defined(__APPLE__)
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) return -1;
	for (const int fd: ends) {
		const int on = 1;
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
	}
#else
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) != 0) return -1;
#endif
	mbedtls_net_context client;
	mbedtls_net_init(&client);
	client.fd = ends[1];
	std::thread(ServeClient, g_state, client).detach();
	return ends[0];
#endif
}

void NoteServerPickerQuestion(const ServerPickerQuestion& question) {
	if (g_state == nullptr) return;
	std::lock_guard lock(g_state->picker_mutex);
	g_state->picker_question = question;
}

std::string PatchPlanJson(const std::string& version, const std::string& process) {
	if (g_state == nullptr) return {};
	return MakePatchPlan(g_state->title_id, version, process, g_state->ca_modulus);
}

bool SignedInForTitle() {
	return g_state != nullptr && g_state->signed_in.load();
}

} // namespace Libs::EaEmulation
