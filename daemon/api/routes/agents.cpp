#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/agents/manager.h"

namespace aegis::daemon::api::routes {

std::optional<Response> agents(const Request &request, const Context &context) {
    if (request.method() != boost::beast::http::verb::get || request.target() != "/api/agents") return std::nullopt;
    if (!context.agentManager) return error(boost::beast::http::status::internal_server_error, "agents_unavailable", "agent service is unavailable");
    nlohmann::json result = nlohmann::json::array();
    for (const auto &agent : context.agentManager->available())
        result.push_back({{"name", agent.name}, {"available", agent.available}, {"structured", agent.capabilities.structured},
                          {"interactive", agent.capabilities.interactive}, {"resumable", agent.capabilities.resumable},
                          {"interruptible", agent.capabilities.interruptible}});
    return jsonResponse(boost::beast::http::status::ok, result);
}

}
