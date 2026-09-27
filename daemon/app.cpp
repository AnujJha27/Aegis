#include "daemon/app.h"

#include <filesystem>

namespace aegis::daemon {

App::App(std::filesystem::path repository, std::filesystem::path webRoot)
    : repository_(std::filesystem::absolute(std::move(repository)).lexically_normal()),
      webRoot_(std::move(webRoot)),
      store_([&] {
          std::filesystem::create_directories(repository_ / ".aegis");
          return repository_ / ".aegis" / "aegis.sqlite";
      }()),
      git_(repository_),
      files_(git_),
      agents_(repository_, store_, events_),
      server_({&store_, &events_, &agents_, &git_, &files_, repository_, webRoot_}) {}

App::~App() {
    stop();
}

bool App::start(std::uint16_t port) {
    return server_.start(port);
}

void App::stop() {
    server_.stop();
}

std::uint16_t App::port() const {
    return server_.port();
}

}
