#include "../include/text_renderer.h"
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstring>
#include <sstream>

struct TextRenderer::Impl {
    FT_Library ft = nullptr;
    FT_Face face = nullptr;
    int pixel_size = 16;
    std::string error;
};

TextRenderer::TextRenderer(): impl_(new Impl()) {}
TextRenderer::~TextRenderer() {
    if (impl_->face) FT_Done_Face(impl_->face);
    if (impl_->ft) FT_Done_FreeType(impl_->ft);
    delete impl_;
}

std::string TextRenderer::last_error() const { return impl_->error; }

bool TextRenderer::init(const std::string &font_path, int pixel_size) {
    impl_->pixel_size = pixel_size;
    int err = FT_Init_FreeType(&impl_->ft);
    if (err) {
        std::ostringstream ss; ss << "FT_Init_FreeType failed: " << err;
        impl_->error = ss.str();
        return false;
    }

    if (!font_path.empty()) {
        err = FT_New_Face(impl_->ft, font_path.c_str(), 0, &impl_->face);
        if (err) {
            std::ostringstream ss; ss << "FT_New_Face failed for " << font_path << " : " << err;
            impl_->error = ss.str();
            return false;
        }
    } else {
        impl_->error = "No font path provided";
        return false;
    }

    FT_Set_Pixel_Sizes(impl_->face, 0, impl_->pixel_size);
    return true;
}

int TextRenderer::measure_text(const std::string &text) {
    if (!impl_->face) return 0;
    int width = 0;
    for (unsigned char c : text) {
        if (FT_Load_Char(impl_->face, c, FT_LOAD_RENDER)) continue;
        width += (impl_->face->glyph->advance.x >> 6);
    }
    return width;
}

static inline void blend_pixel(uint8_t *dst, uint8_t src_alpha, RGBAColor color) {
    // dst is RGBA bytes
    float a = (src_alpha / 255.0f) * (color.a / 255.0f);
    if (a <= 0.0f) return;
    float inv = 1.0f - a;

    dst[0] = (uint8_t)(color.r * a + dst[0] * inv);
    dst[1] = (uint8_t)(color.g * a + dst[1] * inv);
    dst[2] = (uint8_t)(color.b * a + dst[2] * inv);
    dst[3] = (uint8_t)((a + dst[3]/255.0f * inv) * 255.0f);
}

void TextRenderer::render_text_rgba(uint8_t *rgba_buf, int buf_w, int buf_h,
                                   int x, int y, const std::string &text,
                                   RGBAColor color, int outline_px, RGBAColor outline_color) {
    if (!impl_->face || !rgba_buf) return;

    // Helper to blit one glyph at position (gx, gy) with given color
    auto blit_glyph = [&](FT_GlyphSlot slot, int gx, int gy, RGBAColor col) {
        FT_Bitmap &bmp = slot->bitmap;
        int bw = bmp.width;
        int bh = bmp.rows;
        for (int row = 0; row < bh; ++row) {
            for (int colx = 0; colx < bw; ++colx) {
                uint8_t src = bmp.buffer[row * bmp.pitch + colx];
                if (src == 0) continue;
                int px = gx + colx;
                int py = gy + row;
                if (px < 0 || px >= buf_w || py < 0 || py >= buf_h) continue;
                uint8_t *dst = rgba_buf + (py * buf_w + px) * 4;
                blend_pixel(dst, src, col);
            }
        }
    };

    // If outline is requested, draw glyphs shifted by offsets
    const std::vector<std::pair<int,int>> outline_offsets = {
        {-outline_px, -outline_px}, {-outline_px, 0}, {0, -outline_px},
        {outline_px, 0}, {0, outline_px}, {outline_px, outline_px}
    };

    int pen_x = x;
    int pen_y = y + impl_->face->size->metrics.ascender / 64; // baseline adjustment

    // First pass: draw outline glyphs if requested
    if (outline_px > 0) {
        int temp_x = pen_x;
        for (unsigned char ch : text) {
            if (FT_Load_Char(impl_->face, ch, FT_LOAD_RENDER)) { temp_x += impl_->face->glyph->advance.x >> 6; continue; }
            FT_GlyphSlot slot = impl_->face->glyph;
            int gx = temp_x + slot->bitmap_left;
            int gy = pen_y - slot->bitmap_top;
            for (auto ofs : outline_offsets) {
                blit_glyph(slot, gx + ofs.first, gy + ofs.second, outline_color);
            }
            temp_x += (slot->advance.x >> 6);
        }
    }

    // Second pass: draw main glyphs in color
    int temp_x = pen_x;
    for (unsigned char ch : text) {
        if (FT_Load_Char(impl_->face, ch, FT_LOAD_RENDER)) { temp_x += impl_->face->glyph->advance.x >> 6; continue; }
        FT_GlyphSlot slot = impl_->face->glyph;
        int gx = temp_x + slot->bitmap_left;
        int gy = pen_y - slot->bitmap_top;
        blit_glyph(slot, gx, gy, color);
        temp_x += (slot->advance.x >> 6);
    }
}
