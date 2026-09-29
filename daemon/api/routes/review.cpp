#include "daemon/api/route_handlers.h"
#include "daemon/api/route_helpers.h"

namespace aegis::daemon::api::routes {
namespace {

constexpr std::size_t handoffDiffLimit = 24000;
constexpr std::size_t handoffPatchLimit = 16000;

void appendUntrackedFiles(std::string &diff, const std::vector<GitChange> &changes,
                          const repository::Files *files) {
    if (!files) return;
    constexpr std::string_view truncatedMarker = "\n...[handoff content truncated]\n";
    for (const auto &change : changes) {
        if (change.indexStatus != "?" && change.worktreeStatus != "?") continue;
        if (diff.size() >= handoffDiffLimit) break;
        try {
            const auto file = files->read(change.path, repository::FileSource::worktree);
            if (!file.exists) continue;
            const auto header = "\n\n--- Untracked file " + nlohmann::json(change.path).dump() + " ---\n";
            const auto body = file.binary ? std::string("[binary content omitted]\n")
                : file.truncated ? std::string("[file exceeds 1 MiB; content omitted]\n") : file.content;
            const auto remaining = handoffDiffLimit - diff.size();
            if (header.size() >= remaining) break;
            diff += header;
            const auto bodyLimit = handoffDiffLimit - diff.size();
            if (body.size() > bodyLimit) {
                if (bodyLimit > truncatedMarker.size()) {
                    diff.append(body, 0, bodyLimit - truncatedMarker.size());
                    diff += truncatedMarker;
                } else {
                    diff.append(truncatedMarker, 0, bodyLimit);
                }
                break;
            }
            diff += body;
        } catch (const std::exception &) {
            // An unsafe or unreadable worktree entry does not invalidate the handoff.
        }
    }
}

}

std::optional<Response> review(const Request &request, const Context &context) {
    if (request.method() != boost::beast::http::verb::get) return std::nullopt;
    const auto target = std::string(request.target());
    if (const auto taskId = pathId(target, "/handoff")) {
        const auto task = context.store->task(*taskId);
        if (!task) return error(boost::beast::http::status::not_found, "task_not_found", "task not found");
        auto events = context.store->events(*taskId, 20);
        for (auto &event : events) if (event.content.size() > 4000) event.content.resize(4000);
        auto diff = context.git ? context.git->diff(handoffPatchLimit) : std::string{};
        auto verificationHistory = context.store->verifications(*taskId, 1);
        std::optional<VerificationRun> verification;
        if (!verificationHistory.empty()) {
            verification = std::move(verificationHistory.front());
            if (verification->output.size() > 8000) verification->output.resize(8000);
        }
        auto findings = context.store->findings(*taskId, 20);
        for (auto &finding : findings) if (finding.message.size() > 2000) finding.message.resize(2000);
        auto prompt = task->prompt;
        if (prompt.size() > 8000) prompt.resize(8000);
        auto changes = context.git ? context.git->changes(101) : std::vector<GitChange>{};
        const bool changedFilesTruncated = changes.size() > 100;
        if (changedFilesTruncated) changes.resize(100);
        appendUntrackedFiles(diff, changes, context.files);
        std::vector<std::string> changedFiles;
        changedFiles.reserve(changes.size());
        for (auto &change : changes) changedFiles.push_back(std::move(change.path));
        HandoffContext handoff{task->id, std::move(prompt), std::move(events), diff, std::move(changedFiles), changedFilesTruncated, std::move(verification), std::move(findings)};
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
            ProvenanceRecord record{event.id, event.taskId, event.runId, event.agent, event.type, event.timestamp, "unknown", {}};
            if (event.type == "file.changed" && !event.content.empty()) record.changedFiles.push_back(event.content);
            records.push_back(protocol::toJson(record));
        }
        return jsonResponse(boost::beast::http::status::ok, {{"task_id", *taskId}, {"records", records}});
    }
    return std::nullopt;
}

}
