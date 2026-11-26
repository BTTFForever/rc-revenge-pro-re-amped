// Windows only: what the game executable does before anything else.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <string>

// Ask hybrid-graphics laptops for the discrete GPU (NVIDIA Optimus and AMD PowerXpress read these exports from the
// executable at start), as tools/play.sh's PRIME offload does on Linux.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

// A release's game executable (rc_revenge_pro_<usa|eu>.exe) started with no arguments, as a launcher such as Quiver may
// do when it picks the program itself: the game needs the executable from the disc, which rc-launcher.exe beside it
// sets up and passes. Start the launcher for this release instead and end here. Without the launcher beside it (a
// development build), nothing changes.
__attribute__((constructor(101))) void handOffToLauncher()
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
        LocalFree(argv);
    if (argc >= 2)
        return;
    wchar_t self[32768];
    const DWORD n = GetModuleFileNameW(nullptr, self, 32768);
    if (n == 0 || n >= 32768)
        return;
    std::wstring path(self, n);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring folder = path.substr(0, slash + 1), name = path.substr(slash + 1);
    const std::wstring launcher = folder + L"rc-launcher.exe";
    if (GetFileAttributesW(launcher.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    std::wstring command = L"\"" + launcher + L"\"";
    if (_wcsicmp(name.c_str(), L"rc_revenge_pro_usa.exe") == 0)
        command += L" --version usa";
    else if (_wcsicmp(name.c_str(), L"rc_revenge_pro_eu.exe") == 0)
        command += L" --version eu";
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &si, &pi))
        return; // could not: carry on, the game reports the missing executable as before
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

} // namespace
#endif
