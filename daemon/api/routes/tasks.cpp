#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <set>
#include <string_view>

namespace aegis::daemon::api::routes {


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
    if (request.method() == boost::beast::http::verb::patch) {
        const auto taskId = pathId(target, "/context");
        if (taskId) {
            const auto existing = context.store->task(*taskId);
            if (!existing) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
            if (!matchesRepository(existing->repository, context.repository))
                return error(boost::beast::http::status::bad_request, "repository_mismatch", "task repository does not match daemon repository");
            static const std::string_view keys[] = {"goal", "constraints", "decisions", "attempts", "unfinished", "next_action"};
            try {
                const auto body = nlohmann::json::parse(request.body());
                if (!body.is_object() || body.size() != 6)
                    return error(boost::beast::http::status::bad_request, "invalid_context", "context must be an object with exactly the six known fields");
                TaskContext update;
                std::string *values[] = {&update.goal, &update.constraints, &update.decisions,
                                         &update.attempts, &update.unfinished, &update.nextAction};
                for (std::size_t index = 0; index < 6; ++index) {
                    if (!body.contains(keys[index]))
                        return error(boost::beast::http::status::bad_request, "invalid_context", "context must be an object with exactly the six known fields");
                    const auto &value = body.at(keys[index]);
                    if (!value.is_string() || value.get_ref<const std::string &>().size() > 4000 ||
                        value.get_ref<const std::string &>().find('\0') != std::string::npos)
                        return error(boost::beast::http::status::bad_request, "invalid_context", "context fields must be strings of at most 4000 bytes without NUL");
                    *values[index] = value.get_ref<const std::string &>();
                }
                if (!context.store->updateTaskContext(*taskId, update))
                    return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
                const auto updated = context.store->task(*taskId);
                return jsonResponse(boost::beast::http::status::ok, protocol::toJson(*updated));
            } catch (const nlohmann::json::exception &) {
                return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
            }
        }
    }
    if (request.method() == boost::beast::http::verb::post && target == "/api/tasks") {
        try {
            const auto body = nlohmann::json::parse(request.body());
            const auto prompt = body.value("prompt", std::string{});
            const auto repository = body.value("repository", context.repository.string());
            if (prompt.empty()) return error(boost::beast::http::status::bad_request, "missing_prompt", "prompt is required");
            if (repository.find('\0') != std::string::npos)
                return error(boost::beast::http::status::bad_request, "invalid_repository", "repository must be a valid directory");
            std::error_code fsError;
            const std::filesystem::path repositoryPath(repository);
            if (!std::filesystem::exists(repositoryPath, fsError) || fsError ||
                !std::filesystem::is_directory(repositoryPath, fsError) || fsError)
                return error(boost::beast::http::status::bad_request, "invalid_repository", "repository must be a directory");
            if (!matchesRepository(repository, context.repository))
                return error(boost::beast::http::status::bad_request, "repository_mismatch", "repository does not match daemon repository");
            std::error_code canonicalError;
            const auto canonicalRepository = std::filesystem::weakly_canonical(context.repository, canonicalError);
            if (canonicalError)
                return error(boost::beast::http::status::bad_request, "invalid_repository", "repository must be a valid directory");
            return jsonResponse(boost::beast::http::status::created, protocol::toJson(context.store->createTask(prompt, canonicalRepository.string())));
        } catch (const nlohmann::json::exception &) {
            return error(boost::beast::http::status::bad_request, "invalid_json", "request body must be valid JSON");
        }
    }
    if (request.method() == boost::beast::http::verb::post && target == "/api/imports/codex") {
        const auto bad = [](const char *code, const char *message) {
            return error(boost::beast::http::status::bad_request, code, message);
        };
        try {
            const auto body = nlohmann::json::parse(request.body());
            if (!body.is_object()) return bad("invalid_body", "request body must be a JSON object");
            const auto readField = [](const nlohmann::json &object, const char *key, std::size_t maxLen) {
                if (!object.contains(key) || !object.at(key).is_string()) return std::optional<std::string>{};
                const auto &text = object.at(key).get_ref<const std::string &>();
                if (text.size() > maxLen || text.find('\0') != std::string::npos) return std::optional<std::string>{};
                return std::optional<std::string>{text};
            };
            const auto sessionId = readField(body, "session_id", 200);
            const auto sourceName = readField(body, "source_name", 1000);
            const auto parentThreadId = body.contains("parent_thread_id") ? readField(body, "parent_thread_id", 200) : std::optional<std::string>{std::string{}};
            const auto prompt = readField(body, "prompt", 8000);
            if (!sessionId || sessionId->empty() || !sourceName || !parentThreadId || !prompt || prompt->empty())
                return bad("invalid_fields", "session_id, source_name and prompt are required strings within size limits");
            if (!body.contains("context") || !body.at("context").is_object() || body.at("context").size() != 6)
                return bad("invalid_context", "context must be an object with exactly the six known fields");
            static const std::string_view contextKeys[] = {"goal", "constraints", "decisions", "attempts", "unfinished", "next_action"};
            TaskContext taskContext;
            std::string *contextValues[] = {&taskContext.goal, &taskContext.constraints, &taskContext.decisions,
                                            &taskContext.attempts, &taskContext.unfinished, &taskContext.nextAction};
            for (std::size_t index = 0; index < 6; ++index) {
                const auto value = readField(body.at("context"), contextKeys[index].data(), 4000);
                if (!value) return bad("invalid_context", "context fields must be strings of at most 4000 bytes without NUL");
                *contextValues[index] = *value;
            }
            if (!body.contains("messages") || !body.at("messages").is_array() ||
                body.at("messages").empty() || body.at("messages").size() > 1000)
                return bad("invalid_messages", "messages must be an array of 1 to 1000 entries");
            std::vector<ImportedMessage> messages;
            std::set<std::string> messageIds;
            std::size_t totalText = 0;
            for (const auto &entry : body.at("messages")) {
                if (!entry.is_object()) return bad("invalid_messages", "each message must be an object");
                const auto messageId = readField(entry, "id", 200);
                const auto role = readField(entry, "role", 16);
                const auto text = readField(entry, "text", 12000);
                const auto messageSourceName = readField(entry, "source_name", 1000);
                if (!messageId || messageId->empty() || !role || !text || !messageSourceName || messageSourceName->empty())
                    return bad("invalid_messages", "each message requires id, role, text, and non-empty source_name within size limits");
                if (!messageIds.insert(*messageId).second)
                    return bad("invalid_messages", "message ids must be unique within the payload");
                if (*role != "user" && *role != "assistant" && *role != "tool")
                    return bad("invalid_messages", "message role must be user, assistant, or tool");
                if (!entry.contains("source_line") || !entry.at("source_line").is_number_integer())
                    return bad("invalid_messages", "message source_line must be an integer");
                const auto sourceLine = entry.at("source_line").get<std::int64_t>();
                if (sourceLine <= 0) return bad("invalid_messages", "message source_line must be positive");
                totalText += text->size();
                messages.push_back({*messageId, *role, *text, sourceLine, *messageSourceName});
            }
            if (totalText > 500000) return bad("invalid_messages", "total message text exceeds 500000 bytes");
            std::error_code canonicalError;
            const auto canonicalRepository = std::filesystem::weakly_canonical(context.repository, canonicalError);
            if (canonicalError) return bad("invalid_repository", "repository must be a valid directory");
            const auto sourceKey = "codex|" +
                nlohmann::json{canonicalRepository.string(), *sessionId, parentThreadId.value_or(std::string{})}.dump();
            const auto task = context.store->importTask(sourceKey, *prompt, canonicalRepository.string(), taskContext, messages);
            return jsonResponse(boost::beast::http::status::ok, protocol::toJson(task));
        } catch (const nlohmann::json::exception &) {
            return bad("invalid_json", "request body must be valid JSON");
        } catch (const std::exception &) {
            return error(boost::beast::http::status::internal_server_error, "import_failed", "could not persist import");
        }
    }
    return std::nullopt;
}

}
