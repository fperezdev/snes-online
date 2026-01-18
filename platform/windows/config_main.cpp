#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>

#include "ConfigDialog.h"
#include "snesonline/AppConfig.h"

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    snesonline::AppConfig cfg;
    const std::string cfgPath = snesonline::AppConfig::defaultConfigPath();
    (void)snesonline::loadConfig(cfgPath, cfg);

    // Ensure default roms dir exists (nice UX even though this tool is for network settings).
    const std::string defaultRoms = snesonline::ensureDefaultRomsDirExists();
    if (cfg.romsDir.empty() && !defaultRoms.empty()) cfg.romsDir = defaultRoms;

    if (!snesonline::showConfigDialog(cfg)) {
        return 0; // cancelled
    }

    if (!snesonline::saveConfig(cfgPath, cfg)) {
        MessageBoxA(nullptr, "Failed to save config file.", "snes-online Config", MB_OK | MB_ICONERROR);
        return 1;
    }

    MessageBoxA(nullptr, "Configuration saved.", "snes-online Config", MB_OK | MB_ICONINFORMATION);
    return 0;
}
#else
int main() { return 0; }
#endif
