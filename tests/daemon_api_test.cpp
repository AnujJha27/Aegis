#include "daemon/api/routes.h"
#include "daemon/api/origin.h"
#include "daemon/api/server.h"
#include "daemon/api/static_files.h"
#include "daemon/agents/manager.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/repository/git.h"
#include "daemon/repository/files.h"
#include "daemon/session/store.h"
#include "repository_fixture.h"

#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace http = boost::beast::http;
http::response<http::string_body> request(const aegis::daemon::api::Context &context, http::verb method, const std::string &target, std::string body = {}) {
    http::request<http::string_body> message(method, target, 11);
    message.set(http::field::content_type, "application/json");
    message.body() = std::move(body);
    message.prepare_payload();
    return aegis::daemon::api::handle(message, context);
}

std::pair<bool, http::status> eventSocketHandshake(std::uint16_t port, const std::string &origin) {
    namespace asio = boost::asio;
    namespace websocket = boost::beast::websocket;
    asio::io_context io;
    websocket::stream<asio::ip::tcp::socket> socket(io);
    socket.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
    socket.set_option(websocket::stream_base::decorator([&](websocket::request_type &message) {
        if (!origin.empty()) message.set(http::field::origin, origin);
    }));
    http::response<http::string_body> response;
    boost::system::error_code error;
    socket.handshake(response, "127.0.0.1:" + std::to_string(port), "/ws/events", error);
    boost::system::error_code ignored;
    socket.next_layer().close(ignored);
    return {!error, response.result()};
}

http::status httpHealthRequest(std::uint16_t port, const std::string &host) {
    namespace asio = boost::asio;
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    http::request<http::empty_body> request(http::verb::get, "/api/health", 11);
    request.set(http::field::host, host);
    http::write(socket, request);
    http::response<http::string_body> response;
    boost::beast::flat_buffer buffer;
    http::read(socket, buffer, response);
    return response.result();
}

}

int main() {
    const auto database = std::filesystem::temp_directory_path() / "aegis-daemon-api-test.sqlite";
    std::filesystem::remove(database);
    aegis::daemon::Store store(database);
    aegis::daemon::EventHub events;
    aegis::daemon::repository::GitRepository git(std::filesystem::current_path());
    aegis::daemon::repository::Files files(git);
    aegis::daemon::agents::Manager manager(std::filesystem::current_path(), store, events);
    const aegis::daemon::api::Context context{&store, &events, &manager, &git, &files, std::filesystem::current_path(), {}};

    const auto health = request(context, http::verb::get, "/api/health");
    assert(health.result() == http::status::ok);
    assert(health.body().find("healthy") != std::string::npos);
    const auto version = request(context, http::verb::get, "/api/version");
    assert(version.result() == http::status::ok);
    assert(nlohmann::json::parse(version.body()).at("schema_version") == 3);

    const auto file = request(context, http::verb::get, "/api/files/content?path=CMakeLists.txt&source=worktree");
    assert(file.result() == http::status::ok);
    assert(nlohmann::json::parse(file.body()).at("content").get<std::string>().find("project(aegis") != std::string::npos);
    const auto listing = request(context, http::verb::get, "/api/files?scope=all");
    assert(listing.result() == http::status::ok);
    assert(!nlohmann::json::parse(listing.body()).at("entries").empty());
    const auto refreshedGitStatus = request(context, http::verb::get, "/api/git/status?refresh=1");
    assert(refreshedGitStatus.result() == http::status::ok);
    assert(nlohmann::json::parse(refreshedGitStatus.body()).at("version").get<std::uint64_t>() > 0);
    const auto invalidFileOptions = request(context, http::verb::get, "/api/files?scope=all&include_changes=maybe");
    assert(invalidFileOptions.result() == http::status::bad_request);
    const auto invalidRecursive = request(context, http::verb::get, "/api/files?scope=all&recursive=maybe");
    assert(invalidRecursive.result() == http::status::bad_request);
    assert(nlohmann::json::parse(invalidRecursive.body()).at("error").at("code") == "invalid_recursive");
    const auto invalidLarge = request(context, http::verb::get, "/api/files/content?path=CMakeLists.txt&load_large=maybe");
    assert(invalidLarge.result() == http::status::bad_request);
    assert(nlohmann::json::parse(invalidLarge.body()).at("error").at("code") == "invalid_load_large");
    const auto escapedFile = request(context, http::verb::get, "/api/files/content?path=%2e%2e%2fsecret");
    assert(escapedFile.result() == http::status::bad_request);

    RepositoryFixture commitFixture;
    commitFixture.write("space name.cpp", "int value = 0;\n");
    commitFixture.commit("base");
    commitFixture.write("space name.cpp", "int value = 1;\n");
    commitFixture.write("new.cpp", "int fresh = 1;\n");
    commitFixture.commit("review commit");
    auto commitId = commitFixture.git({"rev-parse", "HEAD"});
    while (!commitId.empty() && (commitId.back() == '\n' || commitId.back() == '\r')) commitId.pop_back();
    aegis::daemon::repository::GitRepository commitGit(commitFixture.root());
    aegis::daemon::repository::Files commitFiles(commitGit);
    const aegis::daemon::api::Context commitContext{&store, &events, &manager, &commitGit, &commitFiles, commitFixture.root(), {}};
    const auto commitList = request(commitContext, http::verb::get, "/api/git/commits?limit=2");
    assert(commitList.result() == http::status::ok);
    assert(nlohmann::json::parse(commitList.body()).at(0).at("id") == commitId);
    const auto commitDetail = request(commitContext, http::verb::get, "/api/git/commits/" + commitId);
    assert(commitDetail.result() == http::status::ok);
    assert(nlohmann::json::parse(commitDetail.body()).at("files").size() == 2);
    const auto commitDiff = request(commitContext, http::verb::get,
        "/api/files/compare?path=space%20name.cpp&commit=" + commitId);
    assert(commitDiff.result() == http::status::ok);
    const auto commitComparison = nlohmann::json::parse(commitDiff.body());
    assert(commitComparison.at("original").at("content") == "int value = 0;\n");
    assert(commitComparison.at("modified").at("content") == "int value = 1;\n");
    assert(request(commitContext, http::verb::get, "/api/git/commits/not-a-commit").result() == http::status::not_found);

    RepositoryFixture mergeFixture;
    mergeFixture.write("base.txt", "base\n");
    mergeFixture.commit("base");
    auto currentBranch = mergeFixture.git({"branch", "--show-current"});
    if (!currentBranch.empty() && currentBranch.back() == '\n') currentBranch.pop_back();
    mergeFixture.git({"branch", "feature"});
    mergeFixture.write("main.txt", "main\n");
    mergeFixture.commit("main work");
    mergeFixture.git({"switch", "feature"});
    mergeFixture.write("feature.txt", "feature\n");
    mergeFixture.commit("feature work");
    mergeFixture.git({"switch", currentBranch});
    aegis::daemon::repository::GitRepository mergeGit(mergeFixture.root());
    aegis::daemon::repository::Files mergeFiles(mergeGit);
    const aegis::daemon::api::Context mergeContext{&store, &events, &manager, &mergeGit, &mergeFiles, mergeFixture.root(), {}};
    const auto merged = request(mergeContext, http::verb::post, "/api/git/merge", R"({"branch":"feature"})");
    assert(merged.result() == http::status::ok);
    assert(mergeFiles.read("feature.txt", aegis::daemon::repository::FileSource::worktree).content == "feature\n");
    assert(mergeFiles.read("main.txt", aegis::daemon::repository::FileSource::worktree).content == "main\n");
    const auto guardTask = store.createTask("protect agent work during merge", mergeFixture.root().string());
    const auto activeRun = manager.launch(guardTask.id, "shell");
    assert(activeRun);
    const auto activeMerge = request(mergeContext, http::verb::post, "/api/git/merge", R"({"branch":"feature"})");
    assert(activeMerge.result() == http::status::conflict);
    assert(nlohmann::json::parse(activeMerge.body()).at("error").at("code") == "agent_running");
    assert(manager.terminate(activeRun->id));
    mergeFixture.write("dirty.txt", "keep me\n");
    const auto dirtyMerge = request(mergeContext, http::verb::post, "/api/git/merge", R"({"branch":"feature"})");
    assert(dirtyMerge.result() == http::status::conflict);
    assert(nlohmann::json::parse(dirtyMerge.body()).at("error").at("message").get<std::string>().find("before merging") != std::string::npos);
    assert(std::filesystem::exists(mergeFixture.root() / "dirty.txt"));
    std::filesystem::remove(mergeFixture.root() / "dirty.txt");
    mergeFixture.git({"branch", "conflict"});
    mergeFixture.write("base.txt", "target side\n");
    mergeFixture.commit("target edit");
    mergeFixture.git({"switch", "conflict"});
    mergeFixture.write("base.txt", "source side\n");
    mergeFixture.commit("source edit");
    mergeFixture.git({"switch", currentBranch});
    const auto conflict = request(mergeContext, http::verb::post, "/api/git/merge", R"({"branch":"conflict"})");
    assert(conflict.result() == http::status::conflict);
    assert(nlohmann::json::parse(conflict.body()).at("error").at("message").get<std::string>().find("CONFLICT") != std::string::npos);
    const auto conflictedFile = mergeFiles.read("base.txt", aegis::daemon::repository::FileSource::worktree);
    assert(conflictedFile.content.find("<<<<<<<") != std::string::npos);
    const auto conflicts = mergeGit.changes();
    assert(std::any_of(conflicts.begin(), conflicts.end(), [](const auto &change) {
        return change.path == "base.txt" && change.indexStatus == "U" && change.worktreeStatus == "U";
    }));

    RepositoryFixture handoffFixture;
    handoffFixture.write("modified.cpp", "before\n");
    handoffFixture.write("deleted.cpp", "removed\n");
    handoffFixture.commit("base");
    handoffFixture.write("modified.cpp", "after\n");
    std::filesystem::remove(handoffFixture.root() / "deleted.cpp");
    handoffFixture.write("untracked.cpp", "new\n");
    aegis::daemon::repository::GitRepository handoffGit(handoffFixture.root());
    aegis::daemon::repository::Files handoffFiles(handoffGit);
    const auto handoffTask = store.createTask("review changes", handoffFixture.root().string());
    const aegis::daemon::api::Context handoffContext{&store, &events, &manager, &handoffGit, &handoffFiles, handoffFixture.root(), {}};
    const auto handoffResponse = request(handoffContext, http::verb::get, "/api/tasks/" + handoffTask.id + "/handoff");
    assert(handoffResponse.result() == http::status::ok);
    auto handoffJson = nlohmann::json::parse(handoffResponse.body());
    auto handoffFilesJson = handoffJson.at("changed_files").get<std::vector<std::string>>();
    assert(!handoffJson.at("changed_files_truncated").get<bool>());
    assert(handoffJson.at("diff").get<std::string>().find("new\n") != std::string::npos);
    for (const auto *path : {"modified.cpp", "deleted.cpp", "untracked.cpp"})
        assert(std::find(handoffFilesJson.begin(), handoffFilesJson.end(), path) != handoffFilesJson.end());
    handoffFixture.write("zz-large.txt", std::string(30000, 'x'));
    handoffJson = nlohmann::json::parse(request(handoffContext, http::verb::get,
        "/api/tasks/" + handoffTask.id + "/handoff").body());
    assert(handoffJson.at("diff").get<std::string>().size() <= 24000);
    assert(handoffJson.at("diff").get<std::string>().find("handoff content truncated") != std::string::npos);
    for (int index = 0; index < 105; ++index)
        handoffFixture.write("extra-" + std::to_string(index) + ".txt", "bounded\n");
    handoffJson = nlohmann::json::parse(request(handoffContext, http::verb::get,
        "/api/tasks/" + handoffTask.id + "/handoff").body());
    handoffFilesJson = handoffJson.at("changed_files").get<std::vector<std::string>>();
    assert(handoffFilesJson.size() == 100);
    assert(handoffJson.at("changed_files_truncated").get<bool>());
    assert(handoffJson.at("diff").get<std::string>().size() <= 24000);

    const auto created = request(context, http::verb::post, "/api/tasks", R"({"prompt":"inspect this"})");
    assert(created.result() == http::status::created);
    assert(created.body().find("inspect this") != std::string::npos);
    const auto taskId = nlohmann::json::parse(created.body()).at("id").get<std::string>();

    const auto findingRun = store.startRun(taskId, "codex");
    for (int index = 0; index <= 500; ++index)
        store.appendEvent({"history-" + std::to_string(index), taskId, findingRun.id, "agent.message.completed", "codex", "event", index + 1});
    const auto recentEvents = request(context, http::verb::get, "/api/events?task_id=" + taskId);
    const auto recentEventList = nlohmann::json::parse(recentEvents.body());
    assert(recentEvents.result() == http::status::ok && recentEventList.size() == 500);
    assert(recentEventList.front().at("id") == "history-1" && recentEventList.back().at("id") == "history-500");

    const auto findingCreated = request(context, http::verb::post, "/api/tasks/" + taskId + "/findings",
        nlohmann::json{{"run_id", findingRun.id}, {"file_path", "src/main.cpp"}, {"start_line", 7},
                       {"end_line", 9}, {"message", "Check cleanup on early return."}}.dump());
    assert(findingCreated.result() == http::status::created);
    const auto finding = nlohmann::json::parse(findingCreated.body());
    assert(finding.at("status") == "open" && finding.at("run_id") == findingRun.id);
    const auto findingId = finding.at("id").get<std::string>();
    const auto findingList = request(context, http::verb::get, "/api/tasks/" + taskId + "/findings");
    assert(findingList.result() == http::status::ok && nlohmann::json::parse(findingList.body()).size() == 1);
    const auto findingResolved = request(context, http::verb::patch, "/api/findings/" + findingId, R"({"status":"resolved"})");
    assert(findingResolved.result() == http::status::ok && findingResolved.body().find("resolved") != std::string::npos);
    const auto findingInvalidStatus = request(context, http::verb::patch, "/api/findings/" + findingId, R"({"status":"critical"})");
    assert(findingInvalidStatus.result() == http::status::bad_request);
    const auto findingInvalidPath = request(context, http::verb::post, "/api/tasks/" + taskId + "/findings",
        R"({"file_path":"../outside.cpp","message":"unsafe reference"})");
    assert(findingInvalidPath.result() == http::status::bad_request);
    assert(store.updateRunStatus(findingRun.id, "running"));
    assert(store.updateRunStatus(findingRun.id, "completed"));
    assert(store.deleteRun(findingRun.id));
    assert(!store.findings(taskId).front().runId);

    const auto missingTaskRun = request(context, http::verb::post, "/api/tasks/missing/runs", R"({"agent":"codex"})");
    assert(missingTaskRun.result() == http::status::not_found);
    assert(missingTaskRun.body().find("task_not_found") != std::string::npos);
    const auto missingRunMessage = request(context, http::verb::post, "/api/runs/missing/messages", R"({"message":"hello"})");
    assert(missingRunMessage.result() == http::status::not_found);

    const auto noTaskVerification = request(context, http::verb::post, "/api/verify", R"({"command":["true"]})");
    assert(noTaskVerification.result() == http::status::bad_request);

    const auto verificationSubscription = events.subscribe();
    const auto verification = request(context, http::verb::post, "/api/verify",
        nlohmann::json{{"task_id", taskId}, {"command", {"/usr/bin/printf", "verification-ok"}}}.dump());
    assert(verification.result() == http::status::ok);
    assert(nlohmann::json::parse(verification.body()).at("command").at(1) == "verification-ok");
    aegis::daemon::AgentEvent verificationStarted;
    aegis::daemon::AgentEvent verificationCompleted;
    assert(events.wait(verificationSubscription, verificationStarted, std::chrono::milliseconds(100)));
    assert(events.wait(verificationSubscription, verificationCompleted, std::chrono::milliseconds(100)));
    assert(verificationStarted.type == "verification.started" && verificationStarted.taskId == taskId);
    assert(verificationCompleted.type == "verification.completed" && verificationCompleted.content == "exit_code=0");
    events.unsubscribe(verificationSubscription);
    const auto verificationHistory = request(context, http::verb::get, "/api/tasks/" + taskId + "/verifications");
    assert(verificationHistory.result() == http::status::ok);
    assert(nlohmann::json::parse(verificationHistory.body()).size() == 1);

    const auto tasks = request(context, http::verb::get, "/api/tasks");
    assert(tasks.result() == http::status::ok);
    assert(tasks.body().find("inspect this") != std::string::npos);

    const auto invalid = request(context, http::verb::post, "/api/tasks", "not-json");
    assert(invalid.result() == http::status::bad_request);
    assert(invalid.body().find("invalid_json") != std::string::npos);

    const auto handoff = request(context, http::verb::get, "/api/tasks/" + taskId + "/handoff");
    assert(handoff.result() == http::status::ok);
    assert(handoff.body().find("inspect this") != std::string::npos);
    assert(handoff.body().find("verification-ok") != std::string::npos);
    assert(nlohmann::json::parse(handoff.body()).at("findings").size() == 1);

    RepositoryFixture staticFixture;
    staticFixture.write("web/index.html", "Aegis UI");
    staticFixture.write("outside/secret.txt", "outside secret");
    std::filesystem::create_directories(staticFixture.root() / "web/assets");
    std::filesystem::create_symlink(staticFixture.root() / "outside/secret.txt", staticFixture.root() / "web/assets/leak.txt");
    const auto staticIndex = aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/");
    assert(staticIndex.result() == http::status::ok && staticIndex.body() == "Aegis UI");
    assert(staticIndex[http::field::cache_control] == "no-cache");
    staticFixture.write("web/assets/app.js", "export {};");
    const auto staticAsset = aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/assets/app.js");
    assert(staticAsset[http::field::cache_control] == "public, max-age=31536000, immutable");
    const auto staticQuery = aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/?cache=1");
    assert(staticQuery.result() == http::status::ok && staticQuery.body() == "Aegis UI");
    const auto staticEscape = aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/assets/leak.txt");
    assert(staticEscape.result() == http::status::not_found && staticEscape.body().empty());
    assert(aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/../outside/secret.txt").result() == http::status::bad_request);
    staticFixture.write("web/large.bin", "");
    std::filesystem::resize_file(staticFixture.root() / "web/large.bin", 16 * 1024 * 1024 + 1);
    assert(aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/large.bin").result() == http::status::payload_too_large);

    const auto graph = request(context, http::verb::get, "/api/tasks/" + taskId + "/graph");
    assert(graph.result() == http::status::ok);
    assert(graph.body().find("nodes") != std::string::npos);

    const auto provenance = request(context, http::verb::get, "/api/tasks/" + taskId + "/provenance");
    assert(provenance.result() == http::status::ok);
    assert(provenance.body().find("records") != std::string::npos);

    const auto missing = request(context, http::verb::get, "/api/tasks/missing/handoff");
    assert(missing.result() == http::status::not_found);

    const auto run = store.startRun(taskId, "claude");
    store.appendEvent({"event-1", taskId, run.id, "agent.failed", "claude", "failed", 1});
    const auto deleted = request(context, http::verb::delete_, "/api/runs/" + run.id);
    assert(deleted.result() == http::status::no_content);
    assert(store.runs(taskId).empty());
    const auto remainingEvents = store.events(taskId);
    assert(remainingEvents.size() == 2);
    assert(std::all_of(remainingEvents.begin(), remainingEvents.end(), [](const auto &event) {
        return event.runId.empty() && event.type.starts_with("verification.");
    }));

    using aegis::daemon::api::allowedWebSocketOrigin;
    using aegis::daemon::api::allowedLoopbackHost;
    assert(allowedLoopbackHost("127.0.0.1:46729"));
    assert(allowedLoopbackHost("localhost:5173"));
    assert(!allowedLoopbackHost("attacker.example"));
    assert(!allowedLoopbackHost("localhost.evil.example:5173"));
    assert(!allowedLoopbackHost("127.0.0.1:46729", 0));
    assert(!allowedLoopbackHost("127.0.0.1:46729", 2));
    assert(allowedWebSocketOrigin(""));
    assert(allowedWebSocketOrigin("http://127.0.0.1:46729"));
    assert(allowedWebSocketOrigin("http://localhost:5173"));
    assert(allowedWebSocketOrigin("http://[::1]:5173"));
    assert(!allowedWebSocketOrigin("https://attacker.example"));
    assert(!allowedWebSocketOrigin("http://attacker.example"));
    assert(!allowedWebSocketOrigin("http://localhost.evil.example:5173"));
    assert(!allowedWebSocketOrigin("http://localhost:99999"));
    assert(!allowedWebSocketOrigin("http://localhost:5173/path"));

    aegis::daemon::api::Server server(context);
    if (server.start()) {
        const auto port = server.port();
        assert(httpHealthRequest(port, "127.0.0.1:" + std::to_string(port)) == http::status::ok);
        assert(httpHealthRequest(port, "attacker.example") == http::status::forbidden);
        const auto appOrigin = eventSocketHandshake(port, "http://127.0.0.1:" + std::to_string(port));
        assert(appOrigin.first);
        const auto viteOrigin = eventSocketHandshake(port, "http://localhost:5173");
        assert(viteOrigin.first);
        const auto externalOrigin = eventSocketHandshake(port, "https://attacker.example");
        assert(!externalOrigin.first && externalOrigin.second == http::status::forbidden);
        server.stop();
    } else {
        std::clog << "SKIP WebSocket handshake integration: loopback bind unavailable\n";
    }

    std::filesystem::remove(database);
}
