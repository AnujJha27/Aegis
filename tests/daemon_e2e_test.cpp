#include "repository_fixture.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using Json = nlohmann::json;

struct HttpResult {
    http::status status;
    Json body;
};

HttpResult request(std::uint16_t port, http::verb method, const std::string &target, Json body = nullptr) {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect({asio::ip::make_address("127.0.0.1"), port});
    http::request<http::string_body> message(method, target, 11);
    message.set(http::field::host, "127.0.0.1:" + std::to_string(port));
    message.set(http::field::content_type, "application/json");
    if (!body.is_null()) message.body() = body.dump();
    message.prepare_payload();
    http::write(stream, message);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return {response.result(), response.body().empty() ? Json::object() : Json::parse(response.body())};
}

class Daemon final {
public:
    Daemon(const std::string &executable, const std::filesystem::path &repository, bool managed = false) : repository_(repository) {
        int output[2];
        if (pipe(output) != 0) throw std::runtime_error("could not create daemon output pipe");
        pid_ = fork();
        if (pid_ == 0) {
            close(output[0]);
            dup2(output[1], STDOUT_FILENO);
            close(output[1]);
            if (managed)
                execl(executable.c_str(), executable.c_str(), "--repo", repository.c_str(), "--port", "0", "--managed", nullptr);
            else
                execl(executable.c_str(), executable.c_str(), "--repo", repository.c_str(), "--port", "0", nullptr);
            _exit(127);
        }
        close(output[1]);
        if (pid_ < 0) { close(output[0]); throw std::runtime_error("could not fork daemon"); }

        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline && line.find('\n') == std::string::npos) {
            pollfd ready{output[0], POLLIN, 0};
            if (poll(&ready, 1, 100) <= 0) continue;
            char character;
            const auto count = read(output[0], &character, 1);
            if (count <= 0) break;
            line.push_back(character);
        }
        close(output[0]);
        const auto colon = line.find_last_of(':');
        const auto slash = line.find('/', colon);
        if (colon == std::string::npos || slash == std::string::npos) {
            stop();
            throw std::runtime_error("daemon did not report its listening URL: " + line);
        }
        port_ = static_cast<std::uint16_t>(std::stoul(line.substr(colon + 1, slash - colon - 1)));
    }

    ~Daemon() { stop(); }
    std::uint16_t port() const { return port_; }

    bool waitForExit(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) return true;
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    void stop() {
        if (pid_ <= 0) return;
        kill(pid_, SIGTERM);
        int status = 0;
        for (int i = 0; i < 100; ++i) {
            if (waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }

private:
    std::filesystem::path repository_;
    pid_t pid_ = -1;
    std::uint16_t port_ = 0;
};

websocket::stream<asio::ip::tcp::socket> eventSocket(std::uint16_t port, asio::io_context &io) {
    websocket::stream<asio::ip::tcp::socket> socket(io);
    socket.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
    socket.handshake("127.0.0.1:" + std::to_string(port), "/ws/events");
    return socket;
}

websocket::stream<asio::ip::tcp::socket> terminalSocket(std::uint16_t port, asio::io_context &io, const std::string &runId) {
    websocket::stream<asio::ip::tcp::socket> socket(io);
    socket.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
    socket.handshake("127.0.0.1:" + std::to_string(port), "/ws/terminal/" + runId);
    return socket;
}

websocket::stream<asio::ip::tcp::socket> ptySocket(std::uint16_t port, asio::io_context &io, const std::string &runId) {
    websocket::stream<asio::ip::tcp::socket> socket(io);
    socket.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
    socket.handshake("127.0.0.1:" + std::to_string(port), "/ws/pty/" + runId);
    return socket;
}

bool readBefore(websocket::stream<asio::ip::tcp::socket> &socket, beast::flat_buffer &buffer,
                std::chrono::steady_clock::time_point deadline) {
    auto &io = static_cast<asio::io_context &>(socket.get_executor().context());
    boost::system::error_code error;
    bool complete = false;
    socket.async_read(buffer, [&](boost::system::error_code readError, std::size_t) {
        error = readError;
        complete = true;
    });
    io.restart();
    io.run_for(std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero()));
    if (!complete) {
        boost::system::error_code ignored;
        socket.next_layer().cancel(ignored);
        io.run();
        return false;
    }
    if (error) throw std::runtime_error("WebSocket read failed: " + error.message());
    return true;
}

Json nextEvent(websocket::stream<asio::ip::tcp::socket> &socket, const std::string &type) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    beast::flat_buffer buffer;
    std::string seen;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!readBefore(socket, buffer, deadline)) break;
        auto event = Json::parse(beast::buffers_to_string(buffer.data()));
        buffer.consume(buffer.size());
        if (!seen.empty()) seen += ", ";
        seen += event.value("type", std::string{"unknown"});
        if (event.value("type", std::string{}) == type) return event;
    }
    throw std::runtime_error("timed out waiting for event " + type + "; saw " + seen);
}

Json nextTerminalOutput(websocket::stream<asio::ip::tcp::socket> &socket, const std::string &content) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    beast::flat_buffer buffer;
    std::string received;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!readBefore(socket, buffer, deadline)) break;
        auto event = Json::parse(beast::buffers_to_string(buffer.data()));
        buffer.consume(buffer.size());
        if (event.value("type", std::string{}) == "terminal.output") {
            received += event.value("content", std::string{});
            if (received.find(content) != std::string::npos) return event;
        }
    }
    throw std::runtime_error("timed out waiting for terminal output " + content + "; received " + received);
}

Json get(std::uint16_t port, const std::string &target) {
    const auto response = request(port, http::verb::get, target);
    assert(response.status == http::status::ok);
    return response.body;
}

void post(std::uint16_t port, const std::string &target, Json body, http::status expected) {
    assert(request(port, http::verb::post, target, std::move(body)).status == expected);
}

bool hasPath(const Json &entries, const std::string &path) {
    return std::any_of(entries.begin(), entries.end(), [&](const auto &entry) { return entry.at("path") == path; });
}

std::string processStartTime(pid_t pid, char *state = nullptr) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return {};
    const auto commandEnd = line.rfind(')');
    if (commandEnd == std::string::npos) return {};
    std::istringstream fields(line.substr(commandEnd + 2));
    std::string field;
    if (!(fields >> field)) return {};
    if (state) *state = field.front();
    for (int index = 0; index <= 18; ++index) {
        if (!(fields >> field)) return {};
    }
    return field;
}

}

int runE2e(int argc, char **argv) {
    assert(argc == 2);
    RepositoryFixture fixture;
    fixture.write("modify.cpp", "int value = 0;\n");
    fixture.write("rename-old.cpp", "int renamed = 1;\n");
    fixture.write("delete.cpp", "int deleted = 1;\n");
    fixture.commit("base");
    fixture.git({"branch", "feature"});

    const auto fakeBin = fixture.root() / ".aegis/fake-bin";
    std::filesystem::create_directories(fakeBin);
    const auto fakeLog = fixture.root() / ".aegis/fake-codex.log";
    const auto fakeAgent = fakeBin / "codex";
    {
        std::ofstream script(fakeAgent);
        script << "#!/bin/sh\n"
                  "for prompt do :; done\n"
                  "printf '%s\\n' \"$*\" >> \"$AEGIS_FAKE_CODEX_LOG\"\n"
                  "case \"$prompt\" in\n"
                  "  'edit files') printf 'int value = 1;\\n' > modify.cpp; printf 'int added = 1;\\n' > added.cpp; mv rename-old.cpp renamed.cpp; rm delete.cpp ;;\n"
                  "  'edit staged') printf 'int value = 2;\\n' > modify.cpp ;;\n"
                  "  'hold turn') sleep 1 ;;\n"
                  "  'interrupt startup') sleep 30 ;;\n"
                  "  'wait forever') printf '%s\\n' \"$$\" > \"$AEGIS_FAKE_CODEX_PID\"; exec sleep 30 ;;\n"
                  "esac\n"
                  "printf '%s\\n' '{\"type\":\"thread.started\",\"thread_id\":\"fake-session\"}'\n"
                  "printf '%s\\n' '{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"synthetic response\"}}'\n";
    }
    assert(chmod(fakeAgent.c_str(), 0755) == 0);
    const std::string oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
    const auto path = fakeBin.string() + ":" + oldPath;
    assert(setenv("PATH", path.c_str(), 1) == 0);
    assert(setenv("AEGIS_FAKE_CODEX_LOG", fakeLog.c_str(), 1) == 0);
    const auto fakePid = fixture.root() / ".aegis/fake-codex.pid";
    assert(setenv("AEGIS_FAKE_CODEX_PID", fakePid.c_str(), 1) == 0);

    const auto daemonExecutable = std::filesystem::absolute(argv[1]).string();
    Daemon daemon(daemonExecutable, fixture.root());
    assert(get(daemon.port(), "/api/health").at("status") == "healthy");
    const auto created = request(daemon.port(), http::verb::post, "/api/tasks", {{"prompt", "review this repository"}});
    assert(created.status == http::status::created);
    const auto taskId = created.body.at("id").get<std::string>();
    asio::io_context eventIo;
    auto events = eventSocket(daemon.port(), eventIo);
    auto delayedEvents = eventSocket(daemon.port(), eventIo);
    const auto started = request(daemon.port(), http::verb::post, "/api/tasks/" + taskId + "/runs", {{"agent", "codex"}});
    assert(started.status == http::status::created);
    const auto runId = started.body.at("id").get<std::string>();

    post(daemon.port(), "/api/runs/" + runId + "/messages", {{"message", "hold turn"}}, http::status::accepted);
    const auto overlappingPrompt = request(daemon.port(), http::verb::post, "/api/runs/" + runId + "/messages", {{"message", "second concurrent prompt"}});
    assert(overlappingPrompt.status == http::status::conflict);
    assert(nextEvent(events, "agent.message.completed").at("run_id") == runId);
    assert(nextEvent(events, "turn.completed").at("run_id") == runId);

    post(daemon.port(), "/api/runs/" + runId + "/messages", {{"message", "edit files"}}, http::status::accepted);
    assert(nextEvent(events, "agent.message.completed").at("content") == "synthetic response");
    assert(nextEvent(events, "turn.completed").at("run_id") == runId);
    assert(get(daemon.port(), "/api/tasks/" + taskId + "/runs").at(0).at("status") == "running");

    auto changed = get(daemon.port(), "/api/files?scope=changed").at("entries");
    assert(hasPath(changed, "modify.cpp") && hasPath(changed, "added.cpp") &&
        hasPath(changed, "renamed.cpp") && hasPath(changed, "delete.cpp"));
    const auto deleted = std::find_if(changed.begin(), changed.end(), [](const auto &entry) { return entry.at("path") == "delete.cpp"; });
    assert(deleted != changed.end() && deleted->at("git_status") == "D");
    post(daemon.port(), "/api/git/stage", {{"path", "rename-old.cpp"}}, http::status::ok);
    post(daemon.port(), "/api/git/stage", {{"path", "renamed.cpp"}}, http::status::ok);
    changed = get(daemon.port(), "/api/files?scope=changed").at("entries");
    const auto renamed = std::find_if(changed.begin(), changed.end(), [](const auto &entry) { return entry.at("path") == "renamed.cpp"; });
    assert(renamed != changed.end() && renamed->at("old_path") == "rename-old.cpp");
    const auto content = get(daemon.port(), "/api/files/content?path=modify.cpp&source=worktree");
    assert(content.at("content") == "int value = 1;\n");
    const auto deletedHead = get(daemon.port(), "/api/files/content?path=delete.cpp&source=head");
    assert(deletedHead.at("content") == "int deleted = 1;\n");
    const auto allChanges = get(daemon.port(), "/api/files/compare?path=modify.cpp&base=head&target=worktree");
    assert(allChanges.at("original").at("content") == "int value = 0;\n");
    assert(allChanges.at("modified").at("content") == "int value = 1;\n");

    post(daemon.port(), "/api/git/stage", {{"path", "modify.cpp"}}, http::status::ok);
    const auto staged = get(daemon.port(), "/api/files/compare?path=modify.cpp&base=head&target=index");
    assert(staged.at("modified").at("content") == "int value = 1;\n");
    post(daemon.port(), "/api/runs/" + runId + "/messages", {{"message", "edit staged"}}, http::status::accepted);
    assert(nextEvent(events, "agent.message.completed").at("content") == "synthetic response");
    assert(nextEvent(delayedEvents, "turn.completed").at("run_id") == runId);
    const auto unstaged = get(daemon.port(), "/api/files/compare?path=modify.cpp&base=index&target=worktree");
    assert(unstaged.at("original").at("content") == "int value = 1;\n");
    assert(unstaged.at("modified").at("content") == "int value = 2;\n");
    post(daemon.port(), "/api/git/unstage", {{"path", "modify.cpp"}}, http::status::ok);
    const auto status = get(daemon.port(), "/api/git/status");
    assert(std::none_of(status.at("files").begin(), status.at("files").end(), [](const auto &change) {
        return change.at("path") == "modify.cpp" && change.at("index_status") != " ";
    }));

    for (const auto &[route, body] : std::vector<std::pair<std::string, Json>>{
             {"/api/git/branch", {{"branch", "feature"}}}, {"/api/git/pull", Json::object()}, {"/api/git/merge", {{"branch", "feature"}}}}) {
        const auto blocked = request(daemon.port(), http::verb::post, route, body);
        assert(blocked.status == http::status::conflict && blocked.body.at("error").at("code") == "agent_running");
    }

    const auto verification = request(daemon.port(), http::verb::post, "/api/verify",
        {{"task_id", taskId}, {"run_id", runId}, {"command", {"/usr/bin/printf", "verification-evidence"}}});
    assert(verification.status == http::status::ok && verification.body.at("output") == "verification-evidence");
    const auto finding = request(daemon.port(), http::verb::post, "/api/tasks/" + taskId + "/findings",
        {{"run_id", runId}, {"file_path", "modify.cpp"}, {"start_line", 1}, {"message", "Review this value."}});
    assert(finding.status == http::status::created);
    const auto findingId = finding.body.at("id").get<std::string>();
    assert(request(daemon.port(), http::verb::patch, "/api/findings/" + findingId, {{"status", "resolved"}}).status == http::status::ok);
    const auto handoff = get(daemon.port(), "/api/tasks/" + taskId + "/handoff");
    assert(handoff.at("verification").at("output") == "verification-evidence");
    assert(handoff.at("findings").at(0).at("status") == "resolved");

    assert(request(daemon.port(), http::verb::post, "/api/runs/" + runId + "/terminate", Json::object()).status == http::status::accepted);
    assert(request(daemon.port(), http::verb::delete_, "/api/runs/" + runId).status == http::status::no_content);
    assert(get(daemon.port(), "/api/tasks/" + taskId + "/runs").empty());
    assert(get(daemon.port(), "/api/tasks/" + taskId + "/verifications").at(0).at("output") == "verification-evidence");

    const auto retained = request(daemon.port(), http::verb::post, "/api/tasks/" + taskId + "/runs", {{"agent", "codex"}});
    assert(retained.status == http::status::created);
    const auto retainedRunId = retained.body.at("id").get<std::string>();
    post(daemon.port(), "/api/runs/" + retainedRunId + "/messages", {{"message", "persist history"}}, http::status::accepted);
    assert(nextEvent(events, "agent.message.completed").at("run_id") == retainedRunId);

    const auto startup = request(daemon.port(), http::verb::post, "/api/tasks/" + taskId + "/runs", {{"agent", "codex"}});
    const auto startupRunId = startup.body.at("id").get<std::string>();
    post(daemon.port(), "/api/runs/" + startupRunId + "/messages", {{"message", "interrupt startup"}}, http::status::accepted);
    assert(nextEvent(events, "turn.started").at("run_id") == startupRunId);
    post(daemon.port(), "/api/runs/" + startupRunId + "/interrupt", Json::object(), http::status::accepted);
    assert(nextEvent(events, "run.interrupted").at("run_id") == startupRunId);

    const auto ptyRun = request(daemon.port(), http::verb::post, "/api/tasks/" + taskId + "/runs", {{"agent", "shell"}});
    const auto ptyRunId = ptyRun.body.at("id").get<std::string>();
    asio::io_context ptyIo;
    auto terminal = terminalSocket(daemon.port(), ptyIo, ptyRunId);
    auto pty = ptySocket(daemon.port(), ptyIo, ptyRunId);
    pty.write(asio::buffer(std::string("printf 'first-pty-marker\\n'\n")));
    const auto firstPtyOutput = nextTerminalOutput(terminal, "first-pty-marker");
    pty.write(asio::buffer(std::string("printf 'reconnected-pty-marker\\n'\n")));
    const auto disconnectedPtyOutput = nextTerminalOutput(terminal, "reconnected-pty-marker");
    boost::system::error_code closeError;
    terminal.next_layer().close(closeError);
    auto reconnectedTerminal = terminalSocket(daemon.port(), ptyIo, ptyRunId);
    const auto replayedPtyOutput = nextTerminalOutput(reconnectedTerminal, "reconnected-pty-marker");
    assert(firstPtyOutput.at("run_id") == ptyRunId && disconnectedPtyOutput.at("run_id") == ptyRunId &&
        replayedPtyOutput.at("run_id") == ptyRunId);
    boost::system::error_code ignoredPty;
    reconnectedTerminal.next_layer().close(ignoredPty);
    pty.next_layer().close(ignoredPty);
    post(daemon.port(), "/api/runs/" + ptyRunId + "/terminate", Json::object(), http::status::accepted);

    const auto active = request(daemon.port(), http::verb::post, "/api/tasks/" + taskId + "/runs", {{"agent", "codex"}});
    const auto activeRunId = active.body.at("id").get<std::string>();
    post(daemon.port(), "/api/runs/" + activeRunId + "/messages", {{"message", "wait forever"}}, http::status::accepted);
    assert(nextEvent(events, "turn.started").at("run_id") == activeRunId);
    for (int index = 0; index < 100 && !std::filesystem::exists(fakePid); ++index)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(std::filesystem::exists(fakePid));
    std::ifstream childPidFile(fakePid);
    pid_t childPid = -1;
    childPidFile >> childPid;
    assert(childPid > 0);
    char childState = 0;
    const auto childStartTime = processStartTime(childPid, &childState);
    assert(!childStartTime.empty());
    assert(childState != 'Z');
    boost::system::error_code ignored;
    events.next_layer().close(ignored);
    delayedEvents.next_layer().close(ignored);
    daemon.stop();
    const auto childExitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < childExitDeadline) {
        const auto currentStartTime = processStartTime(childPid, &childState);
        if (currentStartTime != childStartTime || childState == 'Z') break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(processStartTime(childPid, &childState) != childStartTime || childState == 'Z');

    Daemon restarted(daemonExecutable, fixture.root());
    assert(get(restarted.port(), "/api/tasks").at(0).at("id") == taskId);
    assert(get(restarted.port(), "/api/tasks/" + taskId + "/runs").at(0).at("status") == "terminated");
    const auto restartedRuns = get(restarted.port(), "/api/tasks/" + taskId + "/runs");
    assert(std::any_of(restartedRuns.begin(), restartedRuns.end(), [&](const auto &run) {
        return run.at("id") == activeRunId && run.at("status") == "terminated";
    }));
    assert(std::any_of(restartedRuns.begin(), restartedRuns.end(), [&](const auto &run) {
        return run.at("id") == startupRunId && run.at("status") == "interrupted";
    }));
    const auto history = get(restarted.port(), "/api/events?task_id=" + taskId);
    assert(std::any_of(history.begin(), history.end(), [&](const auto &event) {
        return event.at("run_id") == retainedRunId && event.at("type") == "agent.message.completed";
    }));
    assert(get(restarted.port(), "/api/tasks/" + taskId + "/findings").at(0).at("status") == "resolved");

    Daemon managed(daemonExecutable, fixture.root(), true);
    asio::io_context managedIo;
    auto managedEvents = eventSocket(managed.port(), managedIo);
    managedEvents.next_layer().close(ignored);
    assert(managed.waitForExit(std::chrono::seconds(5)));
    return 0;
}

int main(int argc, char **argv) {
    try {
        return runE2e(argc, argv);
    } catch (const std::exception &error) {
        std::cerr << "daemon E2E failed: " << error.what() << '\n';
        return 1;
    }
}
