#include "daemon/verification/runner.h"

#include "daemon/process/process.h"
#include "daemon/repository/git.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <string_view>
#include <stdexcept>

namespace aegis::daemon::verification {
namespace {
constexpr std::size_t maxVerificationOutput = 1024 * 1024;
constexpr std::string_view truncatedMessage = "\n...[verification output truncated]";
}

VerificationRun run(const std::vector<std::string> &command,
                    const std::filesystem::path &directory,
                    std::string taskId,
                    std::optional<std::string> runId) {
    if (command.empty()) throw std::invalid_argument("verification command requires an executable");
    const auto &executable = command.front();
    if (executable.empty() || executable.find('\0') != std::string::npos ||
        std::all_of(executable.begin(), executable.end(), [](unsigned char c) { return std::isspace(c); }))
        throw std::invalid_argument("verification executable must not be blank");
    for (const auto &argument : command)
        if (argument.find('\0') != std::string::npos)
            throw std::invalid_argument("verification arguments must not contain NUL");

    const auto isCtest = std::filesystem::path(executable).filename() == "ctest";
    std::vector<std::string> executed;
    executed.reserve(command.size());
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (isCtest && i > 0) {
            if (command[i].rfind("--no-tests=", 0) == 0) continue;
            if (command[i] == "--no-tests" && i + 1 < command.size()) { ++i; continue; }
        }
        executed.push_back(command[i]);
    }
    if (isCtest)
        executed.push_back("--no-tests=error");

    static std::atomic_uint64_t sequence{0};
    repository::GitRepository repository(directory);
    const auto before = repository.verificationFingerprint();

    const auto started = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    auto result = process::run(executed, directory, std::chrono::seconds(120), maxVerificationOutput);
    if (result.outputTruncated) result.output.append(truncatedMessage);
    const auto finished = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();

    const auto after = repository.verificationFingerprint();
    std::optional<std::string> fingerprint;
    std::string freshness = "unknown";
    if (before && after && *before == *after) {
        fingerprint = before;
        freshness = "current";
    }
    return {"verification-" + std::to_string(started) + "-" + std::to_string(++sequence),
            std::move(taskId), std::move(runId), executed,
            result.timedOut ? 124 : result.exitCode, result.output, started, finished,
            std::move(fingerprint), std::move(freshness)};
}

}
