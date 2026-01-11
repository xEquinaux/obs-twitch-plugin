#pragma once

#include <string>
#include <chrono>
#include <vector>

struct EmoteOccurrence {
    int start;
    int end; // inclusive
    std::string id;   // emote id
    std::string name; // text for this emote (e.g., Kappa)
};

struct ChatMessage {
    std::string username;
    std::string message;
    std::chrono::system_clock::time_point timestamp;
    std::vector<EmoteOccurrence> emotes;
};
