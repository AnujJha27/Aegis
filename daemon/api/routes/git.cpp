#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/agents/manager.h"

#include <nlohmann/json.hpp>

namespace aegis::daemon::api::routes {

std::optional<Response> git(const Request &request, const Context &context) {
    const auto target = std::string(request.target());
    const auto method = request.method();
    if (method == boost::beast::http::verb::get && target == "/api/repository") {
        if (context.git) return jsonResponse(boost::beast::http::status::ok, protocol::toJson(context.git->state()));
        return jsonResponse(boost::beast::http::status::ok, {{"path", context.repository.string()}, {"exists", std::filesystem::is_directory(context.repository)}});
    }
    if (method == boost::beast::http::verb::get && target == "/api/changes") {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        return jsonResponse(boost::beast::http::status::ok, {{"diff", context.git->diff()}});
    }
    if (method == boost::beast::http::verb::get && target == "/api/git/status") {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        return jsonResponse(boost::beast::http::status::ok, gitSnapshot(context));
    }
    if (method == boost::beast::http::verb::post && (target == "/api/git/branch" || target == "/api/git/pull" || target == "/api/git/push")) {
        if (!context.git) return error(boost::beast::http::status::internal_server_error, "git_unavailable", "repository service is unavailable");
        if (target != "/api/git/push" && context.agentManager && context.agentManager->hasRunningRuns())
            return error(boost::beast::http::status::conflict, "agent_running", "stop the active agent before switching branches or pulling");
        try {
            const auto body = request.body().empty() ? nlohmann::json::object() : nlohmann::json::parse(request.body());
            std::string output;
            bool succeeded = false;
            if (target == "/api/git/branch") succeeded = context.git->switchBranch(body.value("branch", std::string{}), output);
            else if (target == "/api/git/pull") succeeded = context.git->pull(output);
            else succeeded = context.git->push(output);
            if (!succeeded) return jsonResponse(target == "/api/git/branch" ? boost::beast::http::status::bad_request : boost::beast::http::status::bad_gateway,
                {{"error", {{"code", "git_operation_failed"}, {"message", output.empty() ? "Git operation failed" : output}}}});
            auto result = gitSnapshot(context);
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
            auto result = gitSnapshot(context);
            result["output"] = output;
            return jsonResponse(boost::beast::http::status::ok, result);
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }
    return std::nullopt;
}

}
