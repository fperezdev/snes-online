#include "ConfigDialog.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <memory>
#include <string>

namespace {

constexpr const char* kDefaultRoomServerUrl = "http://snesonline.freedynamicdns.net:8787";

constexpr int IDD_CONFIG = 101;

#ifndef IDC_NETPLAY_ENABLED
#define IDC_NETPLAY_ENABLED 1001
#endif
#ifndef IDC_LOCAL_PORT
#define IDC_LOCAL_PORT 1004
#endif
#ifndef IDC_ROMS_DIR
#define IDC_ROMS_DIR 1005
#endif
#ifndef IDC_NETPLAY_LOCKSTEP
#define IDC_NETPLAY_LOCKSTEP 1012
#endif
#ifndef IDC_ROOM_SERVER_URL
#define IDC_ROOM_SERVER_URL 1013
#endif
#ifndef IDC_ROOM_API_KEY
#define IDC_ROOM_API_KEY 1014
#endif
#ifndef IDC_ROOM_CODE
#define IDC_ROOM_CODE 1015
#endif

static std::wstring utf8ToWide(const std::string& s);
static std::string wideToUtf8(const std::wstring& w);

static void setDlgItemText(HWND dlg, int id, const std::string& s) {
    const std::wstring w = utf8ToWide(s);
    SetDlgItemTextW(dlg, id, w.c_str());
}

static std::string getDlgItemText(HWND dlg, int id) {
    wchar_t buf[2048] = {};
    GetDlgItemTextW(dlg, id, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    return wideToUtf8(std::wstring(buf));
}

static uint16_t parsePort(const std::string& s, uint16_t fallback) {
    try {
        int p = std::stoi(s);
        if (p > 0 && p <= 65535) return static_cast<uint16_t>(p);
    } catch (...) {
    }
    return fallback;
}

static std::string trimAscii(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
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

static bool startsWithCaseInsensitive(const std::string& s, const char* prefix) {
    const std::string p(prefix ? prefix : "");
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        const unsigned char b = static_cast<unsigned char>(p[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

static bool isValidRoomServerUrl(const std::string& s) {
    const std::string t = trimAscii(s);
    return startsWithCaseInsensitive(t, "http://") || startsWithCaseInsensitive(t, "https://");
}

static bool parseBaseUrlHostPort(const std::string& baseUrlUtf8, std::string& outHostUtf8, uint16_t& outPort) {
    outHostUtf8.clear();
    outPort = 0;

    const std::wstring urlW = utf8ToWide(baseUrlUtf8);
    if (urlW.empty()) return false;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(sizeof(hostBuf) / sizeof(hostBuf[0]));
    uc.dwSchemeLength = (DWORD)-1;

    if (!WinHttpCrackUrl(urlW.c_str(), static_cast<DWORD>(urlW.size()), 0, &uc)) return false;
    if (uc.dwHostNameLength == 0 || !uc.lpszHostName) return false;

    outHostUtf8 = wideToUtf8(std::wstring(uc.lpszHostName, uc.dwHostNameLength));
    outPort = static_cast<uint16_t>(uc.nPort);
    return !outHostUtf8.empty() && outPort != 0;
}

static bool udpWhoamiPublicPort(const std::string& baseUrlUtf8, uint16_t localPort, uint16_t& outPublicPort) {
    outPublicPort = 0;
    std::string host;
    uint16_t port = 0;
    if (!parseBaseUrlHostPort(baseUrlUtf8, host, port)) return false;
    if (localPort == 0) return false;

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
    const int n = recvfrom(s, buf, static_cast<int>(sizeof(buf) - 1), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
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

static std::string winHttpRequestUtf8(const std::string& methodUtf8,
                                     const std::string& urlUtf8,
                                     const std::string& extraHeadersUtf8,
                                     const std::string& bodyUtf8,
                                     DWORD* outStatus) {
    if (outStatus) *outStatus = 0;

    const std::wstring urlW = utf8ToWide(urlUtf8);
    if (urlW.empty()) return {};

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
        return {};
    }

    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    const INTERNET_PORT port = uc.nPort;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (path.empty()) path = L"/";

    HINTERNET hSession = WinHttpOpen(L"snes-online/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return {};
    }

    const std::wstring methodW = utf8ToWide(methodUtf8);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, methodW.c_str(), path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    std::wstring headersW;
    if (!extraHeadersUtf8.empty()) headersW = utf8ToWide(extraHeadersUtf8);
    const void* bodyPtr = bodyUtf8.empty() ? WINHTTP_NO_REQUEST_DATA : bodyUtf8.data();
    const DWORD bodyLen = bodyUtf8.empty() ? 0 : static_cast<DWORD>(bodyUtf8.size());

    BOOL ok = WinHttpSendRequest(
        hRequest,
        headersW.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headersW.c_str(),
        headersW.empty() ? 0 : static_cast<DWORD>(headersW.size()),
        const_cast<void*>(bodyPtr),
        bodyLen,
        bodyLen,
        0);

    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (outStatus) *outStatus = status;

    std::string out;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
        std::string chunk;
        chunk.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), avail, &read) || read == 0) break;
        chunk.resize(read);
        out += chunk;
        if (out.size() > 1024 * 128) break;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return out;
}

struct RoomResult {
    bool ok = false;
    bool created = false; // true if we created the room (Player 1)
    std::string status;
    std::string code;
    std::string ip;
    uint16_t port = 0;
};

struct DialogState {
    snesonline::AppConfig* cfg = nullptr;
    bool accepted = false;
};

static INT_PTR CALLBACK dlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrA(dlg, GWLP_USERDATA));

    switch (msg) {
        case WM_INITDIALOG: {
            st = reinterpret_cast<DialogState*>(lParam);
            SetWindowLongPtrA(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            if (!st || !st->cfg) return TRUE;

            CheckDlgButton(dlg, IDC_NETPLAY_ENABLED, st->cfg->netplayEnabled ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_NETPLAY_LOCKSTEP, st->cfg->netplayLockstep ? BST_CHECKED : BST_UNCHECKED);

            setDlgItemText(dlg, IDC_LOCAL_PORT, std::to_string(st->cfg->localPort));
            setDlgItemText(dlg, IDC_ROMS_DIR, st->cfg->romsDir);

            setDlgItemText(dlg, IDC_ROOM_SERVER_URL, st->cfg->roomServerUrl);
            setDlgItemText(dlg, IDC_ROOM_API_KEY, st->cfg->roomPassword);
            setDlgItemText(dlg, IDC_ROOM_CODE, st->cfg->roomCode);

            if (trimAscii(getDlgItemText(dlg, IDC_ROOM_SERVER_URL)).empty()) {
                setDlgItemText(dlg, IDC_ROOM_SERVER_URL, kDefaultRoomServerUrl);
            }
            return TRUE;
        }

        case WM_COMMAND: {
            const WORD id = LOWORD(wParam);
            switch (id) {
                case IDOK:
                    if (st && st->cfg) {
                        st->cfg->netplayEnabled = (IsDlgButtonChecked(dlg, IDC_NETPLAY_ENABLED) == BST_CHECKED);
                        st->cfg->netplayLockstep = (IsDlgButtonChecked(dlg, IDC_NETPLAY_LOCKSTEP) == BST_CHECKED);

                        // Role is assigned at game-start connect; keep default 1 in config.
                        st->cfg->localPlayerNum = 1;
                        st->cfg->remoteIp.clear();
                        st->cfg->remotePort = 7000;
                        st->cfg->localPort = parsePort(getDlgItemText(dlg, IDC_LOCAL_PORT), st->cfg->localPort);
                        st->cfg->romsDir = getDlgItemText(dlg, IDC_ROMS_DIR);

                        st->cfg->roomServerUrl = getDlgItemText(dlg, IDC_ROOM_SERVER_URL);
                        st->cfg->roomPassword = getDlgItemText(dlg, IDC_ROOM_API_KEY);
                        st->cfg->roomCode = getDlgItemText(dlg, IDC_ROOM_CODE);
                        st->accepted = true;
                    }
                    EndDialog(dlg, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
        }
    }

    return FALSE;
}

} // namespace

namespace snesonline {

bool showConfigDialog(AppConfig& cfg) {
    HINSTANCE hInst = GetModuleHandleA(nullptr);
    DialogState st{&cfg, false};
    const INT_PTR r = DialogBoxParamA(hInst, MAKEINTRESOURCEA(IDD_CONFIG), nullptr, dlgProc, reinterpret_cast<LPARAM>(&st));
    (void)r;
    return st.accepted;
}

} // namespace snesonline

#else
namespace snesonline {
bool showConfigDialog(AppConfig&) { return false; }
}
#endif
