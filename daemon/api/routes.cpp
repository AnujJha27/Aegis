#include "daemon/api/routes.h"

#include "daemon/agents/manager.h"
#include "daemon/protocol/json.h"
#include "daemon/repository/git.h"
#include "daemon/verification/runner.h"

#include <nlohmann/json.hpp>

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
