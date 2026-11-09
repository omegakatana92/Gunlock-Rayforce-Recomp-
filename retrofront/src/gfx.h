// RetroFront - 2D drawing, fonts and Theme Engine (SDL_Renderer based)
#pragma once
#include "rf_internal.h"
#include <map>
#include <memory>

namespace rf {

struct Color { uint8_t r = 255, g = 255, b = 255, a = 255; };
Color rgba(uint32_t hex, uint8_t a = 255);           // 0xRRGGBB
Color mix(Color a, Color b, double t);
Color with_alpha(Color c, double a);

enum class FontId { Display = 0, Body = 1 };

struct Theme {
    std::string id, name;
    Color bg0, bg1, panel, panel_edge, accent, accent2, accent3, text, text_dim, highlight_text, danger;
    std::string backdrop = "grid";   // grid | stripes | stars
    double title_tracking = 2.0;
};
class ThemeManager {
public:
    void init(const std::string &extra_dir);
    const Theme &get(const std::string &id) const;
    std::vector<std::string> ids() const;
    std::string name_of(const std::string &id) const;
private:
    std::vector<Theme> themes;
};

class Gfx {
public:
    SDL_Renderer *r = nullptr;
    int w = 1280, h = 800;   // output size in pixels
    double s = 1.0;          // UI scale (1.0 at 720p)
    bool init(SDL_Renderer *ren);
    void begin_frame();
    int px(double v) const { return (int)(v * s + 0.5); }

    void rect(double x, double y, double w, double h, Color c);              // pixel coords
    void rect_outline(double x, double y, double w, double h, Color c, double t = 1);
    void rounded(double x, double y, double w, double h, double rad, Color c);
    void gradient_v(double x, double y, double w, double h, Color top, Color bottom);
    void gradient_h(double x, double y, double w, double h, Color left, Color right);
    void line(double x0, double y0, double x1, double y1, Color c);
    void circle(double cx, double cy, double rad, Color c);
    void glow_bar(double x, double y, double w, double h, Color c, double intensity);

    // text: size in pixels (already scaled), align: 0 left 1 center 2 right
    double text(FontId f, double size, double x, double y, const std::string &s, Color c, int align = 0, double tracking = 0);
    double text_width(FontId f, double size, const std::string &s, double tracking = 0);
    double text_fit(FontId f, double size, double x, double y, double maxw, const std::string &s, Color c, int align = 0);
    std::vector<std::string> wrap(FontId f, double size, const std::string &s, double maxw);

    // button prompt glyph ("A", "B", "Esc"...) drawn as a key cap
    double prompt(double x, double y, double size, const std::string &cap, const std::string &label, const Theme &t, bool pad_style);

    SDL_Texture *white = nullptr;
private:
    struct Atlas { SDL_Texture *tex = nullptr; int size = 0; double ascent = 0, line_h = 0; std::vector<uint8_t> chardata; };
    std::map<std::pair<int, int>, std::unique_ptr<Atlas>> atlases;
    Atlas *atlas(FontId f, int size);
    std::vector<uint8_t> fontdata[2];
};

} // namespace rf
