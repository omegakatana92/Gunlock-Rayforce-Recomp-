// RetroFront - Save Manager + Gadget Manager
#include "saves.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace rf {

static const char MAGIC[4] = {'R', 'F', 'S', '1'};

void SaveManager::init(const std::string &pref, const std::string &id) {
    game_id = id;
    dir = platform::join(pref, "saves");
    platform::make_dirs(dir);
}
std::string SaveManager::path(const std::string &slot) const { return platform::join(dir, "slot_" + slot + ".rfs"); }

// file: MAGIC | u32 meta_len | meta json | u32 thumb_len | thumb (compressed XRGB) | u32 state_len | state (compressed)
static void put32(std::vector<uint8_t> &v, uint32_t x) { uint8_t b[4]; memcpy(b, &x, 4); v.insert(v.end(), b, b + 4); }
static bool get32(const std::vector<uint8_t> &v, size_t &p, uint32_t &x) { if (p + 4 > v.size()) return false; memcpy(&x, &v[p], 4); p += 4; return true; }

bool SaveManager::save(GameAdapter &a, const std::string &slot, const std::vector<uint32_t> &frame, int fw, int fh, double playtime, std::string &err) {
    std::vector<uint8_t> st;
    if (!a.save_state(st)) { err = "The game could not create a snapshot."; return false; }
    // thumbnail: box-downscale to fit 192x192
    int tw = fw, th = fh, f = 1;
    while (tw / f > 192 || th / f > 192) f++;
    tw = fw / f; th = fh / f;
    std::vector<uint8_t> tb((size_t)tw * th * 4);
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
        uint32_t r = 0, g = 0, b = 0;
        for (int yy = 0; yy < f; yy++) for (int xx = 0; xx < f; xx++) {
            uint32_t c = frame[(size_t)(y * f + yy) * fw + x * f + xx];
            r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255;
        }
        uint32_t n = f * f, c = 0xff000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        memcpy(&tb[((size_t)y * tw + x) * 4], &c, 4);
    }
    json meta = {{"game", game_id}, {"date", platform::now_string()}, {"playtime", playtime}, {"tw", tw}, {"th", th}, {"format", 1}};
    std::string ms = meta.dump();
    auto tc = zpack(tb, 6), sc = zpack(st, 6);
    if (sc.empty()) { err = "Compression failed."; return false; }
    std::vector<uint8_t> out(MAGIC, MAGIC + 4);
    put32(out, (uint32_t)ms.size()); out.insert(out.end(), ms.begin(), ms.end());
    put32(out, (uint32_t)tc.size()); out.insert(out.end(), tc.begin(), tc.end());
    put32(out, (uint32_t)sc.size()); out.insert(out.end(), sc.begin(), sc.end());
    if (!platform::write_file_atomic(path(slot), out.data(), out.size())) { err = "Could not write " + path(slot); return false; }
    return true;
}
static bool parse(const std::vector<uint8_t> &d, json &meta, std::vector<uint8_t> *thumb, std::vector<uint8_t> *state) {
    if (d.size() < 16 || memcmp(d.data(), MAGIC, 4)) return false;
    size_t p = 4; uint32_t n;
    if (!get32(d, p, n) || p + n > d.size()) return false;
    meta = json::parse(d.begin() + p, d.begin() + p + n, nullptr, false); p += n;
    if (meta.is_discarded()) return false;
    if (!get32(d, p, n) || p + n > d.size()) return false;
    if (thumb && !zunpack(&d[p], n, *thumb)) return false;
    p += n;
    if (!get32(d, p, n) || p + n > d.size()) return false;
    if (state && !zunpack(&d[p], n, *state)) return false;
    return true;
}
bool SaveManager::load(GameAdapter &a, const std::string &slot, double *playtime, std::string &err) {
    std::vector<uint8_t> d, st; json meta;
    if (!platform::read_file(path(slot), d)) { err = "No save in this slot."; return false; }
    if (!parse(d, meta, nullptr, &st)) { err = "Save file is damaged."; return false; }
    if (meta.value("game", std::string()) != game_id) { err = "Save belongs to a different game."; return false; }
    if (!a.load_state(st.data(), st.size())) { err = "The game rejected this save (different version?)."; return false; }
    if (playtime) *playtime = meta.value("playtime", 0.0);
    return true;
}
SlotInfo SaveManager::info(const std::string &slot) {
    SlotInfo s; s.id = slot;
    std::vector<uint8_t> d, tb; json meta;
    if (!platform::read_file(path(slot), d) || !parse(d, meta, &tb, nullptr)) return s;
    s.exists = true; s.date = meta.value("date", std::string("?")); s.playtime = meta.value("playtime", 0.0);
    s.tw = meta.value("tw", 0); s.th = meta.value("th", 0);
    if ((size_t)s.tw * s.th * 4 == tb.size()) { s.thumb.resize((size_t)s.tw * s.th); memcpy(s.thumb.data(), tb.data(), tb.size()); }
    return s;
}
void SaveManager::free_textures() { for (auto &s : slots) if (s.tex) { SDL_DestroyTexture(s.tex); s.tex = nullptr; } }
void SaveManager::refresh(SDL_Renderer *r) {
    free_textures();
    slots.clear();
    slots.push_back(info("auto"));
    slots.push_back(info("quick"));
    for (int i = 1; i <= max_slots; i++) slots.push_back(info(std::to_string(i)));
    for (auto &s : slots) if (!s.thumb.empty()) {
        s.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, s.tw, s.th);
        SDL_UpdateTexture(s.tex, nullptr, s.thumb.data(), s.tw * 4);
        SDL_SetTextureScaleMode(s.tex, SDL_ScaleModeLinear);
    }
}
bool SaveManager::remove(const std::string &slot) { return std::remove(path(slot).c_str()) == 0; }

void SaveManager::rewind_push(GameAdapter &a, uint64_t frame) {
    if (!rewind_enabled || frame % rewind_interval) return;
    scratch.clear();
    if (!a.save_state(scratch)) return;
    auto c = zpack(scratch, 1);
    if (c.empty()) return;
    ring_bytes += c.size();
    ring.push_back(std::move(c));
    while (ring_bytes > rewind_budget && !ring.empty()) { ring_bytes -= ring.front().size(); ring.pop_front(); }
}
bool SaveManager::rewind_step(GameAdapter &a) {
    if (ring.empty()) return false;
    auto c = std::move(ring.back()); ring.pop_back(); ring_bytes -= c.size();
    if (!zunpack(c.data(), c.size(), scratch)) return false;
    return a.load_state(scratch.data(), scratch.size());
}

// ============================================================ Gadgets
std::string format_time(double s) {
    int t = (int)s;
    return t >= 3600 ? fmt("%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60) : fmt("%02d:%02d", t / 60, t % 60);
}
void GadgetManager::init(const Capabilities &caps, const json &profile) {
    gadgets.clear();
    // frontend-measured gadgets (always truthful)
    gadgets.push_back({"input", "INPUT", false, 0.02, 0.55, 1.0});
    gadgets.push_back({"fps", "FPS", false, 0.02, 0.06, 1.0});
    gadgets.push_back({"frametime", "FRAME TIME", false, 0.02, 0.17, 1.0});
    gadgets.push_back({"playtime", "PLAY TIME", false, 0.02, 0.30, 1.0});
    // runtime-provided gadgets: only those the adapter declares (verified data sources)
    double y = 0.06;
    for (auto &gi : caps.gadgets) { gadgets.push_back({gi.id, gi.label, false, 0.80, y, 1.0, true}); y += 0.11; }
    if (profile.contains("gadgets") && profile["gadgets"].is_object()) load(profile["gadgets"]);
}
Gadget *GadgetManager::find(const std::string &id) { for (auto &g : gadgets) if (g.id == id) return &g; return nullptr; }
void GadgetManager::load(const json &j) {
    if (!j.is_object()) return;
    enabled = j.value("enabled", enabled);
    if (!j.contains("items") || !j["items"].is_object()) return;
    for (auto &g : gadgets) {
        auto it = j["items"].find(g.id);
        if (it == j["items"].end() || !it->is_object()) continue;
        g.enabled = it->value("on", g.enabled);
        g.x = clampd(it->value("x", g.x), 0, 0.98); g.y = clampd(it->value("y", g.y), 0, 0.98);
        g.scale = clampd(it->value("scale", g.scale), 0.5, 2.5);
    }
}
json GadgetManager::save() const {
    json items = json::object();
    for (auto &g : gadgets) items[g.id] = {{"on", g.enabled}, {"x", g.x}, {"y", g.y}, {"scale", g.scale}};
    return json{{"enabled", enabled}, {"items", items}};
}
void GadgetManager::draw(Gfx &g, const Theme &t, const GadgetContext &c, int edit) {
    if (!enabled && edit < 0) return;
    for (size_t i = 0; i < gadgets.size(); i++) {
        Gadget &gd = gadgets[i];
        bool editing = (int)i == edit;
        if (!gd.enabled && !editing) continue;
        std::string val; bool have = true;
        if (gd.id == "fps") val = fmt("%.1f", c.fps);
        else if (gd.id == "frametime") val = fmt("%.2f ms", c.emu_ms);
        else if (gd.id == "playtime") val = format_time(c.playtime);
        else if (gd.id == "input") val = "";
        else if (gd.from_runtime && c.adapter) {
            GadgetValue v;
            have = c.adapter->gadget(gd.id, v) && v.valid;
            val = have ? v.text : "--";
        }
        if (!have && !editing) continue;
        double sc = g.s * gd.scale;
        double x = gd.x * g.w, y = gd.y * g.h;
        double pw = 200 * sc, ph = gd.id == "input" ? 118 * sc : 66 * sc;
        g.rounded(x, y, pw, ph, 8 * sc, with_alpha(t.panel, 0.82));
        g.rect(x, y + 8 * sc, 3 * sc, ph - 16 * sc, t.accent);
        if (editing) g.rect_outline(x - 2, y - 2, pw + 4, ph + 4, t.accent2, 2);
        g.text(FontId::Display, 9 * sc, x + 14 * sc, y + 10 * sc, gd.label, t.accent, 0, 1);
        if (gd.id == "input" && c.input && c.actions) {
            // P1 stick + buttons, lit when pressed (shows what the game receives, after turbo/remap)
            bool u = 0, d = 0, l = 0, r = 0; std::vector<std::pair<std::string, bool>> btn;
            for (size_t k = 0; k < c.actions->size() && k < c.input->pressed.size(); k++) {
                const auto &a = (*c.actions)[k];
                if (a.player != 0) continue;
                bool p = c.input->pressed[k];
                switch (a.role) {
                case ActionRole::Up: u = p; break; case ActionRole::Down: d = p; break;
                case ActionRole::Left: l = p; break; case ActionRole::Right: r = p; break;
                case ActionRole::Button: btn.push_back({a.short_label.empty() ? a.label.substr(0, 1) : a.short_label, p}); break;
                default: break;
                }
            }
            double cx = x + 44 * sc, cy = y + 72 * sc, R = 26 * sc;
            g.circle(cx, cy, R, with_alpha(t.bg1, 0.9));
            double dx = (r - l) * R * 0.55, dy = (d - u) * R * 0.55;
            g.circle(cx + dx, cy + dy, R * 0.42, (u || d || l || r) ? t.accent : t.text_dim);
            double bx = x + 92 * sc;
            for (auto &b : btn) {
                g.circle(bx + 12 * sc, cy, 12 * sc, b.second ? t.accent2 : with_alpha(t.text_dim, 0.6));
                g.text(FontId::Body, 14 * sc, bx + 12 * sc, cy - 9 * sc, b.first, b.second ? t.highlight_text : t.text, 1);
                bx += 30 * sc;
            }
        } else {
            g.text_fit(FontId::Body, 30 * sc, x + 14 * sc, y + 24 * sc, pw - 24 * sc, val, t.text);
        }
    }
}
} // namespace rf
