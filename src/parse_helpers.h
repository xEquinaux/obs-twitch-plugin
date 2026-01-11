#pragma once
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include "../include/chat_message.h"

// Parse IRC tags string into a map. Tags format: key1=val1;key2=val2
static inline std::unordered_map<std::string,std::string> parse_tags(const std::string &tags) {
    std::unordered_map<std::string,std::string> out;
    size_t i = 0;
    while (i < tags.size()) {
        size_t eq = tags.find('=', i);
        if (eq == std::string::npos) break;
        size_t sem = tags.find(';', eq+1);
        std::string key = tags.substr(i, eq - i);
        std::string val = (sem == std::string::npos) ? tags.substr(eq+1) : tags.substr(eq+1, sem - eq - 1);
        out[key] = val;
        if (sem == std::string::npos) break;
        i = sem + 1;
    }
    return out;
}

// Parse PRIVMSG line and extract username, message, and emote occurrences if present (from tags)
// Handles lines that optionally begin with tags: @tag1=val1;tag2=val2 :username!user@host PRIVMSG #channel :message
static inline std::optional<ChatMessage> parse_privmsg_line_with_tags(const std::string &line) {
    if (line.empty()) return std::nullopt;
    std::string rest = line;
    std::unordered_map<std::string,std::string> tags;
    if (line[0] == '@') {
        // tags until first space
        size_t sp = line.find(' ');
        if (sp == std::string::npos) return std::nullopt;
        std::string tags_str = line.substr(1, sp - 1);
        tags = parse_tags(tags_str);
        rest = line.substr(sp + 1);
    }

    // rest should start with :username!
    if (rest.empty() || rest[0] != ':') return std::nullopt;
    size_t ex = rest.find('!');
    if (ex == std::string::npos) return std::nullopt;
    std::string user = rest.substr(1, ex - 1);
    size_t priv = rest.find("PRIVMSG");
    if (priv == std::string::npos) return std::nullopt;
    size_t msgcol = rest.find(" :", priv);
    if (msgcol == std::string::npos) return std::nullopt;
    std::string message = rest.substr(msgcol + 2);

    ChatMessage cm;
    cm.username = user;
    cm.message = message;
    cm.timestamp = std::chrono::system_clock::now();

    // If tags contain emotes, parse them. Format: emotes=emoteid:start-end,otherstart-otherend/...
    auto it = tags.find("emotes");
    if (it != tags.end() && !it->second.empty()) {
        std::string ems = it->second;
        // split by '/'
        size_t p = 0;
        while (p < ems.size()) {
            size_t slash = ems.find('/', p);
            std::string part = (slash == std::string::npos) ? ems.substr(p) : ems.substr(p, slash - p);
            size_t colon = part.find(':');
            if (colon != std::string::npos) {
                std::string eid = part.substr(0, colon);
                std::string ranges = part.substr(colon + 1);
                // ranges separated by ','
                size_t q = 0;
                while (q < ranges.size()) {
                    size_t comma = ranges.find(',', q);
                    std::string r = (comma == std::string::npos) ? ranges.substr(q) : ranges.substr(q, comma - q);
                    size_t dash = r.find('-');
                    if (dash != std::string::npos) {
                        int start = std::stoi(r.substr(0, dash));
                        int end = std::stoi(r.substr(dash + 1));
                        int len = end - start + 1;
                        // try to extract the emote text by substring (may be utf-8; this uses byte indices)
                        std::string name = message.substr(start, len);
                        EmoteOccurrence eo{start, end, eid, name};
                        cm.emotes.push_back(eo);
                    }
                    if (comma == std::string::npos) break;
                    q = comma + 1;
                }
            }
            if (slash == std::string::npos) break;
            p = slash + 1;
        }
    }

    return cm;
}
