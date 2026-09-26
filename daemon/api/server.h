#pragma once

#include "daemon/api/routes.h"

#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

namespace aegis::daemon::api {

class Server final {
public:
    explicit Server(Context context);
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    bool start(std::uint16_t port = 0);
    void stop();
    std::uint16_t port() const;

private:
    void acceptLoop();
    void serve(boost::asio::ip::tcp::socket socket);
    void serveWebSocket(boost::asio::ip::tcp::socket socket, const Request &request);
    void servePtyWebSocket(boost::asio::ip::tcp::socket socket, const Request &request);
    Response serveStatic(const Request &request) const;

    Context context_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::atomic_bool running_ = false;
    std::thread acceptThread_;
};

}
