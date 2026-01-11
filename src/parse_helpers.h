#pragma once
#include <string>
#include <optional>

// Simple IRC line parsing helper that extracts username and message from a PRIVMSG line.
// Example incoming IRC PRIVMSG line:
// :username!username@username.tmi.twitch.tv PRIVMSG #channel :Hello world

struct ParsedLine {
    std::string username;
    std::string message;
};

static inline std::optional<ParsedLine> parse_privmsg_line(const std::string &line) {
    // Find first ':' at the start
    if (line.empty() || line[0] != ':') return std::nullopt;
    // Find '!' after the username
    size_t ex = line.find('!');
    if (ex == std::string::npos) return std::nullopt;
    std::string user = line.substr(1, ex - 1);
    // Find "PRIVMSG" keyword
    size_t priv = line.find("PRIVMSG");
    if (priv == std::string::npos) return std::nullopt;
    // The message starts after the second ':' that denotes the message text
    size_t msgcol = line.find(" :", priv);
    if (msgcol == std::string::npos) return std::nullopt;
    std::string message = line.substr(msgcol + 2);
    return ParsedLine{user, message};
}
