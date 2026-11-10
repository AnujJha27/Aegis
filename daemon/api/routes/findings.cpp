#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/agents/event_id.h"
#include "daemon/repository/git.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <limits>

namespace aegis::daemon::api::routes {
namespace {

std::optional<std::string> findingId(const std::string &path) {
    constexpr std::string_view prefix = "/api/findings/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto id = path.substr(prefix.size());
    return id.empty() || id.find('/') != std::string::npos ? std::nullopt : std::optional{id};
}

bool validFilePath(const std::string &value) {
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos) return false;
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name()) return false;
    for (const auto &part : path)
        if (part == ".." || part == ".git" || part == ".aegis" || part == "node_modules") return false;
    return true;
}

bool validSourceEventId(const std::string &value) {
    return !value.empty() && value.size() <= 200 && value.find('\0') == std::string::npos;
}

std::optional<int> lineValue(const nlohmann::json &body, const char *key, bool &valid) {
    if (!body.contains(key) || body.at(key).is_null()) return std::nullopt;
    const auto &value = body.at(key);
    if (!value.is_number_integer()) { valid = false; return std::nullopt; }
    const auto line = value.get<std::int64_t>();
    if (line < 1 || line > std::numeric_limits<int>::max()) { valid = false; return std::nullopt; }
    return static_cast<int>(line);
}

}

std::optional<Response> findings(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    const auto path = target.substr(0, target.find('?'));
    const auto taskId = pathId(target, "/findings");
    const auto id = findingId(path);
    if (!taskId && !id) return std::nullopt;

    if (taskId) {
        const auto task = context.store->task(*taskId);
        if (!task) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        if (!matchesRepository(task->repository, context.repository))
            return error(boost::beast::http::status::bad_request, "repository_mismatch", "task repository does not match daemon repository");
    }

    if (taskId && request.method() == boost::beast::http::verb::get) {
        const auto currentFingerprint = context.git ? context.git->verificationFingerprint() : std::nullopt;
        nlohmann::json result = nlohmann::json::array();
        for (auto finding : context.store->findings(*taskId)) {
            finding.freshness = !finding.repositoryFingerprint || !currentFingerprint ? "unknown"
                : (*currentFingerprint == *finding.repositoryFingerprint ? "current" : "stale");
            result.push_back(protocol::toJson(finding));
        }
        return jsonResponse(boost::beast::http::status::ok, result);
    }

    if (taskId && request.method() == boost::beast::http::verb::post) {
        try {
            const auto body = nlohmann::json::parse(request.body());
            if (!body.is_object() || !body.contains("file_path") || !body.at("file_path").is_string() ||
                !body.contains("message") || !body.at("message").is_string())
                return error(boost::beast::http::status::bad_request, "invalid_finding", "file_path and message must be strings");
            const auto filePath = body.at("file_path").get<std::string>();
            const auto message = body.at("message").get<std::string>();
            if (!validFilePath(filePath)) return error(boost::beast::http::status::bad_request, "invalid_file_path", "file_path must be a repository-relative source path");
            if (message.empty() || message.size() > 8000 || message.find('\0') != std::string::npos ||
                std::all_of(message.begin(), message.end(), [](unsigned char c) { return std::isspace(c); }))
                return error(boost::beast::http::status::bad_request, "invalid_message", "message must contain 1 to 8000 non-whitespace bytes without NUL characters");

            bool validLines = true;
            const auto startLine = lineValue(body, "start_line", validLines);
            const auto endLine = lineValue(body, "end_line", validLines);
            if (!validLines || (startLine && endLine && *endLine < *startLine))
                return error(boost::beast::http::status::bad_request, "invalid_line_range", "line numbers must be positive and end_line must not precede start_line");

            std::optional<std::string> runId;
            if (body.contains("run_id") && !body.at("run_id").is_null()) {
                if (!body.at("run_id").is_string()) return error(boost::beast::http::status::bad_request, "invalid_run_id", "run_id must be a string");
                runId = body.at("run_id").get<std::string>();
                const auto run = context.store->run(*runId);
                if (!run || run->taskId != *taskId) return error(boost::beast::http::status::not_found, "run_not_found", "run not found for task");
            }
            std::optional<std::string> sourceEventId;
            if (body.contains("source_event_id") && !body.at("source_event_id").is_null()) {
                if (!body.at("source_event_id").is_string())
                    return error(boost::beast::http::status::bad_request, "invalid_source_event_id", "source_event_id must be a string");
                sourceEventId = body.at("source_event_id").get<std::string>();
                if (!validSourceEventId(*sourceEventId))
                    return error(boost::beast::http::status::bad_request, "invalid_source_event_id", "source_event_id must contain 1 to 200 bytes without NUL characters");
            }

            if (sourceEventId) {
                std::optional<nlohmann::json> inner;
                std::optional<std::string> sourceRunId;
                for (const auto &event : context.store->events(*taskId)) {
                    if (event.type != "import.message" || event.id != *sourceEventId) continue;
                    sourceRunId = event.runId;
                    nlohmann::json content;
                    try { content = nlohmann::json::parse(event.content); }
                    catch (const nlohmann::json::exception &) { break; }
                    if (!content.is_object()) break;
                    const auto &envelope = content;
                    if (!envelope.is_object() || !envelope.value("historical", false) ||
                        !envelope.contains("text") || !envelope.at("text").is_string()) break;
                    nlohmann::json parsed;
                    try { parsed = nlohmann::json::parse(envelope.at("text").get<std::string>()); }
                    catch (const nlohmann::json::exception &) { break; }
                    if (!parsed.is_object() || parsed.value("kind", std::string()) != "finding" ||
                        !parsed.contains("source_origin") || !parsed.contains("source_task_id") || !parsed.contains("id")) break;
                    inner = parsed;
                    break;
                }
                if (!inner)
                    return error(boost::beast::http::status::not_found, "source_event_not_found", "matching historical import source event not found");

                for (const auto &event : context.store->events(*taskId)) {
                    if (event.type != "finding.promoted") continue;
                    nlohmann::json content;
                    try { content = nlohmann::json::parse(event.content); }
                    catch (const nlohmann::json::exception &) { continue; }
                    if (!content.is_object()) continue;
                    if (!content.contains("source_event_id") || !content.at("source_event_id").is_string() ||
                        content.at("source_event_id").get<std::string>() != *sourceEventId) continue;
                    if (!content.contains("finding_id") || !content.at("finding_id").is_string()) continue;
                    const auto promotedId = content.at("finding_id").get<std::string>();
                    for (auto existing : context.store->findings(*taskId, 10000))
                        if (existing.id == promotedId)
                            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(existing));
                }

                const std::string suffix = "\n\nImported source event: " + *sourceEventId + " (historical location; unverified).";
                const auto promotedMessage = message + suffix;
                if (promotedMessage.size() > 8000)
                    return error(boost::beast::http::status::bad_request, "invalid_message", "message must not exceed 8000 bytes after promotion suffix");
                const auto finding = context.store->createFinding(*taskId, std::move(runId), filePath, startLine, endLine, promotedMessage, std::nullopt);
                const nlohmann::json promotedContent{{"source_event_id", *sourceEventId}, {"finding_id", finding.id}, {"original_source", *inner}};
                AgentEvent promotionEvent;
                promotionEvent.id = agents::newEventId();
                promotionEvent.taskId = *taskId;
                promotionEvent.runId = sourceRunId.value_or(std::string{});
                promotionEvent.type = "finding.promoted";
                promotionEvent.agent = "import";
                promotionEvent.content = promotedContent.dump();
                promotionEvent.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                context.store->appendEvent(promotionEvent);
                if (context.events) context.events->publish(promotionEvent);
                return jsonResponse(boost::beast::http::status::created, protocol::toJson(finding));
            }

            const auto finding = context.store->createFinding(*taskId, std::move(runId), filePath, startLine, endLine, message,
                context.git ? context.git->verificationFingerprint() : std::nullopt);
            return jsonResponse(boost::beast::http::status::created, protocol::toJson(finding));
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid finding JSON");
        }
    }

    if (id && request.method() == boost::beast::http::verb::patch) {
        try {
            const auto body = nlohmann::json::parse(request.body());
            if (!body.is_object() || !body.contains("status") || !body.at("status").is_string())
                return error(boost::beast::http::status::bad_request, "invalid_status", "status must be open, resolved, or not_applicable");
            const auto status = body.at("status").get<std::string>();
            if (status != "open" && status != "resolved" && status != "not_applicable")
                return error(boost::beast::http::status::bad_request, "invalid_status", "status must be open, resolved, or not_applicable");
            std::optional<std::string> reason;
            if (body.contains("reason") && !body.at("reason").is_null()) {
                if (!body.at("reason").is_string())
                    return error(boost::beast::http::status::bad_request, "invalid_reason", "reason must be a string");
                reason = body.at("reason").get<std::string>();
                if (reason->empty() || reason->size() > 2000 || reason->find('\0') != std::string::npos ||
                    std::all_of(reason->begin(), reason->end(), [](unsigned char c) { return std::isspace(c); }))
                    return error(boost::beast::http::status::bad_request, "invalid_reason", "reason must contain 1 to 2000 non-whitespace bytes without NUL characters");
            }
            if (status == "not_applicable" && !reason)
                return error(boost::beast::http::status::bad_request, "invalid_reason", "not_applicable requires a reason explaining why the finding no longer applies");
            if (status != "not_applicable" && reason)
                return error(boost::beast::http::status::bad_request, "invalid_reason", "reason is only valid when marking a finding not applicable");
            if (!context.store->updateFindingStatus(*id, status, reason))
                return error(boost::beast::http::status::not_found, "finding_not_found", "review finding not found");
            return jsonResponse(boost::beast::http::status::ok, {{"id", *id}, {"status", status}});
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }
    return std::nullopt;
}

}
