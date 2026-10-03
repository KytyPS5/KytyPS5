// SPDX-License-Identifier: GPL-2.0-only
// Exercise the private transport with short deadlines, without a production test switch.
#include "common/pkgArchive.cpp"
#include <atomic>
#include <barrier>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
using namespace std::chrono_literals;

static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// The runner copies this executable as dotnet[.exe]. No .NET installation is used.
static int Helper(const std::string& mode) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#else
    signal(SIGTERM, SIG_IGN); // Cleanup must not wait forever on a stubborn helper.
#endif
    if (mode == "exit") return 0;
    if (mode == "partial") std::cout.write("KP", 2).flush();
    if (mode == "catalog" || mode == "healthy") {
        const char header[] = {'K','P','K','1',1,0,0,0};
        std::cout.write(header, sizeof(header)).flush();
        if (mode == "healthy") {
            // Root entry: empty name, zero size, directory.
            const char root[13] = {};
            std::cout.write(root, sizeof(root)).flush();
            return 0;
        }
    }
    std::this_thread::sleep_for(10s);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3) return Helper(argv[2]);
    try {
        for (const auto* mode : {"silent", "partial", "catalog"}) {
            const auto start = std::chrono::steady_clock::now();
            bool timed_out = false;
            try { Common::PkgArchiveBackend reader(mode, 500ms); }
            catch (const std::runtime_error& e) {
                timed_out = std::string(e.what()).find("timed out") != std::string::npos;
            }
            Check(timed_out, "stalled startup did not report timeout");
            Check(std::chrono::steady_clock::now() - start < 4s, "startup/cleanup exceeded bound");
        }
        // Failed startup must not poison a subsequent mount.
        Common::PkgArchiveBackend healthy("healthy", 2s);
        Check(healthy.Find("").has_value(), "healthy catalog failed after timeout");
        // Hold some helpers alive while others exit. EOF must not depend on peers exiting.
        std::barrier start(16);
        std::atomic<int> failures = 0;
        std::vector<std::thread> threads;
        for (int i = 0; i < 16; ++i) threads.emplace_back([&, i] {
            start.arrive_and_wait();
            try {
                Common::Pipe pipe;
                pipe.Open(i % 2 ? "silent" : "exit", 2s);
                if (i % 2) { std::this_thread::sleep_for(3s); return; }
                char byte;
                try { pipe.Read(&byte, 1); ++failures; }
                catch (const std::runtime_error& e) {
                    if (std::string(e.what()) != "Package helper closed the stream") ++failures;
                }
            } catch (...) { ++failures; }
        });
        for (auto& thread : threads) thread.join();
        Check(failures == 0, "concurrent helper startup failed or lost EOF");
        std::cout << "Startup tests passed: silent/partial/catalog timeout, cleanup, recovery, concurrent EOF\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
