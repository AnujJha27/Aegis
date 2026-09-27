#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/repository/files.h"

namespace aegis::daemon::api::routes {

std::optional<Response> files(const Request &request, const Context &context) {
    if (request.method() != boost::beast::http::verb::get) return std::nullopt;
    const auto target = std::string(request.target());
    const auto query = target.find('?');
    const auto path = target.substr(0, query);
    if (path != "/api/files" && path != "/api/files/content" && path != "/api/files/compare") return std::nullopt;
    if (!context.files) return error(boost::beast::http::status::service_unavailable, "files_unavailable", "repository file service is unavailable");

    bool validQuery = true;
    const auto requestedPath = queryValue(target, "path", validQuery);
    const auto scopeText = queryValue(target, "scope", validQuery);
    const auto sourceText = queryValue(target, "source", validQuery);
    const auto baseText = queryValue(target, "base", validQuery);
    const auto targetText = queryValue(target, "target", validQuery);
    const auto largeText = queryValue(target, "load_large", validQuery);
    if (!validQuery) return error(boost::beast::http::status::bad_request, "invalid_query", "query parameters must be valid and unique");
    const bool loadLarge = largeText && (*largeText == "1" || *largeText == "true");
    try {
        if (path == "/api/files") {
            const auto scope = scopeText.value_or("changed") == "all" ? repository::FileScope::all : repository::FileScope::changed;
            if (scopeText && *scopeText != "all" && *scopeText != "changed")
                return error(boost::beast::http::status::bad_request, "invalid_scope", "scope must be changed or all");
            const auto listing = context.files->list(requestedPath.value_or(""), scope);
            nlohmann::json entries = nlohmann::json::array();
            for (const auto &entry : listing.entries) entries.push_back(protocol::toJson(entry));
            return jsonResponse(boost::beast::http::status::ok, {{"entries", entries}, {"truncated", listing.truncated}});
        }
        if (!requestedPath || requestedPath->empty())
            return error(boost::beast::http::status::bad_request, "missing_path", "path is required");
        if (path == "/api/files/content") {
            const auto source = repository::parseFileSource(sourceText.value_or("worktree"));
            if (!source) return error(boost::beast::http::status::bad_request, "invalid_source", "source must be head, index, or worktree");
            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(context.files->read(*requestedPath, *source, loadLarge)));
        }
        const auto base = repository::parseFileSource(baseText.value_or("head"));
        const auto targetSource = repository::parseFileSource(targetText.value_or("worktree"));
        if (!base || !targetSource)
            return error(boost::beast::http::status::bad_request, "invalid_source", "base and target must be head, index, or worktree");
        return jsonResponse(boost::beast::http::status::ok,
                            protocol::toJson(context.files->compare(*requestedPath, *base, *targetSource, loadLarge)));
    } catch (const std::invalid_argument &exception) {
        return error(boost::beast::http::status::bad_request, "invalid_path", exception.what());
    } catch (const std::exception &) {
        return error(boost::beast::http::status::internal_server_error, "file_read_failed", "could not read repository file");
    }
}

}
