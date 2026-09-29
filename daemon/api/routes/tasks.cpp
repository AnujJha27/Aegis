#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <string_view>

namespace aegis::daemon::api::routes {
namespace {

bool isDirectory(const std::string &path) {
    return std::filesystem::is_directory(std::filesystem::path(path));
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
        bool validQuery = true;
        const auto taskId = queryValue(target, "task_id", validQuery);
        const auto before = queryValue(target, "before_sequence", validQuery);
        const auto pageLimit = queryValue(target, "limit", validQuery);
        if (!validQuery) return error(boost::beast::http::status::bad_request, "invalid_query", "query parameters must be valid and unique");
        if (!taskId || taskId->empty()) return error(boost::beast::http::status::bad_request, "missing_task_id", "task_id is required");
        if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        if (before) {
            std::int64_t cursor = 0;
            const auto [cursorEnd, cursorError] = std::from_chars(before->data(), before->data() + before->size(), cursor);
            if (cursorError != std::errc{} || cursorEnd != before->data() + before->size() || cursor <= 0)
                return error(boost::beast::http::status::bad_request, "invalid_cursor", "before_sequence must be a positive event sequence");
            std::size_t limit = 100;
            if (pageLimit) {
                unsigned parsedLimit = 0;
                const auto [limitEnd, limitError] = std::from_chars(pageLimit->data(), pageLimit->data() + pageLimit->size(), parsedLimit);
                if (limitError != std::errc{} || limitEnd != pageLimit->data() + pageLimit->size() || parsedLimit == 0 || parsedLimit > 500)
                    return error(boost::beast::http::status::bad_request, "invalid_limit", "limit must be between 1 and 500");
                limit = parsedLimit;
            }
            const auto page = context.store->eventsBefore(*taskId, cursor, limit);
            nlohmann::json events = nlohmann::json::array();
            for (const auto &event : page.events) events.push_back(protocol::toJson(event));
            return jsonResponse(boost::beast::http::status::ok,
                                {{"events", events}, {"next_cursor", page.nextCursor}, {"has_more", page.hasMore}});
        }
        nlohmann::json result = nlohmann::json::array();
        for (const auto &event : context.store->events(*taskId, 500, false)) result.push_back(protocol::toJson(event));
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
