// RetroFront - 2D drawing, fonts and Theme Engine
#include "gfx.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <cmath>
#include <algorithm>

namespace rf {
extern const unsigned char rf_font_display[]; extern const unsigned int rf_font_display_len;
extern const unsigned char rf_font_body[];    extern const unsigned int rf_font_body_len;

Color rgba(uint32_t hex, uint8_t a) { return Color{(uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex, a}; }
Color mix(Color a, Color b, double t) {
    t = clampd(t, 0, 1);
    return Color{(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t)};
}
Color with_alpha(Color c, double a) { c.a = (uint8_t)clampd(a * 255, 0, 255); return c; }

// ------------------------------------------------------------ themes
static Theme mk(const char *id, const char *name, uint32_t bg0, uint32_t bg1, uint32_t panel, uint32_t edge,
                uint32_t acc, uint32_t acc2, uint32_t acc3, uint32_t text, uint32_t dim, uint32_t hl, const char *backdrop) {
    Theme t; t.id = id; t.name = name;
    t.bg0 = rgba(bg0); t.bg1 = rgba(bg1); t.panel = rgba(panel, 215); t.panel_edge = rgba(edge);
    t.accent = rgba(acc); t.accent2 = rgba(acc2); t.accent3 = rgba(acc3); t.text = rgba(text); t.text_dim = rgba(dim);
    t.highlight_text = rgba(hl); t.danger = rgba(0xff4a4a); t.backdrop = backdrop;
    return t;
}
static Color parse_color(const json &j, Color def) {
    if (!j.is_string()) return def;
    std::string s = j.get<std::string>();
    if (s.size() == 7 && s[0] == '#') return rgba((uint32_t)strtoul(s.c_str() + 1, nullptr, 16));
    if (s.size() == 9 && s[0] == '#') { uint32_t v = (uint32_t)strtoul(s.c_str() + 1, nullptr, 16); return rgba(v >> 8, (uint8_t)v); }
    return def;
}
void ThemeManager::init(const std::string &extra_dir) {
    themes.push_back(mk("arcade_gold", "Arcade Gold", 0x07080d, 0x161523, 0x10111c, 0x3a3326, 0xe8b33a, 0xff5c39, 0x4fa3ff, 0xf3efe4, 0x8d8a99, 0x1a1408, "grid"));
    themes.push_back(mk("crimson", "Crimson Cabinet", 0x0b0507, 0x24090f, 0x170a0e, 0x4a1a22, 0xff3b4e, 0xffb347, 0x6fd0ff, 0xf7e9ea, 0x9a8186, 0x1c0408, "stripes"));
    themes.push_back(mk("azure", "Azure Circuit", 0x04070d, 0x0b1830, 0x0b1222, 0x1d3555, 0x48b6ff, 0xffd34a, 0xff5c8a, 0xe8f1ff, 0x7d8ca6, 0x03101f, "stars"));
    themes.push_back(mk("mono", "Monochrome", 0x050505, 0x141414, 0x0e0e0e, 0x333333, 0xe6e6e6, 0xbbbbbb, 0x888888, 0xf2f2f2, 0x8a8a8a, 0x080808, "grid"));
    // user themes: themes/*.json next to the executable
    for (auto &e : platform::list_dir(extra_dir, {".json"})) {
        std::vector<uint8_t> d;
        if (!platform::read_file(e.path, d)) continue;
        json j = json::parse(d.begin(), d.end(), nullptr, false);
        if (j.is_discarded() || !j.contains("id")) continue;
        Theme t = get(j.value("base", std::string("arcade_gold")));
        t.id = j["id"].get<std::string>(); t.name = j.value("name", t.id);
        const json &c = j.value("colors", json::object());
        t.bg0 = parse_color(c.value("background", json()), t.bg0); t.bg1 = parse_color(c.value("background2", json()), t.bg1);
        t.panel = parse_color(c.value("panel", json()), t.panel); t.panel_edge = parse_color(c.value("panel_edge", json()), t.panel_edge);
        t.accent = parse_color(c.value("accent", json()), t.accent); t.accent2 = parse_color(c.value("accent2", json()), t.accent2);
        t.accent3 = parse_color(c.value("accent3", json()), t.accent3); t.text = parse_color(c.value("text", json()), t.text);
        t.text_dim = parse_color(c.value("text_dim", json()), t.text_dim); t.highlight_text = parse_color(c.value("highlight_text", json()), t.highlight_text);
        t.backdrop = j.value("backdrop", t.backdrop);
        themes.push_back(t);
    }
}
const Theme &ThemeManager::get(const std::string &id) const {
    for (auto &t : themes) if (t.id == id) return t;
    return themes.front();
}
std::vector<std::string> ThemeManager::ids() const { std::vector<std::string> v; for (auto &t : themes) v.push_back(t.id); return v; }
std::string ThemeManager::name_of(const std::string &id) const { return get(id).name; }

// ------------------------------------------------------------ gfx
bool Gfx::init(SDL_Renderer *ren) {
    r = ren;
    fontdata[0].assign(rf_font_display, rf_font_display + rf_font_display_len);
    fontdata[1].assign(rf_font_body, rf_font_body + rf_font_body_len);
    uint32_t px = 0xffffffff;
    white = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, 1);
    SDL_UpdateTexture(white, nullptr, &px, 4);
    SDL_SetTextureBlendMode(white, SDL_BLENDMODE_BLEND);
    return true;
}
void Gfx::begin_frame() {
    SDL_GetRendererOutputSize(r, &w, &h);
    s = std::min(h / 720.0, w / 1100.0);
    if (s < 0.5) s = 0.5;
}
void Gfx::rect(double x, double y, double ww, double hh, Color c) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_FRect f{(float)x, (float)y, (float)ww, (float)hh};
    SDL_RenderFillRectF(r, &f);
}
void Gfx::rect_outline(double x, double y, double ww, double hh, Color c, double t) {
    rect(x, y, ww, t, c); rect(x, y + hh - t, ww, t, c); rect(x, y + t, t, hh - 2 * t, c); rect(x + ww - t, y + t, t, hh - 2 * t, c);
}
void Gfx::rounded(double x, double y, double ww, double hh, double rad, Color c) {
    rad = std::min(rad, std::min(ww, hh) / 2);
    if (rad < 1) { rect(x, y, ww, hh, c); return; }
    std::vector<SDL_Vertex> v; std::vector<int> idx;
    SDL_Color col{c.r, c.g, c.b, c.a};
    auto add = [&](double px_, double py_) { v.push_back(SDL_Vertex{SDL_FPoint{(float)px_, (float)py_}, col, SDL_FPoint{0, 0}}); return (int)v.size() - 1; };
    int center = add(x + ww / 2, y + hh / 2);
    const int seg = 6;
    // clockwise in screen space: top-right, bottom-right, bottom-left, top-left
    double cx[4] = {x + ww - rad, x + ww - rad, x + rad, x + rad}, cy[4] = {y + rad, y + hh - rad, y + hh - rad, y + rad};
    double a0[4] = {-M_PI / 2, 0, M_PI / 2, M_PI};
    int first = -1, prev = -1;
    for (int q = 0; q < 4; q++) {
        for (int i = 0; i <= seg; i++) {
            double a = a0[q] + (M_PI / 2) * i / seg;
            int k = add(cx[q] + cos(a) * rad, cy[q] + sin(a) * rad);
            if (first < 0) first = k;
            if (prev >= 0) { idx.push_back(center); idx.push_back(prev); idx.push_back(k); }
            prev = k;
        }
    }
    idx.push_back(center); idx.push_back(prev); idx.push_back(first);
    SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}
void Gfx::gradient_v(double x, double y, double ww, double hh, Color a, Color b) {
    SDL_Color ca{a.r, a.g, a.b, a.a}, cb{b.r, b.g, b.b, b.a};
    SDL_Vertex v[4] = {{{(float)x, (float)y}, ca, {0, 0}}, {{(float)(x + ww), (float)y}, ca, {0, 0}},
                       {{(float)(x + ww), (float)(y + hh)}, cb, {0, 0}}, {{(float)x, (float)(y + hh)}, cb, {0, 0}}};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
}
void Gfx::gradient_h(double x, double y, double ww, double hh, Color a, Color b) {
    SDL_Color ca{a.r, a.g, a.b, a.a}, cb{b.r, b.g, b.b, b.a};
    SDL_Vertex v[4] = {{{(float)x, (float)y}, ca, {0, 0}}, {{(float)(x + ww), (float)y}, cb, {0, 0}},
                       {{(float)(x + ww), (float)(y + hh)}, cb, {0, 0}}, {{(float)x, (float)(y + hh)}, ca, {0, 0}}};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
}
void Gfx::line(double x0, double y0, double x1, double y1, Color c) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderDrawLineF(r, (float)x0, (float)y0, (float)x1, (float)y1);
}
void Gfx::circle(double cx, double cy, double rad, Color c) {
    std::vector<SDL_Vertex> v; std::vector<int> idx;
    SDL_Color col{c.r, c.g, c.b, c.a};
    v.push_back(SDL_Vertex{SDL_FPoint{(float)cx, (float)cy}, col, SDL_FPoint{0, 0}});
    const int seg = 20;
    for (int i = 0; i <= seg; i++) {
        double a = 2 * M_PI * i / seg;
        v.push_back(SDL_Vertex{SDL_FPoint{(float)(cx + cos(a) * rad), (float)(cy + sin(a) * rad)}, col, SDL_FPoint{0, 0}});
        if (i) { idx.push_back(0); idx.push_back(i); idx.push_back(i + 1); }
    }
    SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}
void Gfx::glow_bar(double x, double y, double ww, double hh, Color c, double inten) {
    for (int i = 3; i >= 1; i--) {
        double g = i * 3 * s;
        rounded(x - g, y - g, ww + 2 * g, hh + 2 * g, hh / 2 + g, with_alpha(c, 0.06 * inten * (4 - i)));
    }
}

Gfx::Atlas *Gfx::atlas(FontId f, int size) {
    if (size < 6) size = 6;
    auto key = std::make_pair((int)f, size);
    auto it = atlases.find(key);
    if (it != atlases.end()) return it->second.get();
    auto a = std::make_unique<Atlas>();
    a->size = size;
    int W = 1024, H = 1024;
    while ((size_t)W * H < (size_t)size * size * 140) H *= 2;
    std::vector<uint8_t> bmp((size_t)W * H);
    a->chardata.resize(sizeof(stbtt_packedchar) * 96);
    stbtt_pack_context pc;
    stbtt_PackBegin(&pc, bmp.data(), W, H, 0, 1, nullptr);
    stbtt_PackSetOversampling(&pc, f == FontId::Display ? 1 : 2, 1);
    stbtt_PackFontRange(&pc, fontdata[(int)f].data(), 0, (float)size, 32, 95, (stbtt_packedchar *)a->chardata.data());
    stbtt_PackEnd(&pc);
    stbtt_fontinfo fi;
    stbtt_InitFont(&fi, fontdata[(int)f].data(), 0);
    int asc, desc, gap; stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    double sc = stbtt_ScaleForPixelHeight(&fi, (float)size);
    a->ascent = asc * sc; a->line_h = (asc - desc + gap) * sc;
    std::vector<uint32_t> rgba_((size_t)W * H);
    for (size_t i = 0; i < rgba_.size(); i++) rgba_[i] = ((uint32_t)bmp[i] << 24) | 0xffffff;
    a->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
    SDL_UpdateTexture(a->tex, nullptr, rgba_.data(), W * 4);
    SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(a->tex, f == FontId::Display ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    // remember atlas dims in the tex query
    Atlas *raw = a.get();
    atlases[key] = std::move(a);
    return raw;
}
double Gfx::text_width(FontId f, double size, const std::string &str, double tracking) {
    Atlas *a = atlas(f, (int)(size + 0.5));
    auto *cd = (stbtt_packedchar *)a->chardata.data();
    double wv = 0;
    for (unsigned char ch : str) { if (ch < 32 || ch > 126) ch = '?'; wv += cd[ch - 32].xadvance + tracking; }
    return wv - (str.empty() ? 0 : tracking);
}
double Gfx::text(FontId f, double size, double x, double y, const std::string &str, Color c, int align, double tracking) {
    Atlas *a = atlas(f, (int)(size + 0.5));
    auto *cd = (stbtt_packedchar *)a->chardata.data();
    double wv = text_width(f, size, str, tracking);
    if (align == 1) x -= wv / 2; else if (align == 2) x -= wv;
    int tw, th; SDL_QueryTexture(a->tex, nullptr, nullptr, &tw, &th);
    SDL_SetTextureColorMod(a->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(a->tex, c.a);
    float cx = (float)x, base = (float)(y + a->ascent);
    if (f == FontId::Display) { cx = (float)floor(cx); base = (float)floor(base); }
    for (unsigned char ch : str) {
        if (ch < 32 || ch > 126) ch = '?';
        stbtt_aligned_quad q;
        float xx = cx, yy = base;
        stbtt_GetPackedQuad(cd, tw, th, ch - 32, &xx, &yy, &q, f == FontId::Display ? 1 : 0);
        SDL_Rect src{(int)(q.s0 * tw + 0.5f), (int)(q.t0 * th + 0.5f), (int)((q.s1 - q.s0) * tw + 0.5f), (int)((q.t1 - q.t0) * th + 0.5f)};
        SDL_FRect dst{q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
        if (src.w > 0 && src.h > 0) SDL_RenderCopyF(r, a->tex, &src, &dst);
        cx = xx + (float)tracking;
    }
    return wv;
}
double Gfx::text_fit(FontId f, double size, double x, double y, double maxw, const std::string &str, Color c, int align) {
    double sz = size;
    while (sz > 8 && text_width(f, sz, str) > maxw) sz -= 1;
    if (text_width(f, sz, str) <= maxw) return text(f, sz, x, y + (size - sz) / 2, str, c, align);
    std::string t = str;
    while (t.size() > 1 && text_width(f, sz, t + "...") > maxw) t.pop_back();
    return text(f, sz, x, y, t + "...", c, align);
}
std::vector<std::string> Gfx::wrap(FontId f, double size, const std::string &str, double maxw) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (text_width(f, size, trial) > maxw && !cur.empty()) { lines.push_back(cur); cur = word; }
        else cur = trial;
        word.clear();
    };
    for (char ch : str) {
        if (ch == '\n') { flush_word(); lines.push_back(cur); cur.clear(); }
        else if (ch == ' ') flush_word();
        else word += ch;
    }
    flush_word();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}
double Gfx::prompt(double x, double y, double size, const std::string &cap, const std::string &label, const Theme &t, bool pad_style) {
    double cw = std::max(size * 1.25, text_width(FontId::Body, size * 0.8, cap) + size * 0.7);
    if (pad_style && cap.size() == 1) {
        Color c = cap == "A" ? rgba(0x5fd35f) : cap == "B" ? rgba(0xff5050) : cap == "X" ? rgba(0x4fa3ff) : cap == "Y" ? rgba(0xffd23a) : t.text;
        circle(x + size * 0.62, y + size * 0.62, size * 0.62, with_alpha(c, 0.95));
        text(FontId::Body, size * 0.85, x + size * 0.62, y + size * 0.12, cap, rgba(0x101010), 1);
        cw = size * 1.25;
    } else {
        rounded(x, y, cw, size * 1.25, size * 0.25, with_alpha(t.text, 0.9));
        text(FontId::Body, size * 0.8, x + cw / 2, y + size * 0.2, cap, rgba(0x101010), 1);
    }
    double lw = text(FontId::Body, size, x + cw + size * 0.35, y + size * 0.05, label, t.text);
    return cw + size * 0.35 + lw + size;
}
} // namespace rf
