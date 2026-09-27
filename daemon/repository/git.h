#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aegis::daemon::repository {

class GitRepository final {
public:
    explicit GitRepository(std::filesystem::path path);

    const std::filesystem::path &path() const;
    RepositoryState state() const;
    std::vector<GitChange> changes() const;
    std::vector<std::string> branches() const;
    std::string currentBranch() const;
    bool clean() const;
    std::string diff() const;
    bool stage(const std::string &path, std::string &error) const;
    bool unstage(const std::string &path, std::string &error) const;
    bool commit(const std::string &message, std::string &error) const;
    bool switchBranch(const std::string &branch, std::string &error) const;
    bool pull(std::string &output) const;
    bool push(std::string &output) const;

private:
    std::vector<std::string> command(std::vector<std::string> arguments) const;

    std::filesystem::path path_;
    std::filesystem::path gitDirectory_;
    bool validRepository_ = false;
};

}
