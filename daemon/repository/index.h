#pragma once
#include "daemon/repository/files.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace aegis::daemon::repository {
class RepositoryIndex final {
public:
    struct Entry { FileEntry file; std::string lowerPath; std::size_t nameOffset = 0; };
    struct Snapshot {
        std::vector<Entry> files;
        std::unordered_map<std::string, std::vector<FileEntry>> children;
        std::uint64_t version = 0;
        bool indexing = true;
        bool truncated = false;
        std::string error;
        std::uint64_t rootMicros = 0;
        std::uint64_t buildMicros = 0;
    };
    struct Search { std::vector<FileEntry> entries; std::uint64_t version = 0; bool indexing = false; std::uint64_t micros = 0; };
    explicit RepositoryIndex(const GitRepository &git, bool background = false);
    ~RepositoryIndex();
    std::shared_ptr<const Snapshot> snapshot() const { return snapshot_.load(); }
    bool indexing() const { return building_.load(); }
    void rebuild();
    void requestRebuild();
    Search search(std::string_view query, std::size_t limit = 60) const;
private:
    std::atomic_bool building_ = true;
    const GitRepository &git_;
    std::atomic<std::shared_ptr<const Snapshot>> snapshot_;
    std::mutex buildMutex_, workerMutex_;
    std::condition_variable_any wake_;
    bool requested_ = false;
    std::jthread worker_;
};
std::string fileLanguage(const std::filesystem::path &path);
}
