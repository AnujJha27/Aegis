#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/agents/manager.h"

#include <nlohmann/json.hpp>

namespace aegis::daemon::api::routes {

std::optional<Response> runs(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    const auto method = request.method();
    if (method == boost::beast::http::verb::get) {
        if (const auto taskId = pathId(target, "/runs")) {
            if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            nlohmann::json result = nlohmann::json::array();
            for (const auto &run : context.store->runs(*taskId)) result.push_back(protocol::toJson(run));
            return jsonResponse(boost::beast::http::status::ok, result);
        }
    }
    if (method == boost::beast::http::verb::delete_) {
        const auto runId = runPathId(target, "");
        if (runId) {
            if (context.agentManager && context.agentManager->isRunning(*runId))
                return error(boost::beast::http::status::conflict, "run_active", "stop the active run before deleting it");
            if (context.agentManager) context.agentManager->terminate(*runId);
            if (!context.store->deleteRun(*runId)) return error(boost::beast::http::status::not_found, "run_not_found", "agent run not found");
            Response response{boost::beast::http::status::no_content, 11};
            response.prepare_payload();
            return response;
        }
    }
    if (method == boost::beast::http::verb::post) {
        if (const auto taskId = pathId(target, "/runs")) {
            if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
            try {
                const auto body = nlohmann::json::parse(request.body());
                const auto agent = body.value("agent", std::string{});
                if (agent.empty()) return error(boost::beast::http::status::bad_request, "missing_agent", "agent is required");
                const auto run = context.agentManager->launch(*taskId, agent);
                if (!run) return error(boost::beast::http::status::bad_request, "agent_start_failed", "agent could not be started");
                return jsonResponse(boost::beast::http::status::created, protocol::toJson(*run));
            } catch (const nlohmann::json::exception &) {
                return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
            }
        }
        if (const auto runId = runPathId(target, "/messages")) {
            if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
            try {
                const auto body = nlohmann::json::parse(request.body());
                const auto message = body.value("message", std::string{});
                if (message.empty()) return error(boost::beast::http::status::bad_request, "missing_message", "message is required");
                const auto result = context.agentManager->send(*runId, message);
                if (result == agents::SendResult::busy) return error(boost::beast::http::status::conflict, "run_busy", "an agent turn is already running");
                if (result == agents::SendResult::unavailable) return error(boost::beast::http::status::not_found, "run_not_found", "agent run is not active");
                return jsonResponse(boost::beast::http::status::accepted, {{"status", "sent"}});
            } catch (const nlohmann::json::exception &) {
                return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
            }
        }
        if (const auto runId = runPathId(target, "/interrupt")) {
            if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
            if (!context.agentManager->interrupt(*runId)) return error(boost::beast::http::status::not_found, "run_not_found", "agent run not found");
            return jsonResponse(boost::beast::http::status::accepted, {{"status", "interrupted"}});
        }
        if (const auto runId = runPathId(target, "/terminate")) {
            if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
            if (!context.agentManager->terminate(*runId)) return error(boost::beast::http::status::not_found, "run_not_found", "agent run not found");
            return jsonResponse(boost::beast::http::status::accepted, {{"status", "terminated"}});
        }
    }
    return std::nullopt;
}

}
