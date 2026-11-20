#pragma once

#include "daemon/agents/adapter.h"

#include <string_view>
#include <vector>

namespace aegis::daemon::agents {

std::vector<AgentEvent> parseClaudeJsonLine(std::string_view line, const RunContext &context);

}
