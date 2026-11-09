#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/agents/event_id.h"
#include "daemon/agents/manager.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>

namespace aegis::daemon::api::routes {

std::optional<Response> runs(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    const auto method = request.method();
    if (method == boost::beast::http::verb::get) {
        if (const auto taskId = pathId(target, "/runs")) {
            if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            nlohmann::json result = nlohmann::json::array();
            for (const auto &run : context.store->runs(*taskId)) {
                auto item = protocol::toJson(run);
                if (const auto turn = context.store->latestTurnEvent(run.id)) item["turn_status"] = turn->type;
                result.push_back(std::move(item));
            }
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
        if (const auto continuationId = pathId(target, "/continue")) {
            const auto &taskId = *continuationId;
            if (!context.store->task(taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
            nlohmann::json body;
            try {
                body = nlohmann::json::parse(request.body());
            } catch (const nlohmann::json::exception &) {
                return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
            }
            if (!body.contains("agent") || !body["agent"].is_string() ||
                !body.contains("source_run_id") || !body["source_run_id"].is_string() ||
                !body.contains("prompt") || !body["prompt"].is_string())
                return error(boost::beast::http::status::bad_request, "malformed_body", "agent, source_run_id and prompt are required strings");
            const auto agent = body["agent"].get<std::string>();
            const auto sourceRunId = body["source_run_id"].get<std::string>();
            const auto prompt = body["prompt"].get<std::string>();
            if (agent != "codex" && agent != "claude" && agent != "opencode")
                return error(boost::beast::http::status::bad_request, "unsupported_agent", "continuation only supports codex, claude or opencode");
            const auto nonBlank = [](const std::string &value) {
                if (value.empty() || value.size() > 65536 || value.find('\0') != std::string::npos) return false;
                for (const char c : value)
                    if (!std::isspace(static_cast<unsigned char>(c))) return true;
                return false;
            };
            if (!nonBlank(prompt)) return error(boost::beast::http::status::bad_request, "invalid_prompt", "prompt must be non-blank, without NUL and at most 65536 bytes");
            const auto source = context.store->run(sourceRunId);
            if (!source || source->taskId != taskId) return error(boost::beast::http::status::not_found, "run_not_found", "source run not found for this task");
            const auto task = context.store->task(taskId);
            if (!task) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            if (!matchesRepository(task->repository, context.repository))
                return error(boost::beast::http::status::bad_request, "repository_mismatch", "task repository does not match daemon repository");
            bool targetAvailable = false;
            for (const auto &info : context.agentManager->available())
                if (info.name == agent && info.available) { targetAvailable = true; break; }
            if (!targetAvailable) return error(boost::beast::http::status::bad_request, "agent_unavailable", "target agent is not available");
            if (source->status == "starting")
                return error(boost::beast::http::status::conflict, "source_busy", "source run is starting");
            if (source->status == "running") {
                if (source->agent == "codex") {
                    if (const auto turn = context.store->latestTurnEvent(sourceRunId); turn && turn->type == "turn.started")
                        return error(boost::beast::http::status::conflict, "source_busy", "source run has an active turn");
                } else {
                    return error(boost::beast::http::status::conflict, "run_active", "stop the active run before continuing from it");
                }
            }
            const auto run = context.agentManager->launch(taskId, agent, prompt);
            if (!run) return error(boost::beast::http::status::bad_request, "agent_start_failed", "agent could not be started");
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const aegis::daemon::AgentEvent continued{agents::newEventId(), run->taskId, run->id, "run.continued", run->agent, sourceRunId, nowMs};
            context.store->appendEvent(continued);
            context.events->publish(continued);
            return jsonResponse(boost::beast::http::status::created, protocol::toJson(*run));
        }
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
