// RetroFront - Audio Manager + Controller Manager
#pragma once
#include "rf_internal.h"
#include <array>
#include <map>

namespace rf {

// ------------------------------------------------------------ Audio
class AudioManager {
public:
    int volume = 100;          // master 0..100
    bool mute = false;
    int latency = 1;           // 0 low (512) 1 normal (1024) 2 safe (2048)
    std::string device;        // empty = system default
    bool open(int rate);
    void close();
    void reopen() { close(); open(rate_); }
    void push(const int16_t *stereo, size_t frames);  // applies volume; drops if the queue is over the cap
    void clear();
    void pause(bool p);
    std::vector<std::string> devices() const;
    int buffer_frames() const { return latency == 0 ? 512 : latency == 2 ? 2048 : 1024; }
    bool ok() const { return dev != 0; }
    std::string driver() const;
private:
    SDL_AudioDeviceID dev = 0;
    int rate_ = 48000;
    std::vector<int16_t> tmp;
};

// ------------------------------------------------------------ Input
struct Binding {               // one physical input
    enum Kind { None, Key, Button, AxisPos, AxisNeg } kind = None;
    int code = 0;              // scancode / SDL_GameControllerButton / SDL_GameControllerAxis
    json to_json() const;
    static Binding from_json(const json &j);
    std::string describe() const;
    bool operator==(const Binding &o) const { return kind == o.kind && code == o.code; }
};

enum class UiKey { None, Up, Down, Left, Right, Accept, Back, PageL, PageR, Menu, Aux };

struct UiEvent { UiKey key = UiKey::None; bool repeat = false; bool from_pad = false; };

class InputManager {
public:
    const std::vector<InputAction> *actions = nullptr;
    std::vector<std::vector<Binding>> bindings;  // per action
    std::vector<int> turbo;                      // per action: 0 off, else period in frames
    double deadzone = 0.30;
    std::string menu_combo = "back+start";       // back+start | l3+r3 | guide | none
    bool pad_style_prompts = false;               // last device used was a gamepad
    std::string last_pad_name;

    void init(const std::vector<InputAction> &acts);
    void defaults_from_profile(const json &profile, const std::string &preset);
    void load(const json &j);
    json save() const;
    std::vector<std::string> presets(const json &profile) const;

    void handle_event(const SDL_Event &e, std::vector<UiEvent> &ui); // device add/remove + UI keys
    void tick_ui(std::vector<UiEvent> &ui);                            // stick/dpad auto-repeat
    void sample(InputState &out, uint64_t frame);                      // gameplay state for this frame
    bool any_bound_held();
    void suppress_until_release() { suppress = true; }
    bool menu_combo_pressed();                                         // edge-triggered
    void begin_capture() { capturing = true; captured = Binding{}; capture_start = SDL_GetTicks(); }
    bool capture_done(Binding &b);                                     // true when captured or cancelled (kind None)
    bool capturing = false;
    int pad_count() const;
    std::string pad_name(int i) const;
    void rumble(int ms);
private:
    SDL_GameController *pads[4] = {};
    bool held(const Binding &b, int player) const;
    void refresh_pads();
    bool suppress = false;
    Binding captured;
    uint32_t capture_start = 0;
    bool combo_latch = false;
    // ui repeat
    UiKey rep_key = UiKey::None; uint32_t rep_next = 0; bool rep_pad = false;
    UiKey stick_dir() const;
};

const char *ui_key_name(UiKey k);
} // namespace rf
