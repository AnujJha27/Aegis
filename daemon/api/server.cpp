#include "daemon/api/server.h"
#include "daemon/api/origin.h"
#include "daemon/api/static_files.h"

#include "daemon/protocol/json.h"
#include "daemon/agents/manager.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <system_error>
#include <thread>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>

namespace aegis::daemon::api {

Server::Server(Context context) : context_(std::move(context)), acceptor_(io_) {}

Server::~Server() {
    stop();
}

bool Server::start(std::uint16_t port) {
    if (running_) return false;
    boost::system::error_code error;
    const auto endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port);
    acceptor_.open(endpoint.protocol(), error);
    if (error) { std::cerr << "aegis_daemon: listener open failed: " << error.message() << '\n'; return false; }
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
    if (error) { std::cerr << "aegis_daemon: listener option failed: " << error.message() << '\n'; return false; }
    acceptor_.bind(endpoint, error);
    if (error) { std::cerr << "aegis_daemon: loopback bind failed: " << error.message() << '\n'; return false; }
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
    if (error) { std::cerr << "aegis_daemon: listener setup failed: " << error.message() << '\n'; return false; }
    acceptor_.non_blocking(true, error);
    if (error) { std::cerr << "aegis_daemon: listener configuration failed: " << error.message() << '\n'; return false; }
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::shared_ptr<Connection>> connections;
    {
        std::lock_guard lock(connectionsMutex_);
        for (const auto &connection : connections_) {
            const auto descriptor = connection->socket->native_handle();
            if (descriptor >= 0) ::shutdown(descriptor, SHUT_RDWR);
        }
        connections.swap(connections_);
    }
    for (const auto &connection : connections)
        if (connection->worker.joinable()) connection->worker.join();
}

std::uint16_t Server::port() const {
    boost::system::error_code error;
    const auto endpoint = acceptor_.local_endpoint(error);
    return error ? 0 : endpoint.port();
}

void Server::acceptLoop() {
    while (running_) {
        pollfd listener{acceptor_.native_handle(), POLLIN, 0};
        const auto ready = poll(&listener, 1, 250);
        if (ready == 0) continue;
        if (ready < 0) {
            if (errno == EINTR) continue;
            if (running_) std::cerr << "aegis_daemon: listener poll failed: " << std::generic_category().message(errno) << '\n';
            return;
        }
        if (!(listener.revents & POLLIN)) {
            if (running_) std::cerr << "aegis_daemon: listener became unavailable\n";
            return;
        }
        boost::system::error_code error;
        auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_);
        acceptor_.accept(*socket, error);
        if (error) {
            if (!running_) return;
            if (error == boost::asio::error::would_block || error == boost::asio::error::try_again) {
                continue;
            }
            std::cerr << "aegis_daemon: accept failed: " << error.message() << '\n';
            return;
        }
        auto connection = std::make_shared<Connection>();
        connection->socket = std::move(socket);
        connection->worker = std::thread([this, connection] {
            try { serve(connection->socket); }
            catch (const std::exception &error) { std::cerr << "aegis_daemon: connection failed: " << error.what() << '\n'; }
            catch (...) { std::cerr << "aegis_daemon: connection failed with an unknown error\n"; }
            boost::system::error_code ignored;
            connection->socket->close(ignored);
            connection->finished = true;
        });
        std::vector<std::shared_ptr<Connection>> completed;
        {
            std::lock_guard lock(connectionsMutex_);
            auto it = connections_.begin();
            while (it != connections_.end()) {
                if ((*it)->finished) {
                    completed.push_back(std::move(*it));
                    it = connections_.erase(it);
                } else ++it;
            }
            connections_.push_back(std::move(connection));
        }
        for (const auto &finished : completed)
            if (finished->worker.joinable()) finished->worker.join();
    }
}

void Server::serve(const std::shared_ptr<boost::asio::ip::tcp::socket> &socket) {
    boost::beast::flat_buffer buffer;
    boost::beast::http::request_parser<boost::beast::http::string_body> parser;
    parser.body_limit(1024 * 1024);
    boost::system::error_code error;
    boost::beast::http::read(*socket, buffer, parser, error);
    if (error) {
        if (error == boost::beast::http::error::body_limit) {
            Response response{boost::beast::http::status::payload_too_large, 11};
            response.set(boost::beast::http::field::content_type, "application/json");
            response.body() = R"({"error":{"code":"request_too_large","message":"request body exceeds 1 MiB"}})";
            response.prepare_payload();
            boost::beast::http::write(*socket, response, error);
        } else if (error != boost::beast::http::error::end_of_stream) {
            std::cerr << "aegis_daemon: request read failed: " << error.message() << '\n';
        }
        return;
    }
    const auto request = parser.release();
    const auto host = request[boost::beast::http::field::host];
    if (!allowedLoopbackHost(host, request.count(boost::beast::http::field::host))) {
        Response response{boost::beast::http::status::forbidden, 11};
        response.set(boost::beast::http::field::content_type, "application/json");
        response.body() = R"({"error":{"code":"host_forbidden","message":"HTTP Host must be a loopback name"}})";
        response.prepare_payload();
        boost::beast::http::write(*socket, response, error);
        return;
    }
    if (boost::beast::websocket::is_upgrade(request)) {
        const auto origin = request[boost::beast::http::field::origin];
        if (request.count(boost::beast::http::field::origin) > 1 || !allowedWebSocketOrigin(origin)) {
            Response response{boost::beast::http::status::forbidden, 11};
            response.set(boost::beast::http::field::content_type, "application/json");
            response.body() = R"({"error":{"code":"websocket_origin_forbidden","message":"WebSocket origin must be a loopback page"}})";
            response.keep_alive(false);
            response.prepare_payload();
            boost::beast::http::write(*socket, response, error);
            return;
        }
        if (request.target() == "/ws/events") {
            serveWebSocket(socket, request);
            return;
        }
        if (request.target().starts_with("/ws/pty/")) {
            servePtyWebSocket(socket, request);
            return;
        }
    }
    const auto response = request.target().starts_with("/api/") ? handle(request, context_) : staticFileResponse(context_.webRoot, request.target());
    boost::beast::http::write(*socket, response, error);
    socket->shutdown(boost::asio::ip::tcp::socket::shutdown_send, error);
}

void Server::serveWebSocket(const std::shared_ptr<boost::asio::ip::tcp::socket> &socket, const Request &request) {
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket &> websocket(*socket);
    boost::system::error_code error;
    websocket.accept(request, error);
    if (error) { std::cerr << "aegis_daemon: event WebSocket handshake failed: " << error.message() << '\n'; return; }
    eventClients_.fetch_add(1);
    struct ClientCount final {
        std::atomic_size_t &count;
        ~ClientCount() { count.fetch_sub(1); }
    } clientCount{eventClients_};
    const auto subscription = context_.events->subscribe();
    auto nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (running_) {
        AgentEvent event;
        if (context_.events->wait(subscription, event, std::chrono::seconds(1))) {
            websocket.write(boost::asio::buffer(protocol::toJson(event).dump()), error);
            if (error) break;
            if (event.type == "stream.resync_required") break;
        }
        if (std::chrono::steady_clock::now() >= nextPing) {
            websocket.ping({}, error);
            if (error) break;
            nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        }
    }
    context_.events->unsubscribe(subscription);
    websocket.close(boost::beast::websocket::close_code::normal, error);
}

void Server::servePtyWebSocket(const std::shared_ptr<boost::asio::ip::tcp::socket> &socket, const Request &request) {
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket &> websocket(*socket);
    boost::system::error_code error;
    websocket.accept(request, error);
    if (error) { std::cerr << "aegis_daemon: PTY WebSocket handshake failed: " << error.message() << '\n'; return; }
    const std::string target(request.target());
    const auto runId = target.substr(std::string("/ws/pty/").size());
    websocket.read_message_max(16 * 1024);
    boost::beast::flat_buffer buffer;
    while (running_) {
        websocket.read(buffer, error);
        if (error) break;
        const auto input = boost::beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        try {
            const auto message = nlohmann::json::parse(input);
            if (message.value("type", std::string{}) == "resize") {
                const auto cols = message.value("cols", 0);
                const auto rows = message.value("rows", 0);
                if (cols > 0 && cols <= 500 && rows > 0 && rows <= 300 && context_.agentManager)
                    context_.agentManager->resizePty(runId, static_cast<unsigned short>(cols), static_cast<unsigned short>(rows));
                continue;
            }
        } catch (const nlohmann::json::exception &) {
        }
        if (!context_.agentManager || !context_.agentManager->sendPty(runId, input)) {
            websocket.close(boost::beast::websocket::close_code::policy_error, error);
            return;
        }
    }
}

}
