#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"

namespace aegis::daemon::api::routes {

std::optional<Response> review(const Request &request, const Context &context) {
    if (request.method() != boost::beast::http::verb::get) return std::nullopt;
    const auto target = std::string(request.target());
    if (const auto taskId = pathId(target, "/handoff")) {
        const auto task = context.store->task(*taskId);
        if (!task) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        auto events = context.store->events(*taskId);
        constexpr std::size_t maxEvents = 20;
        if (events.size() > maxEvents) events.erase(events.begin(), events.end() - maxEvents);
        for (auto &event : events) if (event.content.size() > 4000) event.content.resize(4000);
        auto diff = context.git ? context.git->diff() : std::string{};
        constexpr std::size_t maxDiff = 24000;
        diff.resize(std::min(diff.size(), maxDiff));
        auto verificationHistory = context.store->verifications(*taskId, 1);
        std::optional<VerificationRun> verification;
        if (!verificationHistory.empty()) {
            verification = std::move(verificationHistory.front());
            if (verification->output.size() > 8000) verification->output.resize(8000);
        }
        auto prompt = task->prompt;
        if (prompt.size() > 8000) prompt.resize(8000);
        HandoffContext handoff{task->id, std::move(prompt), std::move(events), diff, changedFiles(diff), std::move(verification)};
        return jsonResponse(boost::beast::http::status::ok, protocol::toJson(handoff));
    }
    if (const auto taskId = pathId(target, "/graph")) {
        if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        TaskGraph graph{*taskId, {{"task:" + *taskId, "task", "Task"}}, {}};
        const auto events = context.store->events(*taskId);
        const auto runs = context.store->runs(*taskId);
        for (const auto &run : runs) {
            const auto runNode = "run:" + run.id;
            graph.nodes.push_back({runNode, "run", run.agent});
            graph.edges.push_back({"task:" + *taskId, runNode});
            for (const auto &event : events) {
                if (event.runId != run.id) continue;
                const auto eventNode = "event:" + event.id;
                graph.nodes.push_back({eventNode, "event", event.type});
                graph.edges.push_back({runNode, eventNode});
            }
        }
        const auto files = changedFiles(context.git ? context.git->diff() : std::string{});
        for (const auto &file : files) {
            const auto fileNode = "file:" + file;
            graph.nodes.push_back({fileNode, "file", file});
            graph.edges.push_back({runs.empty() ? "task:" + *taskId : "run:" + runs.back().id, fileNode});
        }
        return jsonResponse(boost::beast::http::status::ok, protocol::toJson(graph));
    }
    if (const auto taskId = pathId(target, "/provenance")) {
        if (!context.store->task(*taskId)) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        nlohmann::json records = nlohmann::json::array();
        for (const auto &event : context.store->events(*taskId)) {
            ProvenanceRecord record{event.id, event.taskId, event.runId, event.agent, event.type, event.timestamp};
            if (event.type == "file.changed" && !event.content.empty()) record.changedFiles.push_back(event.content);
            records.push_back(protocol::toJson(record));
        }
        return jsonResponse(boost::beast::http::status::ok, {{"task_id", *taskId}, {"records", records}});
    }
    return std::nullopt;
}

}
