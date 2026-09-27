#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/verification/runner.h"

#include <nlohmann/json.hpp>

namespace aegis::daemon::api::routes {

std::optional<Response> verification(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    if (request.method() == boost::beast::http::verb::get) {
        if (const auto taskId = pathId(target, "/verifications")) {
            if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            nlohmann::json result = nlohmann::json::array();
            for (const auto &run : context.store->verifications(*taskId)) result.push_back(protocol::toJson(run));
            return jsonResponse(boost::beast::http::status::ok, result);
        }
    }
    if (request.method() == boost::beast::http::verb::post && target == "/api/verify") {
        try {
            const auto body = nlohmann::json::parse(request.body());
            if (!body.contains("command") || !body["command"].is_array()) return error(boost::beast::http::status::bad_request, "invalid_command", "command must be an argument array");
            std::vector<std::string> command;
            for (const auto &part : body["command"]) {
                if (!part.is_string()) return error(boost::beast::http::status::bad_request, "invalid_command", "command arguments must be strings");
                command.push_back(part.get<std::string>());
            }
            if (command.empty()) return error(boost::beast::http::status::bad_request, "invalid_command", "command must not be empty");
            const auto taskId = body.value("task_id", std::string{});
            if (taskId.empty()) return error(boost::beast::http::status::bad_request, "missing_task_id", "task_id is required");
            if (!context.store->task(taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            std::optional<std::string> runId;
            if (body.contains("run_id") && !body["run_id"].is_null()) {
                if (!body["run_id"].is_string()) return error(boost::beast::http::status::bad_request, "invalid_run_id", "run_id must be a string");
                runId = body["run_id"].get<std::string>();
                const auto run = context.store->run(*runId);
                if (!run || run->taskId != taskId) return error(boost::beast::http::status::not_found, "run_not_found", "run not found for task");
            }
            auto result = aegis::daemon::verification::run(command, context.repository, taskId, runId);
            context.store->saveVerification(result);
            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(result));
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }
    return std::nullopt;
}

}
