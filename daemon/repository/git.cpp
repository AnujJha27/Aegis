#include "daemon/repository/git.h"

#include "daemon/process/process.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

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
    RepositoryState result;
    result.path = path_.string();
    if (!validRepository_) return result;
    const auto status = process::run(command({"status", "--short", "--branch"}), path_).output;
    const auto numstat = process::run(command({"diff", "HEAD", "--numstat"}), path_).output;
    std::istringstream statusLines(status);
    std::string line;
    while (std::getline(statusLines, line)) {
        if (line.rfind("## ", 0) == 0) {
            result.branch = line.substr(3);
            const auto separator = result.branch.find("...");
            if (separator != std::string::npos) result.branch.resize(separator);
        } else if (line.size() >= 2) {
            ++result.files;
        }
    }
    std::istringstream numstatLines(numstat);
    while (std::getline(numstatLines, line)) {
        std::istringstream columns(line);
        int added = 0;
        int removed = 0;
        if (columns >> added >> removed) {
            result.insertions += added;
            result.deletions += removed;
        }
    }
    return result;
}

std::vector<GitChange> GitRepository::changes() const {
    if (!validRepository_) return {};
    const auto output = process::run(command({"status", "--porcelain=v1", "-z", "--untracked-files=all"}), path_).output;
    std::vector<GitChange> result;
    for (std::size_t offset = 0; offset + 3 <= output.size();) {
        const auto index = output[offset];
        const auto worktree = output[offset + 1];
        const auto pathStart = offset + 3;
        const auto pathEnd = output.find('\0', pathStart);
        if (pathEnd == std::string::npos) break;
        result.push_back({output.substr(pathStart, pathEnd - pathStart), std::string(1, index), std::string(1, worktree)});
        offset = pathEnd + 1;
        if ((index == 'R' || index == 'C' || worktree == 'R' || worktree == 'C') && offset < output.size()) {
            const auto originalEnd = output.find('\0', offset);
            if (originalEnd == std::string::npos) break;
            offset = originalEnd + 1;
        }
    }
    return result;
}

std::vector<std::string> GitRepository::branches() const {
    if (!validRepository_) return {};
    const auto output = process::run(command({"branch", "--format=%(refname:short)"}), path_).output;
    std::vector<std::string> result;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) if (!line.empty()) result.push_back(line);
    return result;
}

std::string GitRepository::currentBranch() const {
    if (!validRepository_) return {};
    auto branch = process::run(command({"branch", "--show-current"}), path_).output;
    if (!branch.empty() && branch.back() == '\n') branch.pop_back();
    return branch;
}

bool GitRepository::clean() const {
    if (!validRepository_) return false;
    const auto result = process::run(command({"status", "--porcelain=v1", "-z", "--untracked-files=all"}), path_);
    return result.exitCode == 0 && result.output.empty();
}

std::string GitRepository::diff() const {
    return validRepository_ ? process::run(command({"diff", "HEAD", "--no-ext-diff", "--no-color"}), path_).output : std::string{};
}

bool GitRepository::stage(const std::string &path, std::string &error) const {
    if (!validRepository_) { error = "repository is not a Git work tree"; return false; }
    if (!validPath(path, error)) return false;
    const auto result = process::run(command({"add", "--", ":(literal)" + path}), path_);
    error = result.output;
    return result.exitCode == 0;
}

bool GitRepository::unstage(const std::string &path, std::string &error) const {
    if (!validRepository_) { error = "repository is not a Git work tree"; return false; }
    if (!validPath(path, error)) return false;
    const auto result = process::run(command({"restore", "--staged", "--", ":(literal)" + path}), path_);
    error = result.output;
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
    return result.exitCode == 0;
}

bool GitRepository::push(std::string &output) const {
    if (!validRepository_) { output = "repository is not a Git work tree"; return false; }
    const auto result = process::run(command({"push"}), path_, std::chrono::seconds(120));
    output = result.output;
    return result.exitCode == 0;
}

}
