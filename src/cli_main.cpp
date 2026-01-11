#include <iostream>
#include <thread>
#include <chrono>
#include "../include/chat_message.h"

extern "C" {
    typedef void* chat_client_handle;
    typedef void (*chat_callback_t)(const ChatMessage*, void*);
    chat_client_handle chat_client_create();
    void chat_client_destroy(chat_client_handle h);
    void chat_client_start(chat_client_handle h, const char *channel, const char *token, chat_callback_t cb, void* user);
    void chat_client_stop(chat_client_handle h);
}

static void on_message(const ChatMessage *m, void *user) {
    if (!m) return;
    std::cout << m->username << ": " << m->message << std::endl;
}

int main(int argc, char **argv) {
    const char *channel = argc > 1 ? argv[1] : "twitch";
    const char *token = argc > 2 ? argv[2] : "oauth:anonymous";

    auto client = chat_client_create();
    chat_client_start(client, channel, token, on_message, nullptr);

    // Run for a short while to collect messages
    std::this_thread::sleep_for(std::chrono::seconds(8));

    chat_client_stop(client);
    chat_client_destroy(client);
    std::cout << "Exiting." << std::endl;
    return 0;
}
