#include <obs-module.h>
#include "../include/chat_message.h"
#include "../include/text_renderer.h"
#include <vector>
#include <mutex>
#include <deque>
#include <atomic>
#include <string>
#include <memory>

// Forward declaration of chat client C wrapper
extern "C" {
    typedef void* chat_client_handle;
    typedef void (*chat_callback_t)(const ChatMessage*, void*);
    chat_client_handle chat_client_create();
    void chat_client_destroy(chat_client_handle h);
    void chat_client_start(chat_client_handle h, const char *channel, const char *token, chat_callback_t cb, void* user);
    void chat_client_stop(chat_client_handle h);
}

struct chat_source {
    obs_source_t *source;
    std::mutex lock;
    std::deque<ChatMessage> messages;
    int max_messages = 10;
    chat_client_handle client = nullptr;
    std::atomic<bool> running{false};

    // Rendering / fonts
    class TextRenderer *renderer = nullptr;
    std::string font_path;
    int font_size = 22;
    int outline_px = 1;
    RGBAColor outline_color{138,43,226,255}; // #8A2BE2
};

static const char *chat_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Twitch Chat (username outline prototype)";
}

// Callback from chat client C wrapper
static void chat_message_cb(const ChatMessage *m, void *user) {
    if (!m || !user) return;
    chat_source *cs = (chat_source*)user;
    std::lock_guard<std::mutex> guard(cs->lock);
    cs->messages.emplace_back(*m);
    while ((int)cs->messages.size() > cs->max_messages)
        cs->messages.pop_front();
}

static void *chat_create(obs_data_t *settings, obs_source_t *source)
{
    chat_source *cs = new chat_source();
    cs->source = source;

    // Read font settings
    const char *font = obs_data_get_string(settings, "font_path");
    int font_size = (int)obs_data_get_int(settings, "font_size");
    int outline_px = (int)obs_data_get_int(settings, "outline_px");

    if (font && font[0]) cs->font_path = font;
    if (font_size > 0) cs->font_size = font_size;
    if (outline_px >= 0) cs->outline_px = outline_px;

    // Initialize text renderer
    cs->renderer = new TextRenderer();
    if (!cs->renderer->init(cs->font_path, cs->font_size)) {
        blog(LOG_WARNING, "obs-twitch-chat: failed to init font: %s", cs->renderer->last_error().c_str());
        delete cs->renderer;
        cs->renderer = nullptr;
    }

    // Start chat client
    cs->client = chat_client_create();
    const char *channel = obs_data_get_string(settings, "channel");
    const char *token = obs_data_get_string(settings, "oauth");

    cs->running = true;
    chat_client_start(cs->client, channel ? channel : "", token ? token : "", chat_message_cb, cs);

    return cs;
}

static void chat_destroy(void *data)
{
    chat_source *cs = (chat_source*)data;
    if (!cs) return;

    if (cs->client) {
        chat_client_stop(cs->client);
        chat_client_destroy(cs->client);
        cs->client = nullptr;
    }

    if (cs->renderer) {
        delete cs->renderer;
        cs->renderer = nullptr;
    }

    delete cs;
}

static obs_properties_t *chat_get_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_text(props, "channel", "Channel", OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, "oauth", "OAuth Token (pass: oauth:xxxx)", OBS_TEXT_PASSWORD);
    obs_properties_add_int(props, "max_messages", "Max messages", 1, 50, 1);

    obs_properties_add_text(props, "font_path", "Font file path (ttf)", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, "font_size", "Font size (px)", 8, 144, 1);
    obs_properties_add_int(props, "outline_px", "Username outline thickness (px)", 0, 6, 1);

    return props;
}

static void chat_update(void *data, obs_data_t *settings)
{
    chat_source *cs = (chat_source*)data;
    cs->max_messages = (int)obs_data_get_int(settings, "max_messages");
}

static void chat_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    chat_source *cs = (chat_source*)data;

    // Snapshot messages under lock
    std::vector<ChatMessage> snapshot;
    {
        std::lock_guard<std::mutex> guard(cs->lock);
        snapshot.assign(cs->messages.begin(), cs->messages.end());
    }

    int width = obs_source_get_width(cs->source);
    int height = obs_source_get_height(cs->source);
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;

    // Prepare an RGBA buffer to compose text into (simple CPU render)
    std::vector<uint8_t> buffer(width * height * 4, 0);

    int line_h = cs->font_size + 4;
    int y = 4; // top padding
    int idx = 0;
    for (const auto &m : snapshot) {
        // Draw username with outline
        std::string username = m.username + ":";
        int x = 8;
        if (cs->renderer) {
            cs->renderer->render_text_rgba(buffer.data(), width, height, x, y, username,
                                           {255,255,255,255}, cs->outline_px, cs->outline_color);
            int uname_w = cs->renderer->measure_text(username);
            // Draw message to the right of username
            cs->renderer->render_text_rgba(buffer.data(), width, height, x + uname_w + 6, y, m.message,
                                           {255,255,255,255}, 0, {0,0,0,0});
        }
        y += line_h;
        ++idx;
    }

    // Upload buffer as a temporary texture and draw it
    gs_texture_t *tex = gs_texture_create(width, height, GS_RGBA, 1, buffer.data(), GS_DYNAMIC);
    if (tex) {
        gs_reset_blend_state();
        gs_set_render_target(nullptr);
        gs_draw_sprite(tex, 0, width, height);
        gs_texture_destroy(tex);
    }
}

static struct obs_source_info chat_source_info = {
    .id = "obs_twitch_chat_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = chat_get_name,
    .create = chat_create,
    .destroy = chat_destroy,
    .get_properties = chat_get_properties,
    .update = chat_update,
    .video_render = chat_render
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-twitch-chat", "en-US")

bool obs_module_load(void)
{
    obs_register_source(&chat_source_info);
    blog(LOG_INFO, "obs-twitch-chat module loaded");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "obs-twitch-chat module unloaded");
}
