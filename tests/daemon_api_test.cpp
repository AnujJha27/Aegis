#include "daemon/api/server.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/session/store.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

http::response<http::string_body> request(std::uint16_t port, http::verb method, const std::string &target, std::string body = {}) {
    boost::asio::io_context io;
    tcp::socket socket(io);
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
    http::request<http::string_body> message(method, target, 11);
    message.set(http::field::host, "127.0.0.1");
    message.set(http::field::content_type, "application/json");
    message.body() = std::move(body);
    message.prepare_payload();
    http::write(socket, message);
    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
}

}

int main() {
    const auto database = std::filesystem::temp_directory_path() / "aegis-daemon-api-test.sqlite";
    const auto webRoot = std::filesystem::temp_directory_path() / "aegis-daemon-api-web";
    std::filesystem::remove(database);
    std::filesystem::create_directories(webRoot);
    std::ofstream(webRoot / "index.html") << "Aegis UI";
    aegis::daemon::Store store(database);
    aegis::daemon::EventHub events;
    aegis::daemon::api::Server server({&store, &events, nullptr, nullptr, std::filesystem::current_path(), webRoot});
    assert(server.start(0));
    assert(server.port() != 0);

    const auto health = request(server.port(), http::verb::get, "/api/health");
    assert(health.result() == http::status::ok);
    assert(health.body().find("healthy") != std::string::npos);

    const auto created = request(server.port(), http::verb::post, "/api/tasks", R"({"prompt":"inspect this"})");
    assert(created.result() == http::status::created);
    assert(created.body().find("inspect this") != std::string::npos);

    const auto tasks = request(server.port(), http::verb::get, "/api/tasks");
    assert(tasks.result() == http::status::ok);
    assert(tasks.body().find("inspect this") != std::string::npos);

    const auto invalid = request(server.port(), http::verb::post, "/api/tasks", "not-json");
    assert(invalid.result() == http::status::bad_request);
    assert(invalid.body().find("invalid_json") != std::string::npos);

    const auto index = request(server.port(), http::verb::get, "/");
    assert(index.result() == http::status::ok);
    assert(index.body() == "Aegis UI");

    const auto traversal = request(server.port(), http::verb::get, "/../CMakeLists.txt");
    assert(traversal.result() == http::status::bad_request);

    server.stop();
    std::filesystem::remove(database);
    std::filesystem::remove_all(webRoot);
}
