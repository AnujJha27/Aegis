#pragma once

#include "daemon/domain/types.h"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aegis::daemon::repository {

class Files;

struct CommitSummary {
    std::string id;
    std::optional<std::string> parentId;
    std::string author;
    std::int64_t timestamp = 0;
    std::string subject;
};

struct CommitFile {
    std::string path;
    std::optional<std::string> oldPath;
    std::string status;
};

class GitRepository final {
public:
    explicit GitRepository(std::filesystem::path path);

    const std::filesystem::path &path() const;
    RepositoryState state() const;
    std::vector<GitChange> changes(std::optional<std::size_t> limit = std::nullopt) const;
    std::vector<CommitSummary> commits(std::size_t limit = 50) const;
    std::optional<CommitSummary> findCommit(const std::string &id) const;
    std::vector<CommitFile> commitFiles(const std::string &id) const;
    std::vector<std::string> branches() const;
    std::string currentBranch() const;
    bool clean() const;
    std::string diff(std::size_t maxOutputBytes = 1024 * 1024) const;
    bool stage(const std::string &path, std::string &error) const;
    bool unstage(const std::string &path, std::string &error) const;
    bool commit(const std::string &message, std::string &error) const;
    bool switchBranch(const std::string &branch, std::string &error) const;
    bool pull(std::string &output) const;
    bool push(std::string &output) const;

private:
    friend class Files;
    std::vector<std::string> command(std::vector<std::string> arguments) const;
    std::optional<GitChange> change(const std::string &path) const;

    std::filesystem::path path_;
    std::filesystem::path gitDirectory_;
    bool validRepository_ = false;
};

}
