#include "daemon/repository/files.h"

#include "daemon/process/process.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace aegis::daemon::repository {
namespace {

constexpr std::size_t normalFileLimit = 1024 * 1024;
constexpr std::size_t explicitFileLimit = 8 * 1024 * 1024;
constexpr std::size_t gitOutputLimit = 16 * 1024 * 1024;

std::filesystem::path relativePath(const std::string &value, bool allowRoot = false) {
    if (value.empty() && allowRoot) return ".";
    const std::filesystem::path path(value);
    if (value.find('\0') != std::string::npos || path.is_absolute() || path.has_root_name())
        throw std::invalid_argument("path must stay inside the repository");
    for (const auto &component : path)
        if (component == "..") throw std::invalid_argument("path must stay inside the repository");
    if (!allowRoot && (value.empty() || path == "."))
        throw std::invalid_argument("path must name a repository file");
    return path.lexically_normal();
}

bool inside(const std::filesystem::path &root, const std::filesystem::path &target) {
    const auto relative = target.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::string languageFor(const std::filesystem::path &path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".h" || extension == ".hpp" || extension == ".hh") return "cpp";
    if (extension == ".c") return "cpp";
    if (extension == ".ts" || extension == ".tsx") return "typescript";
    if (extension == ".js" || extension == ".jsx" || extension == ".mjs" || extension == ".cjs") return "javascript";
    if (extension == ".json") return "json";
    if (extension == ".md" || extension == ".mdx") return "markdown";
    if (extension == ".html" || extension == ".htm") return "html";
    if (extension == ".css" || extension == ".scss") return "css";
    if (extension == ".py") return "python";
    if (extension == ".sh") return "shell";
    if (extension == ".rs") return "rust";
    if (extension == ".go") return "go";
    if (extension == ".yaml" || extension == ".yml") return "yaml";
    if (extension == ".toml") return "ini";
    if (extension == ".sol") return "solidity";
    return "plaintext";
}

std::string statusFor(const GitChange &change) {
    if (change.indexStatus == "C" || change.worktreeStatus == "C") return "C";
    if (change.oldPath) return "R";
    if (change.indexStatus == "?" || change.worktreeStatus == "?") return "?";
    if (change.indexStatus == "A" || change.worktreeStatus == "A") return "A";
    if (change.indexStatus == "D" || change.worktreeStatus == "D") return "D";
    if (change.indexStatus == "R" || change.worktreeStatus == "R") return "R";
    return "M";
}

bool containsNul(std::string_view content) {
    return content.find('\0') != std::string_view::npos;
}

bool internalPath(const std::string &path) {
    for (const auto &component : std::filesystem::path(path))
        if (component == ".git" || component == ".aegis" || component == "node_modules") return true;
    return false;
}

std::string objectIdFromOutput(std::string output) {
    if (!output.empty() && output.back() == '\0') output.pop_back();
    if (output.size() != 40 && output.size() != 64) return {};
    if (!std::all_of(output.begin(), output.end(), [](unsigned char c) { return std::isxdigit(c); })) return {};
    return output;
}

}

Files::Files(const GitRepository &git) : git_(git) {}

std::vector<std::string> Files::gitPaths(const std::string &prefix, bool &truncated) const {
    auto arguments = std::vector<std::string>{"--literal-pathspecs", "ls-files", "--cached", "--others", "--exclude-standard", "-z"};
    if (!prefix.empty()) arguments.insert(arguments.end(), {"--", prefix});
    process::ChildProcess child;
    if (!child.start(git_.command(std::move(arguments)), git_.path())) throw std::runtime_error("could not start git ls-files");
    std::string output;
    const auto result = child.wait(std::chrono::seconds(30), [&](std::string_view chunk) {
        if (output.size() < gitOutputLimit) {
            const auto remaining = gitOutputLimit - output.size();
            output.append(chunk.substr(0, remaining));
            if (chunk.size() > remaining) truncated = true;
        } else truncated = true;
    }, false);
    if (result.exitCode != 0) throw std::runtime_error("git ls-files failed: " + result.output);
    std::vector<std::string> paths;
    for (std::size_t start = 0; start < output.size();) {
        const auto end = output.find('\0', start);
        if (end == std::string::npos) break;
        paths.push_back(output.substr(start, end - start));
        start = end + 1;
    }
    return paths;
}

std::string fileSourceName(FileSource source) {
    switch (source) {
    case FileSource::head: return "head";
    case FileSource::index: return "index";
    case FileSource::worktree: return "worktree";
    case FileSource::commit: return "commit";
    }
    return "worktree";
}

std::optional<FileSource> parseFileSource(const std::string &source) {
    if (source == "head") return FileSource::head;
    if (source == "index") return FileSource::index;
    if (source == "worktree") return FileSource::worktree;
    return std::nullopt;
}

FileListing Files::list(const std::string &path, FileScope scope, std::size_t limit,
                       bool recursive, bool includeChanges) const {
    const auto relative = relativePath(path, true);
    const auto prefix = relative == "." ? std::string{} : relative.generic_string() + "/";
    if (!git_.validRepository_ && scope == FileScope::all) return {};
    std::map<std::string, GitChange> changes;
    if (scope == FileScope::changed || includeChanges)
        for (const auto &change : git_.changes()) changes.emplace(change.path, change);

    bool truncated = false;
    std::vector<std::string> paths;
    if (scope == FileScope::changed) {
        for (const auto &[file, change] : changes) if (!internalPath(file)) paths.push_back(file);
    } else paths = gitPaths(recursive ? std::string{} : prefix, truncated);

    std::map<std::string, FileEntry> entries;
    for (const auto &file : paths) {
        if (internalPath(file)) continue;
        if (!prefix.empty() && !file.starts_with(prefix)) continue;
        const auto tail = prefix.empty() ? file : file.substr(prefix.size());
        if (tail.empty()) continue;
        const auto slash = tail.find('/');
        const bool directory = slash != std::string::npos && !recursive;
        const auto entryPath = prefix + (directory ? tail.substr(0, slash) : tail);
        auto [where, inserted] = entries.try_emplace(entryPath);
        auto &entry = where->second;
        if (inserted) {
            entry.path = entryPath;
            entry.name = directory ? tail.substr(0, slash) : tail;
            entry.kind = directory ? "directory" : "file";
            entry.language = directory ? "" : languageFor(entryPath);
            if (!directory) {
                const auto change = changes.find(file);
                if (change != changes.end()) {
                    entry.changed = true;
                    entry.gitStatus = statusFor(change->second);
                    entry.oldPath = change->second.oldPath;
                    entry.additions = change->second.additions;
                    entry.deletions = change->second.deletions;
                    entry.binary = change->second.binary;
                }
                std::error_code error;
                const auto status = std::filesystem::symlink_status(git_.path() / file, error);
                if (!error && std::filesystem::is_symlink(status)) entry.kind = "symlink";
                else if (!error && std::filesystem::is_regular_file(status)) {
                    const auto size = std::filesystem::file_size(git_.path() / file, error);
                    if (!error) entry.size = size;
                }
            }
        }
        if (directory) {
            if (const auto change = changes.find(file); change != changes.end()) {
                where->second.changed = true;
                if (where->second.gitStatus.empty()) where->second.gitStatus = statusFor(change->second);
            }
        }
        if (entries.size() > limit) { truncated = true; break; }
    }

    FileListing result;
    result.truncated = truncated || entries.size() > limit;
    result.entries.reserve(std::min(entries.size(), limit));
    for (auto &[_, entry] : entries) {
        if (result.entries.size() == limit) break;
        result.entries.push_back(std::move(entry));
    }
    return result;
}

std::string Files::objectId(const std::string &path, FileSource source) const {
    if (source == FileSource::worktree || !git_.validRepository_) return {};
    std::vector<std::string> arguments;
    if (source == FileSource::head)
        arguments = {"--literal-pathspecs", "ls-tree", "-z", "--format=%(objectname)", "HEAD", "--", path};
    else
        arguments = {"--literal-pathspecs", "ls-files", "--cached", "-z", "--format=%(objectname)", "--", path};
    const auto result = process::run(git_.command(std::move(arguments)), git_.path());
    if (result.exitCode != 0) {
        if (source == FileSource::head && process::run(git_.command({"rev-parse", "--verify", "HEAD"}), git_.path()).exitCode != 0) return {};
        throw std::runtime_error("could not inspect Git file object: " + result.output);
    }
    return objectIdFromOutput(result.output);
}

std::string Files::objectIdAt(const std::string &path, const std::string &revision) const {
    const auto result = process::run(git_.command({"--literal-pathspecs", "ls-tree", "-z", "--format=%(objectname)",
                                                   revision, "--", path}), git_.path());
    if (result.exitCode != 0) throw std::runtime_error("could not inspect commit file: " + result.output);
    return objectIdFromOutput(result.output);
}

FileContent Files::read(const std::string &path, FileSource source, bool loadLarge) const {
    if (source == FileSource::commit) throw std::invalid_argument("commit source requires a commit id");
    const auto relative = relativePath(path);
    if (source == FileSource::worktree) return readWorktree(relative.generic_string(), loadLarge);
    return readObject(relative.generic_string(), source, loadLarge);
}

FileContent Files::readWorktree(const std::string &path, bool loadLarge) const {
    FileContent result;
    result.source = FileSource::worktree;
    const auto root = std::filesystem::canonical(git_.path());
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(root / path, error);
    if (error) throw std::runtime_error("could not resolve repository file: " + error.message());
    if (!inside(root, resolved)) throw std::invalid_argument("file resolves outside the repository");
    if (!std::filesystem::exists(resolved, error)) return result;
    if (error) throw std::runtime_error("could not inspect repository file: " + error.message());

    const auto safeRelative = resolved.lexically_relative(root);
    int descriptor = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "open repository");
    std::vector<std::string> components;
    for (const auto &component : safeRelative) if (component != ".") components.push_back(component.string());
    if (components.empty()) { close(descriptor); throw std::invalid_argument("path must name a file"); }
    for (std::size_t i = 0; i < components.size(); ++i) {
        const bool last = i + 1 == components.size();
        const auto flags = last ? O_RDONLY | O_CLOEXEC | O_NOFOLLOW : O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        const auto next = openat(descriptor, components[i].c_str(), flags);
        close(descriptor);
        if (next < 0) {
            if (errno == ENOENT) return result;
            if (errno == ELOOP) throw std::invalid_argument("repository file may not escape through a symlink");
            throw std::system_error(errno, std::generic_category(), "open repository file");
        }
        descriptor = next;
    }
    struct stat info{};
    if (fstat(descriptor, &info) != 0) { const auto saved = errno; close(descriptor); throw std::system_error(saved, std::generic_category(), "stat repository file"); }
    if (!S_ISREG(info.st_mode)) { close(descriptor); throw std::invalid_argument("path is not a regular file"); }
    result.exists = true;
    result.size = static_cast<std::uint64_t>(info.st_size);
    const auto limit = loadLarge ? explicitFileLimit : normalFileLimit;
    if (result.size > limit) { result.truncated = true; close(descriptor); return result; }
    result.content.reserve(static_cast<std::size_t>(result.size));
    std::array<char, 16384> buffer{};
    while (result.content.size() <= limit) {
        const auto request = std::min(buffer.size(), limit + 1 - result.content.size());
        const auto count = ::read(descriptor, buffer.data(), request);
        if (count < 0) {
            if (errno == EINTR) continue;
            const auto saved = errno;
            close(descriptor);
            throw std::system_error(saved, std::generic_category(), "read repository file");
        }
        if (count == 0) break;
        result.content.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(descriptor);
    if (result.content.size() > limit) {
        result.content.clear();
        result.truncated = true;
    } else if (containsNul(result.content)) {
        result.content.clear();
        result.binary = true;
    }
    return result;
}

FileContent Files::readObject(const std::string &path, FileSource source, bool loadLarge) const {
    const auto oid = objectId(path, source);
    return readBlob(oid, source, source == FileSource::head ? std::optional<std::string>{"HEAD"} : std::optional<std::string>{"INDEX"}, loadLarge);
}

FileContent Files::readCommitObject(const std::string &path, const std::optional<std::string> &revision, bool loadLarge) const {
    if (!revision) {
        FileContent result;
        result.source = FileSource::commit;
        return result;
    }
    return readBlob(objectIdAt(path, *revision), FileSource::commit, revision, loadLarge);
}

FileContent Files::readBlob(const std::string &oid, FileSource source, std::optional<std::string> revision, bool loadLarge) const {
    FileContent result;
    result.source = source;
    result.revision = std::move(revision);
    if (oid.empty()) return result;
    const auto sizeResult = process::run(git_.command({"cat-file", "-s", oid}), git_.path());
    if (sizeResult.exitCode != 0) throw std::runtime_error("could not inspect Git blob size: " + sizeResult.output);
    const auto parsedSize = std::stoull(sizeResult.output);
    result.exists = true;
    result.size = parsedSize;
    const auto limit = loadLarge ? explicitFileLimit : normalFileLimit;
    if (result.size > limit) { result.truncated = true; return result; }

    process::ChildProcess child;
    if (!child.start(git_.command({"cat-file", "blob", oid}), git_.path())) throw std::runtime_error("could not start git cat-file");
    result.content.reserve(static_cast<std::size_t>(result.size));
    const auto readResult = child.wait(std::chrono::seconds(30), [&](std::string_view chunk) {
        if (result.content.size() < limit + 1)
            result.content.append(chunk.substr(0, limit + 1 - result.content.size()));
    }, false);
    if (readResult.exitCode != 0) throw std::runtime_error("could not read Git blob: " + readResult.output);
    if (result.content.size() > limit) {
        result.content.clear();
        result.truncated = true;
    } else if (containsNul(result.content)) {
        result.content.clear();
        result.binary = true;
    }
    return result;
}

FileComparison Files::compare(const std::string &path, FileSource base, FileSource target, bool loadLarge) const {
    const auto relative = relativePath(path);
    FileComparison result;
    result.path = relative.generic_string();
    const auto changes = git_.changes();
    const auto change = std::find_if(changes.begin(), changes.end(), [&](const auto &item) { return item.path == result.path; });
    if (change != changes.end()) {
        result.oldPath = change->oldPath;
        result.status = statusFor(*change);
    }
    const auto originalPath = result.oldPath.value_or(result.path);
    result.original = read(originalPath, base, loadLarge);
    result.modified = read(result.path, target, loadLarge);
    result.binary = result.original.binary || result.modified.binary;
    result.truncated = result.original.truncated || result.modified.truncated;
    return result;
}

FileComparison Files::compareCommit(const std::string &commitId, const std::string &path, bool loadLarge) const {
    const auto relative = relativePath(path);
    const auto commit = git_.findCommit(commitId);
    if (!commit) throw std::invalid_argument("commit was not found");
    const auto files = git_.commitFiles(commit->id);
    const auto changed = std::find_if(files.begin(), files.end(), [&](const auto &file) { return file.path == relative.generic_string(); });
    if (changed == files.end()) throw std::invalid_argument("file is not part of this commit");

    FileComparison result;
    result.path = changed->path;
    result.oldPath = changed->oldPath;
    result.parentCommit = commit->parentId;
    result.commit = commit->id;
    result.status = changed->status;
    result.original = readCommitObject(changed->oldPath.value_or(changed->path), commit->parentId, loadLarge);
    result.modified = readCommitObject(changed->path, commit->id, loadLarge);
    result.binary = result.original.binary || result.modified.binary;
    result.truncated = result.original.truncated || result.modified.truncated;
    return result;
}

}
