#include "ConfigDialog.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <ws2tcpip.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <wincrypt.h>
#pragma comment(lib, "Crypt32.lib")

#include <cstdio>
#include <cstring>
#include <cctype>
#include <memory>
#include <string>

#include "snesonline/StunClient.h"

namespace {

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
#ifndef IDC_ROLE_HOST
#define IDC_ROLE_HOST 1013
#endif
#ifndef IDC_ROLE_JOIN
#define IDC_ROLE_JOIN 1014
#endif
#ifndef IDC_REMOTE_IP
#define IDC_REMOTE_IP 1015
#endif
#ifndef IDC_REMOTE_PORT
#define IDC_REMOTE_PORT 1016
#endif
#ifndef IDC_CONN_CODE
#define IDC_CONN_CODE 1017
#endif
#ifndef IDC_CONN_GET
#define IDC_CONN_GET 1018
#endif
#ifndef IDC_CONN_JOIN
#define IDC_CONN_JOIN 1019
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

static void setEnabled(HWND dlg, int id, bool enabled) {
    HWND h = GetDlgItem(dlg, id);
    if (h) EnableWindow(h, enabled ? TRUE : FALSE);
}

static void copyTextToClipboard(HWND dlg, const std::string& s) {
    if (!OpenClipboard(dlg)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
    if (hMem) {
        void* p = GlobalLock(hMem);
        if (p) {
            std::memcpy(p, s.c_str(), s.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

static std::string base64UrlEncode(const std::string& bytes) {
    if (bytes.empty()) return {};
    DWORD outLen = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &outLen)) {
        return {};
    }
    std::string b64;
    b64.resize(outLen);
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &outLen)) {
        return {};
    }
    if (!b64.empty() && b64.back() == '\0') b64.pop_back();
    while (!b64.empty() && (b64.back() == '\r' || b64.back() == '\n')) b64.pop_back();

    // base64url
    for (char& c : b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    return b64;
}

static bool base64UrlDecode(const std::string& b64url, std::string& outBytes) {
    outBytes.clear();
    std::string b64 = trimAscii(b64url);
    if (b64.empty()) return false;
    for (char& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while ((b64.size() % 4) != 0) b64.push_back('=');

    DWORD binLen = 0;
    if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64, nullptr, &binLen, nullptr, nullptr)) {
        return false;
    }
    outBytes.resize(binLen);
    if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(outBytes.data()), &binLen, nullptr, nullptr)) {
        outBytes.clear();
        return false;
    }
    outBytes.resize(binLen);
    return true;
}

struct ConnInfo {
    std::string publicIp;
    uint16_t publicPort = 0;
    std::string lanIp;
    uint16_t lanPort = 0;
};

static std::string encodeConnectionCode(const ConnInfo& info) {
    std::string payload = "v=1";
    payload += "&pub=" + info.publicIp + ":" + std::to_string(static_cast<unsigned>(info.publicPort));
    if (!info.lanIp.empty() && info.lanPort != 0) {
        payload += "&lan=" + info.lanIp + ":" + std::to_string(static_cast<unsigned>(info.lanPort));
    }
    return "SNO1:" + base64UrlEncode(payload);
}

static bool parseHostPort(const std::string& s, std::string& outHost, uint16_t& outPort) {
    outHost.clear();
    outPort = 0;
    const std::string t = trimAscii(s);
    const size_t colon = t.rfind(':');
    if (colon == std::string::npos) return false;
    outHost = t.substr(0, colon);
    const std::string portStr = t.substr(colon + 1);
    try {
        const int p = std::stoi(portStr);
        if (p > 0 && p <= 65535) outPort = static_cast<uint16_t>(p);
    } catch (...) {
        outPort = 0;
    }
    return !outHost.empty() && outPort != 0;
}

static bool decodeConnectionCode(const std::string& code, ConnInfo& outInfo, std::string& outErr) {
    outErr.clear();
    outInfo = ConnInfo{};
    const std::string t = trimAscii(code);
    if (t.size() < 6 || t.rfind("SNO1:", 0) != 0) {
        outErr = "invalid_code";
        return false;
    }
    std::string payload;
    if (!base64UrlDecode(t.substr(5), payload)) {
        outErr = "invalid_base64";
        return false;
    }

    // payload: v=1&pub=ip:port&lan=ip:port
    size_t pos = 0;
    while (pos < payload.size()) {
        size_t amp = payload.find('&', pos);
        if (amp == std::string::npos) amp = payload.size();
        const std::string part = payload.substr(pos, amp - pos);
        const size_t eq = part.find('=');
        if (eq != std::string::npos) {
            const std::string k = part.substr(0, eq);
            const std::string v = part.substr(eq + 1);
            if (k == "pub") {
                parseHostPort(v, outInfo.publicIp, outInfo.publicPort);
            } else if (k == "lan") {
                parseHostPort(v, outInfo.lanIp, outInfo.lanPort);
            }
        }
        pos = amp + 1;
    }

    if (outInfo.publicIp.empty() || outInfo.publicPort == 0) {
        outErr = "missing_public_endpoint";
        return false;
    }
    if (!outInfo.lanIp.empty() && outInfo.lanPort == 0) {
        outInfo.lanIp.clear();
    }
    return true;
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

struct DialogState {
    snesonline::AppConfig* cfg = nullptr;
    bool accepted = false;
};

static void syncRoleUi(HWND dlg) {
    const bool join = (IsDlgButtonChecked(dlg, IDC_ROLE_JOIN) == BST_CHECKED);
    setEnabled(dlg, IDC_REMOTE_IP, join);
    setEnabled(dlg, IDC_REMOTE_PORT, join);

    if (!join) {
        // Host mode: we intentionally do not use a fixed remote endpoint.
        setDlgItemText(dlg, IDC_REMOTE_IP, "");
        setDlgItemText(dlg, IDC_REMOTE_PORT, "");
    }
}

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

            const bool join = (st->cfg->localPlayerNum == 2);
            CheckDlgButton(dlg, IDC_ROLE_HOST, join ? BST_UNCHECKED : BST_CHECKED);
            CheckDlgButton(dlg, IDC_ROLE_JOIN, join ? BST_CHECKED : BST_UNCHECKED);
            setDlgItemText(dlg, IDC_REMOTE_IP, st->cfg->remoteIp);
            setDlgItemText(dlg, IDC_REMOTE_PORT, st->cfg->remotePort ? std::to_string(st->cfg->remotePort) : std::string());
            setDlgItemText(dlg, IDC_CONN_CODE, std::string());
            syncRoleUi(dlg);
            return TRUE;
        }

        case WM_COMMAND: {
            const WORD id = LOWORD(wParam);
            switch (id) {
                case IDC_ROLE_HOST:
                case IDC_ROLE_JOIN:
                    syncRoleUi(dlg);
                    return TRUE;

                case IDC_CONN_GET: {
                    // Host: discover public endpoint via STUN and generate a shareable code.
                    const uint16_t lp = parsePort(getDlgItemText(dlg, IDC_LOCAL_PORT), 7000);
                    snesonline::StunMappedAddress mapped;
                    if (!snesonline::stunDiscoverMappedAddressDefault(lp, mapped) || mapped.ip.empty() || mapped.port == 0) {
                        MessageBoxA(dlg, "STUN failed (could not discover public UDP mapping).", "Direct Connect", MB_ICONERROR | MB_OK);
                        return TRUE;
                    }

                    ConnInfo info;
                    info.publicIp = mapped.ip;
                    info.publicPort = mapped.port;
                    info.lanIp = localLanIpv4BestEffort();
                    info.lanPort = lp;
                    const std::string code = encodeConnectionCode(info);
                    setDlgItemText(dlg, IDC_CONN_CODE, code);
                    copyTextToClipboard(dlg, code);

                    // Switch to host mode (Player 1 waits for peer).
                    CheckDlgButton(dlg, IDC_ROLE_HOST, BST_CHECKED);
                    CheckDlgButton(dlg, IDC_ROLE_JOIN, BST_UNCHECKED);
                    setDlgItemText(dlg, IDC_REMOTE_IP, "");
                    setDlgItemText(dlg, IDC_REMOTE_PORT, "");
                    syncRoleUi(dlg);

                    MessageBoxA(dlg, "Connection code copied to clipboard. Share it with Player 2.", "Direct Connect", MB_ICONINFORMATION | MB_OK);
                    return TRUE;
                }

                case IDC_CONN_JOIN: {
                    // Join: parse code and fill in remote endpoint + switch to Player 2.
                    ConnInfo info;
                    std::string err;
                    const std::string code = getDlgItemText(dlg, IDC_CONN_CODE);
                    if (!decodeConnectionCode(code, info, err)) {
                        MessageBoxA(dlg, "Invalid connection code.", "Direct Connect", MB_ICONERROR | MB_OK);
                        return TRUE;
                    }

                    // Prefer LAN if we appear to share the same public IP.
                    std::string chosenIp = info.publicIp;
                    uint16_t chosenPort = info.publicPort;
                    if (!info.lanIp.empty() && info.lanPort != 0) {
                        const uint16_t lp = parsePort(getDlgItemText(dlg, IDC_LOCAL_PORT), 7000);
                        snesonline::StunMappedAddress selfMapped;
                        if (snesonline::stunDiscoverMappedAddressDefault(lp, selfMapped) && !selfMapped.ip.empty()) {
                            if (selfMapped.ip == info.publicIp) {
                                chosenIp = info.lanIp;
                                chosenPort = info.lanPort;
                            }
                        }
                    }

                    CheckDlgButton(dlg, IDC_ROLE_HOST, BST_UNCHECKED);
                    CheckDlgButton(dlg, IDC_ROLE_JOIN, BST_CHECKED);
                    setDlgItemText(dlg, IDC_REMOTE_IP, chosenIp);
                    setDlgItemText(dlg, IDC_REMOTE_PORT, std::to_string(static_cast<unsigned>(chosenPort)));
                    syncRoleUi(dlg);

                    // Make it hard to misconfigure.
                    CheckDlgButton(dlg, IDC_NETPLAY_ENABLED, BST_CHECKED);
                    CheckDlgButton(dlg, IDC_NETPLAY_LOCKSTEP, BST_CHECKED);
                    return TRUE;
                }

                case IDOK:
                    if (st && st->cfg) {
                        st->cfg->netplayEnabled = (IsDlgButtonChecked(dlg, IDC_NETPLAY_ENABLED) == BST_CHECKED);
                        st->cfg->netplayLockstep = (IsDlgButtonChecked(dlg, IDC_NETPLAY_LOCKSTEP) == BST_CHECKED);

                        const bool join = (IsDlgButtonChecked(dlg, IDC_ROLE_JOIN) == BST_CHECKED);
                        st->cfg->localPlayerNum = join ? 2 : 1;
                        st->cfg->remoteIp = join ? trimAscii(getDlgItemText(dlg, IDC_REMOTE_IP)) : std::string();
                        st->cfg->remotePort = join ? parsePort(getDlgItemText(dlg, IDC_REMOTE_PORT), 0) : 0;
                        st->cfg->localPort = parsePort(getDlgItemText(dlg, IDC_LOCAL_PORT), st->cfg->localPort);
                        st->cfg->romsDir = getDlgItemText(dlg, IDC_ROMS_DIR);

                        // Disable legacy room mode in config (direct-connect flow).
                        st->cfg->roomServerUrl.clear();
                        st->cfg->roomApiKey.clear();
                        st->cfg->roomCode.clear();
                        st->cfg->roomPassword.clear();
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
