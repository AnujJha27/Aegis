#include "daemon/api/server.h"

#include "daemon/protocol/json.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <system_error>
#include <thread>

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
    if (boost::beast::websocket::is_upgrade(request) && request.target() == "/ws/events") {
        serveWebSocket(std::move(socket), request);
        return;
    }
    const auto response = handle(request, context_);
    boost::beast::http::write(socket, response, error);
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send, error);
}

void Server::serveWebSocket(boost::asio::ip::tcp::socket socket, const Request &request) {
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket(std::move(socket));
    boost::system::error_code error;
    websocket.accept(request, error);
    if (error) return;
    const auto subscription = context_.events->subscribe();
    while (running_) {
        AgentEvent event;
        if (!context_.events->wait(subscription, event, std::chrono::milliseconds(250))) break;
        websocket.write(boost::asio::buffer(protocol::toJson(event).dump()), error);
        if (error) break;
    }
    context_.events->unsubscribe(subscription);
    websocket.close(boost::beast::websocket::close_code::normal, error);
}

}
