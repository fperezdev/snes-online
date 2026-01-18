#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <Shellapi.h>

#include <string>
#include <vector>

// snesonline_win is built as a Windows subsystem application (no console window).
// Provide WinMain and forward to the existing main().
extern int main(int argc, char** argv);

static std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int len = static_cast<int>(wcslen(w));
    if (len == 0) return {};

    const int needed = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};

    std::string out;
    out.resize(static_cast<size_t>(needed));
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), needed, nullptr, nullptr);
    return out;
}

int WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/) {
    int argcW = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    if (!argvW || argcW <= 0) {
        return main(0, nullptr);
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argcW));
    for (int i = 0; i < argcW; ++i) {
        args.push_back(wideToUtf8(argvW[i]));
    }

    std::vector<char*> argv;
    argv.reserve(static_cast<size_t>(argcW) + 1);
    for (int i = 0; i < argcW; ++i) {
        argv.push_back(args[static_cast<size_t>(i)].empty() ? const_cast<char*>("")
                                                           : args[static_cast<size_t>(i)].data());
    }
    argv.push_back(nullptr);

    const int rc = main(argcW, argv.data());
    LocalFree(argvW);
    return rc;
}
#endif
