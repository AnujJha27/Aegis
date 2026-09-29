#include "daemon/process/process.h"
#include "daemon/agents/pty_adapter.h"
#include "daemon/repository/files.h"
#include "daemon/repository/git.h"
#include "daemon/session/store.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

namespace {
using Clock = std::chrono::steady_clock;

long long millis(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

long long percentile(std::vector<long long> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>((values.size() - 1) * fraction)];
}

long long peakRssKiB() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmHWM:")) continue;
        std::istringstream value(line.substr(6));
        long long kib = 0;
        value >> kib;
        return kib;
    }
    return 0;
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("aegis-perf-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto cleanup = [&] { std::error_code ignored; std::filesystem::remove_all(root, ignored); };
    try {
        const auto dbPath = root / "events.sqlite";
        std::string taskId;
        std::string runId;
        {
            aegis::daemon::Store store(dbPath);
            const auto task = store.createTask("100k event benchmark", root.string());
            const auto run = store.startRun(task.id, "synthetic");
            taskId = task.id;
            runId = run.id;
        }
        {
            sqlite3 *database = nullptr;
            if (sqlite3_open(dbPath.c_str(), &database) != SQLITE_OK) throw std::runtime_error("could not open benchmark database");
            char *sqlError = nullptr;
            if (sqlite3_exec(database, "BEGIN", nullptr, nullptr, &sqlError) != SQLITE_OK) throw std::runtime_error(sqlError ? sqlError : "could not begin fixture transaction");
            sqlite3_stmt *insert = nullptr;
            if (sqlite3_prepare_v2(database, "INSERT INTO events (id, task_id, run_id, type, agent, content, timestamp) VALUES (?, ?, ?, 'agent.message.completed', 'synthetic', '', ?)", -1, &insert, nullptr) != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(database));
            const auto started = Clock::now();
            for (int index = 0; index < 100000; ++index) {
                const auto id = "bench-" + std::to_string(index);
                sqlite3_bind_text(insert, 1, id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert, 2, taskId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert, 3, runId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(insert, 4, index);
                if (sqlite3_step(insert) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(database));
                sqlite3_reset(insert);
                sqlite3_clear_bindings(insert);
            }
            const auto insertMs = millis(started);
            sqlite3_finalize(insert);
            if (sqlite3_exec(database, "COMMIT", nullptr, nullptr, &sqlError) != SQLITE_OK) throw std::runtime_error(sqlError ? sqlError : "could not commit fixture transaction");
            sqlite3_close(database);
            std::vector<long long> recentMs;
            aegis::daemon::Store store(dbPath);
            for (int sample = 0; sample < 40; ++sample) {
                const auto query = Clock::now();
                const auto events = store.events(taskId, 20);
                if (events.size() != 20) throw std::runtime_error("latest history returned an unexpected event count");
                recentMs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - query).count());
            }
            std::cout << "events=100000 transactional_fixture_seed_ms=" << insertMs
                      << " seed_per_s=" << (100000000LL / std::max(1LL, insertMs))
                      << " latest20_p50_us=" << percentile(recentMs, .50)
                      << " latest20_p95_us=" << percentile(recentMs, .95)
                      << " sqlite_bytes=" << std::filesystem::file_size(dbPath) << '\n';
        }

        std::mutex ptyMutex;
        std::condition_variable ptyChanged;
        std::size_t ptyBytes = 0;
        std::size_t ptyChunks = 0;
        bool ptyDone = false;
        aegis::daemon::agents::PtyAdapter pty("synthetic", {"/bin/sh", "-c", "dd if=/dev/zero bs=65536 count=2 2>/dev/null"},
            [&](aegis::daemon::AgentEvent event) {
                std::lock_guard lock(ptyMutex);
                if (event.type == "terminal.output") { ptyBytes += event.content.size(); ++ptyChunks; }
                if (event.type == "run.completed" || event.type == "run.failed") { ptyDone = true; ptyChanged.notify_all(); }
            });
        const auto ptyStarted = Clock::now();
        if (!pty.start({"bench-task", "bench-run", "synthetic", root, std::nullopt})) throw std::runtime_error("synthetic PTY failed to start");
        {
            std::unique_lock lock(ptyMutex);
            if (!ptyChanged.wait_for(lock, std::chrono::seconds(10), [&] { return ptyDone; })) throw std::runtime_error("synthetic PTY benchmark timed out");
        }
        const auto ptyElapsed = millis(ptyStarted);
        if (ptyBytes != 128 * 1024) throw std::runtime_error("synthetic PTY output was incomplete");
        std::cout << "pty_bytes=" << ptyBytes << " pty_chunks=" << ptyChunks << " pty_elapsed_ms=" << ptyElapsed << '\n';

        const auto repository = root / "repo";
        std::filesystem::create_directories(repository);
        const auto init = aegis::daemon::process::run({"git", "init", "-q"}, repository);
        if (init.exitCode != 0) throw std::runtime_error("git init failed: " + init.output);
        for (int index = 0; index < 10000; ++index) {
            const auto file = repository / ("dir-" + std::to_string(index / 100)) / ("file-" + std::to_string(index) + ".txt");
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file) << "fixture " << index << '\n';
        }
        aegis::daemon::repository::GitRepository git(repository);
        aegis::daemon::repository::Files files(git);
        auto started = Clock::now();
        const auto changes = git.changes();
        const auto changesMs = millis(started);
        started = Clock::now();
        const auto listing = files.list("", aegis::daemon::repository::FileScope::all, 500);
        const auto listingMs = millis(started);
        started = Clock::now();
        const auto comparison = files.compare("dir-0/file-0.txt");
        const auto compareMs = millis(started);
        if (changes.size() != 10000 || listing.entries.empty() || !comparison.modified.exists)
            throw std::runtime_error("10k-file repository fixture returned incomplete data");
        std::cout << "repo_files=10000 changes_ms=" << changesMs << " root_listing_ms=" << listingMs
                  << " selected_compare_ms=" << compareMs << " listed_entries=" << listing.entries.size()
                  << " benchmark_peak_rss_kib=" << peakRssKiB() << '\n';
        cleanup();
    } catch (const std::exception &error) {
        cleanup();
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
