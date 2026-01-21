#define SDL_MAIN_HANDLED 1
#include <SDL.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <winhttp.h>
#include <cctype>
#include <ws2tcpip.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "snesonline/AppConfig.h"
#include "snesonline/EmulatorEngine.h"
#include "snesonline/InputBits.h"
#include "snesonline/InputMapping.h"
#include "snesonline/LockstepSession.h"
#include "snesonline/NetplaySession.h"
#include "snesonline/StunClient.h"

#include "ConfigDialog.h"

namespace {

static std::string lockstepDebugLogPath() {
#if defined(_WIN32)
    const std::string cfgPath = snesonline::AppConfig::defaultConfigPath();
    const size_t slash = cfgPath.find_last_of("\\/");
    if (slash == std::string::npos) return "lockstep_debug.log";
    return cfgPath.substr(0, slash) + "\\lockstep_debug.log";
#else
    return "lockstep_debug.log";
#endif
}

#if defined(_WIN32)
static void showMessageBox(const char* title, const char* text, unsigned flags = 0) {
    MessageBoxA(nullptr, text ? text : "", title ? title : "snes-online", MB_OK | flags);
}

static std::string getExecutablePath() {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::string(path);
}

static DWORD runProcessAndWait(const std::string& commandLine) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::string cmd = commandLine;
    const BOOL ok = CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    if (!ok) return 0xFFFFFFFFu;

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode;
}

static bool isLoopbackIp(const char* ip) {
    if (!ip || !ip[0]) return false;
    return std::strcmp(ip, "127.0.0.1") == 0 || std::strcmp(ip, "127.0.1.1") == 0;
}

static std::string firewallRuleDisplayName(uint16_t localPort) {
    char buf[128] = {};
    std::snprintf(buf, sizeof(buf), "snes-online (netplay UDP %u)", static_cast<unsigned>(localPort));
    return std::string(buf);
}

static bool firewallRuleExistsForNetplay(uint16_t localPort) {
    // Use PowerShell to check by DisplayName and return exit code.
    // 0: exists, 1: missing.
    const std::string displayName = firewallRuleDisplayName(localPort);
    char cmd[512] = {};
    std::snprintf(
        cmd,
        sizeof(cmd),
        "powershell.exe -NoProfile -Command \"if (Get-NetFirewallRule -DisplayName '%s' -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }\"",
        displayName.c_str());
    const DWORD code = runProcessAndWait(cmd);
    return code == 0;
}

static void promptAndAddFirewallRuleIfNeeded(bool netplayEnabled, const char* remoteIp, uint16_t localPort) {
    if (!netplayEnabled) return;
    // For same-PC loopback testing, firewall isn't relevant.
    if (isLoopbackIp(remoteIp)) return;
    if (localPort == 0) return;
    if (firewallRuleExistsForNetplay(localPort)) return;

    const std::string displayName = firewallRuleDisplayName(localPort);

    const int r = MessageBoxA(
        nullptr,
        "Enable netplay firewall access?\n\n"
        "To allow other players to connect, Windows Firewall must allow inbound UDP for snes-online.\n\n"
        "If you click Yes, snes-online will request Administrator permission (UAC) to add a Windows Firewall rule:\n"
        "  DisplayName: snes-online (netplay UDP <port>)\n"
        "  Program: snesonline_win.exe\n"
        "  Direction: Inbound\n"
        "  Protocol: UDP\n"
        "  LocalPort: your configured local port\n\n"
        "Add the rule now?",
        "snes-online",
        MB_YESNO | MB_ICONQUESTION);

    if (r != IDYES) return;

    const std::string exePath = getExecutablePath();
    if (exePath.empty()) return;

    // Use an elevated PowerShell to add (or replace) the rule.
    // -ErrorAction SilentlyContinue avoids noise if the rule already exists.
    // Note: Quote carefully; exePath may contain spaces.
    char args[2048] = {};
    std::snprintf(
        args,
        sizeof(args),
        "-NoProfile -ExecutionPolicy Bypass -Command "
        "\""
        "Remove-NetFirewallRule -DisplayName '%s' -ErrorAction SilentlyContinue; "
        "New-NetFirewallRule -DisplayName '%s' -Direction Inbound -Action Allow -Enabled True -Profile Any -Protocol UDP -LocalPort %u -Program '%s' | Out-Null; "
        "\"",
        displayName.c_str(),
        displayName.c_str(),
        static_cast<unsigned>(localPort),
        exePath.c_str());

    ShellExecuteA(nullptr, "runas", "powershell.exe", args, nullptr, SW_HIDE);
}

static bool fileExists(const std::string& path) {
    const DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static std::string openFileDialog(const char* title, const char* filter) {
    char fileBuf[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(fileBuf));
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn) == TRUE) {
        return std::string(fileBuf);
    }
    return {};
}

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out;
    out.resize(static_cast<size_t>(needed));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out;
    out.resize(static_cast<size_t>(needed));
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

static bool parseHttpUrlHostPort(const std::string& baseUrlUtf8, std::string& outHostUtf8, uint16_t& outPort) {
    outHostUtf8.clear();
    outPort = 0;

    const std::wstring urlW = utf8ToWide(baseUrlUtf8);
    if (urlW.empty()) return false;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(sizeof(hostBuf) / sizeof(hostBuf[0]));
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>(sizeof(pathBuf) / sizeof(pathBuf[0]));
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(urlW.c_str(), static_cast<DWORD>(urlW.size()), 0, &uc)) return false;

    if (uc.dwHostNameLength == 0) return false;
    std::wstring hostW(uc.lpszHostName, uc.dwHostNameLength);
    outHostUtf8 = wideToUtf8(hostW);
    outPort = static_cast<uint16_t>(uc.nPort);
    return !outHostUtf8.empty() && outPort != 0;
}

static std::string trimAscii(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

static std::string normalizeRoomCode(const std::string& s) {
    const std::string t = trimAscii(s);
    std::string out;
    out.reserve(t.size());
    for (char ch : t) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (ok) out.push_back(c);
    }
    return out;
}

static std::string jsonExtractString(const std::string& json, const char* keyQuoted) {
    if (!keyQuoted) return {};
    const std::string key(keyQuoted);
    const size_t k = json.find(key);
    if (k == std::string::npos) return {};
    const size_t c = json.find(':', k);
    if (c == std::string::npos) return {};
    const size_t q1 = json.find('"', c + 1);
    if (q1 == std::string::npos) return {};
    const size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static std::string jsonEscapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char ch : s) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else out.push_back(ch);
    }
    return out;
}

static bool jsonExtractBool(const std::string& json, const char* keyQuoted, bool fallback) {
    if (!keyQuoted) return fallback;
    const std::string key(keyQuoted);
    const size_t k = json.find(key);
    if (k == std::string::npos) return fallback;
    const size_t c = json.find(':', k);
    if (c == std::string::npos) return fallback;
    size_t i = c + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
    if (json.compare(i, 4, "true") == 0) return true;
    if (json.compare(i, 5, "false") == 0) return false;
    return fallback;
}

static int jsonExtractInt(const std::string& json, const char* keyQuoted, int fallback) {
    if (!keyQuoted) return fallback;
    const std::string key(keyQuoted);
    const size_t k = json.find(key);
    if (k == std::string::npos) return fallback;
    const size_t c = json.find(':', k);
    if (c == std::string::npos) return fallback;
    size_t i = c + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
    size_t j = i;
    while (j < json.size() && (json[j] >= '0' && json[j] <= '9')) ++j;
    if (j <= i) return fallback;
    try {
        return std::stoi(json.substr(i, j - i));
    } catch (...) {
        return fallback;
    }
}

static bool winHttpGetRoom(const std::string& baseUrlUtf8, const std::string& apiKeyUtf8, const std::string& roomCodeUtf8,
                           std::string& outIp, uint16_t& outPort) {
    outIp.clear();
    outPort = 0;

    std::string base = trimAscii(baseUrlUtf8);
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string code = normalizeRoomCode(roomCodeUtf8);
    if (code.size() < 8 || code.size() > 12) return false;

    const std::string urlUtf8 = base + "/rooms/" + code;
    const std::wstring urlW = utf8ToWide(urlUtf8);
    if (urlW.empty()) return false;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(sizeof(hostBuf) / sizeof(hostBuf[0]));
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>(sizeof(pathBuf) / sizeof(pathBuf[0]));
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(urlW.c_str(), static_cast<DWORD>(urlW.size()), 0, &uc)) return false;

    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    const INTERNET_PORT port = uc.nPort;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (path.empty()) path = L"/";

    HINTERNET hSession = WinHttpOpen(L"snes-online/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 3000, 3000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring headersW = L"Accept: application/json\r\n";
    if (!apiKeyUtf8.empty()) {
        headersW += L"X-API-Key: ";
        headersW += utf8ToWide(apiKeyUtf8);
        headersW += L"\r\n";
    }

    BOOL ok = WinHttpSendRequest(hRequest, headersW.c_str(), static_cast<DWORD>(headersW.size()),
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::string resp;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
        std::string chunk;
        chunk.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0) break;
        chunk.resize(read);
        resp += chunk;
        if (resp.size() > 1024 * 64) break;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    outIp = jsonExtractString(resp, "\"ip\"");
    const int p = jsonExtractInt(resp, "\"port\"", 0);
    if (p <= 0 || p > 65535) return false;
    outPort = static_cast<uint16_t>(p);
    return !outIp.empty();
}

struct RoomConnectResp {
    bool ok = false;
    int role = 0;
    bool waiting = false;
    std::string ip;
    std::string localIp;
    uint16_t port = 0;
    std::string creatorToken;
    std::string error;
};

static bool isPrivateLanIpv4HostOrder(uint32_t ipHostOrder) {
    const uint8_t a = static_cast<uint8_t>((ipHostOrder >> 24) & 0xFF);
    const uint8_t b = static_cast<uint8_t>((ipHostOrder >> 16) & 0xFF);
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true;
    return false;
}

static std::string localLanIpv4BestEffort() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return std::string();

    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0) {
        WSACleanup();
        return std::string();
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
        WSACleanup();
        return std::string();
    }

    std::string out;
    for (addrinfo* it = res; it; it = it->ai_next) {
        if (!it->ai_addr || it->ai_addrlen < sizeof(sockaddr_in)) continue;
        const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
        const uint32_t ipHost = ntohl(sin->sin_addr.s_addr);
        const uint8_t a = static_cast<uint8_t>((ipHost >> 24) & 0xFF);
        if (a == 127) continue;
        if (!isPrivateLanIpv4HostOrder(ipHost)) continue;
        char buf[64] = {};
        if (!InetNtopA(AF_INET, (PVOID)&sin->sin_addr, buf, sizeof(buf))) continue;
        out = buf;
        break;
    }

    freeaddrinfo(res);
    WSACleanup();
    return out;
}

static RoomConnectResp winHttpPostRoomConnect(const std::string& baseUrlUtf8, const std::string& roomCodeUtf8, const std::string& passwordUtf8,
                                             uint16_t portOpt, const std::string& creatorTokenOpt = std::string()) {
    RoomConnectResp out;

    std::string base = trimAscii(baseUrlUtf8);
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string code = normalizeRoomCode(roomCodeUtf8);
    if (code.size() < 8 || code.size() > 12) {
        out.error = "invalid_code";
        return out;
    }

    const std::string urlUtf8 = base + "/rooms/connect";
    const std::wstring urlW = utf8ToWide(urlUtf8);
    if (urlW.empty()) {
        out.error = "bad_url";
        return out;
    }

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(sizeof(hostBuf) / sizeof(hostBuf[0]));
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>(sizeof(pathBuf) / sizeof(pathBuf[0]));
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(urlW.c_str(), static_cast<DWORD>(urlW.size()), 0, &uc)) {
        out.error = "bad_url";
        return out;
    }

    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    const INTERNET_PORT port = uc.nPort;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (path.empty()) path = L"/";

    HINTERNET hSession = WinHttpOpen(L"snes-online/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        out.error = "winhttp_open_failed";
        return out;
    }
    WinHttpSetTimeouts(hSession, 4000, 4000, 6000, 6000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        out.error = "winhttp_connect_failed";
        return out;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        out.error = "winhttp_request_failed";
        return out;
    }

    const std::string pwEsc = jsonEscapeString(passwordUtf8);
    const std::string localIpOpt = localLanIpv4BestEffort();
    std::string body = std::string("{\"code\":\"") + code + "\",\"password\":\"" + pwEsc + "\"";
    if (portOpt != 0) {
        body += ",\"port\":" + std::to_string(static_cast<unsigned>(portOpt));
    }
    if (!creatorTokenOpt.empty()) {
        body += ",\"creatorToken\":\"" + jsonEscapeString(creatorTokenOpt) + "\"";
    }
    if (!localIpOpt.empty()) {
        body += ",\"localIp\":\"" + jsonEscapeString(localIpOpt) + "\"";
    }
    body += "}";

    std::wstring headersW = L"Accept: application/json\r\nContent-Type: application/json\r\n";

    BOOL ok = WinHttpSendRequest(hRequest, headersW.c_str(), static_cast<DWORD>(headersW.size()),
                                (LPVOID)body.data(), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        out.error = "winhttp_send_failed";
        return out;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    std::string resp;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
        std::string chunk;
        chunk.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0) break;
        chunk.resize(read);
        resp += chunk;
        if (resp.size() > 1024 * 64) break;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (resp.empty()) {
        out.error = "empty_response";
        return out;
    }

    const bool okField = jsonExtractBool(resp, "\"ok\"", false);
    if (!okField || status != 200) {
        out.error = jsonExtractString(resp, "\"error\"");
        if (out.error.empty()) out.error = "http_" + std::to_string(status);
        return out;
    }

    out.ok = true;
    out.role = jsonExtractInt(resp, "\"role\"", 0);
    out.waiting = jsonExtractBool(resp, "\"waiting\"", false);
    out.ip = jsonExtractString(resp, "\"ip\"");
    out.localIp = jsonExtractString(resp, "\"localIp\"");
    out.creatorToken = jsonExtractString(resp, "\"creatorToken\"");
    const int p = jsonExtractInt(resp, "\"port\"", 0);
    if (p > 0 && p <= 65535) out.port = static_cast<uint16_t>(p);
    return out;
}

static bool udpWhoamiPublicPort(const std::string& baseUrlUtf8, uint16_t localPort, uint16_t& outPublicPort) {
    outPublicPort = 0;
    if (localPort == 0) return false;

    std::string host;
    uint16_t port = 0;
    if (!parseHttpUrlHostPort(baseUrlUtf8, host, port)) return false;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(static_cast<unsigned>(port));
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        WSACleanup();
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        WSACleanup();
        return false;
    }

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(localPort);
    if (bind(s, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        closesocket(s);
        freeaddrinfo(res);
        WSACleanup();
        return false;
    }

    const DWORD timeoutMs = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    const char* msg = "SNO_WHOAMI1\n";
    const int msgLen = static_cast<int>(std::strlen(msg));
    const int sent = sendto(s, msg, msgLen, 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    if (sent != msgLen) {
        closesocket(s);
        WSACleanup();
        return false;
    }

    char buf[256] = {};
    sockaddr_in from{};
    int fromLen = sizeof(from);
    const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    closesocket(s);
    WSACleanup();
    if (n <= 0) return false;
    buf[n] = 0;

    char ip[64] = {};
    int p = 0;
    if (std::sscanf(buf, "SNO_SELF1 %63s %d", ip, &p) != 2) return false;
    if (p < 1 || p > 65535) return false;
    outPublicPort = static_cast<uint16_t>(p);
    return true;
}

static bool roomConnectAtStart(const snesonline::AppConfig& cfg, uint16_t localPort, uint8_t& outRole, std::string& outHostIp, uint16_t& outHostPort,
                               std::string& outError) {
    outRole = 0;
    outHostIp.clear();
    outHostPort = 0;
    outError.clear();

    constexpr const char* kDefaultRoomServerUrl = "https://snes-online-1hgm.onrender.com";
    const std::string url = cfg.roomServerUrl.empty() ? std::string(kDefaultRoomServerUrl) : cfg.roomServerUrl;
    const std::string code = normalizeRoomCode(cfg.roomCode);
    const std::string password = trimAscii(cfg.roomPassword);
    if (code.empty()) {
        outError = "room_code_required";
        return false;
    }
    if (password.empty()) {
        outError = "password_required";
        return false;
    }

    RoomConnectResp r0 = winHttpPostRoomConnect(url, code, password, 0);
    if (!r0.ok) {
        outError = r0.error.empty() ? "connect_failed" : r0.error;
        return false;
    }

    if (r0.role == 1) {
        const std::string creatorToken = r0.creatorToken;
        // Finalize with a discovered public UDP port.
        RoomConnectResp rFinal = r0;
        if (r0.port == 0 || r0.waiting) {
            uint16_t publicPort = 0;

            // Preferred: STUN (public servers). Works even if the room server has no UDP.
            snesonline::StunMappedAddress mapped;
            if (snesonline::stunDiscoverMappedAddressDefault(localPort, mapped)) {
                publicPort = mapped.port;
            }

            // Fallback: legacy UDP WHOAMI to the room server.
            if (publicPort == 0) {
                if (!udpWhoamiPublicPort(url, localPort, publicPort)) {
                    outError = "stun_failed";
                    return false;
                }
            }

            RoomConnectResp r1 = winHttpPostRoomConnect(url, code, password, publicPort, creatorToken);
            if (!r1.ok) {
                outError = r1.error.empty() ? "finalize_failed" : r1.error;
                return false;
            }
            rFinal = r1;
        }

        outRole = 1;
        outHostIp = rFinal.localIp.empty() ? rFinal.ip : rFinal.localIp;
        outHostPort = rFinal.port;
        return true;
    }

    if (r0.role == 2) {
        RoomConnectResp r = r0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while ((r.waiting || r.port == 0) && std::chrono::steady_clock::now() < deadline) {
            Sleep(250);
            r = winHttpPostRoomConnect(url, code, password, 0);
            if (!r.ok) {
                outError = r.error.empty() ? "poll_failed" : r.error;
                return false;
            }
        }
        const std::string chosenIp = !r.localIp.empty() ? r.localIp : r.ip;
        if (chosenIp.empty() || r.port == 0) {
            outError = "host_not_ready";
            return false;
        }
        outRole = 2;
        outHostIp = chosenIp;
        outHostPort = r.port;
        return true;
    }

    outError = "role_not_assigned";
    return false;
}
#endif

struct AudioRing {
    static constexpr uint32_t kCapacityFrames = 48000 * 2; // ~2s @48k
    alignas(64) int16_t samples[kCapacityFrames * 2]; // interleaved stereo
    std::atomic<uint32_t> writeFrame{0};
    std::atomic<uint32_t> readFrame{0};
    std::atomic<uint32_t> maxBufferedFrames{kCapacityFrames / 4}; // ~500ms @48k by default

    void setMaxBufferedFrames(uint32_t frames) noexcept {
        if (frames < 256) frames = 256;
        if (frames > kCapacityFrames) frames = kCapacityFrames;
        maxBufferedFrames.store(frames, std::memory_order_relaxed);
    }

    void push(const int16_t* stereoFrames, uint32_t frameCount) noexcept {
        // Drop-on-overflow policy.
        uint32_t w = writeFrame.load(std::memory_order_relaxed);
        uint32_t r = readFrame.load(std::memory_order_acquire);
        uint32_t used = w - r;
        uint32_t freeFrames = kCapacityFrames - (used > kCapacityFrames ? kCapacityFrames : used);
        if (frameCount > freeFrames) {
            // Drop oldest by advancing read pointer.
            uint32_t drop = frameCount - freeFrames;
            readFrame.store(r + drop, std::memory_order_release);
        }

        w = writeFrame.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frameCount; ++i) {
            const uint32_t idx = (w + i) % kCapacityFrames;
            samples[idx * 2 + 0] = stereoFrames[i * 2 + 0];
            samples[idx * 2 + 1] = stereoFrames[i * 2 + 1];
        }
        const uint32_t newW = w + frameCount;
        writeFrame.store(newW, std::memory_order_release);

        // Clamp buffered audio to avoid latency drifting upward over time.
        // If we get ahead, drop the oldest samples so A/V stays tight.
        const uint32_t maxFrames = maxBufferedFrames.load(std::memory_order_relaxed);
        if (maxFrames > 0) {
            const uint32_t curR = readFrame.load(std::memory_order_acquire);
            uint32_t usedNow = newW - curR;
            if (usedNow > kCapacityFrames) usedNow = kCapacityFrames;
            if (usedNow > maxFrames) {
                const uint32_t drop = usedNow - maxFrames;
                readFrame.store(curR + drop, std::memory_order_release);
            }
        }
    }

    uint32_t pop(int16_t* outStereoFrames, uint32_t frameCount) noexcept {
        uint32_t r = readFrame.load(std::memory_order_relaxed);
        uint32_t w = writeFrame.load(std::memory_order_acquire);
        uint32_t avail = w - r;
        if (avail > kCapacityFrames) avail = kCapacityFrames;
        const uint32_t n = (frameCount < avail) ? frameCount : avail;

        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t idx = (r + i) % kCapacityFrames;
            outStereoFrames[i * 2 + 0] = samples[idx * 2 + 0];
            outStereoFrames[i * 2 + 1] = samples[idx * 2 + 1];
        }

        readFrame.store(r + n, std::memory_order_release);
        return n;
    }

    uint32_t bufferedFrames() const noexcept {
        const uint32_t r = readFrame.load(std::memory_order_relaxed);
        const uint32_t w = writeFrame.load(std::memory_order_acquire);
        uint32_t avail = w - r;
        if (avail > kCapacityFrames) avail = kCapacityFrames;
        return avail;
    }
};

static AudioRing g_audio;

static std::size_t audioSink(void* /*ctx*/, const int16_t* stereoFrames, std::size_t frameCount) noexcept {
    g_audio.push(stereoFrames, static_cast<uint32_t>(frameCount));
    return frameCount;
}

static void sdlAudioCallback(void* /*userdata*/, Uint8* stream, int len) {
    std::memset(stream, 0, static_cast<size_t>(len));
    const uint32_t framesWanted = static_cast<uint32_t>(len / (sizeof(int16_t) * 2));
    auto* out = reinterpret_cast<int16_t*>(stream);
    (void)g_audio.pop(out, framesWanted);

    // Zero-fill remainder already done by memset.
}

struct VideoSinkCtx {
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_PixelFormatEnum sdlFmt = SDL_PIXELFORMAT_ARGB8888;
    unsigned texW = 0;
    unsigned texH = 0;
};

static void videoSink(void* ctx, const void* data, unsigned width, unsigned height, std::size_t pitchBytes) noexcept {
    auto* v = static_cast<VideoSinkCtx*>(ctx);
    if (!v || !v->renderer) return;
    if (!data) return;

    if (!v->texture || v->texW != width || v->texH != height) {
        if (v->texture) SDL_DestroyTexture(v->texture);
        v->texture = SDL_CreateTexture(v->renderer, v->sdlFmt, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
        v->texW = width;
        v->texH = height;
        if (!v->texture) return;

        // Ensure the "X"/alpha bits never affect final output.
        SDL_SetTextureBlendMode(v->texture, SDL_BLENDMODE_NONE);
    }

    // Update the whole texture. pitchBytes is per-row byte stride.
    SDL_UpdateTexture(v->texture, nullptr, data, static_cast<int>(pitchBytes));
}

inline uint16_t keyToSnes(SDL_Keycode key) noexcept {
    switch (key) {
        case SDLK_z: return snesonline::SNES_B;
        case SDLK_x: return snesonline::SNES_A;
        case SDLK_a: return snesonline::SNES_Y;
        case SDLK_s: return snesonline::SNES_X;
        case SDLK_q: return snesonline::SNES_L;
        case SDLK_w: return snesonline::SNES_R;
        case SDLK_RETURN: return snesonline::SNES_START;
        case SDLK_RSHIFT: return snesonline::SNES_SELECT;
        case SDLK_UP: return snesonline::SNES_UP;
        case SDLK_DOWN: return snesonline::SNES_DOWN;
        case SDLK_LEFT: return snesonline::SNES_LEFT;
        case SDLK_RIGHT: return snesonline::SNES_RIGHT;
        default: return 0;
    }
}

struct InputState {
    uint16_t mask = 0;
    SDL_GameController* controller = nullptr;

    void setBit(uint16_t bit, bool down) noexcept {
        if (!bit) return;
        if (down) mask |= bit;
        else mask = static_cast<uint16_t>(mask & ~bit);
    }

    void pollController() noexcept {
        if (!controller) return;

        // Left stick -> dpad.
        const float lx = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        const float ly = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;

        uint16_t dpad = 0;
        const snesonline::Stick2f stick{lx, ly};
        dpad |= snesonline::mapAndroidAxesToDpad(stick); // SDL Y+ is down
        dpad = snesonline::sanitizeDpad(dpad);

        mask &= static_cast<uint16_t>(~(snesonline::SNES_UP | snesonline::SNES_DOWN | snesonline::SNES_LEFT | snesonline::SNES_RIGHT));
        mask |= dpad;

        // Buttons.
        setBit(snesonline::SNES_A, SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B));
        setBit(snesonline::SNES_B, SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A));
        setBit(snesonline::SNES_X, SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y));
        setBit(snesonline::SNES_Y, SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X));

        setBit(snesonline::SNES_L, SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
        setBit(snesonline::SNES_R, SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));

        setBit(snesonline::SNES_START, SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START));
        setBit(snesonline::SNES_SELECT, SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK));

        // D-pad buttons override stick if pressed.
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP)) mask |= snesonline::SNES_UP;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) mask |= snesonline::SNES_DOWN;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mask |= snesonline::SNES_LEFT;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) mask |= snesonline::SNES_RIGHT;
        mask = snesonline::sanitizeDpad(mask);
    }
};

static void usage() {
    std::puts(
    "Usage: snesonline_win --core <path_to_libretro_core.dll> --rom <path_to_rom> [--config] [--netplay] [--player <1|2>] [--remote-ip <ip>] [--remote-port <port>] [--local-port <port>]\n\n"
        "Notes:\n"
    "  - Press F1 to open configuration while running.\n"
    "  - Netplay requires building with -DSNESONLINE_ENABLE_GGPO=ON.\n"
    "  - By default, CMake will fetch/build GGPO automatically (SNESONLINE_FETCH_GGPO=ON).\n");
}

} // namespace

int main(int argc, char** argv) {
    const char* corePath = nullptr;
    const char* romPath = nullptr;

    std::string ownedCorePath;
    std::string ownedRomPath;

    bool wantConfigOnly = false;
    bool wantNetplay = false;
    uint8_t localPlayerNum = 1;
    bool playerSpecified = false;
    const char* remoteIp = nullptr;
    uint16_t remotePort = 0;
    uint16_t localPort = 0;

    bool remoteIpSpecified = false;
    bool remotePortSpecified = false;
    bool localPortSpecified = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--core") == 0 && i + 1 < argc) { corePath = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) { romPath = argv[++i]; continue; }

        if (std::strcmp(argv[i], "--config") == 0) { wantConfigOnly = true; continue; }

        if (std::strcmp(argv[i], "--netplay") == 0) { wantNetplay = true; continue; }

        if (std::strcmp(argv[i], "--player") == 0 && i + 1 < argc) {
            const int n = std::atoi(argv[++i]);
            localPlayerNum = static_cast<uint8_t>((n == 2) ? 2 : 1);
            playerSpecified = true;
            continue;
        }
        if (std::strcmp(argv[i], "--remote-ip") == 0 && i + 1 < argc) { remoteIp = argv[++i]; remoteIpSpecified = true; continue; }
        if (std::strcmp(argv[i], "--remote-port") == 0 && i + 1 < argc) {
            const int p = std::atoi(argv[++i]);
            remotePort = static_cast<uint16_t>((p > 0 && p <= 65535) ? p : 0);
            remotePortSpecified = true;
            continue;
        }
        if (std::strcmp(argv[i], "--local-port") == 0 && i + 1 < argc) {
            const int p = std::atoi(argv[++i]);
            localPort = static_cast<uint16_t>((p > 0 && p <= 65535) ? p : 0);
            localPortSpecified = true;
            continue;
        }
    }

    // Load config and ensure default ROMs folder exists.
    snesonline::AppConfig cfg;
    const std::string cfgPath = snesonline::AppConfig::defaultConfigPath();
    (void)snesonline::loadConfig(cfgPath, cfg);
    const std::string defaultRoms = snesonline::ensureDefaultRomsDirExists();
    if (cfg.romsDir.empty() && !defaultRoms.empty()) cfg.romsDir = defaultRoms;

    // If the user didn't specify --player, default to the config value.
    if (!playerSpecified) {
        localPlayerNum = (cfg.localPlayerNum == 2) ? 2 : 1;
    }

    if (wantConfigOnly) {
        if (snesonline::showConfigDialog(cfg)) {
            if (!snesonline::saveConfig(cfgPath, cfg)) {
                std::fprintf(stderr, "Failed to save config to: %s\n", cfgPath.c_str());
#if defined(_WIN32)
                showMessageBox("snes-online", "Failed to save config file.", MB_ICONERROR);
#endif
                return 1;
            }
        }
        return 0;
    }

    // Friendly defaults / prompts when launched without args.
    // - If you double-click the exe, there are usually no args and the console closes immediately.
    //   Here we prompt for missing ROM/core so the app can be used without a terminal.
#if defined(_WIN32)
    if (!corePath) {
        const std::string exeDir = snesonline::getExecutableDir();
        if (!exeDir.empty()) {
            // Try "<exe_dir>\\cores\\snes9x_libretro.dll" first.
            const std::string candidate = exeDir + "\\cores\\snes9x_libretro.dll";
            if (fileExists(candidate)) {
                ownedCorePath = candidate;
                corePath = ownedCorePath.c_str();
            }
        }
    }

    // If user dragged a ROM onto the exe, Windows passes it as the first arg.
    if (!romPath && argc == 2 && argv[1] && argv[1][0] != '-') {
        ownedRomPath = argv[1];
        romPath = ownedRomPath.c_str();
    }

    if (!romPath) {
        ownedRomPath = openFileDialog(
            "Select a SNES ROM",
            "SNES ROM (*.sfc;*.smc;*.zip)\0*.sfc;*.smc;*.zip\0All files (*.*)\0*.*\0\0");
        if (!ownedRomPath.empty()) romPath = ownedRomPath.c_str();
    }

    if (!corePath) {
        ownedCorePath = openFileDialog(
            "Select a Libretro core DLL (e.g. snes9x_libretro.dll)",
            "Libretro Core (*.dll)\0*.dll\0All files (*.*)\0*.*\0\0");
        if (!ownedCorePath.empty()) corePath = ownedCorePath.c_str();
    }
#endif

    if (!corePath || !romPath) {
        usage();
#if defined(_WIN32)
        showMessageBox(
            "snes-online",
            "Missing arguments.\n\n"
            "You must provide a Libretro core DLL and a ROM.\n\n"
            "Example:\n"
            "  snesonline_win.exe --core .\\cores\\snes9x_libretro.dll --rom C:\\path\\to\\game.sfc\n",
            MB_ICONINFORMATION);
#endif
        return 2;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
#if defined(_WIN32)
        showMessageBox("snes-online", SDL_GetError(), MB_ICONERROR);
#endif
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("snes-online", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 540, SDL_WINDOW_RESIZABLE);
    // Avoid render vsync controlling emulation rate; we already pace frames explicitly.
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!window || !renderer) {
        std::fprintf(stderr, "SDL window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }

    // Initialize core.
    auto& eng = snesonline::EmulatorEngine::instance();
    if (!eng.initialize(corePath, romPath)) {
        std::fprintf(stderr, "EmulatorEngine initialize failed. Check core/rom paths.\n");
#if defined(_WIN32)
        showMessageBox(
            "snes-online",
            "Failed to initialize the emulator.\n\n"
            "Check that:\n"
            "  - The core DLL path is correct\n"
            "  - The ROM path is correct\n",
            MB_ICONERROR);
#endif
        return 1;
    }

    // Optional netplay.
    snesonline::NetplaySession netplay;
    snesonline::LockstepSession lockstep;
    const bool effectiveNetplay = wantNetplay || cfg.netplayEnabled;
    bool netplayStarted = false;
    std::string netplayBaseTitle;
    const bool useLockstep = cfg.netplayLockstep;

    std::ofstream lockstepLog;
    auto lockstepLogLast = std::chrono::steady_clock::time_point{};
    const auto lockstepLogStart = std::chrono::steady_clock::now();
    if (effectiveNetplay) {
#if defined(_WIN32)
        // Room-only mode: connect at game start and let the server assign role.
        uint8_t roomRole = 0;
        std::string roomHostIp;
        uint16_t roomHostPort = 0;
        std::string roomErr;

        const uint16_t lportForRoom = (localPort != 0) ? localPort : cfg.localPort;
        const bool roomConfigured = !cfg.roomCode.empty();
        if (!remoteIpSpecified && roomConfigured) {
            if (!roomConnectAtStart(cfg, lportForRoom, roomRole, roomHostIp, roomHostPort, roomErr)) {
                std::string msg = "Room connect failed: " + roomErr;
                showMessageBox("snes-online", msg.c_str(), MB_ICONERROR);
                return 2;
            }

            localPlayerNum = roomRole;
            if (roomRole == 2) {
                remoteIp = roomHostIp.c_str();
                if (!remotePortSpecified) remotePort = roomHostPort;
            } else {
                // Player 1: remote IP blank (auto-discover from first packet).
                remoteIp = "";
            }
        }
#endif
        // Use CLI overrides if provided, otherwise fall back to config file values.
        const char* ip = remoteIp ? remoteIp : cfg.remoteIp.c_str();
        const uint16_t port = (remotePort != 0) ? remotePort : cfg.remotePort;
        const uint16_t lport = (localPort != 0) ? localPort : cfg.localPort;

    #if defined(_WIN32)
        // Ask once (with UAC) to add a firewall allow rule for netplay.
        promptAndAddFirewallRuleIfNeeded(true, ip, lport);
    #endif

#if defined(_WIN32)
        // Same-PC convenience: because the config file is shared, two double-click launches would otherwise
        // use identical settings. If we're talking to loopback and a second instance is launched, automatically
        // flip to the opposite player and swap ports (unless the user provided explicit CLI overrides).
        uint8_t effectivePlayer = localPlayerNum;
        const char* effectiveIp = ip;
        uint16_t effectiveRemotePort = port;
        uint16_t effectiveLocalPort = lport;

        if (isLoopbackIp(ip)) {
            static HANDLE s_loopbackMutex = CreateMutexA(nullptr, TRUE, "Local\\snes-online-loopback-netplay");
            const DWORD err = GetLastError();
            const bool isSecondInstance = (s_loopbackMutex != nullptr) && (err == ERROR_ALREADY_EXISTS);

            if (isSecondInstance) {
                // Only auto-flip when the user did not specify overrides.
                if (!playerSpecified) {
                    effectivePlayer = (localPlayerNum == 2) ? 1 : 2;
                }
                if (!localPortSpecified && !remotePortSpecified) {
                    if (effectiveLocalPort != 0 && effectiveRemotePort != 0 && effectiveLocalPort != effectiveRemotePort) {
                        const uint16_t tmp = effectiveLocalPort;
                        effectiveLocalPort = effectiveRemotePort;
                        effectiveRemotePort = tmp;
                    }
                }
            }
        }
#else
        uint8_t effectivePlayer = localPlayerNum;
        const char* effectiveIp = ip;
        uint16_t effectiveRemotePort = port;
        uint16_t effectiveLocalPort = lport;
#endif

        const bool autoDiscover = (effectivePlayer == 1) && (!effectiveIp || !effectiveIp[0]);

        if (useLockstep) {
            // Android-compatible UDP lockstep.
            // Player 2 must have a configured remote endpoint.
            if (effectivePlayer == 2 && (!effectiveIp || !effectiveIp[0] || effectiveRemotePort == 0)) {
                std::fprintf(stderr, "Lockstep netplay requires remote IP/port for Player 2.\n");
#if defined(_WIN32)
                showMessageBox(
                    "snes-online",
                    "Lockstep netplay (Android-compatible) requires a Remote IP and Remote Port for Player 2.\n\n"
                    "Use the Config dialog and either:\n"
                    "  - Paste the connection code and click 'Join From Code', or\n"
                    "  - Manually enter Remote IP + Remote Port.\n",
                    MB_ICONERROR);
#endif
                return 2;
            }

            snesonline::LockstepSession::Config np{};
            np.remoteHost = autoDiscover ? "" : effectiveIp;
            np.remotePort = autoDiscover ? 0 : effectiveRemotePort;
            np.localPort = effectiveLocalPort;
            np.localPlayerNum = effectivePlayer;

            if (!lockstep.start(np)) {
                std::fprintf(stderr, "Lockstep netplay failed to start.\n");
#if defined(_WIN32)
                showMessageBox(
                    "snes-online",
                    "Lockstep netplay failed to start.\n\n"
                    "Common causes:\n"
                    "  - Remote IP is not a valid IPv4 address\n"
                    "  - Local port is already in use (try a different Local Port)\n"
                    "  - Firewall is blocking UDP\n",
                    MB_ICONERROR);
#endif
                return 1;
            }

            netplayStarted = true;

            char title[256] = {};
            std::snprintf(
                title,
                sizeof(title),
                "snes-online (lockstep P%d %u->%s:%u)",
                static_cast<int>(effectivePlayer),
                static_cast<unsigned>(effectiveLocalPort),
                autoDiscover ? "(auto)" : effectiveIp,
                static_cast<unsigned>(autoDiscover ? 0 : effectiveRemotePort));
            netplayBaseTitle = std::string(title);
            SDL_SetWindowTitle(window, netplayBaseTitle.c_str());
        } else {
            // GGPO rollback.
            if (!effectiveIp || !effectiveIp[0] || effectiveRemotePort == 0) {
                // Host convenience: allow Player 1 to leave Remote IP empty to auto-discover the peer.
                const bool allowAutoDiscover = (effectivePlayer == 1) && (!effectiveIp || !effectiveIp[0]);
                if (!allowAutoDiscover) {
                    std::fprintf(stderr, "Netplay requires remote IP/port. Use --remote-ip/--remote-port or set them in --config.\n");
                    return 2;
                }
            }
            snesonline::NetplaySession::Config np{};
            np.gameName = "snes-online";
            np.remoteIp = autoDiscover ? "" : effectiveIp;
            np.remotePort = autoDiscover ? 0 : effectiveRemotePort;
            np.localPort = effectiveLocalPort;
            np.frameDelay = cfg.netplayFrameDelay;
            np.localPlayerNum = effectivePlayer;

            if (!netplay.start(np)) {
                std::fprintf(stderr, "Netplay failed to start. Build with -DSNESONLINE_ENABLE_GGPO=ON (and ensure CMake finished fetching/building GGPO).\n");
#if defined(_WIN32)
                showMessageBox(
                    "snes-online",
                    "GGPO netplay failed to start.\n\n"
                    "If you are connecting to Android, enable 'Android-compatible lockstep' in the Config dialog.\n\n"
                    "Otherwise, ensure GGPO was built/enabled and your Remote IP/Port are valid.",
                    MB_ICONERROR);
#endif
                return 1;
            }

            netplayStarted = true;

            char title[256] = {};
            std::snprintf(
                title,
                sizeof(title),
                "snes-online (netplay P%d %u->%s:%u)",
                static_cast<int>(effectivePlayer),
                static_cast<unsigned>(effectiveLocalPort),
                autoDiscover ? "(auto)" : effectiveIp,
                static_cast<unsigned>(autoDiscover ? 0 : effectiveRemotePort));
            netplayBaseTitle = std::string(title);
            SDL_SetWindowTitle(window, netplayBaseTitle.c_str());
        }
    }

    // Configure sinks.
    VideoSinkCtx video{};
    video.renderer = renderer;

    // Pick SDL texture format based on core pixel format.
    const auto fmt = eng.core().pixelFormat();
    switch (fmt) {
        case snesonline::LibretroCore::PixelFormat::RGB565:
            video.sdlFmt = SDL_PIXELFORMAT_RGB565;
            break;
        case snesonline::LibretroCore::PixelFormat::XRGB1555:
            video.sdlFmt = SDL_PIXELFORMAT_XRGB1555;
            break;
        case snesonline::LibretroCore::PixelFormat::XRGB8888:
        default:
            video.sdlFmt = SDL_PIXELFORMAT_XRGB8888;
            break;
    }

    // Texture will be created on first video frame (size comes from the core).

    eng.core().setVideoSink(&video, &videoSink);
    eng.core().setAudioSink(nullptr, &audioSink);

    // Audio device.
    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = static_cast<int>(eng.core().sampleRateHz());
    if (want.freq <= 0) want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    // Smaller buffer reduces perceived latency.
    want.samples = 512;
    want.callback = &sdlAudioCallback;

    SDL_AudioDeviceID audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audioDev != 0) {
        // Keep only a few callbacks worth of audio buffered.
        const uint32_t target = (have.samples > 0) ? (static_cast<uint32_t>(have.samples) * 4u) : 2048u;
        g_audio.setMaxBufferedFrames(target);
        SDL_PauseAudioDevice(audioDev, 0);
    }

    // Controller.
    InputState input{};
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            input.controller = SDL_GameControllerOpen(i);
            if (input.controller) break;
        }
    }

    const double fps = eng.core().framesPerSecond();
    const auto frameDur = std::chrono::duration<double>(1.0 / (fps > 1.0 ? fps : 60.0));

    bool running = true;
    auto next = std::chrono::steady_clock::now();

    while (running) {
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameDur);

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    const bool down = (ev.type == SDL_KEYDOWN);
                    if (down && ev.key.keysym.sym == SDLK_F1) {
                        // Pause-ish: open config dialog, then persist changes.
                        if (snesonline::showConfigDialog(cfg)) {
                            if (!snesonline::saveConfig(cfgPath, cfg)) {
                                std::fprintf(stderr, "Failed to save config to: %s\n", cfgPath.c_str());
                            }
                        }
                        break;
                    }
                    const uint16_t bit = keyToSnes(ev.key.keysym.sym);
                    input.setBit(bit, down);
                    break;
                }

                case SDL_CONTROLLERDEVICEADDED:
                    if (!input.controller && SDL_IsGameController(ev.cdevice.which)) {
                        input.controller = SDL_GameControllerOpen(ev.cdevice.which);
                    }
                    break;

                case SDL_CONTROLLERDEVICEREMOVED:
                    // If it was our controller, close and clear.
                    if (input.controller) {
                        SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(input.controller));
                        if (id == ev.cdevice.which) {
                            SDL_GameControllerClose(input.controller);
                            input.controller = nullptr;
                        }
                    }
                    break;
            }
        }

        input.pollController();

        if (effectiveNetplay) {
            if (useLockstep) {
                const uint32_t f0 = lockstep.localFrame();
                lockstep.setLocalInput(input.mask);
                lockstep.tick();
                const uint32_t f1 = lockstep.localFrame();
                const uint32_t advanced = (f1 >= f0) ? (f1 - f0) : 0u;

                if (netplayStarted) {
                    if (!lockstepLog.is_open()) {
                        lockstepLog.open(lockstepDebugLogPath(), std::ios::out | std::ios::trunc);
                        if (lockstepLog.is_open()) {
                            lockstepLog << "# snes-online lockstep debug\n";
                            lockstepLog << "# columns: ms peer rx last_ms f need rlast rmax wait adv\n";
                            lockstepLog.flush();
                        }
                    }

                    if (lockstepLog.is_open()) {
                        const auto now = std::chrono::steady_clock::now();
                        if (lockstepLogLast.time_since_epoch().count() == 0) lockstepLogLast = now;
                        if (now - lockstepLogLast >= std::chrono::seconds(1)) {
                            lockstepLogLast = now;
                            const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lockstepLogStart).count();
                            const std::string peer = lockstep.peerEndpoint();
                            const uint64_t rx = lockstep.recvCount();
                            const int64_t ageMs = lockstep.lastRecvAgeMs();
                            const uint32_t f = lockstep.localFrame();
                            const uint32_t need = f;
                            const uint32_t rlast = lockstep.lastRemoteFrame();
                            const uint32_t rmax = lockstep.maxRemoteFrame();
                            const int wait = lockstep.waitingForPeer() ? 1 : 0;

                            lockstepLog << ms << " " << peer << " " << rx << " " << ageMs << " " << f << " " << need << " " << rlast
                                        << " " << rmax << " " << wait << " " << advanced << "\n";
                            lockstepLog.flush();
                        }
                    }
                }
            } else {
                netplay.setLocalInput(input.mask);
                netplay.tick();
            }

            if (netplayStarted) {
                // Non-intrusive status in the window title.
                std::string desired = netplayBaseTitle.empty() ? std::string("snes-online (netplay)") : netplayBaseTitle;
                if (useLockstep) {
                    const std::string peer = lockstep.peerEndpoint();
                    const uint64_t rx = lockstep.recvCount();
                    const int64_t ageMs = lockstep.lastRecvAgeMs();
                    const uint32_t f = lockstep.localFrame();
                    const uint32_t need = f + 5; // must match kInputDelayFrames in LockstepSession.cpp
                    const uint32_t rlast = lockstep.lastRemoteFrame();
                    const uint32_t rmax = lockstep.maxRemoteFrame();
                    desired += " [lockstep";
                    if (!peer.empty()) desired += " " + peer;
                    if (rx > 0) {
                        desired += " rx=" + std::to_string(rx);
                        if (ageMs >= 0) desired += " last=" + std::to_string(ageMs) + "ms";
                    } else {
                        desired += " rx=0";
                    }
                    desired += " f=" + std::to_string(f);
                    desired += " need=" + std::to_string(need);
                    if (rx > 0) {
                        desired += " rlast=" + std::to_string(rlast);
                        desired += " rmax=" + std::to_string(rmax);
                    }
                    if (lockstep.waitingForPeer()) desired += " wait";
                    else if (lockstep.connected()) desired += " ok";
                    desired += "]";
                } else {
                    if (netplay.reconnecting()) {
                        desired += " [reconnecting]";
                    } else if (netplay.disconnected()) {
                        desired += " [disconnected]";
                    } else if (netplay.waitingForPeer()) {
                        desired += " [waiting for peer]";
                    } else if (netplay.hasSynchronized()) {
                        desired += " [connected]";
                    }
                }

                const char* cur = SDL_GetWindowTitle(window);
                if (!cur || desired != cur) {
                    SDL_SetWindowTitle(window, desired.c_str());
                }
            }
        } else {
            eng.setLocalInputMask(input.mask);
            eng.advanceFrame();
        }

        // Render last uploaded frame (if any). If the core outputs dynamic sizes, a smarter resize path is needed.
        SDL_RenderClear(renderer);
        if (video.texture) SDL_RenderCopy(renderer, video.texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        std::this_thread::sleep_until(next);
    }

    if (input.controller) SDL_GameControllerClose(input.controller);
    if (audioDev != 0) SDL_CloseAudioDevice(audioDev);

    eng.shutdown();

    if (video.texture) SDL_DestroyTexture(video.texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
