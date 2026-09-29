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
    auto target = std::filesystem::path(line.substr(prefix.size()));
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

}

GitRepository::GitRepository(std::filesystem::path path)
    : path_(std::filesystem::absolute(std::move(path)).lexically_normal()) {
    for (auto candidate = path_; !candidate.empty(); candidate = candidate.parent_path()) {
        if (validGitEntry(candidate)) {
            path_ = candidate;
            validRepository_ = true;
            return;
        }
        const auto aegisDirectory = candidate / ".aegis-git";
        if (gitDirectory(aegisDirectory)) {
            path_ = candidate;
            gitDirectory_ = aegisDirectory;
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
    std::lock_guard lock(snapshotMutex_);
    if (!refresh && snapshotValid_ && std::chrono::steady_clock::now() - snapshotAt_ < std::chrono::milliseconds(250)) return cachedSnapshot_;
    Snapshot result;
    result.state.path = path_.string();
    if (!validRepository_) return result;
    const auto status = process::run(command({"status", "--porcelain=v1", "-z", "--branch", "--untracked-files=all"}), path_);
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
    result.version = ++snapshotVersion_;
    cachedSnapshot_ = result;
    snapshotAt_ = std::chrono::steady_clock::now();
    snapshotValid_ = true;
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
}

std::optional<GitChange> GitRepository::change(const std::string &path) const {
    if (!validRepository_) return std::nullopt;
    std::string error;
    if (!validPath(path, error)) throw std::invalid_argument(error);
    const auto status = process::run(command({"--literal-pathspecs", "status", "--porcelain=v1", "-z", "--untracked-files=all", "--", path}), path_);
    requireComplete(status, "git status");
    if (status.exitCode != 0) throw std::runtime_error("git status failed: " + status.output);
    auto matches = parseChanges(status.output, 1);
    if (matches.empty()) return std::nullopt;
    if (!matches.front().oldPath && (matches.front().indexStatus == "A" || matches.front().worktreeStatus == "A")) {
        // A pathspec makes Git report a staged rename as an add, so recover its source from full status only here.
        const auto allStatus = process::run(command({"status", "--porcelain=v1", "-z", "--untracked-files=all"}), path_);
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
