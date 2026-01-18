#include "snesonline/AppConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <ShlObj.h>
#include <Windows.h>
#endif

namespace snesonline {

static inline std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string getExecutableDir() {
#if defined(_WIN32)
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(path);
    const auto pos = s.find_last_of("\\/");
    if (pos == std::string::npos) return {};
    return s.substr(0, pos);
#else
    return {};
#endif
}

std::string ensureDefaultRomsDirExists() {
    const std::string exeDir = getExecutableDir();
    if (exeDir.empty()) return {};

#if defined(_WIN32)
    const std::string roms = exeDir + "\\roms";
    CreateDirectoryA(roms.c_str(), nullptr);
    return roms;
#else
    return {};
#endif
}

std::string AppConfig::defaultConfigPath() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::string dir(buf);
        dir += "\\snes-online";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\config.ini";
    }
#endif
    return "config.ini";
}

static bool parseBool(const std::string& v, bool fallback) {
    const std::string t = trim(v);
    if (iequals(t, "1") || iequals(t, "true") || iequals(t, "yes") || iequals(t, "on")) return true;
    if (iequals(t, "0") || iequals(t, "false") || iequals(t, "no") || iequals(t, "off")) return false;
    return fallback;
}

static uint16_t parseU16(const std::string& v, uint16_t fallback) {
    const std::string t = trim(v);
    if (t.empty()) return fallback;
    char* end = nullptr;
    long n = std::strtol(t.c_str(), &end, 10);
    if (!end || end == t.c_str()) return fallback;
    if (n <= 0 || n > 65535) return fallback;
    return static_cast<uint16_t>(n);
}

static uint8_t parsePlayerNum(const std::string& v, uint8_t fallback) {
    const std::string t = trim(v);
    if (t.empty()) return fallback;
    if (t == "1") return 1;
    if (t == "2") return 2;
    return fallback;
}

bool loadConfig(const std::string& path, AppConfig& outCfg) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') continue; // ignore sections

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (iequals(key, "netplayEnabled")) outCfg.netplayEnabled = parseBool(val, outCfg.netplayEnabled);
        else if (iequals(key, "netplayLockstep")) outCfg.netplayLockstep = parseBool(val, outCfg.netplayLockstep);
        else if (iequals(key, "localPlayerNum")) outCfg.localPlayerNum = parsePlayerNum(val, outCfg.localPlayerNum);
        else if (iequals(key, "remoteIp")) outCfg.remoteIp = val;
        else if (iequals(key, "remotePort")) outCfg.remotePort = parseU16(val, outCfg.remotePort);
        else if (iequals(key, "localPort")) outCfg.localPort = parseU16(val, outCfg.localPort);
        else if (iequals(key, "roomServerUrl")) outCfg.roomServerUrl = val;
        else if (iequals(key, "roomApiKey")) outCfg.roomApiKey = val;
        else if (iequals(key, "roomCode")) outCfg.roomCode = val;
        else if (iequals(key, "roomPassword")) outCfg.roomPassword = val;
        else if (iequals(key, "romsDir")) outCfg.romsDir = val;
    }

    return true;
}

bool saveConfig(const std::string& path, const AppConfig& cfg) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;

    f << "# snes-online configuration\n";
    f << "# Saved at: " << path << "\n\n";

    f << "[netplay]\n";
    f << "netplayEnabled=" << (cfg.netplayEnabled ? "true" : "false") << "\n";
    f << "netplayLockstep=" << (cfg.netplayLockstep ? "true" : "false") << "\n";
    f << "localPlayerNum=" << static_cast<int>(cfg.localPlayerNum == 2 ? 2 : 1) << "\n";
    f << "localPort=" << cfg.localPort << "\n\n";

    f << "[room]\n";
    f << "roomServerUrl=" << cfg.roomServerUrl << "\n";
    f << "roomApiKey=" << cfg.roomApiKey << "\n";
    f << "roomCode=" << cfg.roomCode << "\n\n";
    f << "roomPassword=" << cfg.roomPassword << "\n\n";

    f << "[paths]\n";
    f << "romsDir=" << cfg.romsDir << "\n";

    return true;
}

} // namespace snesonline
