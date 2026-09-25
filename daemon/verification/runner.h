#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aegis::daemon::verification {

VerificationRun run(const std::vector<std::string> &command, const std::filesystem::path &directory);

}
