// Gunlock / RayForce - RetroFront game adapter
// Connects the recompiled Taito F3 68EC020 program + reimplemented board (src/*.c) to the RetroFront frontend.
#include "rf/frontend.h"
#include "core.h"   // C core: has its own extern "C" guard
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <SDL.h>

extern "C" const char gunlock_profile_json[];   // generated from profiles/gunlock.json
extern "C" uint8_t g_textram[];

namespace {

// adapter-level save-state header (the core blob carries its own magic, version and ROM fingerprint)
constexpr uint32_t kStateMagic = 0x474c4b41u;  // "GLKA"
constexpr uint32_t kStateVersion = 1;

class GunlockAdapter : public rf::GameAdapter {
public:
    GunlockAdapter() { build_actions(); }

    rf::GameInfo info() const override {
        rf::GameInfo g;
        g.id = "gunlock"; g.title = "Gunlock"; g.platform = "Taito F3 System";
        g.year = "1993"; g.developer = "Taito";
        g.refresh_hz = 26686000.0 / 4.0 / 432.0 / 262.0;   // 58.94 Hz, the board's video timing
        g.audio_rate = AUDIO_RATE;
        g.rotation = rf::Rotation::Rot90;                   // vertical cabinet (MAME ROT90)
        return g;
    }
    rf::Capabilities capabilities() const override {
        rf::Capabilities c;
        c.can_pause = true;          // the core advances only when run_frame is called
        c.can_reset = true;
        c.save_states = true;        // explicit, versioned full-machine snapshot (both CPUs, video, sound chips) taken between frames
        c.native_save = true;        // 93C46 EEPROM (high scores + operator settings)
        c.attract_preview = true;
        c.fast_forward = true;
        // Read from the game's own on-screen score counters (text layer), validated every frame.
        c.gadgets = {{"score", "1P Score"}, {"hiscore", "High Score"}, {"score2", "2P Score"}};
        return c;
    }
    rf::ContentInfo content() const override {
        rf::ContentInfo ci;
        ci.description = "MAME romset 'gunlock' (Gunlock Ver 2.3O 1994/01/20) as a .zip or a folder";
        ci.file_names = {"gunlock.zip"};
        ci.extensions = {".zip"};
        return ci;
    }
    const std::vector<rf::InputAction> &input_actions() const override { return acts; }

    bool load_content(const std::string &path, std::string &error) override {
        if (!core_load_romset(path.c_str())) { error = core_last_error(); return false; }
        loaded = true;
        return true;
    }
    void reset() override {
        if (!loaded) return;
        core_reset();
        coin_pulse[0] = coin_pulse[1] = 0; prev_coin[0] = prev_coin[1] = false; test_hold = service_hold = 0;
    }
    void run_frame(const rf::InputState &in) override {
        if (!loaded) return;
        uint16_t p[2] = {0, 0};
        for (size_t i = 0; i < acts.size() && i < in.pressed.size(); i++) if (in.pressed[i]) p[acts[i].player] |= bits[i];
        // one short coin pulse per press, so holding the key inserts one coin
        for (int pl = 0; pl < 2; pl++) {
            bool c = (p[pl] & IN_COIN) != 0;
            if (c && !prev_coin[pl]) coin_pulse[pl] = 6;
            prev_coin[pl] = c;
            p[pl] &= ~IN_COIN;
            if (coin_pulse[pl] > 0) { coin_pulse[pl]--; p[pl] |= IN_COIN; }
        }
        if (test_hold > 0) { test_hold--; p[0] |= IN_TEST; }
        if (service_hold > 0) { service_hold--; p[0] |= IN_SERVICE; }
        g_input[0] = p[0]; g_input[1] = p[1];
        core_run_frame(CPU_MODE_RECOMP);
    }
    rf::VideoFrame video() override {
        rf::VideoFrame f;
        f.pixels = core_framebuffer(); f.width = SCREEN_W; f.height = SCREEN_H; f.pitch = SCREEN_W * 4;
        f.display_aspect = 4.0 / 3.0;
        return f;
    }
    size_t read_audio(int16_t *out, size_t max_frames) override { return (size_t)core_audio_samples(out, (int)max_frames); }

    bool save_state(std::vector<uint8_t> &out) override {
        uint8_t *b = nullptr; size_t n = core_state_save(&b);
        if (!n) return false;
        out.resize(32 + n);
        uint32_t hdr[4] = {kStateMagic, kStateVersion, core_rom_fingerprint(), (uint32_t)n};
        memcpy(&out[0], hdr, 16);
        out[16] = (uint8_t)coin_pulse[0]; out[17] = (uint8_t)coin_pulse[1]; out[18] = prev_coin[0]; out[19] = prev_coin[1];
        out[20] = (uint8_t)test_hold; out[21] = (uint8_t)service_hold;
        memcpy(&out[32], b, n);
        free(b);
        return true;
    }
    bool load_state(const uint8_t *d, size_t n) override {
        if (!loaded || n < 32) return false;
        uint32_t hdr[4]; memcpy(hdr, d, 16);
        if (hdr[0] != kStateMagic || hdr[1] != kStateVersion || hdr[2] != core_rom_fingerprint() || hdr[3] != n - 32) return false;
        if (!core_state_load(d + 32, n - 32)) return false;   // validated completely before anything changes
        coin_pulse[0] = d[16]; coin_pulse[1] = d[17]; prev_coin[0] = d[18]; prev_coin[1] = d[19];
        test_hold = d[20]; service_hold = d[21];
        return true;
    }
    void load_native_save(const std::string &dir) override {
        core_eeprom_load((dir + "gunlock.nv").c_str());   /* missing file = blank EEPROM, as a new board */
    }
    void flush_native_save(const std::string &dir) override { if (loaded) core_eeprom_save((dir + "gunlock.nv").c_str()); }

    // The HUD digits are drawn by the game in the text layer (column 38/39 of the 64x64 map).
    // A value is reported only when the "1P"/"HIGH"/"2P" labels and eight decimal digits are present.
    bool gadget(const std::string &id, rf::GadgetValue &v) override {
        v.valid = false;
        if (!loaded) return false;
        auto ch = [](int col, int row) { return (char)g_textram[(row * 64 + col) * 2 + 1]; };
        int row0, lab_row; const char *label;
        if (id == "score") { row0 = 0; lab_row = 3; label = "1P"; }
        else if (id == "hiscore") { row0 = 10; lab_row = 9; label = "HIGH"; }
        else if (id == "score2") { row0 = 20; lab_row = 23; label = "2P"; }
        else return false;
        for (int i = 0; label[i]; i++) if (ch(39, lab_row + i) != label[i]) return true;
        std::string digits; long long val = 0;
        for (int r = 0; r < 8; r++) {
            char c = ch(38, row0 + r);
            if (c < '0' || c > '9') return true;
            digits += c; val = val * 10 + (c - '0');
        }
        v.value = (double)val; v.text = std::to_string(val); v.valid = true;
        return true;
    }

    std::vector<rf::ExtraAction> extras() override {
        return {
            {"service", "Service Menu (Test Mode)", "The game's own operator menu: difficulty, lives, extends, sound test, input test. Exit it from inside the menu.", true, false},
            {"service_coin", "Insert Service Credit", "Adds a credit through the service switch (not counted as a coin).", true, false},
            {"factory", "Reset High Scores & Game Settings", "Erases the board's EEPROM so the game re-initialises it with factory defaults, then restarts.", false, true},
        };
    }
    bool run_extra(const std::string &id, std::string &msg) override {
        if (!loaded) { msg = "Game not loaded"; return false; }
        if (id == "service") { test_hold = 10; msg = "Opening the service menu..."; return true; }
        if (id == "service_coin") { service_hold = 6; msg = "Service credit added"; return true; }
        if (id == "factory") { core_eeprom_factory(); reset(); msg = "High scores and settings reset"; return true; }
        return false;
    }
    void about(std::vector<std::pair<std::string, std::string>> &kv) override {
        char fp[16]; snprintf(fp, sizeof fp, "%08X", core_rom_fingerprint());
        kv.push_back({"Romset", std::string("gunlock (Ver 2.3O 1994/01/20), fingerprint ") + fp});
        kv.push_back({"Program code", "Motorola 68EC020 statically recompiled to native code"});
        kv.push_back({"Translation", std::to_string(rc_stats_native) + " instructions native, " + std::to_string(rc_stats_fallback) + " via interpreter"});
        kv.push_back({"Video", "TC0630FDP: 4 playfields, sprites, pivot/text, line effects, 320x224 @ 58.94 Hz"});
        kv.push_back({"Sound", "68000 + Ensoniq ES5505 (32 voices), 44.1 kHz stereo; ES5510 effects DSP not emulated"});
        kv.push_back({"Saving", "EEPROM high scores; exact save states"});
    }

private:
    std::vector<rf::InputAction> acts;
    std::vector<uint16_t> bits;
    bool loaded = false;
    int coin_pulse[2] = {0, 0}; bool prev_coin[2] = {false, false};
    int test_hold = 0, service_hold = 0;

    void add(const std::string &id, const std::string &label, int pl, rf::ActionRole r, uint16_t bit, const char *sl = "") {
        rf::InputAction a; a.id = id; a.label = label; a.player = pl; a.role = r; a.short_label = sl;
        acts.push_back(a); bits.push_back(bit);
    }
    void build_actions() {
        using R = rf::ActionRole;
        for (int pl = 0; pl < 2; pl++) {
            std::string P = pl ? "p2." : "p1.";
            std::string n = pl ? "P2 " : "";
            auto id = [&](const char *s) { return P + s; };
            auto lab = [&](const char *s) { return n + s; };
            add(id("up"), lab("Up"), pl, R::Up, IN_UP);
            add(id("down"), lab("Down"), pl, R::Down, IN_DOWN);
            add(id("left"), lab("Left"), pl, R::Left, IN_LEFT);
            add(id("right"), lab("Right"), pl, R::Right, IN_RIGHT);
            add(id("a"), lab("Button A (Shot)"), pl, R::Button, IN_B1, "A");
            add(id("b"), lab("Button B (Lock-on Laser)"), pl, R::Button, IN_B2, "B");
            add(id("start"), lab("Start"), pl, R::Start, IN_START);
            add(id("coin"), lab("Insert Coin"), pl, R::Coin, IN_COIN);
        }
        add("service", "Service Credit", 0, R::Service, IN_SERVICE);
        add("test", "Test Switch (service menu)", 0, R::Test, IN_TEST);
    }
};
} // namespace

int main(int argc, char **argv) {
    rf::FrontendOptions opt;
    opt.default_profile_json = gunlock_profile_json;
    opt.version = "0.1.0";
    return rf::run_frontend(std::make_unique<GunlockAdapter>(), opt, argc, argv);
}
