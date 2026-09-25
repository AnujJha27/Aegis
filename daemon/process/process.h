#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace aegis::daemon::process {

struct Result {
    int exitCode = -1;
    std::string output;
    bool timedOut = false;
};

Result run(const std::vector<std::string> &arguments,
           const std::filesystem::path &directory,
           std::chrono::seconds timeout = std::chrono::seconds(30));

}
