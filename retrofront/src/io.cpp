// RetroFront - Audio Manager + Controller Manager
#include "io.h"
#include <algorithm>
#include <cstring>

namespace rf {

// ============================================================ Audio
bool AudioManager::open(int rate) {
    rate_ = rate;
    SDL_AudioSpec want{}, have{};
    want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = (Uint16)buffer_frames();
    dev = SDL_OpenAudioDevice(device.empty() ? nullptr : device.c_str(), 0, &want, &have, 0);
    if (!dev && !device.empty()) dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // device vanished
    if (dev) SDL_PauseAudioDevice(dev, 0);
    return dev != 0;
}
void AudioManager::close() { if (dev) SDL_CloseAudioDevice(dev); dev = 0; }
void AudioManager::push(const int16_t *s, size_t frames) {
    if (!dev || !frames) return;
    // queue cap: one device buffer + ~3 video frames. We never add buffering beyond that; excess is dropped.
    Uint32 cap = (Uint32)((buffer_frames() + rate_ / 20) * 4);
    if (SDL_GetQueuedAudioSize(dev) > cap) return;
    tmp.resize(frames * 2);
    int v = mute ? 0 : volume;
    if (v == 100) memcpy(tmp.data(), s, frames * 4);
    else for (size_t i = 0; i < frames * 2; i++) tmp[i] = (int16_t)(s[i] * v / 100);
    SDL_QueueAudio(dev, tmp.data(), (Uint32)(frames * 4));
}
void AudioManager::clear() { if (dev) SDL_ClearQueuedAudio(dev); }
void AudioManager::pause(bool p) { if (dev) SDL_PauseAudioDevice(dev, p ? 1 : 0); }
std::vector<std::string> AudioManager::devices() const {
    std::vector<std::string> v;
    int n = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < n; i++) if (const char *s = SDL_GetAudioDeviceName(i, 0)) v.push_back(s);
    return v;
}
std::string AudioManager::driver() const { const char *d = SDL_GetCurrentAudioDriver(); return d ? d : "none"; }

// ============================================================ Bindings
json Binding::to_json() const {
    switch (kind) {
    case Key: return json{{"key", SDL_GetScancodeName((SDL_Scancode)code)}};
    case Button: return json{{"button", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)code)}};
    case AxisPos: return json{{"axis", std::string("+") + SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)code)}};
    case AxisNeg: return json{{"axis", std::string("-") + SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)code)}};
    default: return json();
    }
}
Binding Binding::from_json(const json &j) {
    Binding b;
    if (!j.is_object()) return b;
    if (j.contains("key")) {
        SDL_Scancode sc = SDL_GetScancodeFromName(j["key"].get<std::string>().c_str());
        if (sc != SDL_SCANCODE_UNKNOWN) { b.kind = Key; b.code = sc; }
    } else if (j.contains("button")) {
        SDL_GameControllerButton bt = SDL_GameControllerGetButtonFromString(j["button"].get<std::string>().c_str());
        if (bt != SDL_CONTROLLER_BUTTON_INVALID) { b.kind = Button; b.code = bt; }
    } else if (j.contains("axis")) {
        std::string s = j["axis"].get<std::string>();
        if (s.size() > 1) {
            SDL_GameControllerAxis ax = SDL_GameControllerGetAxisFromString(s.c_str() + 1);
            if (ax != SDL_CONTROLLER_AXIS_INVALID) { b.kind = s[0] == '-' ? AxisNeg : AxisPos; b.code = ax; }
        }
    }
    return b;
}
static std::string pretty_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "Pad A"; case SDL_CONTROLLER_BUTTON_B: return "Pad B";
    case SDL_CONTROLLER_BUTTON_X: return "Pad X"; case SDL_CONTROLLER_BUTTON_Y: return "Pad Y";
    case SDL_CONTROLLER_BUTTON_BACK: return "View/Select"; case SDL_CONTROLLER_BUTTON_START: return "Menu/Start";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide"; case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3"; case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB/L1";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB/R1"; case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Pad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Pad Down"; case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Pad Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Pad Right";
    case SDL_CONTROLLER_BUTTON_PADDLE1: return "Paddle 1"; case SDL_CONTROLLER_BUTTON_PADDLE2: return "Paddle 2";
    case SDL_CONTROLLER_BUTTON_PADDLE3: return "Paddle 3"; case SDL_CONTROLLER_BUTTON_PADDLE4: return "Paddle 4";
    default: { const char *s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b); return s ? s : "?"; }
    }
}
std::string Binding::describe() const {
    switch (kind) {
    case Key: return std::string("Key ") + SDL_GetScancodeName((SDL_Scancode)code);
    case Button: return pretty_button(code);
    case AxisPos: case AxisNeg: {
        switch (code) {
        case SDL_CONTROLLER_AXIS_LEFTX: return kind == AxisPos ? "L-Stick Right" : "L-Stick Left";
        case SDL_CONTROLLER_AXIS_LEFTY: return kind == AxisPos ? "L-Stick Down" : "L-Stick Up";
        case SDL_CONTROLLER_AXIS_RIGHTX: return kind == AxisPos ? "R-Stick Right" : "R-Stick Left";
        case SDL_CONTROLLER_AXIS_RIGHTY: return kind == AxisPos ? "R-Stick Down" : "R-Stick Up";
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "LT/L2";
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "RT/R2";
        }
        return "Axis";
    }
    default: return "-";
    }
}

// ============================================================ InputManager
void InputManager::init(const std::vector<InputAction> &acts) {
    actions = &acts;
    bindings.assign(acts.size(), {});
    turbo.assign(acts.size(), 0);
    refresh_pads();
}
std::vector<std::string> InputManager::presets(const json &profile) const {
    std::vector<std::string> v;
    if (profile.contains("control_presets") && profile["control_presets"].is_object())
        for (auto it = profile["control_presets"].begin(); it != profile["control_presets"].end(); ++it) v.push_back(it.key());
    if (v.empty()) v.push_back("default");
    return v;
}
void InputManager::defaults_from_profile(const json &profile, const std::string &preset) {
    for (auto &b : bindings) b.clear();
    const json *p = nullptr;
    if (profile.contains("control_presets") && profile["control_presets"].contains(preset)) p = &profile["control_presets"][preset];
    else if (profile.contains("control_presets") && profile["control_presets"].contains("default")) p = &profile["control_presets"]["default"];
    for (size_t i = 0; i < actions->size(); i++) {
        const auto &a = (*actions)[i];
        if (p && p->contains(a.id) && (*p)[a.id].is_array()) {
            for (auto &bj : (*p)[a.id]) { Binding b = Binding::from_json(bj); if (b.kind != Binding::None) bindings[i].push_back(b); }
            continue;
        }
        // generic fallbacks by role so any adapter is playable without a profile
        if (a.player == 0) {
            auto K = [&](SDL_Scancode s) { bindings[i].push_back({Binding::Key, s}); };
            auto B = [&](int b) { bindings[i].push_back({Binding::Button, b}); };
            switch (a.role) {
            case ActionRole::Up: K(SDL_SCANCODE_UP); B(SDL_CONTROLLER_BUTTON_DPAD_UP); bindings[i].push_back({Binding::AxisNeg, SDL_CONTROLLER_AXIS_LEFTY}); break;
            case ActionRole::Down: K(SDL_SCANCODE_DOWN); B(SDL_CONTROLLER_BUTTON_DPAD_DOWN); bindings[i].push_back({Binding::AxisPos, SDL_CONTROLLER_AXIS_LEFTY}); break;
            case ActionRole::Left: K(SDL_SCANCODE_LEFT); B(SDL_CONTROLLER_BUTTON_DPAD_LEFT); bindings[i].push_back({Binding::AxisNeg, SDL_CONTROLLER_AXIS_LEFTX}); break;
            case ActionRole::Right: K(SDL_SCANCODE_RIGHT); B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); bindings[i].push_back({Binding::AxisPos, SDL_CONTROLLER_AXIS_LEFTX}); break;
            case ActionRole::Start: K(SDL_SCANCODE_1); B(SDL_CONTROLLER_BUTTON_START); break;
            case ActionRole::Coin: K(SDL_SCANCODE_5); B(SDL_CONTROLLER_BUTTON_BACK); break;
            default: break;
            }
        }
    }
}
void InputManager::load(const json &j) {
    if (!j.is_object()) return;
    deadzone = clampd(j.value("deadzone", deadzone), 0.05, 0.9);
    menu_combo = j.value("menu_combo", menu_combo);
    if (j.contains("bindings") && j["bindings"].is_object())
        for (size_t i = 0; i < actions->size(); i++) {
            auto it = j["bindings"].find((*actions)[i].id);
            if (it == j["bindings"].end() || !it->is_array()) continue;
            bindings[i].clear();
            for (auto &bj : *it) { Binding b = Binding::from_json(bj); if (b.kind != Binding::None) bindings[i].push_back(b); }
        }
    if (j.contains("turbo") && j["turbo"].is_object())
        for (size_t i = 0; i < actions->size(); i++) turbo[i] = j["turbo"].value((*actions)[i].id, 0);
}
json InputManager::save() const {
    json j; j["deadzone"] = deadzone; j["menu_combo"] = menu_combo;
    json b = json::object(), t = json::object();
    for (size_t i = 0; i < actions->size(); i++) {
        json arr = json::array();
        for (auto &x : bindings[i]) arr.push_back(x.to_json());
        b[(*actions)[i].id] = arr;
        if (turbo[i]) t[(*actions)[i].id] = turbo[i];
    }
    j["bindings"] = b; j["turbo"] = t;
    return j;
}
void InputManager::refresh_pads() {
    for (auto &p : pads) if (p) { SDL_GameControllerClose(p); p = nullptr; }
    int n = 0;
    for (int i = 0; i < SDL_NumJoysticks() && n < 4; i++)
        if (SDL_IsGameController(i)) if (SDL_GameController *c = SDL_GameControllerOpen(i)) {
            pads[n++] = c;
        }
    if (n) { const char *nm = SDL_GameControllerName(pads[0]); last_pad_name = nm ? nm : "Controller"; pad_style_prompts = true; }
}
int InputManager::pad_count() const { int n = 0; for (auto p : pads) if (p) n++; return n; }
std::string InputManager::pad_name(int i) const { if (i < 0 || i > 3 || !pads[i]) return "-"; const char *n = SDL_GameControllerName(pads[i]); return n ? n : "Controller"; }
void InputManager::rumble(int ms) { if (pads[0]) SDL_GameControllerRumble(pads[0], 0x4000, 0x4000, ms); }

bool InputManager::held(const Binding &b, int player) const {
    switch (b.kind) {
    case Binding::Key: { const Uint8 *k = SDL_GetKeyboardState(nullptr); return k[b.code] != 0; }
    case Binding::Button: { SDL_GameController *c = pads[player & 3]; return c && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b.code); }
    case Binding::AxisPos: case Binding::AxisNeg: {
        SDL_GameController *c = pads[player & 3];
        if (!c) return false;
        int v = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)b.code);
        int th = (int)(deadzone * 32767);
        if (b.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT || b.code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) th = 8000;
        return b.kind == Binding::AxisPos ? v > th : v < -th;
    }
    default: return false;
    }
}
bool InputManager::any_bound_held() {
    for (size_t i = 0; i < actions->size(); i++)
        for (auto &b : bindings[i]) if (held(b, (*actions)[i].player)) return true;
    return false;
}
void InputManager::sample(InputState &out, uint64_t frame) {
    out.pressed.assign(actions->size(), 0);
    if (capturing) return;
    if (suppress) { if (any_bound_held()) return; suppress = false; }
    for (size_t i = 0; i < actions->size(); i++) {
        bool on = false;
        for (auto &b : bindings[i]) if (held(b, (*actions)[i].player)) { on = true; break; }
        if (on && turbo[i] > 1) on = (frame % turbo[i]) < (uint64_t)(turbo[i] / 2);
        out.pressed[i] = on;
    }
    // block physically impossible opposite directions (some games glitch on them)
    for (size_t i = 0; i < actions->size(); i++) {
        auto r = (*actions)[i].role;
        if (r != ActionRole::Down && r != ActionRole::Right) continue;
        ActionRole opp = r == ActionRole::Down ? ActionRole::Up : ActionRole::Left;
        for (size_t k = 0; k < actions->size(); k++)
            if ((*actions)[k].role == opp && (*actions)[k].player == (*actions)[i].player && out.pressed[k] && out.pressed[i]) out.pressed[i] = 0, out.pressed[k] = 0;
    }
}
bool InputManager::menu_combo_pressed() {
    bool now = false;
    for (auto c : pads) if (c) {
        auto B = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(c, b) != 0; };
        if (menu_combo == "back+start") now |= B(SDL_CONTROLLER_BUTTON_BACK) && B(SDL_CONTROLLER_BUTTON_START);
        else if (menu_combo == "l3+r3") now |= B(SDL_CONTROLLER_BUTTON_LEFTSTICK) && B(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        else if (menu_combo == "guide") now |= B(SDL_CONTROLLER_BUTTON_GUIDE);
        else if (menu_combo == "lb+rb+start") now |= B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) && B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) && B(SDL_CONTROLLER_BUTTON_START);
    }
    bool edge = now && !combo_latch;
    combo_latch = now;
    return edge;
}
bool InputManager::capture_done(Binding &b) {
    if (!capturing) return true;
    if (captured.kind != Binding::None || SDL_GetTicks() - capture_start > 6000) {
        b = captured; capturing = false; suppress = true; return true;
    }
    return false;
}

UiKey InputManager::stick_dir() const {
    for (auto c : pads) if (c) {
        int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        const int th = 20000;
        if (y < -th && abs(y) > abs(x)) return UiKey::Up;
        if (y > th && abs(y) > abs(x)) return UiKey::Down;
        if (x < -th) return UiKey::Left;
        if (x > th) return UiKey::Right;
        auto B = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(c, b) != 0; };
        if (B(SDL_CONTROLLER_BUTTON_DPAD_UP)) return UiKey::Up;
        if (B(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) return UiKey::Down;
        if (B(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) return UiKey::Left;
        if (B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) return UiKey::Right;
    }
    return UiKey::None;
}
void InputManager::tick_ui(std::vector<UiEvent> &ui) {
    if (capturing) return;
    UiKey d = stick_dir();
    uint32_t now = SDL_GetTicks();
    if (d == UiKey::None) { if (rep_pad) rep_key = UiKey::None; rep_pad = false; return; }
    if (d != rep_key || !rep_pad) { rep_key = d; rep_pad = true; rep_next = now + 380; ui.push_back({d, false, true}); pad_style_prompts = true; return; }
    if (now >= rep_next) { rep_next = now + 90; ui.push_back({d, true, true}); }
}
void InputManager::handle_event(const SDL_Event &e, std::vector<UiEvent> &ui) {
    switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED: refresh_pads(); break;
    case SDL_KEYDOWN:
        if (capturing) { if (e.key.keysym.scancode != SDL_SCANCODE_ESCAPE) captured = {Binding::Key, e.key.keysym.scancode}; else capture_start = 0; break; }
        pad_style_prompts = false;
        switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_UP: ui.push_back({UiKey::Up, e.key.repeat != 0}); break;
        case SDL_SCANCODE_DOWN: ui.push_back({UiKey::Down, e.key.repeat != 0}); break;
        case SDL_SCANCODE_LEFT: ui.push_back({UiKey::Left, e.key.repeat != 0}); break;
        case SDL_SCANCODE_RIGHT: ui.push_back({UiKey::Right, e.key.repeat != 0}); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: case SDL_SCANCODE_Z:
            if (!e.key.repeat) ui.push_back({UiKey::Accept}); break;
        case SDL_SCANCODE_ESCAPE: if (!e.key.repeat) ui.push_back({UiKey::Menu}); break;
        case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_X: if (!e.key.repeat) ui.push_back({UiKey::Back}); break;
        case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_Q: ui.push_back({UiKey::PageL}); break;
        case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_E: ui.push_back({UiKey::PageR}); break;
        case SDL_SCANCODE_DELETE: case SDL_SCANCODE_C: if (!e.key.repeat) ui.push_back({UiKey::Aux}); break;
        default: break;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        if (capturing) {
            if (e.cbutton.button != SDL_CONTROLLER_BUTTON_GUIDE) captured = {Binding::Button, e.cbutton.button};
            break;
        }
        pad_style_prompts = true;
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A: ui.push_back({UiKey::Accept, false, true}); break;
        case SDL_CONTROLLER_BUTTON_B: ui.push_back({UiKey::Back, false, true}); break;
        case SDL_CONTROLLER_BUTTON_X: ui.push_back({UiKey::Aux, false, true}); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: ui.push_back({UiKey::PageL, false, true}); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: ui.push_back({UiKey::PageR, false, true}); break;
        case SDL_CONTROLLER_BUTTON_START: ui.push_back({UiKey::Menu, false, true}); break;
        default: break;
        }
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (capturing && abs(e.caxis.value) > 26000) {
            bool trig = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            captured = {trig || e.caxis.value > 0 ? Binding::AxisPos : Binding::AxisNeg, e.caxis.axis};
        }
        break;
    default: break;
    }
}
const char *ui_key_name(UiKey k) {
    switch (k) { case UiKey::Accept: return "Accept"; case UiKey::Back: return "Back"; default: return ""; }
}
} // namespace rf
