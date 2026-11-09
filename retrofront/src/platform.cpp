// RetroFront - Platform Integration Layer + Configuration Manager
#include "rf_internal.h"
#include "miniz.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace rf {

std::string fmt(const char *f, ...) {
    char buf[2048];
    va_list ap; va_start(ap, f); vsnprintf(buf, sizeof buf, f, ap); va_end(ap);
    return buf;
}
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
double ease_out(double t) { t = clampd(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }

namespace platform {
static fs::path u8path(const std::string &s) {
#if defined(__cpp_char8_t)
    return fs::path(reinterpret_cast<const char8_t *>(s.c_str()));
#else
    return fs::u8path(s);
#endif
}
static std::string u8str(const fs::path &p) {
#if defined(__cpp_char8_t)
    auto s = p.u8string(); return std::string(s.begin(), s.end());
#else
    return p.u8string();
#endif
}
std::string exe_dir() {
    char *b = SDL_GetBasePath();
    std::string s = b ? b : "./";
    if (b) SDL_free(b);
    return s;
}
std::string pref_dir(const std::string &game_id) {
    char *p = SDL_GetPrefPath("RetroFront", game_id.c_str());
    std::string s = p ? p : "./";
    if (p) SDL_free(p);
    return s;
}
std::string home_dir() {
    const char *h = getenv("HOME");
#ifdef _WIN32
    if (!h) h = getenv("USERPROFILE");
#endif
    return h ? h : ".";
}
bool file_exists(const std::string &p) { std::error_code e; return fs::is_regular_file(u8path(p), e); }
bool is_dir(const std::string &p) { std::error_code e; return fs::is_directory(u8path(p), e); }
bool make_dirs(const std::string &p) { std::error_code e; fs::create_directories(u8path(p), e); return is_dir(p); }
bool read_file(const std::string &p, std::vector<uint8_t> &out) {
    std::ifstream f(u8path(p), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
bool write_file_atomic(const std::string &p, const void *data, size_t n) {
    std::string tmp = p + ".tmp";
    {
        std::ofstream f(u8path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write((const char *)data, (std::streamsize)n);
        if (!f) return false;
    }
    std::error_code e;
    fs::rename(u8path(tmp), u8path(p), e);
    if (e) { fs::remove(u8path(p), e); fs::rename(u8path(tmp), u8path(p), e); }
    return !e;
}
bool is_steam_deck() {
    const char *d = getenv("SteamDeck");
    if (d && *d == '1') return true;
#ifndef _WIN32
    std::ifstream f("/sys/devices/virtual/dmi/id/board_vendor");
    std::string v; std::getline(f, v);
    std::ifstream g("/sys/devices/virtual/dmi/id/product_name");
    std::string n; std::getline(g, n);
    if (v.find("Valve") != std::string::npos && (n.find("Jupiter") != std::string::npos || n.find("Galileo") != std::string::npos)) return true;
#endif
    return false;
}
bool is_gamescope() { return getenv("GAMESCOPE_WAYLAND_DISPLAY") != nullptr || getenv("SteamGamepadUI") != nullptr; }
std::string join(const std::string &a, const std::string &b) { return u8str(u8path(a) / u8path(b)); }
std::string parent(const std::string &p) { return u8str(u8path(p).parent_path()); }
std::string filename(const std::string &p) { return u8str(u8path(p).filename()); }
std::string now_string() {
    time_t t = time(nullptr); struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[64]; strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm); return b;
}
std::string timestamp_compact() {
    time_t t = time(nullptr); struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[64]; strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm); return b;
}
std::vector<DirEntry> list_dir(const std::string &p, const std::vector<std::string> &exts) {
    std::vector<DirEntry> v;
    std::error_code e;
    for (auto it = fs::directory_iterator(u8path(p), fs::directory_options::skip_permission_denied, e);
         !e && it != fs::directory_iterator(); it.increment(e)) {
        DirEntry d;
        d.name = u8str(it->path().filename());
        if (d.name.empty() || d.name[0] == '.') continue;
        d.path = u8str(it->path());
        std::error_code e2;
        d.dir = it->is_directory(e2);
        d.size = d.dir ? 0 : (uint64_t)it->file_size(e2);
        if (!d.dir && !exts.empty()) {
            std::string ext = u8str(it->path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
        }
        v.push_back(d);
    }
    std::sort(v.begin(), v.end(), [](const DirEntry &a, const DirEntry &b) {
        if (a.dir != b.dir) return a.dir;
        std::string x = a.name, y = b.name;
        std::transform(x.begin(), x.end(), x.begin(), ::tolower);
        std::transform(y.begin(), y.end(), y.begin(), ::tolower);
        return x < y;
    });
    return v;
}
std::vector<std::string> roots() {
    std::vector<std::string> r;
#ifdef _WIN32
    DWORD m = GetLogicalDrives();
    for (int i = 0; i < 26; i++) if (m & (1u << i)) r.push_back(fmt("%c:\\", 'A' + i));
    r.insert(r.begin(), join(home_dir(), "Downloads"));
    r.insert(r.begin(), home_dir());
#else
    r.push_back(home_dir());
    r.push_back(join(home_dir(), "Downloads"));
    r.push_back("/run/media");
    r.push_back("/");
#endif
    return r;
}
} // namespace platform

// ------------------------------------------------------------ Config
bool Config::load_profile(const std::string &game_id, const char *embedded_default) {
    profile = json::parse(embedded_default, nullptr, false);
    if (profile.is_discarded()) profile = json::object();
    // an external profile overrides the built-in one (profiles/<id>.json next to the executable)
    std::vector<std::string> cands = {
        platform::join(platform::join(platform::exe_dir(), "profiles"), game_id + ".json"),
        platform::join(platform::pref_dir(game_id), "profile.json")};
    for (auto &c : cands) {
        std::vector<uint8_t> d;
        if (platform::read_file(c, d)) {
            json j = json::parse(d.begin(), d.end(), nullptr, false);
            if (!j.is_discarded() && j.is_object()) { profile.merge_patch(j); }
            else SDL_Log("profile %s is not valid JSON - ignored", c.c_str());
        }
    }
    return true;
}
void Config::load_user(const std::string &path) {
    user_path = path;
    std::vector<uint8_t> d;
    user = json::object();
    if (platform::read_file(path, d)) {
        json j = json::parse(d.begin(), d.end(), nullptr, false);
        if (!j.is_discarded() && j.is_object()) user = j;
    }
}
void Config::save_user() {
    if (!dirty || user_path.empty()) return;
    std::string s = user.dump(2);
    if (platform::write_file_atomic(user_path, s.data(), s.size())) dirty = false;
}
const json *Config::lookup(const json &root, const std::string &key) const {
    const json *cur = &root;
    size_t start = 0;
    while (true) {
        size_t dot = key.find('.', start);
        std::string part = key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!cur->is_object()) return nullptr;
        auto it = cur->find(part);
        if (it == cur->end()) return nullptr;
        cur = &*it;
        if (dot == std::string::npos) return cur;
        start = dot + 1;
    }
}
#define RF_GET(T, check, conv)                                              \
    if (const json *j = lookup(user, key)) if (j->check()) return conv;     \
    if (const json *j = lookup(profile, key)) if (j->check()) return conv;  \
    return def;
bool Config::get_bool(const std::string &key, bool def) const { RF_GET(bool, is_boolean, j->get<bool>()) }
int Config::get_int(const std::string &key, int def) const { RF_GET(int, is_number, (int)j->get<double>()) }
double Config::get_num(const std::string &key, double def) const { RF_GET(double, is_number, j->get<double>()) }
std::string Config::get_str(const std::string &key, const std::string &def) const { RF_GET(std::string, is_string, j->get<std::string>()) }
void Config::set(const std::string &key, const json &v) {
    json *cur = &user;
    size_t start = 0;
    while (true) {
        size_t dot = key.find('.', start);
        std::string part = key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (dot == std::string::npos) { (*cur)[part] = v; break; }
        if (!(*cur)[part].is_object()) (*cur)[part] = json::object();
        cur = &(*cur)[part];
        start = dot + 1;
    }
    dirty = true;
}
bool Config::feature_requested(const std::string &name, bool def) const {
    if (const json *j = lookup(profile, "features." + name)) if (j->is_boolean()) return j->get<bool>();
    return def;
}

// ------------------------------------------------------------ PNG / compression
bool write_png(const std::string &path, const uint32_t *px, int w, int h, int pitch_px) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t c = px[y * pitch_px + x];
        uint8_t *d = &rgb[((size_t)y * w + x) * 3];
        d[0] = (uint8_t)(c >> 16); d[1] = (uint8_t)(c >> 8); d[2] = (uint8_t)c;
    }
    size_t len = 0;
    void *png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &len);
    if (!png) return false;
    bool ok = platform::write_file_atomic(path, png, len);
    mz_free(png);
    return ok;
}
std::vector<uint8_t> zpack(const std::vector<uint8_t> &in, int level) {
    mz_ulong n = mz_compressBound((mz_ulong)in.size());
    std::vector<uint8_t> out(n + 4);
    uint32_t sz = (uint32_t)in.size();
    memcpy(out.data(), &sz, 4);
    if (mz_compress2(out.data() + 4, &n, in.data(), (mz_ulong)in.size(), level) != MZ_OK) return {};
    out.resize(n + 4);
    return out;
}
bool zunpack(const uint8_t *in, size_t n, std::vector<uint8_t> &out) {
    if (n < 4) return false;
    uint32_t sz; memcpy(&sz, in, 4);
    if (sz > 256u * 1024 * 1024) return false;
    out.resize(sz);
    mz_ulong dl = sz;
    if (mz_uncompress(out.data(), &dl, in + 4, (mz_ulong)(n - 4)) != MZ_OK || dl != sz) return false;
    return true;
}
} // namespace rf
