#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"
#include "daemon/repository/files.h"
#include "daemon/repository/index.h"
#include <charconv>

namespace aegis::daemon::api::routes {

std::optional<Response> files(const Request &request, const Context &context) {
    if (request.method() != boost::beast::http::verb::get) return std::nullopt;
    const auto target = std::string(request.target());
    const auto query = target.find('?');
    const auto path = target.substr(0, query);
    if (path != "/api/files" && path != "/api/files/content" && path != "/api/files/compare" && path != "/api/files/quick-open") return std::nullopt;
    if (!context.files) return error(boost::beast::http::status::service_unavailable, "files_unavailable", "repository file service is unavailable");

    bool validQuery = true;
    const auto offsetText = queryValue(target, "offset", validQuery);
    const auto searchQuery = queryValue(target, "q", validQuery);
    const auto requestedPath = queryValue(target, "path", validQuery);
    const auto scopeText = queryValue(target, "scope", validQuery);
    const auto recursiveText = queryValue(target, "recursive", validQuery);
    const auto includeChangesText = queryValue(target, "include_changes", validQuery);
    const auto sourceText = queryValue(target, "source", validQuery);
    const auto baseText = queryValue(target, "base", validQuery);
    const auto targetText = queryValue(target, "target", validQuery);
    const auto commitText = queryValue(target, "commit", validQuery);
    const auto largeText = queryValue(target, "load_large", validQuery);
    if (!validQuery) return error(boost::beast::http::status::bad_request, "invalid_query", "query parameters must be valid and unique");
    if (recursiveText && *recursiveText != "0" && *recursiveText != "1" && *recursiveText != "false" && *recursiveText != "true")
        return error(boost::beast::http::status::bad_request, "invalid_recursive", "recursive must be a boolean");
    if (largeText && *largeText != "0" && *largeText != "1" && *largeText != "false" && *largeText != "true")
        return error(boost::beast::http::status::bad_request, "invalid_load_large", "load_large must be a boolean");
    std::size_t offset = 0;
    if (offsetText) {
        const auto [end, parsed] = std::from_chars(offsetText->data(), offsetText->data() + offsetText->size(), offset);
        if (parsed != std::errc{} || end != offsetText->data() + offsetText->size() || offset > 500000)
            return error(boost::beast::http::status::bad_request, "invalid_offset", "offset must be between 0 and 500000");
    }
    const bool loadLarge = largeText && (*largeText == "1" || *largeText == "true");
    try {
        if (path == "/api/files/quick-open") {
            const auto found = context.files->index().search(searchQuery.value_or(""));
            nlohmann::json entries = nlohmann::json::array();
            for (const auto &entry : found.entries) entries.push_back(protocol::toJson(entry));
            return jsonResponse(boost::beast::http::status::ok, {{"entries", entries}, {"version", found.version},
                {"indexing", found.indexing}, {"elapsed_us", found.micros}, {"error", context.files->index().snapshot()->error}});
        }
        if (path == "/api/files") {
            const auto scope = scopeText.value_or("changed") == "all" ? repository::FileScope::all : repository::FileScope::changed;
            if (scopeText && *scopeText != "all" && *scopeText != "changed")
                return error(boost::beast::http::status::bad_request, "invalid_scope", "scope must be changed or all");
            if (includeChangesText && *includeChangesText != "0" && *includeChangesText != "1" &&
                *includeChangesText != "false" && *includeChangesText != "true")
                return error(boost::beast::http::status::bad_request, "invalid_include_changes", "include_changes must be a boolean");
            const auto recursive = recursiveText && (*recursiveText == "1" || *recursiveText == "true");
            const auto includeChanges = !includeChangesText || (*includeChangesText != "0" && *includeChangesText != "false");
            const auto listing = context.files->list(requestedPath.value_or(""), scope, 500, recursive, includeChanges, offset);
            nlohmann::json entries = nlohmann::json::array();
            for (const auto &entry : listing.entries) entries.push_back(protocol::toJson(entry));
            return jsonResponse(boost::beast::http::status::ok, {{"entries", entries}, {"truncated", listing.truncated}, {"next_offset", listing.nextOffset ? nlohmann::json(*listing.nextOffset) : nlohmann::json(nullptr)}, {"indexing", context.files->index().indexing()}, {"version", context.files->index().snapshot()->version}});
        }
        if (!requestedPath || requestedPath->empty())
            return error(boost::beast::http::status::bad_request, "missing_path", "path is required");
        if (path == "/api/files/content") {
            const auto source = repository::parseFileSource(sourceText.value_or("worktree"));
            if (!source) return error(boost::beast::http::status::bad_request, "invalid_source", "source must be head, index, or worktree");
            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(context.files->read(*requestedPath, *source, loadLarge)));
        }
        if (commitText) {
            if (baseText || targetText || !context.git)
                return error(boost::beast::http::status::bad_request, "invalid_commit_comparison", "commit comparison cannot be combined with source comparison");
            return jsonResponse(boost::beast::http::status::ok,
                                protocol::toJson(context.files->compareCommit(*commitText, *requestedPath, loadLarge)));
        }
        const auto base = repository::parseFileSource(baseText.value_or("head"));
        const auto targetSource = repository::parseFileSource(targetText.value_or("worktree"));
        if (!base || !targetSource)
            return error(boost::beast::http::status::bad_request, "invalid_source", "base and target must be head, index, or worktree");
        return jsonResponse(boost::beast::http::status::ok,
                            protocol::toJson(context.files->compare(*requestedPath, *base, *targetSource, loadLarge)));
    } catch (const std::invalid_argument &exception) {
        if (commitText && std::string_view(exception.what()) == "commit was not found")
            return error(boost::beast::http::status::not_found, "commit_not_found", exception.what());
        return error(boost::beast::http::status::bad_request, "invalid_path", exception.what());
    } catch (const std::exception &) {
        return error(boost::beast::http::status::internal_server_error, "file_read_failed", "could not read repository file");
    }
}

}
