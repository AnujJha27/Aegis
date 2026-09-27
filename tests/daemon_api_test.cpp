#include "daemon/api/routes.h"
#include "daemon/agents/manager.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/repository/git.h"
#include "daemon/repository/files.h"
#include "daemon/session/store.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
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
    assert(nlohmann::json::parse(version.body()).at("schema_version") == 1);

    const auto file = request(context, http::verb::get, "/api/files/content?path=CMakeLists.txt&source=worktree");
    assert(file.result() == http::status::ok);
    assert(nlohmann::json::parse(file.body()).at("content").get<std::string>().find("project(aegis") != std::string::npos);
    const auto listing = request(context, http::verb::get, "/api/files?scope=all");
    assert(listing.result() == http::status::ok);
    assert(!nlohmann::json::parse(listing.body()).at("entries").empty());
    const auto escapedFile = request(context, http::verb::get, "/api/files/content?path=%2e%2e%2fsecret");
    assert(escapedFile.result() == http::status::bad_request);

    const auto created = request(context, http::verb::post, "/api/tasks", R"({"prompt":"inspect this"})");
    assert(created.result() == http::status::created);
    assert(created.body().find("inspect this") != std::string::npos);
    const auto taskId = nlohmann::json::parse(created.body()).at("id").get<std::string>();

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

    std::filesystem::remove(database);
}
