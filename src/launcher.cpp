#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

namespace {

volatile std::sig_atomic_t stopping = 0;

void requestStop(int) { stopping = 1; }

std::string selfPath(const char *fallback) {
    char buffer[4096];
    const auto length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0) return std::string(buffer, static_cast<std::size_t>(length));
    return std::filesystem::absolute(fallback).string();
}

std::string findExecutable(const std::string &name) {
    if (name.find('/') != std::string::npos)
        return access(name.c_str(), X_OK) == 0 ? name : std::string{};
    const char *path = std::getenv("PATH");
    if (!path) return {};
    std::string paths(path);
    std::size_t start = 0;
    while (start <= paths.size()) {
        const auto end = paths.find(':', start);
        const auto directory = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto candidate = (directory.empty() ? "." : directory) + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

std::string companion(const std::string &name, const std::string &executable) {
    const auto sibling = (std::filesystem::path(executable).parent_path() / name).string();
    if (access(sibling.c_str(), X_OK) == 0) return sibling;
    return findExecutable(name);
}

std::string frontendRoot(const std::string &repository, const std::string &executable) {
    std::vector<std::filesystem::path> candidates{
        std::filesystem::path(executable).parent_path() / "web" / "dist",
        std::filesystem::current_path() / "web" / "dist",
        std::filesystem::path(repository) / "web" / "dist"};
#ifdef AEGIS_SOURCE_WEB_ROOT
    candidates.emplace_back(AEGIS_SOURCE_WEB_ROOT);
#endif
    for (const auto &candidate : candidates)
        if (std::filesystem::is_regular_file(candidate / "index.html")) return candidate.string();
    return {};
}

bool spawnDetached(const std::string &executable, const std::vector<std::string> &arguments) {
    const auto first = fork();
    if (first < 0) return false;
    if (first == 0) {
        setsid();
        const auto second = fork();
        if (second < 0) _exit(127);
        if (second > 0) _exit(0);
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO); }
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(executable.c_str()));
        for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(first, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool openBrowser(const std::string &url) {
    if (std::getenv("WSL_DISTRO_NAME")) {
        const auto wslview = findExecutable("wslview");
        if (!wslview.empty() && spawnDetached(wslview, {url})) return true;
        const auto command = findExecutable("cmd.exe");
        return !command.empty() && spawnDetached(command, {"/c", "start", "", url});
    }
    const auto opener = findExecutable("xdg-open");
    return !opener.empty() && spawnDetached(opener, {url});
}

std::string startDaemon(const std::string &executable,
                        const std::string &repository,
                        const std::string &webRoot,
                        pid_t &pid) {
    int output[2];
    if (pipe(output) != 0) return {};
    pid = fork();
    if (pid < 0) { close(output[0]); close(output[1]); return {}; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(output[1], STDOUT_FILENO);
        close(output[0]); close(output[1]);
        execl(executable.c_str(), executable.c_str(), "--repo", repository.c_str(), "--port", "0", "--web-root", webRoot.c_str(), "--managed", nullptr);
        _exit(127);
    }
    close(output[1]);
    const auto flags = fcntl(output[0], F_GETFL, 0);
    if (flags >= 0) fcntl(output[0], F_SETFL, flags | O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string received;
    char buffer[2048];
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{output[0], POLLIN | POLLHUP, 0};
        (void)poll(&descriptor, 1, 100);
        for (;;) {
            const auto count = read(output[0], buffer, sizeof(buffer));
            if (count <= 0) break;
            received.append(buffer, static_cast<std::size_t>(count));
            const auto newline = received.find('\n');
            if (newline != std::string::npos) {
                const auto url = received.substr(0, newline);
                close(output[0]);
                if (url.starts_with("http://127.0.0.1:")) return url;
                break;
            }
        }
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) { pid = -1; break; }
    }
    close(output[0]);
    return {};
}

void stopDaemon(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

int directDaemon(const std::string &daemon, int argc, char **argv) {
    std::vector<char *> forwarded;
    forwarded.push_back(const_cast<char *>(daemon.c_str()));
    for (int index = 1; index < argc; ++index)
        if (std::string_view(argv[index]) != "--daemon") forwarded.push_back(argv[index]);
    forwarded.push_back(nullptr);
    execv(daemon.c_str(), forwarded.data());
    std::cerr << "aegis: could not execute aegis_daemon: " << std::strerror(errno) << '\n';
    return 1;
}

}

int main(int argc, char **argv) {
    const auto executable = selfPath(argv[0]);
    const auto daemon = companion("aegis_daemon", executable);
    if (argc > 1 && std::string_view(argv[1]) == "--daemon") {
        if (daemon.empty()) { std::cerr << "aegis: aegis_daemon was not found beside the executable or on PATH\n"; return 1; }
        return directDaemon(daemon, argc, argv);
    }
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        std::cout << "Usage: aegis [repository]\n       aegis --daemon --repo <path> --port <port> --web-root <path>\n       aegis --legacy-ui [repository] (with AEGIS_BUILD_LEGACY=ON)\n";
        return 0;
    }

    std::string repository = ".";
    bool explicitRepository = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--legacy-ui") {
            const auto legacy = companion("aegis_legacy", executable);
            if (legacy.empty()) { std::cerr << "aegis: legacy Qt prototype was not built; configure with -DAEGIS_BUILD_LEGACY=ON\n"; return 2; }
            std::vector<char *> forwarded{const_cast<char *>(legacy.c_str())};
            for (int i = 1; i < argc; ++i) forwarded.push_back(argv[i]);
            forwarded.push_back(nullptr);
            execv(legacy.c_str(), forwarded.data());
            std::cerr << "aegis: could not start legacy UI\n";
            return 1;
        }
        if (argument == "--agent") { if (index + 1 < argc) ++index; continue; }
        if (argument == "--paranoia") {
            std::cerr << "aegis: --paranoia is available only in the legacy Qt prototype\n";
            return 2;
        }
        if (!argument.empty() && argument.front() == '-') {
            std::cerr << "aegis: unknown option " << argument << '\n';
            return 2;
        }
        if (!explicitRepository) { repository = argument; explicitRepository = true; }
    }
    const auto absoluteRepository = std::filesystem::absolute(repository).lexically_normal();
    if (!std::filesystem::is_directory(absoluteRepository)) {
        std::cerr << "aegis: not a repository directory: " << absoluteRepository.string() << '\n';
        return 2;
    }
    const auto webRoot = frontendRoot(absoluteRepository.string(), executable);
    if (daemon.empty() || webRoot.empty()) {
        std::cerr << "aegis: daemon or frontend bundle not found; build aegis and run npm run build in web first\n";
        return 1;
    }

    pid_t daemonPid = -1;
    const auto url = startDaemon(daemon, absoluteRepository.string(), webRoot, daemonPid);
    if (url.empty()) {
        std::cerr << "aegis: local daemon did not become ready\n";
        stopDaemon(daemonPid);
        return 1;
    }
    std::cout << url << '\n' << std::flush;
    if (!openBrowser(url)) std::cerr << "aegis: browser could not be opened; use " << url << '\n';

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    int status = 0;
    while (!stopping) {
        const auto result = waitpid(daemonPid, &status, WNOHANG);
        if (result == daemonPid) return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (result < 0 && errno != EINTR) return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    stopDaemon(daemonPid);
    return 0;
}
