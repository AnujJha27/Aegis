#include "daemon/api/routes.h"
#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"

namespace aegis::daemon::api {

Response handle(const Request &request, const Context &context) {
    if (request.method() == boost::beast::http::verb::get && request.target() == "/api/health")
        return routes::jsonResponse(boost::beast::http::status::ok, {{"status", "healthy"}});
    for (const auto handler : {routes::agents, routes::tasks, routes::runs, routes::git, routes::verification, routes::review, routes::files})
        if (auto response = handler(request, context)) return std::move(*response);
    return routes::error(boost::beast::http::status::not_found, "not_found", "route not found");
}

}
