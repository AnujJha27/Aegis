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
    std::string diff() const;
    bool stage(const std::string &path, std::string &error) const;
    bool unstage(const std::string &path, std::string &error) const;
    bool commit(const std::string &message, std::string &error) const;

private:
    std::filesystem::path path_;
};

}
