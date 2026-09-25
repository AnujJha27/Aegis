#pragma once

#include "daemon/protocol/event_hub.h"
#include "daemon/session/store.h"

#include <boost/beast/http.hpp>

#include <filesystem>

namespace aegis::daemon::api {

struct Context {
    Store *store = nullptr;
    EventHub *events = nullptr;
    std::filesystem::path repository;
};

using Request = boost::beast::http::request<boost::beast::http::string_body>;
using Response = boost::beast::http::response<boost::beast::http::string_body>;

Response handle(const Request &request, const Context &context);

}
