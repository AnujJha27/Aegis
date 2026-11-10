#include "daemon/api/routes.h"
#include "daemon/api/origin.h"
#include "daemon/api/server.h"
#include "daemon/api/static_files.h"
#include "daemon/agents/manager.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/repository/git.h"
#include "daemon/repository/files.h"
#include "daemon/session/store.h"
#include "repository_fixture.h"

#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace http = boost::beast::http;
http::response<http::string_body> request(const aegis::daemon::api::Context &context, http::verb method, const std::string &target, std::string body = {}) {
    http::request<http::string_body> message(method, target, 11);
    message.set(http::field::content_type, "application/json");
    message.body() = std::move(body);
    message.prepare_payload();
    return aegis::daemon::api::handle(message, context);
}

std::pair<bool, http::status> eventSocketHandshake(std::uint16_t port, const std::string &origin) {
    namespace asio = boost::asio;
    namespace websocket = boost::beast::websocket;
    asio::io_context io;
    websocket::stream<asio::ip::tcp::socket> socket(io);
    socket.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
    socket.set_option(websocket::stream_base::decorator([&](websocket::request_type &message) {
        if (!origin.empty()) message.set(http::field::origin, origin);
    }));
    http::response<http::string_body> response;
    boost::system::error_code error;
    socket.handshake(response, "127.0.0.1:" + std::to_string(port), "/ws/events", error);
    boost::system::error_code ignored;
    socket.next_layer().close(ignored);
    return {!error, response.result()};
}

http::status httpHealthRequest(std::uint16_t port, const std::string &host) {
    namespace asio = boost::asio;
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    http::request<http::empty_body> request(http::verb::get, "/api/health", 11);
    request.set(http::field::host, host);
    http::write(socket, request);
    http::response<http::string_body> response;
    boost::beast::flat_buffer buffer;
    http::read(socket, buffer, response);
    return response.result();
}

}

int main() {
    const auto database = std::filesystem::temp_directory_path() / "aegis-daemon-api-test.sqlite";
    std::filesystem::remove(database);
    aegis::daemon::Store store(database);
    aegis::daemon::EventHub events;
    aegis::daemon::repository::GitRepository git(std::filesystem::current_path());
    aegis::daemon::repository::Files files(git);
    aegis::daemon::agents::Manager manager(std::filesystem::current_path(), store, events);
    const aegis::daemon::api::Context context{&store, &events, &manager, &git, &files, std::filesystem::current_path(), {}};

    const auto health = request(context, http::verb::get, "/api/health");
    assert(health.result() == http::status::ok);
    assert(health.body().find("healthy") != std::string::npos);
    const auto version = request(context, http::verb::get, "/api/version");
    assert(version.result() == http::status::ok);
    assert(nlohmann::json::parse(version.body()).at("schema_version") == aegis::daemon::Store::currentSchemaVersion);

    // Regression: B1 profile API — initial revision, optimistic CAS updates, and validation.
    const auto profileInitial = request(context, http::verb::get, "/api/profile");
    assert(profileInitial.result() == http::status::ok);
    const auto profileInitialJson = nlohmann::json::parse(profileInitial.body());
    assert(profileInitialJson.at("revision") == 0);
    assert(profileInitialJson.at("instructions").is_array());
    const auto profileSave = request(context, http::verb::put, "/api/profile",
        nlohmann::json{{"version", 1},
                       {"instructions", nlohmann::json::array({nlohmann::json{{"text", "Small changes"}, {"source", "manual"}}})},
                       {"workflows", nlohmann::json::array()},
                       {"expected_revision", 0}}.dump());
    assert(profileSave.result() == http::status::ok);
    const auto profileSavedJson = nlohmann::json::parse(profileSave.body());
    assert(profileSavedJson.at("revision") == 1);
    const auto profileAfterSave = nlohmann::json::parse(request(context, http::verb::get, "/api/profile").body());
    assert(profileAfterSave.at("revision") == 1);
    assert(profileAfterSave.at("instructions").size() == 1);
    assert(profileAfterSave.at("instructions")[0].at("text") == "Small changes");
    const auto profileStale = request(context, http::verb::put, "/api/profile",
        nlohmann::json{{"version", 1},
                       {"instructions", nlohmann::json::array({nlohmann::json{{"text", "Small changes"}, {"source", "manual"}}})},
                       {"workflows", nlohmann::json::array()},
                       {"expected_revision", 0}}.dump());
    assert(profileStale.result() == http::status::conflict);
    assert(nlohmann::json::parse(profileStale.body()).at("error").at("code") == "profile_conflict");
    assert(nlohmann::json::parse(request(context, http::verb::get, "/api/profile").body()).at("instructions")[0].at("text") == "Small changes");
    const auto expectProfileRejected = [&](const nlohmann::json &body) {
        const auto response = request(context, http::verb::put, "/api/profile", body.dump());
        assert(response.result() == http::status::bad_request);
    };
    expectProfileRejected(nlohmann::json{{"version", 1},
        {"instructions", nlohmann::json::array({nlohmann::json{{"text", std::string(1001, 'x')}, {"source", "manual"}}})},
        {"workflows", nlohmann::json::array()}, {"expected_revision", 1}});
    expectProfileRejected(nlohmann::json{{"version", 1},
        {"instructions", nlohmann::json::array({{"text", "ok"}, {"source", "manual"}})}, {"expected_revision", 1}});
    assert(request(context, http::verb::put, "/api/profile", "not-json").result() == http::status::bad_request);
    expectProfileRejected(nlohmann::json{{"version", 1},
        {"instructions", nlohmann::json::array({{"text", "ok"}, {"source", "manual"}})},
        {"workflows", nlohmann::json::array()}, {"expected_revision", 1.5}});

    // Regression: durable task context optimistic revision (context_revision).
    const auto revCreated = request(context, http::verb::post, "/api/tasks", R"({"prompt":"context revision"})");
    assert(revCreated.result() == http::status::created);
    const auto revTaskId = nlohmann::json::parse(revCreated.body()).at("id").get<std::string>();
    const auto listTasks = [&context]() {
        const auto response = request(context, http::verb::get, "/api/tasks");
        assert(response.result() == http::status::ok);
        return nlohmann::json::parse(response.body());
    };
    const auto findTaskRevision = [&](const std::string &id) {
        for (const auto &entry : listTasks())
            if (entry.at("id") == id) return entry.at("context_revision").get<std::int64_t>();
        assert(false);
        return std::int64_t{0};
    };
    assert(findTaskRevision(revTaskId) == 0);
    const nlohmann::json sixFields{{"goal", "rev-goal"}, {"constraints", "c"}, {"decisions", "d"},
        {"attempts", "a"}, {"unfinished", "u"}, {"next_action", "n"}};
    const auto firstSave = request(context, http::verb::patch, "/api/tasks/" + revTaskId + "/context",
        sixFields.dump());
    assert(firstSave.result() == http::status::ok);
    assert(nlohmann::json::parse(firstSave.body()).at("context_revision") == 1);
    nlohmann::json optimistic = sixFields;
    optimistic["goal"] = "optimistic-goal";
    optimistic["expected_revision"] = 1;
    const auto optimisticSave = request(context, http::verb::patch, "/api/tasks/" + revTaskId + "/context",
        optimistic.dump());
    assert(optimisticSave.result() == http::status::ok);
    assert(nlohmann::json::parse(optimisticSave.body()).at("context_revision") == 2);
    const auto staleSave = request(context, http::verb::patch, "/api/tasks/" + revTaskId + "/context",
        optimistic.dump());
    assert(staleSave.result() == http::status::conflict);
    bool goalPreserved = false;
    for (const auto &entry : listTasks())
        if (entry.at("id") == revTaskId)
            goalPreserved = entry.at("context").at("goal") == "optimistic-goal";
    assert(goalPreserved);
    const auto legacySave = request(context, http::verb::patch, "/api/tasks/" + revTaskId + "/context",
        sixFields.dump());
    assert(legacySave.result() == http::status::ok);
    assert(nlohmann::json::parse(legacySave.body()).at("context_revision") == 3);
    for (const auto &badRevision : nlohmann::json::array({-1, 1.5, "1", nullptr, 9007199254740993})) {
        nlohmann::json bad = sixFields;
        bad["expected_revision"] = badRevision;
        const auto badSave = request(context, http::verb::patch, "/api/tasks/" + revTaskId + "/context", bad.dump());
        assert(badSave.result() == http::status::bad_request);
    }
    // Regression: an unknown seventh key must be rejected; only expected_revision may be the seventh field.
    nlohmann::json unknownKey = sixFields;
    unknownKey["mystery"] = "x";
    assert(request(context, http::verb::patch, "/api/tasks/" + revTaskId + "/context", unknownKey.dump()).result() == http::status::bad_request);
    // Regression: nlohmann parses large positive integers as unsigned; expected_revision 0 must work as the first CAS,
    // and a valid maximal unsigned expected_revision must be accepted (as a conflict here), not rejected.
    const auto casCreated = request(context, http::verb::post, "/api/tasks", R"({"prompt":"cas zero"})");
    assert(casCreated.result() == http::status::created);
    const auto casTaskId = nlohmann::json::parse(casCreated.body()).at("id").get<std::string>();
    nlohmann::json casZero = sixFields;
    casZero["expected_revision"] = 0;
    const auto casZeroSave = request(context, http::verb::patch, "/api/tasks/" + casTaskId + "/context", casZero.dump());
    assert(casZeroSave.result() == http::status::ok);
    assert(nlohmann::json::parse(casZeroSave.body()).at("context_revision") == 1);
    assert(request(context, http::verb::patch, "/api/tasks/" + casTaskId + "/context", casZero.dump()).result() == http::status::conflict);
    nlohmann::json maxRevision = sixFields;
    maxRevision["expected_revision"] = 9007199254740991;
    assert(request(context, http::verb::patch, "/api/tasks/" + casTaskId + "/context", maxRevision.dump()).result() == http::status::conflict);

    // Regression: verification freshness lifecycle, ctest normalization, and
    // command validation. The freshness/migration implementation is forthcoming;
    // these assertions pin the expected serialized behavior.
    RepositoryFixture verifyFixture;
    verifyFixture.write("source.cpp", "int alpha = 1;\n");
    verifyFixture.commit("base");
    aegis::daemon::repository::GitRepository verifyGit(verifyFixture.root());
    aegis::daemon::repository::Files verifyFiles(verifyGit);
    aegis::daemon::agents::Manager verifyManager(verifyFixture.root(), store, events);
    const aegis::daemon::api::Context verifyContext{&store, &events, &verifyManager, &verifyGit, &verifyFiles, verifyFixture.root(), {}};
    const auto verifyTask = store.createTask("verification freshness", verifyFixture.root().string());
    const auto firstVerify = request(verifyContext, http::verb::post, "/api/verify",
        nlohmann::json{{"task_id", verifyTask.id}, {"command", {"/usr/bin/true"}}}.dump());
    assert(firstVerify.result() == http::status::ok);
    const auto firstVerifyJson = nlohmann::json::parse(firstVerify.body());
    assert(firstVerifyJson.at("freshness") == "current");
    assert(!firstVerifyJson.at("repository_fingerprint").get<std::string>().empty());
    const auto firstVerificationId = firstVerifyJson.at("id").get<std::string>();
    const auto fetchVerifications = [&verifyContext, &verifyTask]() {
        const auto response = request(verifyContext, http::verb::get,
            "/api/tasks/" + verifyTask.id + "/verifications");
        assert(response.result() == http::status::ok);
        return nlohmann::json::parse(response.body());
    };
    const auto currentHistory = fetchVerifications();
    assert(currentHistory.size() == 1);
    assert(currentHistory[0].at("id") == firstVerificationId);
    assert(currentHistory[0].at("freshness") == "current");

    verifyFixture.write("source.cpp", "int alpha = 2;\n");
    verifyFixture.commit("change tracked source");
    const auto staleHistory = fetchVerifications();
    bool staleFound = false;
    for (const auto &entry : staleHistory)
        if (entry.at("id") == firstVerificationId) {
            staleFound = true;
            assert(entry.at("freshness") == "stale");
        }
    assert(staleFound);
    const auto staleHandoff = request(verifyContext, http::verb::get,
        "/api/tasks/" + verifyTask.id + "/handoff");
    assert(staleHandoff.result() == http::status::ok);
    const auto staleHandoffJson = nlohmann::json::parse(staleHandoff.body());
    assert(staleHandoffJson.at("verification").at("id") == firstVerificationId);
    assert(staleHandoffJson.at("verification").at("freshness") == "stale");

    // Restoring the original bytes cannot restore the baseline once the commit
    // changed HEAD: the fingerprint includes HEAD, so the verification stays stale.
    verifyFixture.write("source.cpp", "int alpha = 1;\n");
    const auto restoredHistory = fetchVerifications();
    bool restoredFound = false;
    for (const auto &entry : restoredHistory)
        if (entry.at("id") == firstVerificationId) {
            restoredFound = true;
            assert(entry.at("freshness") == "stale");
        }
    assert(restoredFound);

    verifyFixture.write("scratch.txt", "scratch\n");
    const auto untrackedHistory = fetchVerifications();
    bool untrackedStaleFound = false;
    for (const auto &entry : untrackedHistory)
        if (entry.at("id") == firstVerificationId) {
            untrackedStaleFound = true;
            assert(entry.at("freshness") == "stale");
        }
    assert(untrackedStaleFound);

    const auto ctestTask = store.createTask("ctest validation", verifyFixture.root().string());
    const auto ctestVerificationsBefore = store.verifications(ctestTask.id).size();
    const auto ctestEventsBefore = store.events(ctestTask.id).size();
    verifyFixture.write("build/.keep", "");
    const auto ctestRun = request(verifyContext, http::verb::post, "/api/verify",
        nlohmann::json{{"task_id", ctestTask.id}, {"command", {"ctest", "--test-dir", "build"}}}.dump());
    assert(ctestRun.result() == http::status::ok);
    const auto ctestJson = nlohmann::json::parse(ctestRun.body());
    assert(ctestJson.at("exit_code").get<int>() != 0);
    const auto ctestCommand = ctestJson.at("command").get<std::vector<std::string>>();
    assert(std::find(ctestCommand.begin(), ctestCommand.end(), "--no-tests=error") != ctestCommand.end());
    assert(store.verifications(ctestTask.id).size() == ctestVerificationsBefore + 1);
    assert(store.events(ctestTask.id).size() == ctestEventsBefore + 2);

    const auto invalidTask = store.createTask("invalid verification", verifyFixture.root().string());
    const auto expectInvalidCommand = [&verifyContext, &invalidTask](const nlohmann::json &command) {
        const auto response = request(verifyContext, http::verb::post, "/api/verify",
            nlohmann::json{{"task_id", invalidTask.id}, {"command", command}}.dump());
        assert(response.result() == http::status::bad_request);
        assert(nlohmann::json::parse(response.body()).at("error").at("code") == "invalid_command");
    };
    expectInvalidCommand(nlohmann::json::array());
    expectInvalidCommand(std::vector<std::string>{""});
    expectInvalidCommand(std::vector<std::string>{"   "});
    const std::string nulArgument = std::string("bad\0arg", 7);
    expectInvalidCommand(std::vector<std::string>{"/usr/bin/true", nulArgument});
    assert(store.verifications(invalidTask.id).empty());
    assert(store.events(invalidTask.id).empty());

    const auto legacyTask = store.createTask("legacy verification", verifyFixture.root().string());
    aegis::daemon::VerificationRun legacyVerification;
    legacyVerification.id = "legacy-verification";
    legacyVerification.taskId = legacyTask.id;
    legacyVerification.command = {"/usr/bin/true"};
    legacyVerification.exitCode = 0;
    legacyVerification.output = "exit_code=0";
    store.saveVerification(legacyVerification);
    const auto legacyHistory = request(verifyContext, http::verb::get,
        "/api/tasks/" + legacyTask.id + "/verifications");
    assert(legacyHistory.result() == http::status::ok);
    const auto legacyEntries = nlohmann::json::parse(legacyHistory.body());
    bool legacyFound = false;
    for (const auto &entry : legacyEntries)
        if (entry.at("id") == legacyVerification.id) {
            legacyFound = true;
            assert(entry.at("freshness") == "unknown");
        }
    assert(legacyFound);

    // Regression: POST /api/tasks must reject repositories different from the context
    // repository and canonicalize equivalent paths for the same repository.
    RepositoryFixture mismatchFixture;
    mismatchFixture.write("marker.txt", "other repo\n");
    mismatchFixture.commit("base");
    const auto mismatchCreated = request(context, http::verb::post, "/api/tasks",
        nlohmann::json{{"prompt", "mismatched repo"}, {"repository", mismatchFixture.root().string()}}.dump());
    assert(mismatchCreated.result() == http::status::bad_request);
    assert(nlohmann::json::parse(mismatchCreated.body()).at("error").at("code") == "repository_mismatch");

    const auto dotRepository = (std::filesystem::current_path() / ".").string();
    const auto dotCreated = request(context, http::verb::post, "/api/tasks",
        nlohmann::json{{"prompt", "dot canonical repo"}, {"repository", dotRepository}}.dump());
    assert(dotCreated.result() == http::status::created);
    const auto dotTaskId = nlohmann::json::parse(dotCreated.body()).at("id").get<std::string>();
    assert(store.task(dotTaskId)->repository == std::filesystem::current_path().string());

    const auto canonicalLink = std::filesystem::temp_directory_path() /
        ("aegis-canonical-link-" + std::to_string(getpid()));
    std::filesystem::remove(canonicalLink);
    std::filesystem::create_symlink(std::filesystem::current_path(), canonicalLink);
    const auto linkCreated = request(context, http::verb::post, "/api/tasks",
        nlohmann::json{{"prompt", "symlink canonical repo"}, {"repository", canonicalLink.string()}}.dump());
    assert(linkCreated.result() == http::status::created);
    const auto linkTaskId = nlohmann::json::parse(linkCreated.body()).at("id").get<std::string>();
    assert(store.task(linkTaskId)->repository == std::filesystem::current_path().string());
    std::filesystem::remove(canonicalLink);

    // Regression: manager.launch must refuse tasks whose repository differs from the
    // manager repository, without creating runs.
    const auto mismatchTask = store.createTask("mismatched launch", mismatchFixture.root().string());
    const auto mismatchRun = manager.launch(mismatchTask.id, "shell");
    if (mismatchRun) manager.terminate(mismatchRun->id);
    assert(!mismatchRun);
    assert(store.runs(mismatchTask.id).empty());

    // Regression: handoff recent_events must drop terminal.output noise while keeping
    // the user.message correction that precedes it.
    const auto correctionTask = store.createTask("handoff correction", std::filesystem::current_path().string());
    const auto correctionRun = store.startRun(correctionTask.id, "shell");
    store.appendEvent({"correction-user-message", correctionTask.id, correctionRun.id,
                       "user.message", "shell", "actually focus on the parser", 1});
    for (int index = 0; index < 25; ++index)
        store.appendEvent({"correction-terminal-" + std::to_string(index), correctionTask.id,
                           correctionRun.id, "terminal.output", "shell", "noise " + std::to_string(index), index + 2});
    const auto correctionHandoff = request(context, http::verb::get, "/api/tasks/" + correctionTask.id + "/handoff");
    assert(correctionHandoff.result() == http::status::ok);
    const auto correctionHandoffJson = nlohmann::json::parse(correctionHandoff.body());
    const auto correctionEvents = correctionHandoffJson.at("recent_events").get<std::vector<nlohmann::json>>();
    bool correctionPresent = false;
    for (const auto &event : correctionEvents) {
        assert(event.at("type").get<std::string>() != "terminal.output");
        if (event.at("id") == "correction-user-message") correctionPresent = true;
    }
    assert(correctionPresent);

    // Regression: handoff and verify must reject store-created tasks whose repository
    // differs from the context repository before doing any work.
    const auto mismatchHandoff = request(context, http::verb::get, "/api/tasks/" + mismatchTask.id + "/handoff");
    assert(mismatchHandoff.result() == http::status::bad_request);
    assert(nlohmann::json::parse(mismatchHandoff.body()).at("error").at("code") == "repository_mismatch");
    const auto mismatchVerification = request(context, http::verb::post, "/api/verify",
        nlohmann::json{{"task_id", mismatchTask.id}, {"command", {"/usr/bin/true"}}}.dump());
    assert(mismatchVerification.result() == http::status::bad_request);
    assert(nlohmann::json::parse(mismatchVerification.body()).at("error").at("code") == "repository_mismatch");
    assert(store.verifications(mismatchTask.id).empty());
    assert(store.events(mismatchTask.id).empty());

    const auto historyTask = store.createTask("history", std::filesystem::current_path().string());
    const auto historyRun = store.startRun(historyTask.id, "shell");
    store.appendEvent({"history-1", historyTask.id, historyRun.id, "user.message", "shell", "one", 1});
    store.appendEvent({"history-terminal", historyTask.id, historyRun.id, "terminal.output", "shell", "raw", 2});
    store.appendEvent({"history-2", historyTask.id, historyRun.id, "agent.message.completed", "shell", "two", 3});
    const auto initialHistory = request(context, http::verb::get, "/api/events?task_id=" + historyTask.id);
    const auto initialEvents = nlohmann::json::parse(initialHistory.body());
    assert(initialEvents.size() == 2 && initialEvents[0].at("sequence").get<std::int64_t>() > 0);
    const auto olderHistory = request(context, http::verb::get,
        "/api/events?task_id=" + historyTask.id + "&before_sequence=" + std::to_string(initialEvents[1].at("sequence").get<std::int64_t>()) + "&limit=1");
    const auto olderPage = nlohmann::json::parse(olderHistory.body());
    assert(olderPage.at("events").size() == 1 && olderPage.at("events")[0].at("id") == "history-1");
    assert(!olderPage.at("has_more").get<bool>());
    store.appendEvent({"history-turn", historyTask.id, historyRun.id, "turn.started", "shell", "", 4});
    const auto runSnapshot = request(context, http::verb::get, "/api/tasks/" + historyTask.id + "/runs");
    assert(nlohmann::json::parse(runSnapshot.body()).at(0).at("turn_status") == "turn.started");

    const auto file = request(context, http::verb::get, "/api/files/content?path=CMakeLists.txt&source=worktree");
    assert(file.result() == http::status::ok);
    assert(nlohmann::json::parse(file.body()).at("content").get<std::string>().find("project(aegis") != std::string::npos);
    const auto listing = request(context, http::verb::get, "/api/files?scope=all");
    assert(listing.result() == http::status::ok);
    assert(!nlohmann::json::parse(listing.body()).at("entries").empty());
    const auto refreshedGitStatus = request(context, http::verb::get, "/api/git/status?refresh=1");
    assert(refreshedGitStatus.result() == http::status::ok);
    assert(nlohmann::json::parse(refreshedGitStatus.body()).at("version").get<std::uint64_t>() > 0);
    const auto invalidFileOptions = request(context, http::verb::get, "/api/files?scope=all&include_changes=maybe");
    assert(invalidFileOptions.result() == http::status::bad_request);
    const auto invalidRecursive = request(context, http::verb::get, "/api/files?scope=all&recursive=maybe");
    assert(invalidRecursive.result() == http::status::bad_request);
    assert(nlohmann::json::parse(invalidRecursive.body()).at("error").at("code") == "invalid_recursive");
    const auto invalidLarge = request(context, http::verb::get, "/api/files/content?path=CMakeLists.txt&load_large=maybe");
    assert(invalidLarge.result() == http::status::bad_request);
    assert(nlohmann::json::parse(invalidLarge.body()).at("error").at("code") == "invalid_load_large");
    const auto escapedFile = request(context, http::verb::get, "/api/files/content?path=%2e%2e%2fsecret");
    assert(escapedFile.result() == http::status::bad_request);

    RepositoryFixture commitFixture;
    commitFixture.write("space name.cpp", "int value = 0;\n");
    commitFixture.commit("base");
    commitFixture.write("space name.cpp", "int value = 1;\n");
    commitFixture.write("new.cpp", "int fresh = 1;\n");
    commitFixture.commit("review commit");
    auto commitId = commitFixture.git({"rev-parse", "HEAD"});
    while (!commitId.empty() && (commitId.back() == '\n' || commitId.back() == '\r')) commitId.pop_back();
    aegis::daemon::repository::GitRepository commitGit(commitFixture.root());
    aegis::daemon::repository::Files commitFiles(commitGit);
    const aegis::daemon::api::Context commitContext{&store, &events, &manager, &commitGit, &commitFiles, commitFixture.root(), {}};
    const auto commitList = request(commitContext, http::verb::get, "/api/git/commits?limit=2");
    assert(commitList.result() == http::status::ok);
    assert(nlohmann::json::parse(commitList.body()).at(0).at("id") == commitId);
    const auto commitDetail = request(commitContext, http::verb::get, "/api/git/commits/" + commitId);
    assert(commitDetail.result() == http::status::ok);
    assert(nlohmann::json::parse(commitDetail.body()).at("files").size() == 2);
    const auto commitDiff = request(commitContext, http::verb::get,
        "/api/files/compare?path=space%20name.cpp&commit=" + commitId);
    assert(commitDiff.result() == http::status::ok);
    const auto commitComparison = nlohmann::json::parse(commitDiff.body());
    assert(commitComparison.at("original").at("content") == "int value = 0;\n");
    assert(commitComparison.at("modified").at("content") == "int value = 1;\n");
    assert(request(commitContext, http::verb::get, "/api/git/commits/not-a-commit").result() == http::status::not_found);

    RepositoryFixture mergeFixture;
    mergeFixture.write("base.txt", "base\n");
    mergeFixture.commit("base");
    auto currentBranch = mergeFixture.git({"branch", "--show-current"});
    if (!currentBranch.empty() && currentBranch.back() == '\n') currentBranch.pop_back();
    mergeFixture.git({"branch", "feature"});
    mergeFixture.write("main.txt", "main\n");
    mergeFixture.commit("main work");
    mergeFixture.git({"switch", "feature"});
    mergeFixture.write("feature.txt", "feature\n");
    mergeFixture.commit("feature work");
    mergeFixture.git({"switch", currentBranch});
    aegis::daemon::repository::GitRepository mergeGit(mergeFixture.root());
    aegis::daemon::repository::Files mergeFiles(mergeGit);
    aegis::daemon::agents::Manager mergeManager(mergeFixture.root(), store, events);
    const aegis::daemon::api::Context mergeContext{&store, &events, &mergeManager, &mergeGit, &mergeFiles, mergeFixture.root(), {}};
    const auto merged = request(mergeContext, http::verb::post, "/api/git/merge", R"({"branch":"feature"})");
    assert(merged.result() == http::status::ok);
    assert(mergeFiles.read("feature.txt", aegis::daemon::repository::FileSource::worktree).content == "feature\n");
    assert(mergeFiles.read("main.txt", aegis::daemon::repository::FileSource::worktree).content == "main\n");
    const auto guardTask = store.createTask("protect agent work during merge", mergeFixture.root().string());
    const auto activeRun = mergeManager.launch(guardTask.id, "shell");
    assert(activeRun);
    const auto activeMerge = request(mergeContext, http::verb::post, "/api/git/merge", R"({"branch":"feature"})");
    assert(activeMerge.result() == http::status::conflict);
    assert(nlohmann::json::parse(activeMerge.body()).at("error").at("code") == "agent_running");
    assert(mergeManager.terminate(activeRun->id));
    mergeFixture.write("dirty.txt", "keep me\n");
    const auto dirtyMerge = request(mergeContext, http::verb::post, "/api/git/merge", R"({"branch":"feature"})");
    assert(dirtyMerge.result() == http::status::conflict);
    assert(nlohmann::json::parse(dirtyMerge.body()).at("error").at("message").get<std::string>().find("before merging") != std::string::npos);
    assert(std::filesystem::exists(mergeFixture.root() / "dirty.txt"));
    std::filesystem::remove(mergeFixture.root() / "dirty.txt");
    mergeFixture.git({"branch", "conflict"});
    mergeFixture.write("base.txt", "target side\n");
    mergeFixture.commit("target edit");
    mergeFixture.git({"switch", "conflict"});
    mergeFixture.write("base.txt", "source side\n");
    mergeFixture.commit("source edit");
    mergeFixture.git({"switch", currentBranch});
    const auto conflict = request(mergeContext, http::verb::post, "/api/git/merge", R"({"branch":"conflict"})");
    assert(conflict.result() == http::status::conflict);
    assert(nlohmann::json::parse(conflict.body()).at("error").at("message").get<std::string>().find("CONFLICT") != std::string::npos);
    const auto conflictedFile = mergeFiles.read("base.txt", aegis::daemon::repository::FileSource::worktree);
    assert(conflictedFile.content.find("<<<<<<<") != std::string::npos);
    const auto conflicts = mergeGit.changes();
    assert(std::any_of(conflicts.begin(), conflicts.end(), [](const auto &change) {
        return change.path == "base.txt" && change.indexStatus == "U" && change.worktreeStatus == "U";
    }));

    RepositoryFixture handoffFixture;
    handoffFixture.write("modified.cpp", "before\n");
    handoffFixture.write("deleted.cpp", "removed\n");
    handoffFixture.commit("base");
    handoffFixture.write("modified.cpp", "after\n");
    std::filesystem::remove(handoffFixture.root() / "deleted.cpp");
    handoffFixture.write("untracked.cpp", "new\n");
    aegis::daemon::repository::GitRepository handoffGit(handoffFixture.root());
    aegis::daemon::repository::Files handoffFiles(handoffGit);
    const auto handoffTask = store.createTask("review changes", handoffFixture.root().string());
    const aegis::daemon::api::Context handoffContext{&store, &events, &manager, &handoffGit, &handoffFiles, handoffFixture.root(), {}};
    const auto handoffResponse = request(handoffContext, http::verb::get, "/api/tasks/" + handoffTask.id + "/handoff");
    assert(handoffResponse.result() == http::status::ok);
    auto handoffJson = nlohmann::json::parse(handoffResponse.body());
    auto handoffFilesJson = handoffJson.at("changed_files").get<std::vector<std::string>>();
    assert(!handoffJson.at("changed_files_truncated").get<bool>());
    assert(handoffJson.at("diff").get<std::string>().find("new\n") != std::string::npos);
    for (const auto *path : {"modified.cpp", "deleted.cpp", "untracked.cpp"})
        assert(std::find(handoffFilesJson.begin(), handoffFilesJson.end(), path) != handoffFilesJson.end());
    handoffFixture.write("zz-large.txt", std::string(30000, 'x'));
    handoffJson = nlohmann::json::parse(request(handoffContext, http::verb::get,
        "/api/tasks/" + handoffTask.id + "/handoff").body());
    assert(handoffJson.at("diff").get<std::string>().size() <= 24000);
    assert(handoffJson.at("diff").get<std::string>().find("handoff content truncated") != std::string::npos);
    for (int index = 0; index < 105; ++index)
        handoffFixture.write("extra-" + std::to_string(index) + ".txt", "bounded\n");
    handoffJson = nlohmann::json::parse(request(handoffContext, http::verb::get,
        "/api/tasks/" + handoffTask.id + "/handoff").body());
    handoffFilesJson = handoffJson.at("changed_files").get<std::vector<std::string>>();
    assert(handoffFilesJson.size() == 100);
    assert(handoffJson.at("changed_files_truncated").get<bool>());
    assert(handoffJson.at("diff").get<std::string>().size() <= 24000);

    // Regression: findings freshness lifecycle via the findings endpoint.
    const auto findingFreshnessTask = store.createTask("finding freshness", handoffFixture.root().string());
    const auto findingCreatedFresh = request(handoffContext, http::verb::post,
        "/api/tasks/" + findingFreshnessTask.id + "/findings",
        nlohmann::json{{"file_path", "modified.cpp"}, {"message", "check modified behavior"}}.dump());
    assert(findingCreatedFresh.result() == http::status::created);
    const auto findingFresh = nlohmann::json::parse(findingCreatedFresh.body());
    assert(!findingFresh.at("repository_fingerprint").get<std::string>().empty());
    const auto findingFreshList = request(handoffContext, http::verb::get,
        "/api/tasks/" + findingFreshnessTask.id + "/findings");
    assert(findingFreshList.result() == http::status::ok);
    const auto findingFreshEntries = nlohmann::json::parse(findingFreshList.body());
    assert(findingFreshEntries.size() == 1);
    assert(findingFreshEntries[0].at("freshness") == "current");
    const auto findingFreshHandoff = request(handoffContext, http::verb::get,
        "/api/tasks/" + findingFreshnessTask.id + "/handoff");
    assert(nlohmann::json::parse(findingFreshHandoff.body()).at("findings")[0].at("freshness") == "current");

    handoffFixture.write("modified.cpp", "rewritten after review\n");
    const auto findingStaleList = request(handoffContext, http::verb::get,
        "/api/tasks/" + findingFreshnessTask.id + "/findings");
    assert(nlohmann::json::parse(findingStaleList.body())[0].at("freshness") == "stale");
    assert(nlohmann::json::parse(findingStaleList.body())[0].at("status") == "open");

    const auto legacyFindingTask = store.createTask("legacy findings", handoffFixture.root().string());
    const auto legacyFindingId = store.createFinding(legacyFindingTask.id, std::nullopt, "legacy.cpp",
        std::nullopt, std::nullopt, "legacy finding without fingerprint", std::nullopt).id;
    const auto legacyFindingList = request(handoffContext, http::verb::get,
        "/api/tasks/" + legacyFindingTask.id + "/findings");
    assert(legacyFindingList.result() == http::status::ok);
    const auto legacyFindingEntries = nlohmann::json::parse(legacyFindingList.body());
    assert(legacyFindingEntries.size() == 1 && legacyFindingEntries[0].at("id") == legacyFindingId);
    assert(legacyFindingEntries[0].at("freshness") == "unknown");

    const auto created = request(context, http::verb::post, "/api/tasks", R"({"prompt":"inspect this"})");
    assert(created.result() == http::status::created);
    assert(created.body().find("inspect this") != std::string::npos);
    const auto taskId = nlohmann::json::parse(created.body()).at("id").get<std::string>();

    const auto findingRun = store.startRun(taskId, "codex");
    for (int index = 0; index <= 500; ++index)
        store.appendEvent({"paged-history-" + std::to_string(index), taskId, findingRun.id, "agent.message.completed", "codex", "event", index + 1});
    const auto recentEvents = request(context, http::verb::get, "/api/events?task_id=" + taskId);
    const auto recentEventList = nlohmann::json::parse(recentEvents.body());
    assert(recentEvents.result() == http::status::ok && recentEventList.size() == 500);
    assert(recentEventList.front().at("id") == "paged-history-1" && recentEventList.back().at("id") == "paged-history-500");

    const auto findingCreated = request(context, http::verb::post, "/api/tasks/" + taskId + "/findings",
        nlohmann::json{{"run_id", findingRun.id}, {"file_path", "src/main.cpp"}, {"start_line", 7},
                       {"end_line", 9}, {"message", "Check cleanup on early return."}}.dump());
    assert(findingCreated.result() == http::status::created);
    const auto finding = nlohmann::json::parse(findingCreated.body());
    assert(finding.at("status") == "open" && finding.at("run_id") == findingRun.id);
    const auto findingId = finding.at("id").get<std::string>();
    const auto findingList = request(context, http::verb::get, "/api/tasks/" + taskId + "/findings");
    assert(findingList.result() == http::status::ok && nlohmann::json::parse(findingList.body()).size() == 1);
    const auto findingResolved = request(context, http::verb::patch, "/api/findings/" + findingId, R"({"status":"resolved"})");
    assert(findingResolved.result() == http::status::ok && findingResolved.body().find("resolved") != std::string::npos);
    const auto findingInvalidStatus = request(context, http::verb::patch, "/api/findings/" + findingId, R"({"status":"critical"})");
    assert(findingInvalidStatus.result() == http::status::bad_request);
    const auto findingInvalidPath = request(context, http::verb::post, "/api/tasks/" + taskId + "/findings",
        R"({"file_path":"../outside.cpp","message":"unsafe reference"})");
    assert(findingInvalidPath.result() == http::status::bad_request);
    const auto findingsBeforeNul = store.findings(taskId).size();
    const auto findingInvalidNul = request(context, http::verb::post, "/api/tasks/" + taskId + "/findings",
        nlohmann::json{{"file_path", "src/main.cpp"}, {"message", std::string("embedded\0nul message", 19)}}.dump());
    assert(findingInvalidNul.result() == http::status::bad_request);
    assert(nlohmann::json::parse(findingInvalidNul.body()).at("error").at("code") == "invalid_message");
    assert(store.findings(taskId).size() == findingsBeforeNul);
    assert(store.updateRunStatus(findingRun.id, "running"));
    assert(store.updateRunStatus(findingRun.id, "completed"));
    assert(store.deleteRun(findingRun.id));
    assert(!store.findings(taskId).front().runId);

    const auto missingTaskRun = request(context, http::verb::post, "/api/tasks/missing/runs", R"({"agent":"codex"})");
    assert(missingTaskRun.result() == http::status::not_found);
    assert(missingTaskRun.body().find("task_not_found") != std::string::npos);
    const auto missingRunMessage = request(context, http::verb::post, "/api/runs/missing/messages", R"({"message":"hello"})");
    assert(missingRunMessage.result() == http::status::not_found);

    const auto noTaskVerification = request(context, http::verb::post, "/api/verify", R"({"command":["true"]})");
    assert(noTaskVerification.result() == http::status::bad_request);

    const auto verificationSubscription = events.subscribe();
    const auto verification = request(context, http::verb::post, "/api/verify",
        nlohmann::json{{"task_id", taskId}, {"command", {"/usr/bin/printf", "verification-ok"}}}.dump());
    assert(verification.result() == http::status::ok);
    assert(nlohmann::json::parse(verification.body()).at("command").at(1) == "verification-ok");
    aegis::daemon::AgentEvent verificationStarted;
    aegis::daemon::AgentEvent verificationCompleted;
    assert(events.wait(verificationSubscription, verificationStarted, std::chrono::milliseconds(100)));
    assert(events.wait(verificationSubscription, verificationCompleted, std::chrono::milliseconds(100)));
    assert(verificationStarted.type == "verification.started" && verificationStarted.taskId == taskId);
    assert(verificationCompleted.type == "verification.completed" && verificationCompleted.content == "exit_code=0");
    events.unsubscribe(verificationSubscription);
    const auto verificationHistory = request(context, http::verb::get, "/api/tasks/" + taskId + "/verifications");
    assert(verificationHistory.result() == http::status::ok);
    assert(nlohmann::json::parse(verificationHistory.body()).size() == 1);

    const auto tasks = request(context, http::verb::get, "/api/tasks");
    assert(tasks.result() == http::status::ok);
    assert(tasks.body().find("inspect this") != std::string::npos);

    const auto invalid = request(context, http::verb::post, "/api/tasks", "not-json");
    assert(invalid.result() == http::status::bad_request);
    assert(invalid.body().find("invalid_json") != std::string::npos);

    const auto handoff = request(context, http::verb::get, "/api/tasks/" + taskId + "/handoff");
    assert(handoff.result() == http::status::ok);
    assert(handoff.body().find("inspect this") != std::string::npos);
    assert(handoff.body().find("verification-ok") != std::string::npos);
    assert(nlohmann::json::parse(handoff.body()).at("findings").size() == 1);

    RepositoryFixture staticFixture;
    staticFixture.write("web/index.html", "Aegis UI");
    staticFixture.write("outside/secret.txt", "outside secret");
    std::filesystem::create_directories(staticFixture.root() / "web/assets");
    std::filesystem::create_symlink(staticFixture.root() / "outside/secret.txt", staticFixture.root() / "web/assets/leak.txt");
    const auto staticIndex = aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/");
    assert(staticIndex.result() == http::status::ok && staticIndex.body() == "Aegis UI");
    assert(staticIndex[http::field::cache_control] == "no-cache");
    staticFixture.write("web/assets/app.js", "export {};");
    const auto staticAsset = aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/assets/app.js");
    assert(staticAsset[http::field::cache_control] == "public, max-age=31536000, immutable");
    const auto staticQuery = aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/?cache=1");
    assert(staticQuery.result() == http::status::ok && staticQuery.body() == "Aegis UI");
    const auto staticEscape = aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/assets/leak.txt");
    assert(staticEscape.result() == http::status::not_found && staticEscape.body().empty());
    assert(aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/../outside/secret.txt").result() == http::status::bad_request);
    staticFixture.write("web/large.bin", "");
    std::filesystem::resize_file(staticFixture.root() / "web/large.bin", 16 * 1024 * 1024 + 1);
    assert(aegis::daemon::api::staticFileResponse(staticFixture.root() / "web", "/large.bin").result() == http::status::payload_too_large);

    const auto graph = request(context, http::verb::get, "/api/tasks/" + taskId + "/graph");
    assert(graph.result() == http::status::ok);
    assert(graph.body().find("nodes") != std::string::npos);

    const auto provenance = request(context, http::verb::get, "/api/tasks/" + taskId + "/provenance");
    assert(provenance.result() == http::status::ok);
    assert(provenance.body().find("records") != std::string::npos);

    const auto missing = request(context, http::verb::get, "/api/tasks/missing/handoff");
    assert(missing.result() == http::status::not_found);

    const auto run = store.startRun(taskId, "claude");
    store.appendEvent({"event-1", taskId, run.id, "agent.failed", "claude", "failed", 1});
    const auto deleted = request(context, http::verb::delete_, "/api/runs/" + run.id);
    assert(deleted.result() == http::status::no_content);
    assert(store.runs(taskId).empty());
    const auto remainingEvents = store.events(taskId);
    assert(remainingEvents.size() == 2);
    assert(std::all_of(remainingEvents.begin(), remainingEvents.end(), [](const auto &event) {
        return event.runId.empty() && event.type.starts_with("verification.");
    }));

    using aegis::daemon::api::allowedWebSocketOrigin;
    using aegis::daemon::api::allowedLoopbackHost;
    assert(allowedLoopbackHost("127.0.0.1:46729"));
    assert(allowedLoopbackHost("localhost:5173"));
    assert(!allowedLoopbackHost("attacker.example"));
    assert(!allowedLoopbackHost("localhost.evil.example:5173"));
    assert(!allowedLoopbackHost("127.0.0.1:46729", 0));
    assert(!allowedLoopbackHost("127.0.0.1:46729", 2));
    assert(allowedWebSocketOrigin(""));
    assert(allowedWebSocketOrigin("http://127.0.0.1:46729"));
    assert(allowedWebSocketOrigin("http://localhost:5173"));
    assert(allowedWebSocketOrigin("http://[::1]:5173"));
    assert(!allowedWebSocketOrigin("https://attacker.example"));
    assert(!allowedWebSocketOrigin("http://attacker.example"));
    assert(!allowedWebSocketOrigin("http://localhost.evil.example:5173"));
    assert(!allowedWebSocketOrigin("http://localhost:99999"));
    assert(!allowedWebSocketOrigin("http://localhost:5173/path"));

    aegis::daemon::api::Server server(context);
    if (server.start()) {
        const auto port = server.port();
        assert(httpHealthRequest(port, "127.0.0.1:" + std::to_string(port)) == http::status::ok);
        assert(httpHealthRequest(port, "attacker.example") == http::status::forbidden);
        const auto appOrigin = eventSocketHandshake(port, "http://127.0.0.1:" + std::to_string(port));
        assert(appOrigin.first);
        const auto viteOrigin = eventSocketHandshake(port, "http://localhost:5173");
        assert(viteOrigin.first);
        const auto externalOrigin = eventSocketHandshake(port, "https://attacker.example");
        assert(!externalOrigin.first && externalOrigin.second == http::status::forbidden);
        server.stop();
    } else {
        std::clog << "SKIP WebSocket handshake integration: loopback bind unavailable\n";
    }

    // Regression: independent codex import scope — validation, idempotency, and data-only text.
    {
        const auto importBody = [](const nlohmann::json &overrideFields = nlohmann::json::object()) {
            nlohmann::json body{{"session_id", "sess-1"}, {"source_name", "codex.jsonl"},
                {"prompt", "imported"},
                {"context", {{"goal", "g"}, {"constraints", "c"}, {"decisions", "d"},
                              {"attempts", "a"}, {"unfinished", "u"}, {"next_action", "n"}}},
                {"messages", nlohmann::json::array({
                    {{"id", "m1"}, {"role", "user"}, {"text", "hello"}, {"source_line", 1}, {"source_name", "codex.jsonl"}},
                    {{"id", "m2"}, {"role", "tool"}, {"text", "{\"raw\":true}"}, {"source_line", 2}, {"source_name", "codex.jsonl"}}})}};
            body.update(overrideFields);
            return body.dump();
        };
        const auto imported = request(context, http::verb::post, "/api/imports/codex", importBody());
        assert(imported.result() == http::status::ok);
        const auto importedTask = nlohmann::json::parse(imported.body());
        const auto importedId = importedTask.at("id").get<std::string>();
        assert(store.task(importedId)->repository == std::filesystem::current_path().string());
        const auto importedEvents = store.events(importedId);
        std::size_t toolMessages = 0;
        for (const auto &event : importedEvents)
            if (event.type == "import.message") {
                const auto payload = nlohmann::json::parse(event.content);
                assert(payload.at("historical").get<bool>());
                if (payload.at("role") == "tool") {
                    ++toolMessages;
                    assert(payload.at("text") == "{\"raw\":true}");
                }
            }
        assert(toolMessages == 1);
        assert(store.verifications(importedId).empty());
        assert(store.runs(importedId).size() == 1);
        const auto duplicate = request(context, http::verb::post, "/api/imports/codex", importBody());
        assert(duplicate.result() == http::status::ok);
        assert(nlohmann::json::parse(duplicate.body()).at("id") == importedId);
        assert(store.runs(importedId).size() == 1);
        const auto expectRejected = [&](const nlohmann::json &overrideFields) {
            const auto response = request(context, http::verb::post, "/api/imports/codex", importBody(overrideFields));
            assert(response.result() == http::status::bad_request);
        };
        expectRejected(nlohmann::json{{"context", nlohmann::json{{"goal", std::string(4001, 'x')}, {"constraints", "c"},
            {"decisions", "d"}, {"attempts", "a"}, {"unfinished", "u"}, {"next_action", "n"}}}});
        expectRejected(nlohmann::json{{"messages", nlohmann::json::array({
            {{"id", "m1"}, {"role", "system"}, {"text", "x"}, {"source_line", 1}, {"source_name", "s"}}})}});
        expectRejected(nlohmann::json{{"messages", nlohmann::json::array({
            {{"id", "m1"}, {"role", "user"}, {"text", "x"}, {"source_line", -1}, {"source_name", "s"}}})}});
        expectRejected(nlohmann::json{{"messages", nlohmann::json::array({
            {{"id", "m1"}, {"role", "user"}, {"text", "x"}, {"source_line", 1}, {"source_name", ""}}})}});
        expectRejected(nlohmann::json{{"messages", nlohmann::json::array({
            {{"id", "m1"}, {"role", "user"}, {"text", "x"}, {"source_line", 1}, {"source_name", "s"}},
            {{"id", "m1"}, {"role", "user"}, {"text", "y"}, {"source_line", 2}, {"source_name", "s"}}})}});
    }

    std::filesystem::remove(database);
}
