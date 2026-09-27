#include "daemon/api/routes.h"
#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"

#include <iostream>

#ifndef AEGIS_VERSION
#define AEGIS_VERSION "dev"
#endif
#ifndef AEGIS_GIT_COMMIT
#define AEGIS_GIT_COMMIT "unknown"
#endif

namespace aegis::daemon::api {

Response handle(const Request &request, const Context &context) {
    if (request.method() == boost::beast::http::verb::get && request.target() == "/api/health")
        return routes::jsonResponse(boost::beast::http::status::ok, {{"status", "healthy"}});
    if (request.method() == boost::beast::http::verb::get && request.target() == "/api/version")
        return routes::jsonResponse(boost::beast::http::status::ok,
                                    {{"version", AEGIS_VERSION}, {"git_commit", AEGIS_GIT_COMMIT}, {"schema_version", 1}});
    try {
        for (const auto handler : {routes::agents, routes::tasks, routes::runs, routes::git, routes::verification, routes::review, routes::files})
            if (auto response = handler(request, context)) return std::move(*response);
        return routes::error(boost::beast::http::status::not_found, "not_found", "route not found");
    } catch (const std::exception &error) {
        std::cerr << "aegis_daemon: unexpected API exception: " << error.what() << '\n';
        return routes::error(boost::beast::http::status::internal_server_error, "internal_error", "the local daemon could not complete the request");
    } catch (...) {
        std::cerr << "aegis_daemon: unexpected unknown API exception\n";
        return routes::error(boost::beast::http::status::internal_server_error, "internal_error", "the local daemon could not complete the request");
    }
}

}
