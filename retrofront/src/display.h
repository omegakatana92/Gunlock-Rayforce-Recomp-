// RetroFront - Display Manager
#pragma once
#include "gfx.h"

namespace rf {

struct DisplaySettings {
    std::string orientation = "auto";  // auto (follow cabinet) | tate | yoko
    std::string aspect = "original";   // original | square | stretch
    bool integer_scale = true;
    std::string filter = "nearest";    // nearest | linear | sharp
    bool vsync = true;
    bool fullscreen = false;
    bool crt = false;
    double scanlines = 0.45;           // 0..1
    double curvature = 0.0;            // 0..1
    std::string mask = "none";         // none | aperture | shadow
    double mask_strength = 0.35;
    double glow = 0.0;                 // phosphor bloom 0..1
    double brightness = 1.0;           // 0.5..1.5
    double gamma = 1.0;                // 0.6..1.6
    double zoom = 1.0;                 // manual size 0.5..1 when not integer scaled
    std::string border = "theme";      // theme | black | image
    std::string wallpaper;             // png/jpg path for border=image
};

class DisplayManager {
public:
    DisplaySettings set;
    bool init(SDL_Renderer *r);
    void upload(const VideoFrame &f, Rotation cabinet);   // orientation + colour LUT -> texture
    SDL_Rect layout(int W, int H) const;                  // where the game goes
    void draw(Gfx &g, const SDL_Rect &dst, double alpha = 1.0);
    bool rotated() const { return rot_ != 0; }
    int out_w() const { return ow; }
    int out_h() const { return oh; }
    const std::vector<uint32_t> &pixels() const { return buf; } // oriented, colour-corrected frame
    bool has_frame() const { return ow > 0; }
    void invalidate_luts() { lut_ok = false; }
    SDL_Texture *wallpaper_tex(SDL_Renderer *r);
    int rot_degrees(Rotation cabinet) const;
private:
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr, *sharp_rt = nullptr, *crt_rt = nullptr, *scan_tex = nullptr, *mask_tex = nullptr, *glow_tex = nullptr;
    int ow = 0, oh = 0, rot_ = 0, src_lines = 0;
    std::vector<uint32_t> buf;
    uint8_t lut[256];
    bool lut_ok = false;
    double lut_b = -1, lut_g = -1;
    int sharp_w = 0, sharp_h = 0, crt_w = 0, crt_h = 0, scan_n = 0, mask_w = 0, mask_h = 0;
    double scan_amt = -1; std::string mask_kind; double mask_amt = -1; bool scan_vertical = false, scan_vert_built = false;
    SDL_Texture *wall = nullptr; std::string wall_path;
    double display_aspect = 4.0 / 3.0;
    void ensure_tex();
};

} // namespace rf
