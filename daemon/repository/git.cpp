#include "daemon/repository/git.h"

#include "daemon/process/process.h"

#include <sstream>

namespace aegis::daemon::repository {

GitRepository::GitRepository(std::filesystem::path path) : path_(std::move(path)) {}

const std::filesystem::path &GitRepository::path() const {
    return path_;
}

RepositoryState GitRepository::state() const {
    RepositoryState result;
    result.path = path_.string();
    const auto status = process::run({"git", "status", "--short", "--branch"}, path_).output;
    const auto numstat = process::run({"git", "diff", "HEAD", "--numstat"}, path_).output;
    std::istringstream statusLines(status);
    std::string line;
    while (std::getline(statusLines, line)) {
        if (line.rfind("## ", 0) == 0) {
            result.branch = line.substr(3);
            const auto separator = result.branch.find("...");
            if (separator != std::string::npos) result.branch.resize(separator);
        } else if (line.size() >= 2) {
            ++result.files;
        }
    }
    std::istringstream numstatLines(numstat);
    while (std::getline(numstatLines, line)) {
        std::istringstream columns(line);
        int added = 0;
        int removed = 0;
        if (columns >> added >> removed) {
            result.insertions += added;
            result.deletions += removed;
        }
    }
    return result;
}

std::string GitRepository::diff() const {
    return process::run({"git", "diff", "HEAD", "--no-ext-diff", "--no-color"}, path_).output;
}

}
