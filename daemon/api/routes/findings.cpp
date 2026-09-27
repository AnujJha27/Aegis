#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"

#include <algorithm>
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

    if (taskId && request.method() == boost::beast::http::verb::get) {
        if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        nlohmann::json result = nlohmann::json::array();
        for (const auto &finding : context.store->findings(*taskId)) result.push_back(protocol::toJson(finding));
        return jsonResponse(boost::beast::http::status::ok, result);
    }

    if (taskId && request.method() == boost::beast::http::verb::post) {
        if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        try {
            const auto body = nlohmann::json::parse(request.body());
            if (!body.is_object() || !body.contains("file_path") || !body.at("file_path").is_string() ||
                !body.contains("message") || !body.at("message").is_string())
                return error(boost::beast::http::status::bad_request, "invalid_finding", "file_path and message must be strings");
            const auto filePath = body.at("file_path").get<std::string>();
            const auto message = body.at("message").get<std::string>();
            if (!validFilePath(filePath)) return error(boost::beast::http::status::bad_request, "invalid_file_path", "file_path must be a repository-relative source path");
            if (message.empty() || message.size() > 8000 ||
                std::all_of(message.begin(), message.end(), [](unsigned char c) { return std::isspace(c); }))
                return error(boost::beast::http::status::bad_request, "invalid_message", "message must contain 1 to 8000 non-whitespace bytes");

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
            const auto finding = context.store->createFinding(*taskId, std::move(runId), filePath, startLine, endLine, message);
            return jsonResponse(boost::beast::http::status::created, protocol::toJson(finding));
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid finding JSON");
        }
    }

    if (id && request.method() == boost::beast::http::verb::patch) {
        try {
            const auto body = nlohmann::json::parse(request.body());
            if (!body.is_object() || !body.contains("status") || !body.at("status").is_string())
                return error(boost::beast::http::status::bad_request, "invalid_status", "status must be open or resolved");
            const auto status = body.at("status").get<std::string>();
            if (status != "open" && status != "resolved")
                return error(boost::beast::http::status::bad_request, "invalid_status", "status must be open or resolved");
            if (!context.store->updateFindingStatus(*id, status))
                return error(boost::beast::http::status::not_found, "finding_not_found", "review finding not found");
            return jsonResponse(boost::beast::http::status::ok, {{"id", *id}, {"status", status}});
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }
    return std::nullopt;
}

}
