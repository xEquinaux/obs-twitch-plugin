#include "../include/chat_message.h"
#include "parse_helpers.h"

#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <mutex>
#include <queue>
#include <sstream>
#include <iostream>

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#include <openssl/ssl.h>

using websocketpp::connection_hdl;
using ws_client = websocketpp::client<websocketpp::config::asio_tls_client>;

// Real ChatClient using websocketpp to connect to Twitch IRC over WSS
class ChatClient {
public:
    using Callback = std::function<void(const ChatMessage &)>;

    ChatClient(): running_(false), connected_(false) {}

    ~ChatClient() {
        stop();
    }

    void start(const std::string &channel, const std::string &oauth_token, Callback cb) {
        channel_ = channel;
        token_ = oauth_token;
        cb_ = cb;

        // Setup client
        client_.init_asio();

        client_.set_tls_init_handler([this](connection_hdl){
            // Create SSL context
            auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23);
            try {
                ctx->set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2);
                // In production you should verify the server certificate. For this prototype
                // we'll disable verification so the connection is simpler to configure.
                ctx->set_verify_mode(boost::asio::ssl::verify_none);
            } catch (std::exception &e) {
                fprintf(stderr, "[chatclient] TLS setup failed: %s\n", e.what());
            }
            return ctx;
        });

        client_.set_open_handler([this](connection_hdl hdl){
            fprintf(stderr, "[chatclient] connection opened\n");
            connected_ = true;
            this->conn_hdl_ = hdl;
            // Request tags and commands capabilities so we get emote tags
            std::string cap = "CAP REQ :twitch.tv/tags twitch.tv/commands twitch.tv/membership";
            client_.send(hdl, cap, websocketpp::frame::opcode::text);
            // Send PASS and NICK and JOIN
            std::string pass = "PASS " + token_;
            std::string nick = "NICK twitchbot"; // nickname isn't important for Twitch bots
            std::string join = "JOIN #" + channel_;
            client_.send(hdl, pass, websocketpp::frame::opcode::text);
            client_.send(hdl, nick, websocketpp::frame::opcode::text);
            client_.send(hdl, join, websocketpp::frame::opcode::text);
        });

        client_.set_message_handler([this](connection_hdl, ws_client::message_ptr msg){
            std::string payload = msg->get_payload();
            // Twitch messages can contain multiple IRC lines separated by \r\n
            std::istringstream ss(payload);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                // Respond to PING
                if (line.rfind("PING", 0) == 0) {
                    std::string pong = "PONG :tmi.twitch.tv";
                    try { client_.send(this->conn_hdl_, pong, websocketpp::frame::opcode::text); } catch(...) {}
                    continue;
                }

                auto parsed = parse_privmsg_line(line);
                if (parsed) {
                    ChatMessage m;
                    m.username = parsed->username;
                    m.message = parsed->message;
                    m.timestamp = std::chrono::system_clock::now();
                    if (cb_) cb_(m);
                }
            }
        });

        client_.set_fail_handler([this](connection_hdl){
            fprintf(stderr, "[chatclient] connection failed\n");
            connected_ = false;
        });

        client_.set_close_handler([this](connection_hdl){
            fprintf(stderr, "[chatclient] connection closed\n");
            connected_ = false;
        });

        // Create connection
        websocketpp::lib::error_code ec;
        std::string uri = "wss://irc-ws.chat.twitch.tv:443";
        auto con = client_.get_connection(uri, ec);
        if (ec) {
            fprintf(stderr, "[chatclient] connection error: %s\n", ec.message().c_str());
            return;
        }

        // Run client in background thread
        running_ = true;
        worker_ = std::thread([this, con]() {
            client_.connect(con);
            try {
                client_.run();
            } catch (const std::exception &e) {
                fprintf(stderr, "[chatclient] run exception: %s\n", e.what());
            }
        });
    }

    void stop() {
        running_ = false;
        if (connected_) {
            try {
                client_.close(conn_hdl_, websocketpp::close::status::normal, "shutdown");
            } catch(...) {}
        }
        client_.stop();
        if (worker_.joinable()) worker_.join();
        connected_ = false;
    }

private:
    std::string channel_;
    std::string token_;
    Callback cb_;

    ws_client client_;
    connection_hdl conn_hdl_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::thread worker_;
};

// Expose a minimal C-style wrapper for easier usage in the OBS source
extern "C" {
    typedef void* chat_client_handle;
    typedef void (*chat_callback_t)(const ChatMessage*, void*);

    chat_client_handle chat_client_create() { return new ChatClient(); }
    void chat_client_destroy(chat_client_handle h) { delete static_cast<ChatClient*>(h); }
    void chat_client_start(chat_client_handle h, const char *channel, const char *token, chat_callback_t cb, void* user) {
        ChatClient *c = static_cast<ChatClient*>(h);
        c->start(channel ? channel : "", token ? token : "", [cb, user](const ChatMessage &m){ if (cb) cb(&m, user); });
    }
    void chat_client_stop(chat_client_handle h) { static_cast<ChatClient*>(h)->stop(); }
}
