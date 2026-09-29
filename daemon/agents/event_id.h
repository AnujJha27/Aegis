#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace aegis::daemon::agents {

inline std::string newEventId() {
    static std::atomic_uint64_t sequence = 0;
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "event-" + std::to_string(timestamp) + "-" + std::to_string(++sequence);
}

}
