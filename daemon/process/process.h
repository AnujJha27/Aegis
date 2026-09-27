#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <sys/types.h>

namespace aegis::daemon::process {

struct Result {
    int exitCode = -1;
    std::string output;
    bool timedOut = false;
};

Result run(const std::vector<std::string> &arguments,
           const std::filesystem::path &directory,
           std::chrono::seconds timeout = std::chrono::seconds(30));

class ChildProcess final {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    bool start(const std::vector<std::string> &arguments, const std::filesystem::path &directory);
    Result wait(std::chrono::seconds timeout,
                const std::function<void(std::string_view)> &onOutput = {},
                bool captureOutput = true);
    void interrupt();
    void terminate();

private:
    std::mutex mutex_;
    pid_t pid_ = -1;
    int output_ = -1;
    bool interruptRequested_ = false;
    bool terminationRequested_ = false;
};

}
