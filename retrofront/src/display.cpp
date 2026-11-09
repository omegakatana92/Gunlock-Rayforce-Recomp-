// RetroFront - Display Manager: scaling, filtering, colour and CRT pipeline
#include "display.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include <cmath>
#include <algorithm>

namespace rf {

bool DisplayManager::init(SDL_Renderer *r) { ren = r; return true; }

int DisplayManager::rot_degrees(Rotation cab) const {
    if (set.orientation == "yoko") return 0;
    if (set.orientation == "tate") return cab == Rotation::None ? 270 : (int)cab; // force vertical
    return (int)cab;
}

void DisplayManager::upload(const VideoFrame &f, Rotation cab) {
    if (!f.pixels || f.width <= 0) return;
    display_aspect = f.display_aspect;
    rot_ = rot_degrees(cab);
    int w = f.width, h = f.height;
    int nw = (rot_ == 90 || rot_ == 270) ? h : w, nh = (rot_ == 90 || rot_ == 270) ? w : h;
    src_lines = h;                       // scanlines run along source rows
    scan_vertical = (rot_ == 90 || rot_ == 270);
    if (nw != ow || nh != oh) { ow = nw; oh = nh; buf.assign((size_t)ow * oh, 0); ensure_tex(); }
    if (!lut_ok || lut_b != set.brightness || lut_g != set.gamma) {
        for (int i = 0; i < 256; i++) {
            double v = pow(i / 255.0, 1.0 / set.gamma) * set.brightness;
            lut[i] = (uint8_t)clampd(v * 255.0 + 0.5, 0, 255);
        }
        lut_ok = true; lut_b = set.brightness; lut_g = set.gamma;
    }
    const bool ident = set.brightness == 1.0 && set.gamma == 1.0;
    const uint8_t *src = (const uint8_t *)f.pixels;
    auto conv = [&](uint32_t c) -> uint32_t {
        if (ident) return c | 0xff000000u;
        return 0xff000000u | (lut[(c >> 16) & 255] << 16) | (lut[(c >> 8) & 255] << 8) | lut[c & 255];
    };
    for (int y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)y * f.pitch);
        for (int x = 0; x < w; x++) {
            int nx, ny;
            switch (rot_) {
            case 270: nx = y; ny = w - 1 - x; break;        // rotate 90 CCW
            case 90: nx = h - 1 - y; ny = x; break;         // rotate 90 CW
            case 180: nx = w - 1 - x; ny = h - 1 - y; break;
            default: nx = x; ny = y; break;
            }
            buf[(size_t)ny * ow + nx] = conv(row[x]);
        }
    }
    SDL_UpdateTexture(tex, nullptr, buf.data(), ow * 4);
}

void DisplayManager::ensure_tex() {
    if (tex) SDL_DestroyTexture(tex);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
    if (glow_tex) { SDL_DestroyTexture(glow_tex); glow_tex = nullptr; }
}

SDL_Rect DisplayManager::layout(int W, int H) const {
    if (ow <= 0) return SDL_Rect{0, 0, W, H};
    double ar;   // displayed aspect (w/h)
    bool rot = rot_ == 90 || rot_ == 270;
    if (set.aspect == "square") ar = (double)ow / oh;
    else if (set.aspect == "stretch") ar = (double)W / H;
    else ar = rot ? 1.0 / display_aspect : display_aspect;
    double dw, dh;
    if ((double)W / H > ar) { dh = H; dw = H * ar; } else { dw = W; dh = W / ar; }
    if (set.aspect != "stretch") {
        if (set.integer_scale) {
            // integer multiple of the source on the axis that limits, other axis follows aspect
            int k = (int)std::floor(dh / oh);
            if (set.aspect == "square") k = std::min((int)(W / ow), (int)(H / oh));
            if (k >= 1) { dh = oh * k; dw = set.aspect == "square" ? ow * k : dh * ar; if (dw > W) { dw = W; } }
        } else { dw *= set.zoom; dh *= set.zoom; }
    }
    return SDL_Rect{(int)((W - dw) / 2), (int)((H - dh) / 2), (int)dw, (int)dh};
}

SDL_Texture *DisplayManager::wallpaper_tex(SDL_Renderer *r) {
    if (set.wallpaper == wall_path) return wall;
    if (wall) { SDL_DestroyTexture(wall); wall = nullptr; }
    wall_path = set.wallpaper;
    std::vector<uint8_t> d;
    if (wall_path.empty() || !platform::read_file(wall_path, d)) return nullptr;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(d.data(), (int)d.size(), &w, &h, &n, 4);
    if (!px) return nullptr;
    wall = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    SDL_UpdateTexture(wall, nullptr, px, w * 4);
    SDL_SetTextureScaleMode(wall, SDL_ScaleModeLinear);
    stbi_image_free(px);
    return wall;
}

static SDL_Texture *make_target(SDL_Renderer *r, SDL_Texture *old, int w, int h) {
    if (old) SDL_DestroyTexture(old);
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    return t;
}

void DisplayManager::draw(Gfx &g, const SDL_Rect &dst, double alpha) {
    if (!tex || dst.w <= 0 || dst.h <= 0) return;
    SDL_Texture *prev_target = SDL_GetRenderTarget(ren);
    // 1. the picture, filtered
    SDL_Texture *pic = tex;
    SDL_SetTextureScaleMode(tex, set.filter == "linear" ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    if (set.filter == "sharp") {
        // integer nearest prescale, then a linear pass: crisp pixels without shimmering at fractional scales
        int k = std::max(1, std::min(6, (int)std::ceil(std::max((double)dst.w / ow, (double)dst.h / oh))));
        if (!sharp_rt || sharp_w != ow * k || sharp_h != oh * k) {
            sharp_rt = make_target(ren, sharp_rt, ow * k, oh * k); sharp_w = ow * k; sharp_h = oh * k;
            SDL_SetTextureScaleMode(sharp_rt, SDL_ScaleModeLinear);
        }
        if (sharp_rt) {
            SDL_SetRenderTarget(ren, sharp_rt);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_SetRenderTarget(ren, prev_target);
            pic = sharp_rt;
        }
    }
    const bool crt = set.crt;
    if (!crt) {
        SDL_SetTextureBlendMode(pic, alpha < 1 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        SDL_SetTextureAlphaMod(pic, (uint8_t)(alpha * 255));
        SDL_RenderCopy(ren, pic, nullptr, &dst);
        SDL_SetTextureAlphaMod(pic, 255);
        return;
    }
    // 2. CRT: compose into a target the size of the output rect
    if (!crt_rt || crt_w != dst.w || crt_h != dst.h) {
        crt_rt = make_target(ren, crt_rt, dst.w, dst.h); crt_w = dst.w; crt_h = dst.h;
        SDL_SetTextureScaleMode(crt_rt, SDL_ScaleModeLinear);
        mask_w = -1;
    }
    if (!crt_rt) { SDL_RenderCopy(ren, pic, nullptr, &dst); return; }
    SDL_SetRenderTarget(ren, crt_rt);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_SetTextureBlendMode(pic, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(ren, pic, nullptr, nullptr);
    // phosphor glow: a blurred (downscaled, linear) copy added on top
    if (set.glow > 0.01) {
        if (!glow_tex) {
            glow_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, std::max(1, ow / 4), std::max(1, oh / 4));
            SDL_SetTextureScaleMode(glow_tex, SDL_ScaleModeLinear);
        }
        if (glow_tex) {
            SDL_SetRenderTarget(ren, glow_tex);
            SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_SetRenderTarget(ren, crt_rt);
            SDL_SetTextureBlendMode(glow_tex, SDL_BLENDMODE_ADD);
            SDL_SetTextureAlphaMod(glow_tex, (uint8_t)(set.glow * 140));
            SDL_RenderCopy(ren, glow_tex, nullptr, nullptr);
        }
    }
    // scanlines follow the *source* rows: vertical on a rotated (TATE) picture, like a turned monitor
    if (set.scanlines > 0.01) {
        if (!scan_tex || scan_n != src_lines || scan_amt != set.scanlines || scan_vert_built != scan_vertical) {
            if (scan_tex) SDL_DestroyTexture(scan_tex);
            int n = src_lines * 4;
            std::vector<uint32_t> p(n);
            for (int i = 0; i < n; i++) {
                double ph = (i % 4 + 0.5) / 4.0;                  // position within one source line
                double prof = 0.5 + 0.5 * cos((ph - 0.5) * 2 * M_PI); // 1 at line centre, 0 between lines
                double v = 1.0 - set.scanlines * (1.0 - prof);
                uint8_t c = (uint8_t)clampd(v * 255, 0, 255);
                p[i] = 0xff000000u | (c << 16) | (c << 8) | c;
            }
            scan_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, scan_vertical ? n : 1, scan_vertical ? 1 : n);
            SDL_UpdateTexture(scan_tex, nullptr, p.data(), (scan_vertical ? n : 1) * 4);
            SDL_SetTextureScaleMode(scan_tex, SDL_ScaleModeLinear);
            SDL_SetTextureBlendMode(scan_tex, SDL_BLENDMODE_MOD);
            scan_n = src_lines; scan_amt = set.scanlines; scan_vert_built = scan_vertical;
        }
        SDL_RenderCopy(ren, scan_tex, nullptr, nullptr);
    }
    if (set.mask != "none" && set.mask_strength > 0.01) {
        if (mask_w != dst.w || mask_h != dst.h || mask_kind != set.mask || mask_amt != set.mask_strength) {
            if (mask_tex) SDL_DestroyTexture(mask_tex);
            std::vector<uint32_t> p((size_t)dst.w * dst.h);
            double m = set.mask_strength;
            uint8_t lo = (uint8_t)clampd((1 - m) * 255, 0, 255);
            for (int y = 0; y < dst.h; y++) for (int x = 0; x < dst.w; x++) {
                int ph;
                if (set.mask == "aperture") ph = x % 3;
                else ph = (x + (y / 2 % 2) * 2) % 3;   // offset triads every other pair of rows
                uint32_t r_ = ph == 0 ? 255 : lo, g_ = ph == 1 ? 255 : lo, b_ = ph == 2 ? 255 : lo;
                if (set.mask == "shadow" && (y % 2 == 1)) { r_ = r_ * 9 / 10; g_ = g_ * 9 / 10; b_ = b_ * 9 / 10; }
                p[(size_t)y * dst.w + x] = 0xff000000u | (r_ << 16) | (g_ << 8) | b_;
            }
            mask_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, dst.w, dst.h);
            SDL_UpdateTexture(mask_tex, nullptr, p.data(), dst.w * 4);
            SDL_SetTextureBlendMode(mask_tex, SDL_BLENDMODE_MOD);
            mask_w = dst.w; mask_h = dst.h; mask_kind = set.mask; mask_amt = set.mask_strength;
        }
        SDL_RenderCopy(ren, mask_tex, nullptr, nullptr);
        // masks darken: compensate a little
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
        uint8_t comp = (uint8_t)(set.mask_strength * 28);
        SDL_SetRenderDrawColor(ren, comp, comp, comp, 255);
        SDL_RenderFillRect(ren, nullptr);
    }
    SDL_SetRenderTarget(ren, prev_target);
    // 3. present with barrel curvature + vignette through a warped mesh
    const int GX = 32, GY = 24;
    double k = set.curvature * 0.12;
    std::vector<SDL_Vertex> v; v.reserve((GX + 1) * (GY + 1));
    std::vector<int> idx; idx.reserve(GX * GY * 6);
    double cx = dst.x + dst.w / 2.0, cy = dst.y + dst.h / 2.0;
    for (int j = 0; j <= GY; j++) for (int i = 0; i <= GX; i++) {
        double u = (double)i / GX, t = (double)j / GY;
        double qx = u * 2 - 1, qy = t * 2 - 1;
        double r2 = qx * qx + qy * qy;
        double sc = k > 0 ? (1 + k) / (1 + k * r2) : 1.0;
        double sx = cx + qx * sc * dst.w / 2.0, sy = cy + qy * sc * dst.h / 2.0;
        double vig = set.curvature > 0 ? 1.0 - 0.35 * set.curvature * pow(std::max(fabs(qx), fabs(qy)), 6) : 1.0;
        uint8_t c = (uint8_t)clampd(vig * 255, 0, 255);
        v.push_back(SDL_Vertex{SDL_FPoint{(float)sx, (float)sy}, SDL_Color{c, c, c, (uint8_t)(alpha * 255)}, SDL_FPoint{(float)u, (float)t}});
    }
    for (int j = 0; j < GY; j++) for (int i = 0; i < GX; i++) {
        int a = j * (GX + 1) + i, b = a + 1, c = a + GX + 1, d = c + 1;
        idx.insert(idx.end(), {a, b, d, a, d, c});
    }
    SDL_SetTextureBlendMode(crt_rt, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(ren, crt_rt, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

} // namespace rf
