#pragma once

#include <cstdint>
#include <string>

namespace snesonline {

struct AppConfig {
    // Netplay
    bool netplayEnabled = false;
    // If true, use UDP lockstep netplay (Android-compatible) instead of GGPO.
    bool netplayLockstep = false;
    // Must be 1 or 2.
    uint8_t localPlayerNum = 1;
    std::string remoteIp = "127.0.0.1";
    uint16_t remotePort = 7000;
    uint16_t localPort = 7000;

    // Room server (optional): exchange IP/port by room code.
    // Example URL: http://example.com:8787
    std::string roomServerUrl;
    // Deprecated: was used for admin-only endpoints.
    std::string roomApiKey;
    std::string roomCode;
    // Required for Room mode.
    std::string roomPassword;

    // Paths
    std::string romsDir; // empty -> auto ("<exe_dir>/roms")

    static std::string defaultConfigPath();
};

bool loadConfig(const std::string& path, AppConfig& outCfg);
bool saveConfig(const std::string& path, const AppConfig& cfg);

// Returns the directory where the current executable lives (no trailing slash).
std::string getExecutableDir();

// Creates "<exe_dir>/roms" if it doesn't exist. Returns the full path.
std::string ensureDefaultRomsDirExists();

} // namespace snesonline
