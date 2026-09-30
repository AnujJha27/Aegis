#include "daemon/app.h"

#include <csignal>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;

void stop(int) {
    stopping = 1;
}

std::string argument(int argc, char **argv, const std::string &name, std::string fallback = {}) {
    for (int index = 1; index + 1 < argc; ++index)
        if (argv[index] == name) return argv[index + 1];
    return fallback;
}

bool hasArgument(int argc, char **argv, const std::string &name) {
    for (int index = 1; index < argc; ++index) if (argv[index] == name) return true;
    return false;
}

}

int main(int argc, char **argv) {
    const auto repository = argument(argc, argv, "--repo", ".");
    const auto webRoot = argument(argc, argv, "--web-root");
    const auto portText = argument(argc, argv, "--port", "0");
    const auto port = static_cast<std::uint16_t>(std::strtoul(portText.c_str(), nullptr, 10));
    const bool managed = hasArgument(argc, argv, "--managed");
    if (!std::filesystem::is_directory(repository)) {
        std::cerr << "aegis_daemon: repository is not a directory\n";
        return 2;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        aegis::daemon::App app(repository, webRoot);
        if (!app.start(port)) {
            std::cerr << "aegis_daemon: failed to bind 127.0.0.1\n";
            return 1;
        }
        std::clog << "aegis_daemon: listening at 127.0.0.1:" << app.port() << " for " << std::filesystem::absolute(repository).lexically_normal().string() << '\n';
        std::cout << "http://127.0.0.1:" << app.port() << "/\n" << std::flush;
        bool browserConnected = false;
        auto disconnectedAt = std::chrono::steady_clock::time_point{};
        while (!stopping) {
            if (managed) {
                if (app.activeEventClients() > 0) {
                    browserConnected = true;
                    disconnectedAt = {};
                } else if (browserConnected || app.hasHadEventClient()) {
                    browserConnected = true;
                    if (disconnectedAt == std::chrono::steady_clock::time_point{}) disconnectedAt = std::chrono::steady_clock::now();
                    if (std::chrono::steady_clock::now() - disconnectedAt > std::chrono::seconds(3)) break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::clog << "aegis_daemon: shutting down\n";
    } catch (const std::exception &error) {
        std::cerr << "aegis_daemon: " << error.what() << '\n';
        return 1;
    }
}
