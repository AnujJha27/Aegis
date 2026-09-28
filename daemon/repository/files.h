#pragma once

#include "daemon/repository/git.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegis::daemon::repository {

enum class FileSource { head, index, worktree };
enum class FileScope { changed, all };

struct FileEntry {
    std::string path;
    std::string name;
    std::string kind;
    std::string language;
    std::uint64_t size = 0;
    bool changed = false;
    std::string gitStatus;
    std::optional<std::string> oldPath;
    int additions = 0;
    int deletions = 0;
    bool binary = false;
};

struct FileListing {
    std::vector<FileEntry> entries;
    bool truncated = false;
};

struct FileContent {
    FileSource source = FileSource::worktree;
    std::uint64_t size = 0;
    bool exists = false;
    bool binary = false;
    bool truncated = false;
    std::string content;
};

struct FileComparison {
    std::string path;
    std::optional<std::string> oldPath;
    std::string status;
    FileContent original;
    FileContent modified;
    bool binary = false;
    bool truncated = false;
};

class Files final {
public:
    explicit Files(const GitRepository &git);

    FileListing list(const std::string &path, FileScope scope, std::size_t limit = 500,
                     bool recursive = false, bool includeChanges = true) const;
    FileContent read(const std::string &path, FileSource source, bool loadLarge = false) const;
    FileComparison compare(const std::string &path,
                           FileSource base = FileSource::head,
                           FileSource target = FileSource::worktree,
                           bool loadLarge = false) const;

private:
    std::vector<std::string> gitPaths(const std::string &prefix, bool &truncated) const;
    std::string objectId(const std::string &path, FileSource source) const;
    FileContent readWorktree(const std::string &path, bool loadLarge) const;
    FileContent readObject(const std::string &path, FileSource source, bool loadLarge) const;

    const GitRepository &git_;
};

std::string fileSourceName(FileSource source);
std::optional<FileSource> parseFileSource(const std::string &source);

}
