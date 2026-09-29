#pragma once

#include "daemon/api/routes.h"

#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
    std::size_t activeEventClients() const { return eventClients_.load(); }

private:
    struct Connection {
        std::shared_ptr<boost::asio::ip::tcp::socket> socket;
        std::thread worker;
        std::atomic_bool finished = false;
    };

    void acceptLoop();
    void serve(const std::shared_ptr<boost::asio::ip::tcp::socket> &socket);
    void serveWebSocket(const std::shared_ptr<boost::asio::ip::tcp::socket> &socket, const Request &request);
    void servePtyWebSocket(const std::shared_ptr<boost::asio::ip::tcp::socket> &socket, const Request &request);
    void serveTerminalWebSocket(const std::shared_ptr<boost::asio::ip::tcp::socket> &socket, const Request &request);
    Context context_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::atomic_bool running_ = false;
    std::thread acceptThread_;
    std::mutex connectionsMutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::atomic_size_t eventClients_ = 0;
};

}
