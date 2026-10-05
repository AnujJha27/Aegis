#include "daemon/repository/index.h"
#include "daemon/process/process.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <queue>
#include <stdexcept>

namespace aegis::daemon::repository {
namespace {
using Clock = std::chrono::steady_clock;
bool excluded(std::string_view path) {
    for (const auto &part : std::filesystem::path(path))
        if (part == ".git" || part == ".aegis" || part == ".aegis-git" || part == "node_modules") return true;
    return false;
}
std::string lower(std::string_view value) {
    std::string result(value);
    for (auto &c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}
int score(const RepositoryIndex::Entry &entry, std::string_view query, std::string_view folded) {
    const auto &path = entry.lowerPath;
    if (folded.empty()) return -static_cast<int>(path.size());
    std::size_t cursor = 0;
    for (char c : folded) {
        cursor = path.find(c, cursor);
        if (cursor == std::string::npos) return -1000000;
        ++cursor;
    }
    const auto end = cursor;
    int result = 0;
    std::size_t previous = path.size() + 1;
    for (std::size_t i = folded.size(); i-- > 0;) {
        cursor = path.rfind(folded[i], cursor - 1);
        if (cursor + 1 == previous) result += 12;
        if (cursor >= entry.nameOffset) result += 8;
        if (cursor == 0 || path[cursor - 1] == '/' || path[cursor - 1] == '_' || path[cursor - 1] == '-') result += 15;
        if (cursor > 0 && std::islower(static_cast<unsigned char>(entry.file.path[cursor - 1])) && std::isupper(static_cast<unsigned char>(entry.file.path[cursor]))) result += 12;
        if (entry.file.path[cursor] == query[i]) result += 2;
        previous = cursor;
    }
    if (std::string_view(path).substr(entry.nameOffset).find(folded) != std::string_view::npos) result += 80;
    result -= static_cast<int>(end - cursor - folded.size());
    result -= static_cast<int>(path.size() / 4 + std::count(path.begin(), path.end(), '/') * 3);
    return result;
}
}
RepositoryIndex::RepositoryIndex(const GitRepository &git, bool background) : git_(git), snapshot_(std::make_shared<Snapshot>()) {
    if (background) requestRebuild(); else rebuild();
}
RepositoryIndex::~RepositoryIndex() { worker_.request_stop(); wake_.notify_all(); }
void RepositoryIndex::requestRebuild() {
    std::lock_guard lock(workerMutex_);
    requested_ = true;
    if (!worker_.joinable()) {
    worker_ = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::unique_lock lock(workerMutex_);
            if (!wake_.wait(lock, stop, [this] { return requested_; })) break;
            requested_ = false;
            lock.unlock();
            try { rebuild(); }
            catch (...) {
                auto failed = std::make_shared<Snapshot>(*snapshot());
                failed->indexing = false;
                failed->error = "Repository indexing failed; retry on the next repository change.";
                snapshot_.store(std::move(failed));
            }
        }
    });
    }
    wake_.notify_one();
}
void RepositoryIndex::rebuild() {
    std::lock_guard buildLock(buildMutex_); // Only builders serialize; readers never wait for Git or disk.
    building_.store(true);
    struct Reset { std::atomic_bool &value; ~Reset() { value.store(false); } } reset{building_};
    const auto started = Clock::now();
    auto next = std::make_shared<Snapshot>();
    next->version = snapshot()->version + 1;
    auto rootListing = [&](const std::string &output) {
        std::map<std::string, FileEntry> root;
        for (std::size_t start = 0; start < output.size();) {
            const auto end = output.find('\0', start);
            if (end == std::string::npos) break;
            const auto path = output.substr(start, end - start);
            start = end + 1;
            if (excluded(path)) continue;
            const auto slash = path.find('/');
            auto name = path.substr(0, slash);
            FileEntry entry;
            entry.path = entry.name = name;
            entry.kind = slash == std::string::npos ? "file" : "directory";
            entry.language = entry.kind == "file" ? fileLanguage(path) : "";
            root.try_emplace(name, std::move(entry));
        }
        for (auto &[_, entry] : root) next->children[""].push_back(std::move(entry));
    };
    if (snapshot()->version == 0 && git_.validRepository_) {
        // Bootstrap only: Git collapses wholly-untracked directories without enumerating their contents.
        const auto roots = process::run(git_.command({"ls-files", "--cached", "--others", "--exclude-standard", "--directory", "--no-empty-directory", "-z"}), git_.path());
        if (roots.exitCode != 0) throw std::runtime_error("repository root enumeration failed");
        rootListing(roots.output);
        next->rootMicros = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
        snapshot_.store(std::make_shared<Snapshot>(*next));
        next->children.clear();
    }
    std::vector<std::string> paths;
    if (git_.validRepository_) {
        auto result = process::run(git_.command({"ls-files", "--cached", "--others", "--exclude-standard", "-z"}), git_.path());
        if (result.exitCode != 0) throw std::runtime_error("repository enumeration failed");
        next->truncated = result.outputTruncated;
        for (std::size_t start = 0; start < result.output.size();) {
            const auto end = result.output.find('\0', start);
            if (end == std::string::npos) break;
            auto path = result.output.substr(start, end - start);
            start = end + 1;
            if (excluded(path)) continue;
            paths.push_back(std::move(path));
            if (paths.size() == 500000) { next->truncated = true; break; }
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    next->files.reserve(paths.size());
    std::map<std::string, FileEntry> directories;
    for (auto &path : paths) {
        Entry indexed;
        indexed.lowerPath = lower(path);
        indexed.nameOffset = path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1;
        auto &file = indexed.file;
        file.path = std::move(path);
        file.name = file.path.substr(indexed.nameOffset);
        file.kind = "file";
        file.language = fileLanguage(file.path);
        std::error_code error;
        const auto info = std::filesystem::symlink_status(git_.path() / file.path, error);
        if (!error && std::filesystem::is_symlink(info)) file.kind = "symlink";
        else if (!error && std::filesystem::is_regular_file(info)) {
            auto size = std::filesystem::file_size(git_.path() / file.path, error);
            if (!error) file.size = size;
        }
        auto parent = file.path.substr(0, indexed.nameOffset ? indexed.nameOffset - 1 : 0);
        next->children[parent].push_back(file);
        while (!parent.empty()) {
            if (!directories.contains(parent)) {
                FileEntry dir;
                dir.path = parent;
                const auto slash = parent.rfind('/');
                dir.name = parent.substr(slash == std::string::npos ? 0 : slash + 1);
                dir.kind = "directory";
                directories.emplace(parent, std::move(dir));
            }
            const auto slash = parent.rfind('/');
            parent = slash == std::string::npos ? "" : parent.substr(0, slash);
        }
        next->files.push_back(std::move(indexed));
    }
    for (auto &[path, dir] : directories) {
        const auto slash = path.rfind('/');
        next->children[slash == std::string::npos ? "" : path.substr(0, slash)].push_back(std::move(dir));
    }
    for (auto &[_, children] : next->children) std::sort(children.begin(), children.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    next->indexing = false;
    next->buildMicros = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
    snapshot_.store(std::move(next));
}
RepositoryIndex::Search RepositoryIndex::search(std::string_view query, std::size_t limit) const {
    if (query.size() > 256) throw std::invalid_argument("quick-open query exceeds 256 bytes");
    limit = std::min(limit, std::size_t{100});
    const auto started = Clock::now();
    const auto view = snapshot();
    const auto folded = lower(query);
    using Hit = std::pair<int, std::size_t>;
    std::priority_queue<Hit, std::vector<Hit>, std::greater<>> best;
    for (std::size_t i = 0; i < view->files.size(); ++i) {
        const auto rank = score(view->files[i], query, folded);
        if (rank == -1000000 || !limit) continue;
        const Hit hit{rank, view->files.size() - i};
        if (best.size() < limit) best.push(hit);
        else if (hit > best.top()) { best.pop(); best.push(hit); }
    }
    Search result;
    result.version = view->version;
    result.indexing = indexing();
    while (!best.empty()) { result.entries.push_back(view->files[view->files.size() - best.top().second].file); best.pop(); }
    std::reverse(result.entries.begin(), result.entries.end());
    result.micros = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
    return result;
}
}
