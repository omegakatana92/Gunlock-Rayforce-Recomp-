// RetroFront internal shared declarations: platform layer + configuration manager
#pragma once
#include "rf/adapter.h"
#include "json.hpp"
#include <SDL.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace rf {
using json = nlohmann::json;

// ------------------------------------------------------------ Platform Integration Layer
namespace platform {
std::string exe_dir();                 // with trailing separator
std::string pref_dir(const std::string &game_id); // writable per-game dir, trailing separator
std::string home_dir();
bool file_exists(const std::string &p);
bool is_dir(const std::string &p);
bool make_dirs(const std::string &p);
bool read_file(const std::string &p, std::vector<uint8_t> &out);
bool write_file_atomic(const std::string &p, const void *data, size_t n);
bool is_steam_deck();                  // SteamOS / Steam Deck hardware
bool is_gamescope();
std::string join(const std::string &a, const std::string &b);
std::string parent(const std::string &p);
std::string filename(const std::string &p);
std::string now_string();              // "2026-11-07 14:03"
std::string timestamp_compact();       // "20261107-140312"
struct DirEntry { std::string name, path; bool dir; uint64_t size; };
std::vector<DirEntry> list_dir(const std::string &p, const std::vector<std::string> &exts);
std::vector<std::string> roots();      // drives on Windows, "/" + home on Linux
}

// ------------------------------------------------------------ Configuration Manager
// Three layers: built-in defaults < game profile (JSON shipped with the game) < user settings (pref dir)
class Config {
public:
    json profile;   // game profile (read-only at runtime)
    json user;      // persisted user settings
    std::string user_path;

    bool load_profile(const std::string &game_id, const char *embedded_default);
    void load_user(const std::string &path);
    void save_user();
    // typed getters: user setting -> profile default -> fallback
    bool   get_bool(const std::string &key, bool def) const;
    int    get_int(const std::string &key, int def) const;
    double get_num(const std::string &key, double def) const;
    std::string get_str(const std::string &key, const std::string &def) const;
    void set(const std::string &key, const json &v);
    // profile feature request ("features.save_states") - still has to be validated against the adapter
    bool feature_requested(const std::string &name, bool def = true) const;
    bool has_user(const std::string &key) const { return lookup(user, key) != nullptr; }
private:
    const json *lookup(const json &root, const std::string &key) const;
    bool dirty = false;
};

// helpers
std::string fmt(const char *f, ...);
double clampd(double v, double lo, double hi);
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
double ease_out(double t);

// PNG + compression helpers (miniz)
bool write_png(const std::string &path, const uint32_t *xrgb, int w, int h, int pitch_px);
std::vector<uint8_t> zpack(const std::vector<uint8_t> &in, int level);
bool zunpack(const uint8_t *in, size_t n, std::vector<uint8_t> &out);
} // namespace rf
