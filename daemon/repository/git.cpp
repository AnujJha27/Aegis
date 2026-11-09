#include "daemon/repository/git.h"

#include "daemon/process/process.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>

#include <boost/uuid/detail/sha1.hpp>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace aegis::daemon::repository {
namespace {

bool validPath(const std::string &value, std::string &error) {
    const std::filesystem::path path(value);
    if (value.empty() || value.find('\0') != std::string::npos || path.is_absolute() || value == ".") {
        error = "path must name a repository-relative file";
        return false;
    }
    for (const auto &component : path)
        if (component == "..") {
            error = "path must stay inside the repository";
            return false;
        }
    return true;
}

bool gitDirectory(const std::filesystem::path &path) {
    return std::filesystem::is_regular_file(path / "HEAD");
}

bool validGitEntry(const std::filesystem::path &root) {
    const auto entry = root / ".git";
    if (std::filesystem::is_directory(entry)) return gitDirectory(entry);
    if (!std::filesystem::is_regular_file(entry)) return false;
    std::ifstream input(entry);
    std::string line;
    std::getline(input, line);
    constexpr std::string_view prefix = "gitdir:";
    if (!line.starts_with(prefix)) return false;
    auto targetText = line.substr(prefix.size());
    while (!targetText.empty() && std::isspace(static_cast<unsigned char>(targetText.front()))) targetText.erase(targetText.begin());
    while (!targetText.empty() && std::isspace(static_cast<unsigned char>(targetText.back()))) targetText.pop_back();
    auto target = std::filesystem::path(targetText);
    if (target.is_relative()) target = root / target;
    return gitDirectory(target.lexically_normal());
}

bool validCommitId(const std::string &id) {
    return (id.size() == 40 || id.size() == 64) &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); });
}

void requireComplete(const process::Result &result, std::string_view operation) {
    if (result.outputTruncated) throw std::runtime_error(std::string(operation) + " output exceeded the 16 MiB capture limit");
}

std::vector<GitChange> parseChanges(std::string_view output, std::optional<std::size_t> limit = std::nullopt,
                                    bool *truncated = nullptr) {
    std::vector<GitChange> result;
    for (std::size_t offset = 0; offset + 3 <= output.size();) {
        const auto index = output[offset];
        const auto worktree = output[offset + 1];
        const auto pathStart = offset + 3;
        const auto pathEnd = output.find('\0', pathStart);
        if (pathEnd == std::string_view::npos) break;
        GitChange change;
        if (index == '#' && worktree == '#') {
            offset = pathEnd + 1;
            continue;
        }
        change.path = output.substr(pathStart, pathEnd - pathStart);
        change.indexStatus = std::string(1, index);
        change.worktreeStatus = std::string(1, worktree);
        offset = pathEnd + 1;
        if ((index == 'R' || index == 'C' || worktree == 'R' || worktree == 'C') && offset < output.size()) {
            const auto originalEnd = output.find('\0', offset);
            if (originalEnd == std::string_view::npos) break;
            change.oldPath = output.substr(offset, originalEnd - offset);
            offset = originalEnd + 1;
        }
        result.push_back(std::move(change));
        if (limit && result.size() > *limit) {
            result.resize(*limit);
            if (truncated) *truncated = true;
            break;
        }
    }
    return result;
}

std::optional<CommitSummary> parseCommitSummary(const std::string &line) {
    std::array<std::string_view, 5> fields;
    std::size_t start = 0;
    for (std::size_t index = 0; index + 1 < fields.size(); ++index) {
        const auto end = line.find('\x1f', start);
        if (end == std::string::npos) return std::nullopt;
        fields[index] = std::string_view(line).substr(start, end - start);
        start = end + 1;
    }
    fields.back() = std::string_view(line).substr(start);
    CommitSummary result;
    result.id = fields[0];
    if (!validCommitId(result.id)) return std::nullopt;
    const auto parentEnd = fields[1].find(' ');
    if (!fields[1].empty()) result.parentId = std::string(fields[1].substr(0, parentEnd));
    result.author = fields[2];
    const auto [end, error] = std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), result.timestamp);
    if (error != std::errc{} || end != fields[3].data() + fields[3].size()) return std::nullopt;
    result.subject = fields[4];
    return result;
}

std::optional<std::string> sha1Hex(boost::uuids::detail::sha1 &digest) {
    unsigned int value[5] = {0, 0, 0, 0, 0};
    digest.get_digest(value);
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(40);
    for (const auto word : value)
        for (int shift = 28; shift >= 0; shift -= 4) result.push_back(digits[(word >> shift) & 0xF]);
    return result;
}

// Opens relative under root without following symlinks in any ancestor or the final component.
int openWithin(const std::filesystem::path &root, const std::string &relative) {
    int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    std::size_t start = 0;
    while (start < relative.size()) {
        const auto slash = relative.find('/', start);
        const auto component = relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        const bool last = slash == std::string::npos;
        if (component.empty() || component == ".") {
            if (last) break;
            start = slash + 1;
            continue;
        }
        if (component == "..") {
            ::close(fd);
            return -1;
        }
        const auto next = ::openat(fd, component.c_str(),
            (last ? O_RDONLY | O_NONBLOCK : O_RDONLY | O_DIRECTORY) | O_NOFOLLOW | O_CLOEXEC);
        const int openErrno = errno;
        ::close(fd);
        if (next < 0) { errno = openErrno; return -1; }
        if (last) return next;
        fd = next;
        start = slash + 1;
    }
    return fd;
}

}

GitRepository::GitRepository(std::filesystem::path path)
    : path_(std::filesystem::absolute(std::move(path)).lexically_normal()) {
    for (auto candidate = path_; !candidate.empty(); candidate = candidate.parent_path()) {
        if (validGitEntry(candidate)) {
            path_ = candidate;
            validRepository_ = true;
            metadataDirectory_ = candidate / ".git";
            if (std::filesystem::is_regular_file(metadataDirectory_)) {
                std::ifstream file(metadataDirectory_); std::string line; std::getline(file, line);
                auto text = line.substr(7); text.erase(0, text.find_first_not_of(" "));
                metadataDirectory_ = std::filesystem::path(text);
                if (metadataDirectory_.is_relative()) metadataDirectory_ = candidate / metadataDirectory_;
            }
            commonDirectory_ = metadataDirectory_;
            std::ifstream common(metadataDirectory_ / "commondir"); std::string line;
            if (std::getline(common, line)) commonDirectory_ = (metadataDirectory_ / line).lexically_normal();
            return;
        }
        const auto aegisDirectory = candidate / ".aegis-git";
        if (gitDirectory(aegisDirectory)) {
            path_ = candidate;
            gitDirectory_ = aegisDirectory;
            metadataDirectory_ = commonDirectory_ = aegisDirectory;
            validRepository_ = true;
            return;
        }
        if (candidate == candidate.root_path()) break;
    }
}

std::vector<std::string> GitRepository::command(std::vector<std::string> arguments) const {
    std::vector<std::string> result{"git"};
    if (!gitDirectory_.empty()) {
        result.push_back("--git-dir=" + gitDirectory_.string());
        result.push_back("--work-tree=" + path_.string());
    }
    result.insert(result.end(), std::make_move_iterator(arguments.begin()), std::make_move_iterator(arguments.end()));
    return result;
}

const std::filesystem::path &GitRepository::path() const {
    return path_;
}

RepositoryState GitRepository::state() const {
    return snapshot().state;
}

GitRepository::Snapshot GitRepository::snapshot(bool refresh) const {
    std::unique_lock lock(snapshotMutex_);
    if (!refresh && snapshotValid_ && std::chrono::steady_clock::now() - snapshotAt_ < std::chrono::milliseconds(250)) return cachedSnapshot_;
    if (scanning_) {
        snapshotReady_.wait(lock, [this] { return !scanning_; });
        if (snapshotValid_) return cachedSnapshot_;
    }
    scanning_ = true;
    const auto generation = invalidation_;
    const auto sequence = ++scanSequence_;
    lock.unlock();
    struct Finish {
        std::mutex &mutex; std::condition_variable &ready; bool &running;
        ~Finish() { std::lock_guard guard(mutex); running = false; ready.notify_all(); }
    } finish{snapshotMutex_, snapshotReady_, scanning_};
    Snapshot result;
    result.state.path = path_.string();
    if (!validRepository_) return result;
    const auto status = process::run(command({"--no-optional-locks", "status", "--porcelain=v1", "-z", "--branch", "--untracked-files=all"}), path_);
    requireComplete(status, "git status");
    if (status.exitCode != 0) throw std::runtime_error("git status failed: " + status.output);
    const auto branchEnd = status.output.find('\0');
    if (status.output.starts_with("## ")) {
        result.state.branch = status.output.substr(3, branchEnd == std::string::npos ? std::string::npos : branchEnd - 3);
        const auto separator = result.state.branch.find("...");
        if (separator != std::string::npos) result.state.branch.resize(separator);
    }
    result.changes = parseChanges(status.output);

    std::map<std::string, std::size_t> indexes;
    for (std::size_t index = 0; index < result.changes.size(); ++index) indexes.emplace(result.changes[index].path, index);
    const auto numstatResult = process::run(command({"diff", "--numstat", "-z", "HEAD"}), path_);
    requireComplete(numstatResult, "git numstat");
    if (numstatResult.exitCode != 0 && process::run(command({"rev-parse", "--verify", "HEAD"}), path_).exitCode == 0)
        throw std::runtime_error("git numstat failed: " + numstatResult.output);
    const auto &numstat = numstatResult.output;
    const auto parseCount = [](std::string_view value) {
        int count = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
        return error == std::errc{} && end == value.data() + value.size() ? count : 0;
    };
    for (std::size_t offset = 0; offset < numstat.size();) {
        const auto firstTab = numstat.find('\t', offset);
        const auto secondTab = firstTab == std::string::npos ? std::string::npos : numstat.find('\t', firstTab + 1);
        if (firstTab == std::string::npos || secondTab == std::string::npos) break;
        const auto addedField = std::string_view(numstat).substr(offset, firstTab - offset);
        const auto removedField = std::string_view(numstat).substr(firstTab + 1, secondTab - firstTab - 1);
        const auto pathStart = secondTab + 1;
        const auto pathEnd = numstat.find('\0', pathStart);
        if (pathEnd == std::string::npos) break;
        auto filePath = numstat.substr(pathStart, pathEnd - pathStart);
        offset = pathEnd + 1;
        if (filePath.empty()) {
            const auto oldEnd = numstat.find('\0', offset);
            const auto newEnd = oldEnd == std::string::npos ? std::string::npos : numstat.find('\0', oldEnd + 1);
            if (oldEnd == std::string::npos || newEnd == std::string::npos) break;
            filePath = numstat.substr(oldEnd + 1, newEnd - oldEnd - 1);
            offset = newEnd + 1;
        }
        const auto found = indexes.find(filePath);
        if (found == indexes.end()) continue;
        auto &change = result.changes[found->second];
        change.additions = parseCount(addedField);
        change.deletions = parseCount(removedField);
        change.binary = change.binary || addedField == "-" || removedField == "-";
    }
    result.state.files = static_cast<int>(result.changes.size());
    for (const auto &change : result.changes) {
        result.state.insertions += change.additions;
        result.state.deletions += change.deletions;
    }
    lock.lock();
    result.version = cachedSnapshot_.state == result.state && cachedSnapshot_.changes == result.changes ? cachedSnapshot_.version : ++snapshotVersion_;
    if (generation != invalidation_ || sequence != scanSequence_) result = cachedSnapshot_;
    else {
        cachedSnapshot_ = result;
        snapshotAt_ = std::chrono::steady_clock::now();
        snapshotValid_ = true;
    }
    lock.unlock();
    return result;
}

std::optional<std::string> GitRepository::verificationFingerprint() const {
    // shortcut: symlinks and submodules report unknown, support them when dependencies can be fingerprinted safely.
    if (!validRepository_) return std::nullopt;
    std::error_code canonicalError;
    const auto root = std::filesystem::canonical(path_, canonicalError);
    if (canonicalError) return std::nullopt;
    const auto metaBefore = metadataKey();
    boost::uuids::detail::sha1 digest;
    const auto feed = [&digest](const void *data, std::size_t size) { digest.process_bytes(data, size); };
    const auto feedText = [&feed](std::string text) { auto framed = std::to_string(text.size()) + ":" + text; feed(framed.data(), framed.size()); };
    const auto trimOutput = [](std::string value) {
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    };

    feedText("aegis-verification-fingerprint-v1");
    feedText("root");
    feedText(root.string());

    const auto headCommand = command({"--no-optional-locks", "rev-parse", "--verify", "HEAD"});
    const auto symbolicCommand = command({"--no-optional-locks", "symbolic-ref", "HEAD"});
    const auto head = process::run(headCommand, path_);
    if (head.outputTruncated) return std::nullopt;
    std::string unbornRef;
    bool headWasUnborn = false;
    if (head.exitCode == 0 && validCommitId(trimOutput(head.output))) {
        feedText("head");
        feedText(trimOutput(head.output));
    } else {
        const auto symbolic = process::run(symbolicCommand, path_);
        if (symbolic.outputTruncated || symbolic.exitCode != 0) return std::nullopt;
        const auto ref = trimOutput(symbolic.output);
        if (ref.empty() || ref.find("..") != std::string::npos) return std::nullopt;
        headWasUnborn = true;
        unbornRef = ref;
        feedText("unborn");
        feedText(ref);
    }

    const auto indexCommand = command({"--no-optional-locks", "ls-files", "--stage", "-z"});
    const auto index = process::run(indexCommand, path_);
    if (index.outputTruncated || index.exitCode != 0) return std::nullopt;
    for (std::size_t offset = 0; offset < index.output.size();) {
        const auto end = index.output.find('\0', offset);
        if (end == std::string::npos) return std::nullopt;
        const auto entry = std::string_view(index.output).substr(offset, end - offset);
        offset = end + 1;
        if (entry.substr(0, 6) == "160000") return std::nullopt; // submodule gitlink
    }
    feedText("index");
    feedText(index.output);

    const auto listingCommand = command({"--no-optional-locks", "ls-files", "--cached", "--others", "--exclude-standard", "-z"});
    const auto trackedPaths = [](const process::Result &result) -> std::optional<std::vector<std::string>> {
        if (result.outputTruncated || result.exitCode != 0) return std::nullopt;
        std::vector<std::string> paths;
        for (std::size_t offset = 0; offset < result.output.size();) {
            const auto end = result.output.find('\0', offset);
            if (end == std::string::npos) return std::nullopt;
            paths.push_back(result.output.substr(offset, end - offset));
            offset = end + 1;
        }
        paths.erase(std::remove_if(paths.begin(), paths.end(), [](const std::string &path) {
            return path == ".aegis" || path == ".aegis-git" || path.rfind(".aegis/", 0) == 0 || path.rfind(".aegis-git/", 0) == 0;
        }), paths.end());
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        return paths;
    };
    const auto listing = process::run(listingCommand, path_);
    auto paths = trackedPaths(listing);
    if (!paths) return std::nullopt;
    feedText("files");
    feedText(std::to_string(paths->size()));
    for (const auto &relative : *paths) {
        feedText("file");
        feedText(relative);
        const auto fd = openWithin(root, relative);
        if (fd < 0) {
            if (errno == ENOENT) {
                feedText("deleted");
                continue;
            }
            return std::nullopt;
        }
        struct stat before {};
        if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)) {
            ::close(fd);
            return std::nullopt;
        }
        feedText("mode");
        feedText(std::to_string(static_cast<unsigned long long>(before.st_mode)));
        feedText("bytes");
        feedText(std::to_string(static_cast<unsigned long long>(before.st_size)));
        std::array<char, 65536> buffer{};
        for (;;) {
            const auto readCount = ::read(fd, buffer.data(), buffer.size());
            if (readCount < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return std::nullopt;
            }
            if (readCount == 0) break;
            feed(buffer.data(), static_cast<std::size_t>(readCount));
        }
        struct stat after {};
        if (::fstat(fd, &after) != 0 || before.st_mode != after.st_mode || before.st_size != after.st_size ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
            ::close(fd);
            return std::nullopt;
        }
        ::close(fd);
        feedText("end");
    }
    const auto headAgain = process::run(headCommand, path_);
    if (headAgain.outputTruncated || headAgain.exitCode != head.exitCode || headAgain.output != head.output) return std::nullopt;
    if (headAgain.exitCode != 0) {
        const auto symbolicAgain = process::run(symbolicCommand, path_);
        if (!headWasUnborn || symbolicAgain.outputTruncated || symbolicAgain.exitCode != 0 ||
            trimOutput(symbolicAgain.output) != unbornRef) return std::nullopt;
    }
    const auto indexAgain = process::run(indexCommand, path_);
    if (indexAgain.outputTruncated || indexAgain.exitCode != 0 || indexAgain.output != index.output) return std::nullopt;
    const auto listingAgain = process::run(listingCommand, path_);
    const auto pathsAgain = trackedPaths(listingAgain);
    if (!pathsAgain || *pathsAgain != paths) return std::nullopt;
    if (metadataKey() != metaBefore) return std::nullopt;
    return sha1Hex(digest);
}

std::string GitRepository::metadataKey() const {
    std::string result;
    auto stamp = [&](const std::filesystem::path &path) {
        struct stat info{};
        if (stat(path.c_str(), &info) != 0) { result += ":missing"; return; }
        result += ":" + std::to_string(info.st_ino) + ":" + std::to_string(info.st_size) + ":" +
            std::to_string(info.st_mtim.tv_sec) + ":" + std::to_string(info.st_mtim.tv_nsec) + ":" +
            std::to_string(info.st_ctim.tv_sec) + ":" + std::to_string(info.st_ctim.tv_nsec);
    };
    stamp(metadataDirectory_ / "index"); stamp(metadataDirectory_ / "HEAD");
    stamp(commonDirectory_ / "packed-refs");
    std::ifstream head(metadataDirectory_ / "HEAD");
    std::array<char, 4096> buffer{}; head.getline(buffer.data(), buffer.size());
    const std::string line(buffer.data());
    if (line.starts_with("ref: refs/") && line.size() < 4096 && line.find("..") == std::string::npos)
        stamp(commonDirectory_ / line.substr(5));
    return result;
}

std::vector<GitChange> GitRepository::changes(std::optional<std::size_t> limit, bool refresh) const {
    auto result = snapshot(refresh).changes;
    if (limit && result.size() > *limit) result.resize(*limit);
    return result;
}

void GitRepository::invalidateSnapshot() const {
    std::lock_guard lock(snapshotMutex_);
    snapshotValid_ = false;
    ++invalidation_;
}

std::optional<GitChange> GitRepository::change(const std::string &path) const {
    if (!validRepository_) return std::nullopt;
    std::string error;
    if (!validPath(path, error)) throw std::invalid_argument(error);
    const auto status = process::run(command({"--no-optional-locks", "--literal-pathspecs", "status", "--porcelain=v1", "-z", "--untracked-files=all", "--", path}), path_);
    requireComplete(status, "git status");
    if (status.exitCode != 0) throw std::runtime_error("git status failed: " + status.output);
    auto matches = parseChanges(status.output, 1);
    if (matches.empty()) return std::nullopt;
    if (!matches.front().oldPath && (matches.front().indexStatus == "A" || matches.front().worktreeStatus == "A")) {
        // A pathspec makes Git report a staged rename as an add, so recover its source from full status only here.
        const auto allStatus = process::run(command({"--no-optional-locks", "status", "--porcelain=v1", "-z", "--untracked-files=all"}), path_);
        requireComplete(allStatus, "git status");
        if (allStatus.exitCode != 0) throw std::runtime_error("git status failed: " + allStatus.output);
        auto allChanges = parseChanges(allStatus.output);
        const auto change = std::find_if(allChanges.begin(), allChanges.end(), [&](const auto &item) { return item.path == path; });
        if (change != allChanges.end()) return std::move(*change);
    }
    return std::move(matches.front());
}

std::vector<CommitSummary> GitRepository::commits(std::size_t limit) const {
    if (!validRepository_) return {};
    limit = std::clamp<std::size_t>(limit, 1, 100);
    const auto log = process::run(command({"log", "-n", std::to_string(limit),
        "--format=%H%x1f%P%x1f%an%x1f%at%x1f%s"}), path_);
    requireComplete(log, "git log");
    const auto &output = log.output;
    std::vector<CommitSummary> result;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line))
        if (auto commit = parseCommitSummary(line)) result.push_back(std::move(*commit));
    return result;
}

std::optional<CommitSummary> GitRepository::findCommit(const std::string &id) const {
    if (!validRepository_ || !validCommitId(id)) return std::nullopt;
    const auto output = process::run(command({"show", "-s", "--format=%H%x1f%P%x1f%an%x1f%at%x1f%s", id}), path_);
    requireComplete(output, "git show");
    if (output.exitCode != 0) return std::nullopt;
    auto line = output.output;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return parseCommitSummary(line);
}

std::vector<CommitFile> GitRepository::commitFiles(const std::string &id) const {
    const auto selected = findCommit(id);
    if (!selected) return {};
    const auto output = process::run(command({"diff-tree", "--root", "--first-parent", "--no-commit-id",
        "--name-status", "-r", "-M", "-z", selected->id}), path_);
    requireComplete(output, "git diff-tree");
    if (output.exitCode != 0) throw std::runtime_error("could not list commit changes: " + output.output);
    std::vector<CommitFile> result;
    for (std::size_t offset = 0; offset < output.output.size();) {
        const auto statusEnd = output.output.find('\0', offset);
        if (statusEnd == std::string::npos) break;
        const auto status = output.output.substr(offset, statusEnd - offset);
        offset = statusEnd + 1;
        const auto firstEnd = output.output.find('\0', offset);
        if (firstEnd == std::string::npos) break;
        CommitFile file;
        file.status = status.substr(0, 1);
        if ((file.status == "R" || file.status == "C") && firstEnd + 1 < output.output.size()) {
            file.oldPath = output.output.substr(offset, firstEnd - offset);
            const auto secondEnd = output.output.find('\0', firstEnd + 1);
            if (secondEnd == std::string::npos) break;
            file.path = output.output.substr(firstEnd + 1, secondEnd - firstEnd - 1);
            offset = secondEnd + 1;
        } else {
            file.path = output.output.substr(offset, firstEnd - offset);
            offset = firstEnd + 1;
        }
        result.push_back(std::move(file));
    }
    return result;
}

std::vector<std::string> GitRepository::branches() const {
    if (!validRepository_) return {};
    const auto branches = process::run(command({"branch", "--format=%(refname:short)"}), path_);
    requireComplete(branches, "git branch");
    const auto &output = branches.output;
    std::vector<std::string> result;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) if (!line.empty()) result.push_back(line);
    return result;
}

std::string GitRepository::currentBranch() const {
    return state().branch;
}

bool GitRepository::clean() const {
    return validRepository_ && snapshot(true).changes.empty();
}

std::string GitRepository::diff(std::size_t maxOutputBytes) const {
    if (!validRepository_) return {};
    auto result = process::run(command({"diff", "HEAD", "--no-ext-diff", "--no-color"}), path_, std::chrono::seconds(30), maxOutputBytes);
    constexpr std::string_view marker = "...[diff truncated]";
    if (result.outputTruncated) {
        if (maxOutputBytes <= marker.size()) return std::string(marker.substr(0, maxOutputBytes));
        result.output.resize(maxOutputBytes - marker.size());
        result.output.append(marker);
    }
    return result.output;
}

bool GitRepository::stage(const std::string &path, std::string &error) const {
    if (!validRepository_) { error = "repository is not a Git work tree"; return false; }
    if (!validPath(path, error)) return false;
    const auto result = process::run(command({"add", "--", ":(literal)" + path}), path_);
    error = result.output;
    invalidateSnapshot();
    return result.exitCode == 0;
}

bool GitRepository::unstage(const std::string &path, std::string &error) const {
    if (!validRepository_) { error = "repository is not a Git work tree"; return false; }
    if (!validPath(path, error)) return false;
    const auto result = process::run(command({"restore", "--staged", "--", ":(literal)" + path}), path_);
    error = result.output;
    invalidateSnapshot();
    return result.exitCode == 0;
}

bool GitRepository::commit(const std::string &message, std::string &error) const {
    if (!validRepository_) { error = "repository is not a Git work tree"; return false; }
    if (message.empty() || message.find('\0') != std::string::npos) {
        error = "commit message is required";
        return false;
    }
    const auto result = process::run(command({"commit", "-m", message}), path_);
    error = result.output;
    invalidateSnapshot();
    return result.exitCode == 0;
}

bool GitRepository::switchBranch(const std::string &branch, std::string &error) const {
    if (!validRepository_) { error = "repository is not a Git work tree"; return false; }
    const auto available = branches();
    if (std::find(available.begin(), available.end(), branch) == available.end()) {
        error = "branch does not exist locally";
        return false;
    }
    if (!clean()) {
        error = "commit or discard working tree changes before switching branches";
        return false;
    }
    const auto result = process::run(command({"switch", "--", branch}), path_);
    error = result.output;
    invalidateSnapshot();
    return result.exitCode == 0;
}

bool GitRepository::merge(const std::string &branch, std::string &output) const {
    if (!validRepository_) { output = "repository is not a Git work tree"; return false; }
    const auto available = branches();
    if (std::find(available.begin(), available.end(), branch) == available.end()) {
        output = "branch does not exist locally";
        return false;
    }
    if (branch == currentBranch()) { output = "cannot merge the current branch"; return false; }
    if (!clean()) { output = "commit or discard working tree changes before merging"; return false; }
    const auto result = process::run(command({"merge", "--no-edit", "--", branch}), path_, std::chrono::seconds(120));
    output = result.output;
    invalidateSnapshot();
    return result.exitCode == 0;
}

bool GitRepository::pull(std::string &output) const {
    if (!validRepository_) { output = "repository is not a Git work tree"; return false; }
    if (!clean()) {
        output = "commit or discard working tree changes before pulling";
        return false;
    }
    const auto result = process::run(command({"-c", "pull.ff=true", "pull", "--no-rebase"}), path_, std::chrono::seconds(120));
    output = result.output;
    invalidateSnapshot();
    return result.exitCode == 0;
}

bool GitRepository::push(std::string &output) const {
    if (!validRepository_) { output = "repository is not a Git work tree"; return false; }
    const auto result = process::run(command({"push"}), path_, std::chrono::seconds(120));
    output = result.output;
    return result.exitCode == 0;
}

}
