#pragma once

#include "daemon/api/routes.h"

#include <string_view>

namespace aegis::daemon::api {

Response staticFileResponse(const std::filesystem::path &webRoot, std::string_view target);

}
