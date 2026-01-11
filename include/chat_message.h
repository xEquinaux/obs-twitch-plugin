#pragma once

#include <string>
#include <chrono>

struct ChatMessage {
    std::string username;
    std::string message;
    std::chrono::system_clock::time_point timestamp;
};
