#include "daemon/api/routes.h"

#include "daemon/protocol/json.h"

#include <nlohmann/json.hpp>

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
        return jsonResponse(boost::beast::http::status::ok,
                            {{"path", context.repository.string()}, {"exists", isDirectory(context.repository.string())}});
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

    return error(boost::beast::http::status::not_found, "not_found", "route not found");
}

}
