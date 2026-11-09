// RetroFront - public entry point
#pragma once
#include "rf/adapter.h"

namespace rf {

struct FrontendOptions {
    const char *default_profile_json = "{}";  // built-in profile; profiles/<game_id>.json overrides it
    const char *version = "1.0";
};

// Runs the complete frontend (menus, settings, saves, gadgets, display/audio/input) around one adapter.
int run_frontend(std::unique_ptr<GameAdapter> adapter, const FrontendOptions &opt, int argc, char **argv);

constexpr const char *kFrontendName = "RetroFront";
constexpr const char *kFrontendVersion = "1.0.0";
} // namespace rf
