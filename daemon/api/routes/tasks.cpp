#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"

#include <nlohmann/json.hpp>

namespace aegis::daemon::api::routes {
namespace {

bool isDirectory(const std::string &path) {
    return std::filesystem::is_directory(std::filesystem::path(path));
}

std::optional<std::string> queryValue(const std::string &target, const std::string &name) {
    const auto query = target.find('?');
    if (query == std::string::npos) return std::nullopt;
    auto start = query + 1;
    while (start <= target.size()) {
        const auto end = target.find('&', start);
        const auto part = target.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto equal = part.find('=');
        if (equal != std::string::npos && part.substr(0, equal) == name) return part.substr(equal + 1);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

}

std::optional<Response> tasks(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    if (request.method() == boost::beast::http::verb::get && target == "/api/tasks") {
        nlohmann::json result = nlohmann::json::array();
        for (const auto &task : context.store->tasks()) result.push_back(protocol::toJson(task));
        return jsonResponse(boost::beast::http::status::ok, result);
    }
    if (request.method() == boost::beast::http::verb::get && target.starts_with("/api/events")) {
        const auto taskId = queryValue(target, "task_id");
        if (!taskId || taskId->empty()) return error(boost::beast::http::status::bad_request, "missing_task_id", "task_id is required");
        if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        nlohmann::json result = nlohmann::json::array();
        for (const auto &event : context.store->events(*taskId, 500)) result.push_back(protocol::toJson(event));
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
    return std::nullopt;
}

}
