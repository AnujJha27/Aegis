#include "daemon/repository/watcher.h"
#include "daemon/repository/index.h"
#include "daemon/protocol/json.h"
#include <condition_variable>
#include <iostream>
#include <sys/stat.h>
#include <unordered_map>
namespace aegis::daemon::repository {
RepositoryWatcher::RepositoryWatcher(GitRepository &git, Files &files, EventHub &events, std::function<bool()> active) {
    thread_ = std::jthread([&git, &files, &events, active = std::move(active)](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        auto metadata = git.metadataKey();
        std::string signature, paths;
        std::uint64_t indexVersion = 0, version = 0;
        auto nextScan = std::chrono::steady_clock::now();
        auto interval = std::chrono::milliseconds(1000);
        bool wasActive = false;
        std::unordered_map<std::string, std::string> stamps;
        while (!stop.stop_requested()) {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::chrono::milliseconds(250), [] { return false; });
            lock.unlock();
            if (stop.stop_requested()) break;
            try {
                const auto now = std::chrono::steady_clock::now();
                const auto key = git.metadataKey();
                const bool metadataChanged = key != metadata;
                const bool watching = active();
                const auto index = files.index().snapshot();
                bool indexChanged = !index->indexing && index->version != indexVersion;
                if (!metadataChanged && (!watching || (wasActive && now < nextScan)) && !indexChanged) { wasActive = watching; continue; }
                if (metadataChanged) { git.invalidateSnapshot(); files.index().requestRebuild(); }
                metadata = key;
                nlohmann::json affected = nlohmann::json::array();
                bool changed = metadataChanged || indexChanged;
                if (metadataChanged || watching) {
                    const auto started = std::chrono::steady_clock::now();
                    const auto snapshot = git.snapshot(true);
                    auto current = protocol::toJson(snapshot.state).dump();
                    std::string currentPaths;
                    std::unordered_map<std::string, std::string> currentStamps;
                    for (const auto &file : snapshot.changes) {
                        auto stamp = protocol::toJson(file).dump();
                        currentPaths += file.path + '\0';
                        struct stat info{};
                        if (lstat((git.path() / file.path).c_str(), &info) == 0) stamp += std::to_string(info.st_mtim.tv_sec) + ":" + std::to_string(info.st_mtim.tv_nsec) + ":" + std::to_string(info.st_ctim.tv_nsec);
                        current += stamp;
                        if (!stamps.contains(file.path) || stamps[file.path] != stamp) if (affected.size() < 256) affected.push_back(file.path);
                        currentStamps.emplace(file.path, std::move(stamp));
                    }
                    for (const auto &[path, _] : stamps) if (!currentStamps.contains(path) && affected.size() < 256) affected.push_back(path);
                    stamps = std::move(currentStamps);
                    if (currentPaths != paths) files.index().requestRebuild();
                    paths = std::move(currentPaths);
                    changed = changed || current != signature;
                    signature = std::move(current);
                    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
                    interval = std::clamp(duration * 10, std::chrono::milliseconds(1000), std::chrono::milliseconds(5000));
                    nextScan = std::chrono::steady_clock::now() + interval;
                }
                wasActive = watching;
                if (indexChanged) indexVersion = index->version;
                if (changed) {
                    ++version;
                    events.publish({"repository-" + std::to_string(version), "", "", "repository.changed", "aegis",
                        nlohmann::json{{"version", version}, {"index_version", indexVersion}, {"paths", affected}, {"invalidate_all", metadataChanged || affected.size() >= 256}}.dump(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()});
                }
            } catch (const std::exception &) {
                nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                indexVersion = files.index().snapshot()->version;
                std::cerr << "aegis_daemon: repository watcher refresh failed; retrying after backoff\n";
            }
        }
    });
}
RepositoryWatcher::~RepositoryWatcher() { thread_.request_stop(); }
}
