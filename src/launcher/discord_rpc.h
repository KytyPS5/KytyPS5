#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

class DiscordRPC {
public:
    static void UpdatePresence([[maybe_unused]] const std::string& gameTitle, [[maybe_unused]] const std::string& titleId) {
#ifdef _WIN32
        HANDLE hPipe = INVALID_HANDLE_VALUE;

        for (int i = 0; i < 10; ++i) {
            std::string pipePath = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
            hPipe = CreateFileA(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hPipe != INVALID_HANDLE_VALUE) {
                break;
            }
        }

        if (hPipe == INVALID_HANDLE_VALUE) {
            return;
        }

        auto safeWrite = [](HANDLE pipe, const void* data, DWORD size) -> bool {
            DWORD written = 0;
            return WriteFile(pipe, data, size, &written, NULL) && (written == size);
        };

        // Escape characters that would break the JSON payload (quotes, backslashes, control chars)
        auto jsonEscape = [](const std::string& s) -> std::string {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:   out += c;      break;
                }
            }
            return out;
        };

        // 1. Send Discord handshake (Opcode 0)
        // UPDATE CLIENT ID: replace the placeholder below with your real Discord
        // Application ID from https://discord.com/developers/applications
        // before this will actually work.
        std::string handshakeJson = "{\"v\":1,\"client_id\":\"1550565969708064858\"}"; // UPDATE CLIENT ID
        uint32_t handshakeOpcode = 0;
        uint32_t handshakeLength = static_cast<uint32_t>(handshakeJson.size());

        if (!safeWrite(hPipe, &handshakeOpcode, sizeof(handshakeOpcode)) ||
            !safeWrite(hPipe, &handshakeLength, sizeof(handshakeLength)) ||
            !safeWrite(hPipe, handshakeJson.c_str(), handshakeLength)) {
            CloseHandle(hPipe);
            return;
        }

        // 2. Send activity frame with process ID (Opcode 1)
        DWORD pid = GetCurrentProcessId();
        std::string safeGameTitle = jsonEscape(gameTitle);
        std::string safeTitleId = jsonEscape(titleId);

        std::string activityJson = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
                                    ",\"activity\":{\"details\":\"" + safeGameTitle + "\",\"state\":\"" + safeTitleId + "\"}},\"nonce\":\"1\"}";
        uint32_t frameOpcode = 1;
        uint32_t frameLength = static_cast<uint32_t>(activityJson.size());

        if (!safeWrite(hPipe, &frameOpcode, sizeof(frameOpcode)) ||
            !safeWrite(hPipe, &frameLength, sizeof(frameLength)) ||
            !safeWrite(hPipe, activityJson.c_str(), frameLength)) {
            CloseHandle(hPipe);
            return;
        }

        CloseHandle(hPipe);
#endif
    }
};
