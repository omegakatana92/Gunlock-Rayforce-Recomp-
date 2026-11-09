// RetroFront - Game Adapter API
// The contract between the (game-agnostic) frontend and a native recompilation runtime.
// A runtime implements GameAdapter; the frontend never touches game internals directly.
#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace rf {

enum class PixelFormat { XRGB8888 };

// How the original monitor was mounted (MAME convention: ROT270 = vertical cabinet, rotate 90 deg CCW to view)
enum class Rotation { None = 0, Rot90 = 90, Rot180 = 180, Rot270 = 270 };

struct VideoFrame {
    // Software path: the runtime hands over a CPU pixel buffer each frame.
    const void *pixels = nullptr;
    int width = 0, height = 0, pitch = 0;
    PixelFormat format = PixelFormat::XRGB8888;
    // Aspect ratio of the original *display* (unrotated), e.g. 4:3 for arcade monitors.
    double display_aspect = 4.0 / 3.0;
    // Optional: a runtime that renders itself with the same SDL_Renderer may hand over an SDL_Texture*.
    // (OpenGL/Vulkan runtimes need an interop adapter - see docs/ARCHITECTURE.md.)
    void *sdl_texture = nullptr;
};

enum class ActionRole { Button, Up, Down, Left, Right, Start, Coin, Service, Test };

struct InputAction {
    std::string id;      // stable id used in config files, e.g. "p1.shot"
    std::string label;   // shown in the controller settings
    int player = 0;      // 0-based
    ActionRole role = ActionRole::Button;
    std::string short_label; // for the input display gadget, e.g. "A"
};

struct InputState {
    std::vector<uint8_t> pressed; // one entry per InputAction, same order
};

struct GadgetValue {
    std::string text;   // formatted value
    double value = 0;
    bool valid = false; // false = runtime cannot currently provide it (gadget hides itself)
};

struct GadgetInfo { std::string id, label; };

struct ExtraAction { std::string id, label, description; bool needs_game = false; bool confirm = false; };

struct Capabilities {
    bool can_pause = true;        // run_frame can simply stop being called
    bool can_reset = true;
    bool save_states = false;     // save_state/load_state are exact engine snapshots
    bool native_save = false;     // the game has its own persistent save (EEPROM, memory card...)
    bool attract_preview = false; // the game can run unattended behind the main menu
    bool fast_forward = true;
    std::vector<GadgetInfo> gadgets; // game-data gadgets the runtime can supply (verified sources only)
};

struct ContentInfo {
    std::string description;               // e.g. "MAME 'esprade' romset"
    std::vector<std::string> file_names;   // names to look for, e.g. "esprade.zip"
    std::vector<std::string> extensions;   // shown in the file picker, e.g. ".zip"
};

struct GameInfo {
    std::string id, title, platform, year, developer;
    double refresh_hz = 60.0;
    int audio_rate = 48000;   // interleaved stereo int16
    Rotation rotation = Rotation::None;
};

class GameAdapter {
public:
    virtual ~GameAdapter() = default;
    virtual GameInfo info() const = 0;
    virtual Capabilities capabilities() const = 0;
    virtual ContentInfo content() const = 0;
    virtual const std::vector<InputAction> &input_actions() const = 0;

    virtual bool load_content(const std::string &path, std::string &error) = 0;
    virtual void reset() = 0;
    virtual void run_frame(const InputState &in) = 0;      // emulate exactly one frame
    virtual VideoFrame video() = 0;                         // the frame produced by run_frame
    virtual size_t read_audio(int16_t *out, size_t max_frames) = 0; // drain produced samples

    // Exact engine snapshots, only called between frames.
    virtual bool save_state(std::vector<uint8_t> &) { return false; }
    virtual bool load_state(const uint8_t *, size_t) { return false; }

    // Native persistence (high scores, settings, memory cards). dir is per-game and writable.
    virtual void load_native_save(const std::string &) {}
    virtual void flush_native_save(const std::string &) {}

    virtual bool gadget(const std::string &, GadgetValue &) { return false; }
    virtual std::vector<ExtraAction> extras() { return {}; }
    virtual bool run_extra(const std::string &, std::string &message) { (void)message; return false; }
    // For extras that need a held input (e.g. "hold TEST for a frame") - merged into the next frames.
    virtual void about(std::vector<std::pair<std::string, std::string>> &) {}
};

using AdapterFactory = std::unique_ptr<GameAdapter> (*)();

} // namespace rf
