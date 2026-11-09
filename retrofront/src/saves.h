// RetroFront - Save Manager + Gadget Manager
#pragma once
#include "gfx.h"
#include <deque>

namespace rf {

struct SlotInfo {
    std::string id;        // "1".."8", "auto", "quick"
    bool exists = false;
    std::string date;
    double playtime = 0;
    int tw = 0, th = 0;
    std::vector<uint32_t> thumb;
    SDL_Texture *tex = nullptr;
};

class SaveManager {
public:
    std::string dir, game_id;
    int max_slots = 8;
    void init(const std::string &pref, const std::string &id);
    std::string path(const std::string &slot) const;
    bool save(GameAdapter &a, const std::string &slot, const std::vector<uint32_t> &frame, int fw, int fh, double playtime, std::string &err);
    bool load(GameAdapter &a, const std::string &slot, double *playtime, std::string &err);
    SlotInfo info(const std::string &slot);           // reads header + thumbnail
    void free_textures();
    std::vector<SlotInfo> slots;                        // cache for the menu
    void refresh(SDL_Renderer *r);
    bool exists(const std::string &slot) const { return platform::file_exists(path(slot)); }
    bool remove(const std::string &slot);

    // rewind ring buffer (compressed snapshots)
    bool rewind_enabled = false;
    int rewind_interval = 3;            // frames between snapshots
    size_t rewind_budget = 96u << 20;   // bytes
    void rewind_push(GameAdapter &a, uint64_t frame);
    bool rewind_step(GameAdapter &a);   // restores the previous snapshot
    void rewind_clear() { ring.clear(); ring_bytes = 0; }
    size_t rewind_seconds(double hz) const { return ring.empty() ? 0 : (size_t)(ring.size() * rewind_interval / hz); }
private:
    std::deque<std::vector<uint8_t>> ring;
    size_t ring_bytes = 0;
    std::vector<uint8_t> scratch;
};

// ------------------------------------------------------------ Gadgets
struct Gadget {
    std::string id, label;
    bool enabled = false;
    double x = 0.03, y = 0.1;     // normalised top-left position on screen
    double scale = 1.0;
    bool from_runtime = false;    // value supplied by the game adapter (verified data source)
};

struct GadgetContext {
    double fps = 0, frame_ms = 0, emu_ms = 0, playtime = 0;
    const InputState *input = nullptr;
    const std::vector<InputAction> *actions = nullptr;
    GameAdapter *adapter = nullptr;
};

class GadgetManager {
public:
    std::vector<Gadget> gadgets;
    bool enabled = true;          // master switch
    void init(const Capabilities &caps, const json &profile);
    void load(const json &j);
    json save() const;
    void draw(Gfx &g, const Theme &t, const GadgetContext &c, int edit_index);
    Gadget *find(const std::string &id);
};

std::string format_time(double seconds);
} // namespace rf
