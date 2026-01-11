#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct RGBAColor {
    uint8_t r, g, b, a;
};

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    // Initialize with a font file path and pixel size. Returns false on failure.
    bool init(const std::string &font_path, int pixel_size);

    // Measure width of a string in pixels using current font size.
    int measure_text(const std::string &text);

    // Render a single line of text into an RGBA buffer (width*height*4).
    // x,y are top-left coordinates where text begins. Clipping is performed.
    void render_text_rgba(uint8_t *rgba_buf, int buf_w, int buf_h,
                          int x, int y, const std::string &text,
                          RGBAColor color, int outline_px = 0, RGBAColor outline_color = {0,0,0,255});

    // Returns last error string if init failed
    std::string last_error() const;

private:
    struct Impl;
    Impl *impl_;
};
