#pragma once

#include "daemon/repository/git.h"
#include "daemon/repository/cache.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace aegis::daemon::repository {

enum class FileSource { head, index, worktree, commit };
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
    std::optional<std::size_t> nextOffset;
};

struct FileContent {
    FileSource source = FileSource::worktree;
    std::optional<std::string> revision;
    std::uint64_t size = 0;
    bool exists = false;
    bool binary = false;
    bool truncated = false;
    std::string content;
    std::string cacheIdentity;
};

struct FileComparison {
    std::string path;
    std::optional<std::string> oldPath;
    std::optional<std::string> parentCommit;
    std::optional<std::string> commit;
    std::string status;
    FileContent original;
    FileContent modified;
    bool binary = false;
    bool truncated = false;
};

class RepositoryIndex;

class Files final {
public:
    explicit Files(const GitRepository &git, bool backgroundIndex = false);
    ~Files();
    RepositoryIndex &index() const { return *index_; }
    struct CacheStats { ReviewCache<FileContent>::Stats contents; ReviewCache<FileComparison>::Stats comparisons; };
    CacheStats cacheStats() const { return {contents_.stats(), comparisons_.stats()}; }

    FileListing list(const std::string &path, FileScope scope, std::size_t limit = 500,
                     bool recursive = false, bool includeChanges = true, std::size_t offset = 0) const;
    FileContent read(const std::string &path, FileSource source, bool loadLarge = false) const;
    FileComparison compare(const std::string &path,
                           FileSource base = FileSource::head,
                           FileSource target = FileSource::worktree,
                           bool loadLarge = false) const;
    FileComparison compareCommit(const std::string &commit, const std::string &path,
                                 bool loadLarge = false) const;

private:
    std::string objectId(const std::string &path, FileSource source) const;
    std::string objectIdAt(const std::string &path, const std::string &revision) const;
    FileContent readWorktree(const std::string &path, bool loadLarge) const;
    FileContent readObject(const std::string &path, FileSource source, bool loadLarge) const;
    FileContent readCommitObject(const std::string &path, const std::optional<std::string> &revision,
                                 bool loadLarge) const;
    FileContent readBlob(const std::string &oid, FileSource source, std::optional<std::string> revision,
                         bool loadLarge) const;

    const GitRepository &git_;
    std::unique_ptr<RepositoryIndex> index_;
    mutable ReviewCache<FileContent> contents_;
    mutable ReviewCache<std::string> objects_{1024 * 1024, 4096};
    mutable ReviewCache<FileComparison> comparisons_;
};

std::string fileSourceName(FileSource source);
std::optional<FileSource> parseFileSource(const std::string &source);

}
