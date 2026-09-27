#pragma once

#include "daemon/api/routes.h"

#include <optional>

namespace aegis::daemon::api::routes {

std::optional<Response> agents(const Request &, const Context &);
std::optional<Response> tasks(const Request &, const Context &);
std::optional<Response> runs(const Request &, const Context &);
std::optional<Response> git(const Request &, const Context &);
std::optional<Response> verification(const Request &, const Context &);
std::optional<Response> review(const Request &, const Context &);
std::optional<Response> files(const Request &, const Context &);

}
