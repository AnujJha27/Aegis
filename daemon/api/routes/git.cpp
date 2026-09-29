#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/agents/manager.h"

#include <nlohmann/json.hpp>

#include <charconv>

namespace aegis::daemon::api::routes {

std::optional<Response> git(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    const auto method = request.method();
    if (method == boost::beast::http::verb::get && target == "/api/repository") {
        if (context.git) return jsonResponse(boost::beast::http::status::ok, protocol::toJson(context.git->state()));
        return jsonResponse(boost::beast::http::status::ok, {{"path", context.repository.string()}, {"exists", std::filesystem::is_directory(context.repository)}});
    }
    if (method == boost::beast::http::verb::get && target.substr(0, target.find('?')) == "/api/git/commits") {
        if (!context.git) return error(boost::beast::http::status::service_unavailable, "git_unavailable", "repository service is unavailable");
        bool valid = true;
        const auto limitText = queryValue(target, "limit", valid);
        if (!valid) return error(boost::beast::http::status::bad_request, "invalid_query", "query parameters must be valid and unique");
        std::size_t limit = 50;
        if (limitText) {
            const auto [end, parseError] = std::from_chars(limitText->data(), limitText->data() + limitText->size(), limit);
            if (parseError != std::errc{} || end != limitText->data() + limitText->size() || limit == 0 || limit > 100)
                return error(boost::beast::http::status::bad_request, "invalid_limit", "limit must be between 1 and 100");
        }
        nlohmann::json commits = nlohmann::json::array();
        for (const auto &commit : context.git->commits(limit)) commits.push_back(protocol::toJson(commit));
        return jsonResponse(boost::beast::http::status::ok, commits);
    }
    constexpr std::string_view commitPrefix = "/api/git/commits/";
    if (method == boost::beast::http::verb::get && target.starts_with(commitPrefix)) {
        const auto end = target.find_first_of("/?", commitPrefix.size());
        if (end == std::string::npos || target[end] == '?') {
            if (!context.git) return error(boost::beast::http::status::service_unavailable, "git_unavailable", "repository service is unavailable");
            const auto id = target.substr(commitPrefix.size(), (end == std::string::npos ? target.size() : end) - commitPrefix.size());
            const auto commit = context.git->findCommit(id);
            if (!commit) return error(boost::beast::http::status::not_found, "commit_not_found", "commit was not found");
            nlohmann::json files = nlohmann::json::array();
            for (const auto &file : context.git->commitFiles(commit->id)) files.push_back(protocol::toJson(file));
            return jsonResponse(boost::beast::http::status::ok, {{"commit", protocol::toJson(*commit)}, {"files", files}});
        }
    }
    if (method == boost::beast::http::verb::get && target == "/api/changes") {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        return jsonResponse(boost::beast::http::status::ok, {{"diff", context.git->diff()}});
    }
    if (method == boost::beast::http::verb::get && target.substr(0, target.find('?')) == "/api/git/status") {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        bool valid = true;
        const auto refresh = queryValue(target, "refresh", valid);
        if (!valid || (refresh && *refresh != "0" && *refresh != "1"))
            return error(boost::beast::http::status::bad_request, "invalid_query", "refresh must be 0 or 1");
        return jsonResponse(boost::beast::http::status::ok, gitSnapshot(context, refresh == "1"));
    }
    if (method == boost::beast::http::verb::post && (target == "/api/git/branch" || target == "/api/git/merge" || target == "/api/git/pull" || target == "/api/git/push")) {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        if (target != "/api/git/push" && context.agentManager && context.agentManager->hasRunningRuns())
            return error(boost::beast::http::status::conflict, "agent_running", "stop the active agent before switching branches, merging, or pulling");
        try {
            const auto body = request.body().empty() ? nlohmann::json::object() : nlohmann::json::parse(request.body());
            std::string output;
            bool succeeded = false;
            if (target == "/api/git/branch") succeeded = context.git->switchBranch(body.value("branch", std::string{}), output);
            else if (target == "/api/git/merge") succeeded = context.git->merge(body.value("branch", std::string{}), output);
            else if (target == "/api/git/pull") succeeded = context.git->pull(output);
            else succeeded = context.git->push(output);
            if (!succeeded) return jsonResponse(target == "/api/git/merge" ? boost::beast::http::status::conflict
                    : target == "/api/git/branch" ? boost::beast::http::status::bad_request : boost::beast::http::status::bad_gateway,
                {{"error", {{"code", "git_operation_failed"}, {"message", output.empty() ? "Git operation failed" : output}}}});
            auto result = gitSnapshot(context, true);
            result["output"] = output;
            return jsonResponse(boost::beast::http::status::ok, result);
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }
    if (method == boost::beast::http::verb::post && (target == "/api/git/stage" || target == "/api/git/unstage" || target == "/api/git/commit")) {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        try {
            const auto body = nlohmann::json::parse(request.body());
            std::string output;
            bool succeeded;
            if (target == "/api/git/commit") succeeded = context.git->commit(body.value("message", std::string{}), output);
            else {
                const auto path = body.value("path", std::string{});
                succeeded = target == "/api/git/stage" ? context.git->stage(path, output) : context.git->unstage(path, output);
            }
            if (!succeeded) return error(boost::beast::http::status::bad_request, "git_operation_failed", output.empty() ? "Git operation failed" : output);
            auto result = gitSnapshot(context, true);
            result["output"] = output;
            return jsonResponse(boost::beast::http::status::ok, result);
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }
    return std::nullopt;
}

}
