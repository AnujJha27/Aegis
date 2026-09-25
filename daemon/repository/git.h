#pragma once

#include "daemon/domain/types.h"

#include <filesystem>
#include <string>

namespace aegis::daemon::repository {

class GitRepository final {
public:
    explicit GitRepository(std::filesystem::path path);

    const std::filesystem::path &path() const;
    RepositoryState state() const;
    std::string diff() const;

private:
    std::filesystem::path path_;
};

}
