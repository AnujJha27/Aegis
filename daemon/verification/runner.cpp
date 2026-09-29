#include "daemon/verification/runner.h"

#include "daemon/process/process.h"

#include <chrono>
#include <cstddef>
#include <string_view>

namespace aegis::daemon::verification {
namespace {
constexpr std::size_t maxVerificationOutput = 1024 * 1024;
constexpr std::string_view truncatedMessage = "\n...[verification output truncated]";
}

VerificationRun run(const std::vector<std::string> &command,
                    const std::filesystem::path &directory,
                    std::string taskId,
                    std::optional<std::string> runId) {
    const auto started = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    auto result = process::run(command, directory, std::chrono::seconds(120), maxVerificationOutput);
    if (result.outputTruncated) result.output.append(truncatedMessage);
    const auto finished = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    return {"verification-" + std::to_string(started), std::move(taskId), std::move(runId), command,
            result.timedOut ? 124 : result.exitCode, result.output, started, finished};
}

}
