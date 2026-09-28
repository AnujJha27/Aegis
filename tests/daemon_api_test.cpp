#include "daemon/api/routes.h"
#include "daemon/api/origin.h"
#include "daemon/api/server.h"
#include "daemon/agents/manager.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/repository/git.h"
#include "daemon/repository/files.h"
#include "daemon/session/store.h"
#include "repository_fixture.h"

#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

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
    assert(nlohmann::json::parse(version.body()).at("schema_version") == 2);

    const auto file = request(context, http::verb::get, "/api/files/content?path=CMakeLists.txt&source=worktree");
    assert(file.result() == http::status::ok);
    assert(nlohmann::json::parse(file.body()).at("content").get<std::string>().find("project(aegis") != std::string::npos);
    const auto listing = request(context, http::verb::get, "/api/files?scope=all");
    assert(listing.result() == http::status::ok);
    assert(!nlohmann::json::parse(listing.body()).at("entries").empty());
    const auto invalidFileOptions = request(context, http::verb::get, "/api/files?scope=all&include_changes=maybe");
    assert(invalidFileOptions.result() == http::status::bad_request);
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

    const auto created = request(context, http::verb::post, "/api/tasks", R"({"prompt":"inspect this"})");
    assert(created.result() == http::status::created);
    assert(created.body().find("inspect this") != std::string::npos);
    const auto taskId = nlohmann::json::parse(created.body()).at("id").get<std::string>();

    const auto findingRun = store.startRun(taskId, "codex");
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

    const auto verification = request(context, http::verb::post, "/api/verify",
        nlohmann::json{{"task_id", taskId}, {"command", {"/usr/bin/printf", "verification-ok"}}}.dump());
    assert(verification.result() == http::status::ok);
    assert(nlohmann::json::parse(verification.body()).at("command").at(1) == "verification-ok");
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
    assert(store.events(taskId).empty());

    using aegis::daemon::api::allowedWebSocketOrigin;
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
