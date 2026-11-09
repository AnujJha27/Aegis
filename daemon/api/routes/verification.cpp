#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/repository/git.h"
#include "daemon/verification/runner.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace aegis::daemon::api::routes {
namespace {

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string nextEventId() {
    static std::atomic<std::uint64_t> sequence{};
    return "verification-event-" + std::to_string(now()) + "-" + std::to_string(++sequence);
}

void publishVerificationEvent(const Context &context, const std::string &taskId,
                              const std::optional<std::string> &runId,
                              std::string type, std::string content = {}) {
    AgentEvent event{nextEventId(), taskId, runId.value_or(""), std::move(type), "verification", std::move(content), now()};
    context.store->appendEvent(event);
    context.events->publish(event);
}

}

std::optional<Response> verification(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    if (request.method() == boost::beast::http::verb::get) {
        if (const auto taskId = pathId(target, "/verifications")) {
            const auto task = context.store->task(*taskId);
            if (!task) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            if (!matchesRepository(task->repository, context.repository))
                return error(boost::beast::http::status::bad_request, "repository_mismatch", "task repository does not match daemon repository");
            const auto records = context.store->verifications(*taskId);
            std::optional<std::string> currentFingerprint;
            if (!records.empty() && std::any_of(records.begin(), records.end(),
                    [](const VerificationRun &run) { return run.repositoryFingerprint.has_value(); }) && context.git)
                currentFingerprint = context.git->verificationFingerprint();
            nlohmann::json result = nlohmann::json::array();
            for (auto run : records) {
                run.freshness = !run.repositoryFingerprint || !currentFingerprint ? "unknown"
                    : (*currentFingerprint == *run.repositoryFingerprint ? "current" : "stale");
                result.push_back(protocol::toJson(run));
            }
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
            const auto task = context.store->task(taskId);
            if (!task) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            if (!matchesRepository(task->repository, context.repository))
                return error(boost::beast::http::status::bad_request, "repository_mismatch", "task repository does not match daemon repository");
            std::optional<std::string> runId;
            if (body.contains("run_id") && !body["run_id"].is_null()) {
                if (!body["run_id"].is_string()) return error(boost::beast::http::status::bad_request, "invalid_run_id", "run_id must be a string");
                runId = body["run_id"].get<std::string>();
                const auto run = context.store->run(*runId);
                if (!run || run->taskId != taskId) return error(boost::beast::http::status::not_found, "run_not_found", "run not found for task");
            }
            if (command.front().empty() || command.front().find('\0') != std::string::npos ||
                std::all_of(command.front().begin(), command.front().end(), [](unsigned char c) { return std::isspace(c); }))
                return error(boost::beast::http::status::bad_request, "invalid_command", "command executable must not be blank");
            for (const auto &argument : command)
                if (argument.find('\0') != std::string::npos)
                    return error(boost::beast::http::status::bad_request, "invalid_command", "command arguments must not contain NUL");
            publishVerificationEvent(context, taskId, runId, "verification.started");
            auto result = aegis::daemon::verification::run(command, context.repository, taskId, runId);
            context.store->saveVerification(result);
            publishVerificationEvent(context, taskId, runId, "verification.completed", "exit_code=" + std::to_string(result.exitCode));
            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(result));
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }
    return std::nullopt;
}

}
