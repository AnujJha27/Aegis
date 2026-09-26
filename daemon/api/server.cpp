#include "daemon/api/server.h"

#include "daemon/protocol/json.h"
#include "daemon/agents/manager.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <system_error>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cctype>

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
    if (error) return false;
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), error);
    if (error) return false;
    acceptor_.bind(endpoint, error);
    if (error) return false;
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
    if (error) return false;
    acceptor_.non_blocking(true, error);
    if (error) return false;
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    if (acceptThread_.joinable()) acceptThread_.join();
}

std::uint16_t Server::port() const {
    boost::system::error_code error;
    const auto endpoint = acceptor_.local_endpoint(error);
    return error ? 0 : endpoint.port();
}

void Server::acceptLoop() {
    while (running_) {
        boost::system::error_code error;
        boost::asio::ip::tcp::socket socket(io_);
        acceptor_.accept(socket, error);
        if (error) {
            if (!running_) return;
            if (error == boost::asio::error::would_block || error == boost::asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return;
        }
        std::thread(&Server::serve, this, std::move(socket)).detach();
    }
}

void Server::serve(boost::asio::ip::tcp::socket socket) {
    boost::beast::flat_buffer buffer;
    Request request;
    boost::system::error_code error;
    boost::beast::http::read(socket, buffer, request, error);
    if (error) return;
    if (boost::beast::websocket::is_upgrade(request)) {
        if (request.target() == "/ws/events") {
            serveWebSocket(std::move(socket), request);
            return;
        }
        if (request.target().starts_with("/ws/pty/")) {
            servePtyWebSocket(std::move(socket), request);
            return;
        }
    }
    const auto response = request.target().starts_with("/api/") ? handle(request, context_) : serveStatic(request);
    boost::beast::http::write(socket, response, error);
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send, error);
}

Response Server::serveStatic(const Request &request) const {
    const auto target = std::string(request.target());
    std::string normalized = target;
    for (auto &character : normalized) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (target.find("..") != std::string::npos || normalized.find("%2e") != std::string::npos)
        return Response{boost::beast::http::status::bad_request, 11};
    if (context_.webRoot.empty()) return Response{boost::beast::http::status::not_found, 11};
    const auto root = std::filesystem::absolute(context_.webRoot).lexically_normal();
    const auto relative = target == "/" ? "index.html" : target.substr(1);
    const auto candidate = (root / relative).lexically_normal();
    const auto relativeCandidate = std::filesystem::relative(candidate, root);
    if (relativeCandidate.empty() || relativeCandidate.string().starts_with(".."))
        return Response{boost::beast::http::status::bad_request, 11};
    std::ifstream input(candidate, std::ios::binary);
    if (!input) return Response{boost::beast::http::status::not_found, 11};
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Response response{boost::beast::http::status::ok, 11};
    const auto extension = candidate.extension().string();
    const auto contentType = extension == ".html" ? "text/html" : extension == ".js" ? "text/javascript" : extension == ".css" ? "text/css" : "application/octet-stream";
    response.set(boost::beast::http::field::content_type, contentType);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

void Server::serveWebSocket(boost::asio::ip::tcp::socket socket, const Request &request) {
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket(std::move(socket));
    boost::system::error_code error;
    websocket.accept(request, error);
    if (error) return;
    const auto subscription = context_.events->subscribe();
    while (running_) {
        AgentEvent event;
        if (!context_.events->wait(subscription, event, std::chrono::milliseconds(250))) continue;
        websocket.write(boost::asio::buffer(protocol::toJson(event).dump()), error);
        if (error) break;
    }
    context_.events->unsubscribe(subscription);
    websocket.close(boost::beast::websocket::close_code::normal, error);
}

void Server::servePtyWebSocket(boost::asio::ip::tcp::socket socket, const Request &request) {
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket(std::move(socket));
    boost::system::error_code error;
    websocket.accept(request, error);
    if (error) return;
    const std::string target(request.target());
    const auto runId = target.substr(std::string("/ws/pty/").size());
    websocket.read_message_max(16 * 1024);
    boost::beast::flat_buffer buffer;
    while (running_) {
        websocket.read(buffer, error);
        if (error) break;
        const auto input = boost::beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        if (!context_.agentManager || !context_.agentManager->sendPty(runId, input)) {
            websocket.close(boost::beast::websocket::close_code::policy_error, error);
            return;
        }
    }
}

}
