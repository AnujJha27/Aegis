#include "daemon/api/routes.h"

#include "daemon/agents/manager.h"
#include "daemon/protocol/json.h"
#include "daemon/repository/git.h"
#include "daemon/verification/runner.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace aegis::daemon::api {
namespace {

Response jsonResponse(boost::beast::http::status status, const nlohmann::json &body) {
    Response response{status, 11};
    response.set(boost::beast::http::field::content_type, "application/json");
    response.body() = body.dump();
    response.prepare_payload();
    return response;
}

Response error(boost::beast::http::status status, const char *code, const char *message) {
    return jsonResponse(status, {{"error", {{"code", code}, {"message", message}}}});
}

bool isDirectory(const std::string &path) {
    return std::filesystem::is_directory(std::filesystem::path(path));
}

std::optional<std::string> pathId(const std::string &target, const std::string &suffix) {
    constexpr std::string_view prefix = "/api/tasks/";
    if (!target.starts_with(prefix) || !target.ends_with(suffix)) return std::nullopt;
    const auto id = target.substr(prefix.size(), target.size() - prefix.size() - suffix.size());
    return id.empty() ? std::nullopt : std::optional{id};
}

std::vector<std::string> changedFiles(const std::string &diff) {
    std::vector<std::string> files;
    std::size_t start = 0;
    while (start < diff.size()) {
        const auto end = diff.find('\n', start);
        const auto line = diff.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.starts_with("+++ b/") && line != "+++ /dev/null") files.push_back(line.substr(6));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return files;
}

nlohmann::json gitSnapshot(const Context &context) {
    nlohmann::json files = nlohmann::json::array();
    for (const auto &file : context.git->changes()) files.push_back(protocol::toJson(file));
    return {{"repository", protocol::toJson(context.git->state())},
            {"files", files},
            {"branches", context.git->branches()},
            {"current_branch", context.git->currentBranch()},
            {"clean", context.git->clean()},
            {"agent_running", context.agentManager && context.agentManager->hasRunningRuns()}};
}

}

Response handle(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    if (request.method() == boost::beast::http::verb::get && target == "/api/health")
        return jsonResponse(boost::beast::http::status::ok, {{"status", "healthy"}});

    if (request.method() == boost::beast::http::verb::get && target == "/api/repository") {
        if (context.git) return jsonResponse(boost::beast::http::status::ok, protocol::toJson(context.git->state()));
        return jsonResponse(boost::beast::http::status::ok, {{"path", context.repository.string()}, {"exists", isDirectory(context.repository.string())}});
    }

    if (request.method() == boost::beast::http::verb::get && target == "/api/changes") {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        return jsonResponse(boost::beast::http::status::ok, {{"diff", context.git->diff()}});
    }

    if (request.method() == boost::beast::http::verb::get && target == "/api/git/status") {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        return jsonResponse(boost::beast::http::status::ok, gitSnapshot(context));
    }

    if (request.method() == boost::beast::http::verb::post && (target == "/api/git/branch" || target == "/api/git/pull" || target == "/api/git/push")) {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        if (target != "/api/git/push" && context.agentManager && context.agentManager->hasRunningRuns())
            return error(boost::beast::http::status::conflict, "agent_running", "stop the active agent before switching branches or pulling");
        try {
            const auto body = request.body().empty() ? nlohmann::json::object() : nlohmann::json::parse(request.body());
            std::string output;
            bool succeeded = false;
            if (target == "/api/git/branch") {
                const auto branch = body.value("branch", std::string{});
                succeeded = context.git->switchBranch(branch, output);
            } else if (target == "/api/git/pull") succeeded = context.git->pull(output);
            else succeeded = context.git->push(output);
            if (!succeeded) return jsonResponse(target == "/api/git/push" || target == "/api/git/pull" ? boost::beast::http::status::bad_gateway : boost::beast::http::status::bad_request,
                                                {{"error", {{"code", "git_operation_failed"}, {"message", output.empty() ? "Git operation failed" : output}}}});
            auto result = gitSnapshot(context);
            result["output"] = output;
            return jsonResponse(boost::beast::http::status::ok, result);
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }

    if (request.method() == boost::beast::http::verb::post && (target == "/api/git/stage" || target == "/api/git/unstage" || target == "/api/git/commit")) {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        try {
            const auto body = nlohmann::json::parse(request.body());
            std::string output;
            bool succeeded = false;
            if (target == "/api/git/commit") {
                const auto message = body.value("message", std::string{});
                succeeded = context.git->commit(message, output);
            } else {
                const auto path = body.value("path", std::string{});
                succeeded = target == "/api/git/stage" ? context.git->stage(path, output) : context.git->unstage(path, output);
            }
            if (!succeeded) return error(boost::beast::http::status::bad_request, "git_operation_failed", output.empty() ? "Git operation failed" : output.c_str());
            auto result = gitSnapshot(context);
            result["output"] = output;
            return jsonResponse(boost::beast::http::status::ok, result);
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }

    if (request.method() == boost::beast::http::verb::get && target == "/api/agents") {
        if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
        nlohmann::json result = nlohmann::json::array();
        for (const auto &agent : context.agentManager->available())
            result.push_back({{"name", agent.name}, {"available", agent.available}, {"structured", agent.capabilities.structured}, {"interactive", agent.capabilities.interactive}});
        return jsonResponse(boost::beast::http::status::ok, result);
    }

    if (request.method() == boost::beast::http::verb::get && target == "/api/tasks") {
        nlohmann::json result = nlohmann::json::array();
        for (const auto &task : context.store->tasks()) result.push_back(protocol::toJson(task));
        return jsonResponse(boost::beast::http::status::ok, result);
    }

    if (request.method() == boost::beast::http::verb::get) {
        if (const auto taskId = pathId(target, "/runs")) {
            nlohmann::json result = nlohmann::json::array();
            for (const auto &run : context.store->runs(*taskId)) result.push_back(protocol::toJson(run));
            return jsonResponse(boost::beast::http::status::ok, result);
        }
    }

    if (request.method() == boost::beast::http::verb::get) {
        if (const auto taskId = pathId(target, "/handoff")) {
            const auto task = context.store->task(*taskId);
            if (!task) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            auto events = context.store->events(*taskId);
            constexpr std::size_t maxEvents = 20;
            if (events.size() > maxEvents) events.erase(events.begin(), events.end() - maxEvents);
            for (auto &event : events) {
                constexpr std::size_t maxContent = 4000;
                if (event.content.size() > maxContent) event.content.resize(maxContent);
            }
            const auto diff = context.git ? context.git->diff() : std::string{};
            HandoffContext handoff{task->id, task->prompt, std::move(events), diff, changedFiles(diff), std::nullopt};
            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(handoff));
        }
        if (const auto taskId = pathId(target, "/graph")) {
            if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            TaskGraph graph{*taskId, {{"task:" + *taskId, "task", "Task"}}, {}};
            for (const auto &run : context.store->runs(*taskId)) {
                const auto runNode = "run:" + run.id;
                graph.nodes.push_back({runNode, "run", run.agent});
                graph.edges.push_back({"task:" + *taskId, runNode});
                for (const auto &event : context.store->events(*taskId)) {
                    if (event.runId != run.id) continue;
                    const auto eventNode = "event:" + event.id;
                    graph.nodes.push_back({eventNode, "event", event.type});
                    graph.edges.push_back({runNode, eventNode});
                }
            }
            const auto files = changedFiles(context.git ? context.git->diff() : std::string{});
            const auto runId = context.store->runs(*taskId);
            for (const auto &file : files) {
                const auto fileNode = "file:" + file;
                graph.nodes.push_back({fileNode, "file", file});
                graph.edges.push_back({runId.empty() ? "task:" + *taskId : "run:" + runId.back().id, fileNode});
            }
            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(graph));
        }
        if (const auto taskId = pathId(target, "/provenance")) {
            if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            nlohmann::json records = nlohmann::json::array();
            for (const auto &event : context.store->events(*taskId)) {
                ProvenanceRecord record{event.id, event.taskId, event.runId, event.agent, event.type, event.timestamp};
                if (event.type == "file.changed" && !event.content.empty()) record.changedFiles.push_back(event.content);
                records.push_back(protocol::toJson(record));
            }
            return jsonResponse(boost::beast::http::status::ok, {{"task_id", *taskId}, {"records", records}});
        }
    }

    if (request.method() == boost::beast::http::verb::get && target.starts_with("/api/events")) {
        const auto marker = target.find("task_id=");
        if (marker == std::string::npos) return error(boost::beast::http::status::bad_request, "missing_task_id", "task_id is required");
        const auto taskId = target.substr(marker + 8);
        nlohmann::json result = nlohmann::json::array();
        for (const auto &event : context.store->events(taskId)) result.push_back(protocol::toJson(event));
        return jsonResponse(boost::beast::http::status::ok, result);
    }

    if (request.method() == boost::beast::http::verb::post && target == "/api/tasks") {
        try {
            const auto body = nlohmann::json::parse(request.body());
            const auto prompt = body.value("prompt", std::string{});
            const auto repository = body.value("repository", context.repository.string());
            if (prompt.empty()) return error(boost::beast::http::status::bad_request, "missing_prompt", "prompt is required");
            if (!isDirectory(repository)) return error(boost::beast::http::status::bad_request, "invalid_repository", "repository must be a directory");
            return jsonResponse(boost::beast::http::status::created, protocol::toJson(context.store->createTask(prompt, repository)));
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }

    if (request.method() == boost::beast::http::verb::post && target.starts_with("/api/tasks/") && target.ends_with("/runs")) {
        if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
        try {
            const auto body = nlohmann::json::parse(request.body());
            const auto agent = body.value("agent", std::string{});
            const auto taskId = target.substr(11, target.size() - 16);
            if (agent.empty()) return error(boost::beast::http::status::bad_request, "missing_agent", "agent is required");
            const auto run = context.agentManager->launch(taskId, agent);
            if (!run) return error(boost::beast::http::status::bad_request, "agent_start_failed", "agent could not be started");
            return jsonResponse(boost::beast::http::status::created, protocol::toJson(*run));
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }

    if (request.method() == boost::beast::http::verb::post && target.starts_with("/api/runs/") && target.ends_with("/messages")) {
        if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
        try {
            const auto body = nlohmann::json::parse(request.body());
            const auto message = body.value("message", std::string{});
            const auto runId = target.substr(10, target.size() - 19);
            if (message.empty()) return error(boost::beast::http::status::bad_request, "missing_message", "message is required");
            if (!context.agentManager->send(runId, message)) return error(boost::beast::http::status::not_found, "run_not_found", "agent run not found");
            return jsonResponse(boost::beast::http::status::accepted, {{"status", "sent"}});
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }

    if (request.method() == boost::beast::http::verb::post && target.starts_with("/api/runs/") && target.ends_with("/interrupt")) {
        if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
        const auto runId = target.substr(10, target.size() - 20);
        if (!context.agentManager->interrupt(runId)) return error(boost::beast::http::status::not_found, "run_not_found", "agent run not found");
        return jsonResponse(boost::beast::http::status::accepted, {{"status", "interrupted"}});
    }

    if (request.method() == boost::beast::http::verb::post && target == "/api/verify") {
        try {
            const auto body = nlohmann::json::parse(request.body());
            if (!body.contains("command") || !body["command"].is_array()) return error(boost::beast::http::status::bad_request, "invalid_command", "command must be an argument array");
            std::vector<std::string> command;
            for (const auto &part : body["command"]) command.push_back(part.get<std::string>());
            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(aegis::daemon::verification::run(command, context.repository)));
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }

    return error(boost::beast::http::status::not_found, "not_found", "route not found");
}

}
