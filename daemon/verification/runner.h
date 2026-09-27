#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aegis::daemon::verification {

VerificationRun run(const std::vector<std::string> &command,
                    const std::filesystem::path &directory,
                    std::string taskId,
                    std::optional<std::string> runId = std::nullopt);

}
