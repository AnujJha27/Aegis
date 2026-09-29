#include "daemon/process/process.h"
#include "daemon/agents/pty_adapter.h"
#include "daemon/agents/manager.h"
#include "daemon/api/routes.h"
#include "daemon/api/server.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/repository/files.h"
#include "daemon/repository/git.h"
#include "daemon/session/store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>

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

long long residentRssKiB() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmRSS:")) continue;
        std::istringstream value(line.substr(6));
        long long kib = 0;
        value >> kib;
        return kib;
    }
    return 0;
}

long long cpuMs() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000LL +
        (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000LL;
}

void websocketSoak(const std::filesystem::path &root) {
    namespace asio = boost::asio;
    namespace websocket = boost::beast::websocket;
    using Tcp = asio::ip::tcp;

    aegis::daemon::Store store(root / "websocket.sqlite");
    aegis::daemon::EventHub events;
    aegis::daemon::api::Context context{&store, &events, nullptr, nullptr, nullptr, root, {}};
    aegis::daemon::api::Server server(context);
    if (!server.start()) throw std::runtime_error("WebSocket soak server failed to start");

    asio::io_context io;
    websocket::stream<Tcp::socket> fast(io);
    websocket::stream<Tcp::socket> slow(io);
    auto connect = [&](auto &socket, bool limitReadBuffer) {
        socket.next_layer().open(Tcp::v4());
        if (limitReadBuffer) socket.next_layer().set_option(asio::socket_base::receive_buffer_size(1024));
        socket.next_layer().connect({asio::ip::make_address("127.0.0.1"), server.port()});
        socket.handshake("127.0.0.1:" + std::to_string(server.port()), "/ws/events");
    };
    connect(fast, false);
    connect(slow, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    constexpr std::size_t eventCount = 1200;
    std::atomic_size_t fastEvents = 0;
    std::atomic_bool fastResync = false;
    std::mutex fastMutex;
    std::condition_variable fastChanged;
    bool fastDone = false;
    std::thread fastReader([&] {
        boost::beast::flat_buffer buffer;
        for (std::size_t index = 0; index < eventCount; ++index) {
            boost::system::error_code error;
            fast.read(buffer, error);
            if (error) break;
            const auto event = nlohmann::json::parse(boost::beast::buffers_to_string(buffer.data()));
            buffer.consume(buffer.size());
            if (event.value("type", std::string{}) == "stream.resync_required") {
                fastResync = true;
                break;
            }
            ++fastEvents;
        }
        {
            std::lock_guard lock(fastMutex);
            fastDone = true;
        }
        fastChanged.notify_one();
    });

    const std::string burst(8192, 'x');
    const auto started = Clock::now();
    for (std::size_t index = 0; index < eventCount; ++index) {
        events.publish({"ws-" + std::to_string(index), "task", "run", "agent.message.completed", "synthetic", burst,
            static_cast<std::int64_t>(index)});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        std::unique_lock lock(fastMutex);
        if (!fastChanged.wait_for(lock, std::chrono::seconds(15), [&] { return fastDone; })) {
            ::shutdown(fast.next_layer().native_handle(), SHUT_RDWR);
            fastReader.join();
            server.stop();
            throw std::runtime_error("fast WebSocket reader did not receive its event burst");
        }
    }

    bool slowResync = false;
    std::size_t slowEvents = 0;
    boost::beast::flat_buffer buffer;
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline && !slowResync) {
        pollfd ready{slow.next_layer().native_handle(), POLLIN, 0};
        if (poll(&ready, 1, 100) <= 0) continue;
        boost::system::error_code error;
        slow.read(buffer, error);
        if (error) break;
        const auto event = nlohmann::json::parse(boost::beast::buffers_to_string(buffer.data()));
        buffer.consume(buffer.size());
        ++slowEvents;
        slowResync = event.value("type", std::string{}) == "stream.resync_required";
    }
    boost::system::error_code ignored;
    fast.next_layer().close(ignored);
    slow.next_layer().close(ignored);
    fastReader.join();
    server.stop();
    if (!slowResync || fastResync || fastEvents != eventCount)
        throw std::runtime_error("WebSocket soak failed: fast_events=" + std::to_string(fastEvents) +
            " fast_resync=" + std::to_string(fastResync) + " slow_events=" + std::to_string(slowEvents) +
            " slow_resync=" + std::to_string(slowResync));
    std::cout << "websocket_clients=2 fast_events=" << fastEvents << " slow_events_before_resync=" << slowEvents
              << " flood_and_recovery_ms=" << millis(started) << '\n';
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("aegis-perf-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto cpuStarted = cpuMs();
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
            aegis::daemon::EventHub requestEvents;
            aegis::daemon::api::Context context{&store, &requestEvents, nullptr, nullptr, nullptr, root, {}};
            std::vector<long long> verifyMs;
            std::vector<long long> handoffMs;
            for (int cycle = 0; cycle < 50; ++cycle) {
                const auto cycleTask = store.createTask("synthetic soak cycle " + std::to_string(cycle), root.string());
                const auto cycleRun = store.startRun(cycleTask.id, "synthetic");
                store.updateRunStatus(cycleRun.id, "running");
                for (int event = 0; event < 20; ++event)
                    store.appendEvent({"soak-" + std::to_string(cycle) + "-" + std::to_string(event), cycleTask.id, cycleRun.id,
                        "agent.message.completed", "synthetic", "burst output", event});
                store.updateRunStatus(cycleRun.id, "completed");
                store.createFinding(cycleTask.id, cycleRun.id, "synthetic.cpp", 1, 1, "Synthetic soak finding.");

                aegis::daemon::api::Request verify{boost::beast::http::verb::post, "/api/verify", 11};
                verify.set(boost::beast::http::field::content_type, "application/json");
                verify.body() = nlohmann::json{{"task_id", cycleTask.id}, {"run_id", cycleRun.id},
                    {"command", {"/usr/bin/printf", "soak-evidence"}}}.dump();
                verify.prepare_payload();
                auto startedAt = Clock::now();
                const auto verified = aegis::daemon::api::handle(verify, context);
                verifyMs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - startedAt).count());
                if (verified.result() != boost::beast::http::status::ok || nlohmann::json::parse(verified.body()).at("output") != "soak-evidence")
                    throw std::runtime_error("verification evidence was not retained");

                aegis::daemon::api::Request handoff{boost::beast::http::verb::get, "/api/tasks/" + cycleTask.id + "/handoff", 11};
                startedAt = Clock::now();
                const auto handedOff = aegis::daemon::api::handle(handoff, context);
                handoffMs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - startedAt).count());
                const auto contextJson = nlohmann::json::parse(handedOff.body());
                if (handedOff.result() != boost::beast::http::status::ok || contextJson.at("verification").at("output") != "soak-evidence" || contextJson.at("findings").empty())
                    throw std::runtime_error("handoff history omitted verification or findings");
            }
            std::cout << "events=100000 transactional_fixture_seed_ms=" << insertMs
                      << " seed_per_s=" << (100000000LL / std::max(1LL, insertMs))
                      << " latest20_p50_us=" << percentile(recentMs, .50)
                      << " latest20_p95_us=" << percentile(recentMs, .95)
                      << " soak_cycles=50 verify_p50_us=" << percentile(verifyMs, .50)
                      << " verify_p95_us=" << percentile(verifyMs, .95)
                      << " handoff_p50_us=" << percentile(handoffMs, .50)
                      << " handoff_p95_us=" << percentile(handoffMs, .95)
                      << " sqlite_bytes=" << std::filesystem::file_size(dbPath) << '\n';
        }

        aegis::daemon::EventHub hub;
        const auto slow = hub.subscribe();
        const auto fast = hub.subscribe();
        aegis::daemon::AgentEvent delivered;
        const std::string burst(8192, 'x');
        std::size_t fastEvents = 0;
        for (std::size_t index = 0; index < 700; ++index) {
            hub.publish({"queue-" + std::to_string(index), "task", "run", "agent.message.completed", "synthetic", burst, static_cast<std::int64_t>(index)});
            if (hub.wait(fast, delivered, std::chrono::milliseconds(1))) ++fastEvents;
        }
        std::size_t slowEvents = 0;
        bool resync = false;
        while (hub.wait(slow, delivered, std::chrono::milliseconds(1))) {
            ++slowEvents;
            if (delivered.type == "stream.resync_required") { resync = true; break; }
        }
        const auto queueHighWater = hub.highWater(slow);
        if (!resync || fastEvents != 700 || queueHighWater.events > aegis::daemon::EventHub::maxQueuedEvents ||
            queueHighWater.bytes > aegis::daemon::EventHub::maxQueuedBytes)
            throw std::runtime_error("bounded slow-subscriber workload did not resynchronize");
        std::cout << "event_subscribers=2 slow_subscriber_events_before_resync=" << slowEvents
                  << " queue_high_water_events=" << queueHighWater.events
                  << " queue_high_water_bytes=" << queueHighWater.bytes << '\n';
        websocketSoak(root);

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
        {
            aegis::daemon::Store store(dbPath);
            aegis::daemon::EventHub events;
            aegis::daemon::agents::Manager manager(repository, store, events);
            std::vector<long long> cycleMs;
            for (int cycle = 0; cycle < 8; ++cycle) {
                const auto started = Clock::now();
                const auto task = store.createTask("agent cycle " + std::to_string(cycle), repository.string());
                const auto run = manager.launch(task.id, "shell");
                if (!run || manager.send(run->id, "sleep 30") != aegis::daemon::agents::SendResult::accepted)
                    throw std::runtime_error("synthetic agent cycle failed to start");
                manager.interrupt(run->id);
                manager.terminate(run->id);
                const auto finished = store.run(run->id);
                if (!finished || (finished->status != "interrupted" && finished->status != "terminated"))
                    throw std::runtime_error("synthetic agent cycle did not reach a terminal state");
                cycleMs.push_back(millis(started));
            }
            if (manager.hasRunningRuns()) throw std::runtime_error("agent cycles left a run active");
            std::cout << "agent_cycles=8 interrupt_terminate_p50_ms=" << percentile(cycleMs, .50)
                      << " interrupt_terminate_p95_ms=" << percentile(cycleMs, .95) << '\n';
        }
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
        std::vector<long long> refreshMs;
        for (int sample = 0; sample < 10; ++sample) {
            const auto refresh = Clock::now();
            if (git.changes(std::nullopt, true).size() != 10000) throw std::runtime_error("repository refresh lost fixture files");
            refreshMs.push_back(millis(refresh));
        }
        std::cout << "repo_files=10000 changes_ms=" << changesMs << " root_listing_ms=" << listingMs
                  << " selected_compare_ms=" << compareMs << " refresh10_p50_ms=" << percentile(refreshMs, .50)
                  << " refresh10_p95_ms=" << percentile(refreshMs, .95) << " listed_entries=" << listing.entries.size()
                  << " benchmark_peak_rss_kib=" << peakRssKiB() << '\n';
        const auto sqliteBytesAfterCycles = std::filesystem::file_size(dbPath);
        cleanup();
        std::cout << "sqlite_bytes_after_cycles=" << sqliteBytesAfterCycles
                  << " cpu_time_ms=" << cpuMs() - cpuStarted << " peak_rss_kib=" << peakRssKiB()
                  << " post_cleanup_rss_kib=" << residentRssKiB() << " temp_files_removed=" << !std::filesystem::exists(root) << '\n';
    } catch (const std::exception &error) {
        cleanup();
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
