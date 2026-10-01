#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "common/logging/log.h"
#include "common/profiler.h"
#include "libs/debugSnapshots.h"
#include "loader/systemContent.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>

// Serves the launcher's debug-tools windows. Each connection carries one request line
// ("threads\n", "memread 0x1000 256\n", ...) and gets back one JSON document followed by a
// newline. Only the loopback interface is bound.

namespace Libs::DebugServer {

namespace {

#if defined(_WIN32)
using SocketHandle                     = SOCKET;
constexpr SocketHandle INVALID_HANDLE = INVALID_SOCKET;
void                   CloseHandle(SocketHandle s) { ::closesocket(s); }
#else
using SocketHandle                     = int;
constexpr SocketHandle INVALID_HANDLE = -1;
void                   CloseHandle(SocketHandle s) { ::close(s); }
#endif

const auto g_start_time = std::chrono::steady_clock::now();

nlohmann::json Ping() {
	std::string title_id;
	std::string title;
	(void)Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id);
	(void)Loader::SystemContentParamSfoGetString("TITLE", &title);
	const auto uptime =
	    std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start_time).count();
	return {{"title_id", title_id}, {"title", title}, {"uptime", uptime}};
}

nlohmann::json SelectRenderTarget(uint32_t format, uint32_t index) {
	// The swapchain polls this file; "0 0" returns to the normal scan-out image.
	const char* env_path = std::getenv("KYTY_DEBUG_RT_FILE");
	const char* path     = env_path != nullptr ? env_path : "rt_select.txt";
	if (FILE* f = std::fopen(path, "w"); f != nullptr) {
		std::fprintf(f, "%u %u\n", format, index);
		(void)std::fclose(f);
		return {{"ok", true}};
	}
	return {{"ok", false}, {"error", "cannot write render-target selection file"}};
}

nlohmann::json Dispatch(const std::string& line) {
	std::istringstream in(line);
	std::string        command;
	in >> command;
	if (command == "ping") {
		return Ping();
	}
	if (command == "memory") {
		return LibKernel::Memory::DebugMemorySnapshot();
	}
	if (command == "memread") {
		std::string address_text;
		uint32_t    size = 0;
		in >> address_text >> size;
		const auto address = std::strtoull(address_text.c_str(), nullptr, 0);
		return LibKernel::Memory::DebugMemoryRead(address, size);
	}
	if (command == "threads") {
		return LibKernel::DebugThreadSnapshot();
	}
	if (command == "audio") {
		return Audio::DebugAudioSnapshot();
	}
	if (command == "gpu") {
		return Graphics::DebugGpuSnapshot();
	}
	if (command == "rtselect") {
		uint32_t format = 0;
		uint32_t index  = 0;
		in >> format >> index;
		return SelectRenderTarget(format, index);
	}
	if (command == "imports") {
		return Loader::DebugImportSnapshot();
	}
	return {{"error", "unknown command: " + command}};
}

void Serve(SocketHandle client) {
	// A request is one short line; give a stalled client a second before dropping it.
	std::string line;
	char        buffer[512];
	const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (line.find('\n') == std::string::npos && line.size() < 4096 &&
	       std::chrono::steady_clock::now() < deadline) {
		fd_set read_set;
		FD_ZERO(&read_set);
		FD_SET(client, &read_set);
		timeval timeout {0, 100000};
		if (::select(static_cast<int>(client + 1), &read_set, nullptr, nullptr, &timeout) <= 0) {
			continue;
		}
		const auto received = ::recv(client, buffer, sizeof(buffer), 0);
		if (received <= 0) {
			break;
		}
		line.append(buffer, static_cast<size_t>(received));
	}
	if (const auto end = line.find_first_of("\r\n"); end != std::string::npos) {
		line.resize(end);
	}
	std::string reply;
	try {
		reply = Dispatch(line).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	} catch (const std::exception& e) {
		reply = nlohmann::json({{"error", e.what()}}).dump();
	}
	reply += '\n';
	for (size_t sent = 0; sent < reply.size();) {
		const auto n = ::send(client, reply.data() + sent, static_cast<int>(reply.size() - sent), 0);
		if (n <= 0) {
			break;
		}
		sent += static_cast<size_t>(n);
	}
	CloseHandle(client);
}

void Listen(uint16_t port) {
	KYTY_PROFILER_THREAD("DebugServer");
	const SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_HANDLE) {
		LOGF("DebugServer: socket() failed\n");
		return;
	}
	int reuse = 1;
	(void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
	                   sizeof(reuse));
	sockaddr_in address {};
	address.sin_family      = AF_INET;
	address.sin_port        = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
	    ::listen(listener, 8) != 0) {
		LOGF("DebugServer: cannot listen on 127.0.0.1:%u; debug tools stay offline\n",
		     static_cast<unsigned>(port));
		CloseHandle(listener);
		return;
	}
	LOGF("DebugServer: listening on 127.0.0.1:%u\n", static_cast<unsigned>(port));
	for (;;) {
		const SocketHandle client = ::accept(listener, nullptr, nullptr);
		if (client == INVALID_HANDLE) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		Serve(client);
	}
}

} // namespace

void Start(uint16_t port) {
	if (port == 0) {
		return;
	}
#if defined(_WIN32)
	WSADATA wsa {};
	if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		LOGF("DebugServer: WSAStartup failed\n");
		return;
	}
#endif
	// Lives for the whole process; the emulator exits through quick_exit, so no join is needed.
	std::thread(Listen, port).detach();
}

} // namespace Libs::DebugServer
