#pragma once

#include "daemon/api/routes.h"
#include "daemon/agents/manager.h"
#include "daemon/protocol/json.h"
#include "daemon/repository/git.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>

namespace aegis::daemon::api::routes {

inline Response jsonResponse(boost::beast::http::status status, const nlohmann::json &body) {
    Response response{status, 11};
    response.set(boost::beast::http::field::content_type, "application/json");
    response.body() = body.dump();
    response.prepare_payload();
    return response;
}

inline Response error(boost::beast::http::status status, const std::string &code, const std::string &message) {
    return jsonResponse(status, {{"error", {{"code", code}, {"message", message}}}});
}

inline std::optional<std::string> pathId(const std::string &target, const std::string &suffix) {
    constexpr std::string_view prefix = "/api/tasks/";
    const auto path = target.substr(0, target.find('?'));
    if (!path.starts_with(prefix) || !path.ends_with(suffix)) return std::nullopt;
    const auto id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    return id.empty() || id.find('/') != std::string::npos ? std::nullopt : std::optional{id};
}

inline std::optional<std::string> runPathId(const std::string &target, const std::string &suffix) {
    constexpr std::string_view prefix = "/api/runs/";
    const auto path = target.substr(0, target.find('?'));
    if (!path.starts_with(prefix) || !path.ends_with(suffix)) return std::nullopt;
    const auto id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    return id.empty() || id.find('/') != std::string::npos ? std::nullopt : std::optional{id};
}

inline std::vector<std::string> changedFiles(const std::string &diff) {
    std::vector<std::string> files;
    std::size_t start = 0;
    while (start < diff.size()) {
        const auto end = diff.find('\n', start);
        const auto line = diff.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.starts_with("+++ b/")) files.push_back(line.substr(6));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return files;
}

inline nlohmann::json gitSnapshot(const Context &context) {
    nlohmann::json files = nlohmann::json::array();
    for (const auto &file : context.git->changes()) files.push_back(protocol::toJson(file));
    return {{"repository", protocol::toJson(context.git->state())},
            {"files", files}, {"branches", context.git->branches()},
            {"current_branch", context.git->currentBranch()}, {"clean", context.git->clean()},
            {"agent_running", context.agentManager && context.agentManager->hasRunningRuns()}};
}

}
