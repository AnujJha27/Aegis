#include "daemon/verification/runner.h"

#include "daemon/process/process.h"

#include <chrono>

namespace aegis::daemon::verification {

VerificationRun run(const std::vector<std::string> &command, const std::filesystem::path &directory) {
    const auto started = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    const auto result = process::run(command, directory, std::chrono::seconds(120));
    const auto finished = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    std::string commandLine;
    for (const auto &part : command) {
        if (!commandLine.empty()) commandLine += ' ';
        commandLine += part;
    }
    return {"verification-" + std::to_string(started), commandLine, result.timedOut ? 124 : result.exitCode, result.output, started, finished};
}

}
