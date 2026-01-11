#include <obs-module.h>
#include "../include/chat_message.h"
#include "../include/text_renderer.h"
#include <vector>
#include <mutex>
#include <deque>
#include <atomic>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <curl/curl.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
using namespace Gdiplus;
#endif

static size_t curl_write_mem(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    auto *buf = (std::vector<uint8_t>*)userp;
    buf->insert(buf->end(), (uint8_t*)contents, (uint8_t*)contents + realsize);
    return realsize;
}

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
    bool emotes_enabled = false;

    // Basic emote storage: name -> RGBA bitmap (width*height*4)
    std::unordered_map<std::string, std::vector<uint8_t>> emote_images;
    int emote_size = 24;
};

static const char *chat_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Twitch Chat (username outline prototype)";
}

// ---------- Emote helpers (basic built-in placeholders) ----------

static inline std::string strip_punct(const std::string &s) {
    size_t i = 0, j = s.size();
    while (i < j && ispunct((unsigned char)s[i])) ++i;
    while (j > i && ispunct((unsigned char)s[j-1])) --j;
    return s.substr(i, j - i);
}

struct MsgSegment {
    bool is_emote;
    std::string text;    // used when !is_emote
    std::string emote;   // used when is_emote
};

static std::vector<MsgSegment> parse_segments(const std::string &msg, const std::unordered_set<std::string> &emotes) {
    std::vector<MsgSegment> out;
    std::istringstream ss(msg);
    std::string tok;
    std::string acc;

    auto flush_acc = [&]{ if (!acc.empty()) { out.push_back({false, acc, ""}); acc.clear(); } };

    while (ss >> tok) {
        std::string key = strip_punct(tok);
        if (!key.empty() && emotes.find(key) != emotes.end()) {
            flush_acc();
            out.push_back({true, "", key});
        } else {
            if (!acc.empty()) acc += " ";
            acc += tok;
        }
    }
    flush_acc();
    return out;
}

static void generate_builtin_emotes(chat_source *cs) {
    // A small set of placeholder emotes with distinct colors
    std::vector<std::pair<std::string, std::array<uint8_t,3>>> defs = {
        {"Kappa", {200,100,100}},
        {"PogChamp", {100,200,120}},
        {"LUL", {120,140,220}},
        {"KEKW", {220,180,90}},
        {"PogU", {200,120,220}}
    };

    int sz = cs->emote_size;
    for (auto &d : defs) {
        std::vector<uint8_t> buf(sz * sz * 4, 0);
        auto col = d.second;
        for (int y = 0; y < sz; ++y) {
            for (int x = 0; x < sz; ++x) {
                int idx = (y * sz + x) * 4;
                // simple circle alpha mask for nicer shape
                int cx = sz/2, cy = sz/2;
                float dx = (x - cx) / float(sz/2);
                float dy = (y - cy) / float(sz/2);
                float r2 = dx*dx + dy*dy;
                uint8_t alpha = (r2 <= 1.0f) ? (uint8_t)(255 * (1.0f - 0.4f * r2)) : 0;
                buf[idx + 0] = col[0];
                buf[idx + 1] = col[1];
                buf[idx + 2] = col[2];
                buf[idx + 3] = alpha;
            }
        }
        cs->emote_images[d.first] = std::move(buf);
    }
}

// Attempt to fetch emote image from Twitch CDN asynchronously. Stores as key "twitch:<id>" and also with name when provided.
static void fetch_emote_async(chat_source *cs, const std::string &emote_id, const std::string &name) {
    std::string key = std::string("twitch:") + emote_id;
    {
        std::lock_guard<std::mutex> guard(cs->lock);
        if (cs->emote_images.find(key) != cs->emote_images.end()) return; // already have
    }

    std::thread([cs, emote_id, name, key]() {
        int size = cs->emote_size > 0 ? cs->emote_size : 28;
        std::string url = "https://static-cdn.jtvnw.net/emoticons/v1/" + emote_id + "/3"; // size 3 (~112)
        std::vector<uint8_t> mem;

        CURL *curl = curl_easy_init();
        if (!curl) return;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_mem);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || mem.empty()) return;

#ifdef _WIN32
        // Use GDI+ to decode image from memory
        IStream *stream = SHCreateMemStream(mem.data(), (UINT)mem.size());
        if (!stream) return;
        Bitmap *bmp = Bitmap::FromStream(stream);
        stream->Release();
        if (!bmp) return;
        BitmapData bd;
        Rect r(0,0,bmp->GetWidth(), bmp->GetHeight());
        if (bmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) { delete bmp; return; }
        int w = bd.Width, h = bd.Height;
        std::vector<uint8_t> out(w * h * 4);
        for (int y = 0; y < h; ++y) {
            uint8_t *row = (uint8_t*)bd.Scan0 + y * bd.Stride;
            for (int x = 0; x < w; ++x) {
                uint8_t b = row[x*4 + 0];
                uint8_t g = row[x*4 + 1];
                uint8_t rch = row[x*4 + 2];
                uint8_t a = row[x*4 + 3];
                int idx = (y * w + x) * 4;
                out[idx+0] = rch; out[idx+1] = g; out[idx+2] = b; out[idx+3] = a;
            }
        }
        bmp->UnlockBits(&bd);
        delete bmp;

        // Optionally scale to emote_size (naive nearest neighbor)
        if (w != cs->emote_size || h != cs->emote_size) {
            int sw = cs->emote_size, sh = cs->emote_size;
            std::vector<uint8_t> scaled(sw * sh * 4);
            for (int yy = 0; yy < sh; ++yy) {
                for (int xx = 0; xx < sw; ++xx) {
                    int sx = xx * w / sw;
                    int sy = yy * h / sh;
                    int sidx = (sy * w + sx) * 4;
                    int didx = (yy * sw + xx) * 4;
                    scaled[didx+0] = out[sidx+0];
                    scaled[didx+1] = out[sidx+1];
                    scaled[didx+2] = out[sidx+2];
                    scaled[didx+3] = out[sidx+3];
                }
            }
            std::lock_guard<std::mutex> guard(cs->lock);
            cs->emote_images[key] = std::move(scaled);
            if (!name.empty()) cs->emote_images[name] = cs->emote_images[key];
        } else {
            std::lock_guard<std::mutex> guard(cs->lock);
            cs->emote_images[key] = std::move(out);
            if (!name.empty()) cs->emote_images[name] = cs->emote_images[key];
        }
#else
        // Non-Windows: not implemented in this prototype
        (void)name; (void)mem;
#endif
    }).detach();
}

static inline void blit_rgba(uint8_t *dst_buf, int dst_w, int dst_h, int dst_x, int dst_y,
                             const uint8_t *src_buf, int src_w, int src_h) {
    for (int y = 0; y < src_h; ++y) {
        int dy = dst_y + y;
        if (dy < 0 || dy >= dst_h) continue;
        for (int x = 0; x < src_w; ++x) {
            int dx = dst_x + x;
            if (dx < 0 || dx >= dst_w) continue;
            const uint8_t *s = src_buf + (y * src_w + x) * 4;
            uint8_t sa = s[3];
            if (sa == 0) continue;
            uint8_t *d = dst_buf + (dy * dst_w + dx) * 4;
            float a = sa / 255.0f;
            for (int c = 0; c < 3; ++c) {
                d[c] = (uint8_t)(s[c] * a + d[c] * (1.0f - a));
            }
            // update alpha channel as simple max
            d[3] = (uint8_t)std::min(255, (int)d[3] + sa);
        }
    }
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
    uint32_t outline_col = (uint32_t)obs_data_get_int(settings, "outline_color");
    bool emotes = obs_data_get_bool(settings, "emotes");

    if (font && font[0]) cs->font_path = font;
    if (font_size > 0) cs->font_size = font_size;
    if (outline_px >= 0) cs->outline_px = outline_px;
    cs->emotes_enabled = emotes;

    // decode RGB from int (0xRRGGBB)
    cs->outline_color.r = (outline_col >> 16) & 0xFF;
    cs->outline_color.g = (outline_col >> 8) & 0xFF;
    cs->outline_color.b = (outline_col) & 0xFF;
    cs->outline_color.a = 255;

    // Initialize text renderer
    cs->renderer = new TextRenderer();
    if (!cs->renderer->init(cs->font_path, cs->font_size)) {
        blog(LOG_WARNING, "obs-twitch-chat: failed to init font: %s", cs->renderer->last_error().c_str());
        delete cs->renderer;
        cs->renderer = nullptr;
    }

    // Generate builtin emotes (placeholder images)
    generate_builtin_emotes(cs);

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

    // Font selection
    obs_properties_add_path(props, "font_path", "Font file path (ttf)", OBS_PATH_FILE, "Font files (*.ttf;*.otf);;*.*", NULL);
    obs_properties_add_int(props, "font_size", "Font size (px)", 8, 144, 1);

    // Outline settings for usernames
    obs_properties_add_int(props, "outline_px", "Username outline thickness (px)", 0, 12, 1);
    obs_properties_add_color(props, "outline_color", "Outline color");

    // Other features
    obs_properties_add_bool(props, "emotes", "Enable emotes (first iteration: placeholder)");

    return props;
}

static void chat_update(void *data, obs_data_t *settings)
{
    chat_source *cs = (chat_source*)data;

    int max_messages = (int)obs_data_get_int(settings, "max_messages");
    const char *font = obs_data_get_string(settings, "font_path");
    int font_size = (int)obs_data_get_int(settings, "font_size");
    int outline_px = (int)obs_data_get_int(settings, "outline_px");
    uint32_t outline_col = (uint32_t)obs_data_get_int(settings, "outline_color");
    bool emotes = obs_data_get_bool(settings, "emotes");

    cs->max_messages = max_messages;
    cs->emotes_enabled = emotes;
    cs->outline_px = outline_px;
    cs->outline_color.r = (outline_col >> 16) & 0xFF;
    cs->outline_color.g = (outline_col >> 8) & 0xFF;
    cs->outline_color.b = (outline_col) & 0xFF;
    cs->outline_color.a = 255;

    // Reinitialize renderer if font changed
    bool need_reinit = false;
    if (font && font[0]) {
        if (cs->font_path != font) { cs->font_path = font; need_reinit = true; }
    }
    if (font_size > 0 && cs->font_size != font_size) { cs->font_size = font_size; need_reinit = true; }

    if (need_reinit) {
        if (cs->renderer) { delete cs->renderer; cs->renderer = nullptr; }
        cs->renderer = new TextRenderer();
        if (!cs->renderer->init(cs->font_path, cs->font_size)) {
            blog(LOG_WARNING, "obs-twitch-chat: failed to init font (update): %s", cs->renderer->last_error().c_str());
            delete cs->renderer; cs->renderer = nullptr;
        }
        // regenerate emotes with new size
        cs->emote_size = cs->font_size;
        cs->emote_images.clear();
        generate_builtin_emotes(cs);
    }
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

    int line_h = cs->font_size + 6;
    int y = 4; // top padding
    int idx = 0;
    // Build a set of available emote keys for parsing
    std::unordered_set<std::string> emote_keys;
    for (const auto &p : cs->emote_images) emote_keys.insert(p.first);

    for (const auto &m : snapshot) {
        // Draw username with outline
        std::string username = m.username + ":";
        int x = 8;
        if (cs->renderer) {
            cs->renderer->render_text_rgba(buffer.data(), width, height, x, y, username,
                                           {255,255,255,255}, cs->outline_px, cs->outline_color);
            int uname_w = cs->renderer->measure_text(username);
            int cur_x = x + uname_w + 6;

            // Parse message into segments (text / emote)
            if (cs->emotes_enabled && !emote_keys.empty()) {
                auto segs = parse_segments(m.message, emote_keys);
                for (auto &s : segs) {
                    if (s.is_emote) {
                        auto it = cs->emote_images.find(s.emote);
                        if (it != cs->emote_images.end()) {
                            // Blit emote image scaled to emote_size
                            blit_rgba(buffer.data(), width, height, cur_x, y, it->second.data(), cs->emote_size, cs->emote_size);
                            cur_x += cs->emote_size + 4;
                        } else {
                            // fallback to text
                            cs->renderer->render_text_rgba(buffer.data(), width, height, cur_x, y, s.emote,
                                                           {255,255,255,255}, 0, {0,0,0,0});
                            cur_x += cs->renderer->measure_text(s.emote) + 4;
                        }
                    } else {
                        cs->renderer->render_text_rgba(buffer.data(), width, height, cur_x, y, s.text,
                                                       {255,255,255,255}, 0, {0,0,0,0});
                        cur_x += cs->renderer->measure_text(s.text) + 4;
                    }
                }
            } else {
                // Emotes disabled or none available: render raw message
                cs->renderer->render_text_rgba(buffer.data(), width, height, x + uname_w + 6, y, m.message,
                                               {255,255,255,255}, 0, {0,0,0,0});
            }
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
