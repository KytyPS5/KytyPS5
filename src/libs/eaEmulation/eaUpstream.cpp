#include "libs/eaEmulation/eaUpstream.h"

#include "common/logging/log.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <array>
#include <fmt/format.h>
#include <string>

namespace Libs::EaEmulation {

namespace {

constexpr size_t   HEADER_SIZE = 16;
constexpr uint32_t MAX_FRAME   = 1u << 22;

void Log(const std::string& text) {
	Log::WriteToConsoleAndLog(fmt::format("EA emulation upstream: {}\n", text));
}

std::string TlsError(int code) {
	std::array<char, 160> text {};
	mbedtls_strerror(code, text.data(), text.size());
	return fmt::format("{} (-0x{:04x})", text.data(), static_cast<unsigned>(-code));
}

// One TLS client connection.
struct Channel {
	mbedtls_entropy_context  entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_ssl_config       config;
	mbedtls_ssl_context      ssl;
	mbedtls_net_context      net;
	bool                     open = false;

	Channel() {
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		mbedtls_ssl_config_init(&config);
		mbedtls_ssl_init(&ssl);
		mbedtls_net_init(&net);
	}
	~Channel() {
		if (open) mbedtls_ssl_close_notify(&ssl);
		mbedtls_net_free(&net);
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&config);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
	}

	bool Connect(const std::string& host, const std::string& port) {
		int result = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
		if (result == 0) {
			result = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
			                                     MBEDTLS_SSL_PRESET_DEFAULT);
		}
		if (result == 0) {
			mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
			mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
			result = mbedtls_ssl_setup(&ssl, &config);
		}
		if (result == 0) result = mbedtls_ssl_set_hostname(&ssl, host.c_str());
		if (result == 0) result = mbedtls_net_connect(&net, host.c_str(), port.c_str(), MBEDTLS_NET_PROTO_TCP);
		if (result != 0) {
			Log(fmt::format("{}:{}: {}", host, port, TlsError(result)));
			return false;
		}
		mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, nullptr);
		for (;;) {
			result = mbedtls_ssl_handshake(&ssl);
			if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) break;
		}
		if (result != 0) {
			Log(fmt::format("{}:{}: TLS handshake: {}", host, port, TlsError(result)));
			return false;
		}
		open = true;
		return true;
	}

	bool Write(const uint8_t* data, size_t size) {
		size_t done = 0;
		while (done < size) {
			const int result = mbedtls_ssl_write(&ssl, data + done, size - done);
			if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
			if (result <= 0) return false;
			done += static_cast<size_t>(result);
		}
		return true;
	}

	bool Read(uint8_t* data, size_t size) {
		size_t done = 0;
		while (done < size) {
			const int result = mbedtls_ssl_read(&ssl, data + done, size - done);
			if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
			if (result <= 0) return false;
			done += static_cast<size_t>(result);
		}
		return true;
	}

	// Reads until the server closes the stream.
	std::string ReadToEnd() {
		std::string text;
		std::array<char, 2048> buffer {};
		for (;;) {
			const int result = mbedtls_ssl_read(&ssl, reinterpret_cast<unsigned char*>(buffer.data()), buffer.size());
			if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
			if (result <= 0) break;
			text.append(buffer.data(), static_cast<size_t>(result));
		}
		return text;
	}
};

std::string Between(const std::string& text, const std::string& open, const std::string& close) {
	const auto start = text.find(open);
	if (start == std::string::npos) return {};
	const auto end = text.find(close, start + open.size());
	if (end == std::string::npos) return {};
	return text.substr(start + open.size(), end - start - open.size());
}

} // namespace

struct Upstream::Impl {
	Channel channel;
};

Upstream::Upstream(): m_impl(std::make_unique<Impl>()) {}
Upstream::~Upstream() = default;

bool Upstream::Connect(const ServerPickerQuestion& question) {
	if (question.host.empty() || question.body.empty()) {
		Log("the game has not asked the server picker yet");
		return false;
	}
	std::string host;
	std::string port;
	{
		Channel picker;
		if (!picker.Connect(question.host, "443")) return false;
		const std::string request =
		    fmt::format("POST /redirector/getServerInstance HTTP/1.1\r\nHost: {}\r\nContent-Type: application/xml\r\n"
		                "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
		                question.host, question.body.size(), question.body);
		if (!picker.Write(reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
			Log("the server picker did not take the question");
			return false;
		}
		const auto answer = picker.ReadToEnd();
		host              = Between(answer, "<hostname>", "</hostname>");
		port              = Between(answer, "<port>", "</port>");
	}
	if (host.empty() || port.empty()) {
		Log("the server picker gave no server");
		return false;
	}
	Log(fmt::format("server {}:{}", host, port));
	return m_impl->channel.Connect(host, port);
}

bool Upstream::Exchange(const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
	auto& channel = m_impl->channel;
	if (!channel.Write(request.data(), request.size())) return false;
	reply.resize(HEADER_SIZE);
	if (!channel.Read(reply.data(), HEADER_SIZE)) return false;
	const uint32_t size = (uint32_t(reply[0]) << 24) | (uint32_t(reply[1]) << 16) | (uint32_t(reply[2]) << 8) | reply[3];
	if (size > MAX_FRAME) return false;
	reply.resize(HEADER_SIZE + size);
	return size == 0 || channel.Read(reply.data() + HEADER_SIZE, size);
}

} // namespace Libs::EaEmulation
