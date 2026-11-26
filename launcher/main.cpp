// RC Revenge Pro: Re-Amped launcher. Contains no game code: it asks for the player's own disc image on the first run,
// checks it is the USA (SLUS-20153) or Europe (SLES-50077) release, prepares what the game binary needs (its executable
// copied from the disc, the disc image, memory cards) and starts the matching game binary.
//
//   rc-launcher                  plays (asks for the disc image when none is set up yet, or it went missing)
//   rc-launcher --choose-disc    asks for a disc image again (switch release or image)
//   rc-launcher --version usa|eu plays that release, when its disc was set up before
//   rc-launcher --disc <image>   sets that image up without dialogs (errors go to the terminal), then plays;
//                                --setup-only stops after the set-up
//
// Files: ~/.config/rc-revenge-pro/launcher.ini (the chosen release and images), ~/.local/share/rc-revenge-pro/<usa|eu>/
// (game/ holding the executable and links to the memory cards mc0, mc1, which tools/play.sh shares; the disc image or a
// link to it; logs/session.log), ~/.cache/rc-revenge-pro/<usa|eu>/shaders.
// Windows: %APPDATA%\rc-revenge-pro\launcher.ini and %LOCALAPPDATA%\rc-revenge-pro\<usa|eu>\ (game\ with the
// executable and the memory cards mc0, mc1 as real folders; logs\; shaders\). No links (they need privileges there): an
// .iso is read where it is, through RC_DISC_IMAGE; a .bin is converted once into that folder.
// Environment: RC_GAME_BINARY (the game binary to start), RC_GPU=default (no PRIME offload), and every variable the game
// reads (the defaults below are set only where the environment has none).

#include "disc_image.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using launcher::DiscImage;

namespace {

const char *kTitle = "RC Revenge Pro: Re-Amped";

struct Release
{
    const char *id, *label, *elf, *sha1, *isoName;
};
constexpr Release kReleases[] = {
    {"usa", "USA (SLUS-20153)", "SLUS_201.53", "a3f472d489e688525ea5e5513ce4c9dc6e6acd93", "RC Revenge Pro (USA).iso"},
    {"eu", "Europe (SLES-50077)", "SLES_500.77", "6957407aeb69c89f614a8a675bfcaae92dca83b3", "RC Revenge Pro (Europe).iso"},
};

const Release *releaseById(const std::string &id)
{
    for (const Release &r : kReleases)
        if (id == r.id)
            return &r;
    return nullptr;
}

// Paths as UTF-8 text (the settings file, messages) and back.
std::string utf8(const fs::path &p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path fromUtf8(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

void log(const std::string &line) { std::cerr << "[launcher] " << line << std::endl; }

// Sets an environment variable; overwrite false keeps a value already there.
void setEnv(const char *name, const std::string &value, bool overwrite)
{
#ifdef _WIN32
    if (!overwrite && std::getenv(name))
        return;
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), overwrite ? 1 : 0);
#endif
}

// ---- Folders and the launcher's settings ----
#ifdef _WIN32
fs::path knownDir(const wchar_t *var)
{
    if (const wchar_t *v = _wgetenv(var); v && *v)
        return fs::path(v);
    return fs::current_path();
}
fs::path configDir() { return knownDir(L"APPDATA") / "rc-revenge-pro"; }
fs::path dataDir(const Release &r) { return knownDir(L"LOCALAPPDATA") / "rc-revenge-pro" / r.id; }
fs::path cacheDir(const Release &r) { return dataDir(r); }
#else
fs::path xdg(const char *var, const char *fallback)
{
    if (const char *v = std::getenv(var); v && *v == '/')
        return v;
    const char *home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / fallback;
}
fs::path configDir() { return xdg("XDG_CONFIG_HOME", ".config") / "rc-revenge-pro"; }
fs::path dataDir(const Release &r) { return xdg("XDG_DATA_HOME", ".local/share") / "rc-revenge-pro" / r.id; }
fs::path cacheDir(const Release &r) { return xdg("XDG_CACHE_HOME", ".cache") / "rc-revenge-pro" / r.id; }
#endif

using Config = std::map<std::string, std::string>;

Config loadConfig()
{
    Config c;
    std::ifstream in(configDir() / "launcher.ini");
    for (std::string line; std::getline(in, line);)
        if (const size_t eq = line.find('='); eq != std::string::npos && line[0] != '#')
            c[line.substr(0, eq)] = line.substr(eq + 1);
    return c;
}

void saveConfig(const Config &c)
{
    std::error_code ec;
    fs::create_directories(configDir(), ec);
    std::ofstream out(configDir() / "launcher.ini");
    out << "# RC Revenge Pro: Re-Amped launcher (rc-launcher --choose-disc picks another disc image)\n";
    for (const auto &[k, v] : c)
        out << k << '=' << v << '\n';
}

// ---- Dialogs ----
bool g_sdl = false;
#ifndef _WIN32
volatile pid_t g_child = 0;
void forwardSignal(int sig)
{
    if (g_child > 0)
        kill(g_child, sig);
}
#endif
bool g_quiet = false; // --disc: no dialogs, messages to the terminal
void ensureSdl()
{
    if (!g_sdl)
        g_sdl = SDL_Init(SDL_INIT_VIDEO);
}

// Buttons from left to right; returns the index pressed, -1 if closed.
int ask(SDL_MessageBoxFlags kind, const std::string &text, std::initializer_list<const char *> buttons)
{
    if (g_quiet)
    {
        log(text);
        return buttons.size() == 1 ? 0 : -1;
    }
    ensureSdl();
    std::vector<SDL_MessageBoxButtonData> data;
    int id = 0;
    for (const char *b : buttons)
    {
        SDL_MessageBoxButtonFlags flags = 0;
        if (id == 0)
            flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        if (id == static_cast<int>(buttons.size()) - 1)
            flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        data.push_back({flags, id++, b});
    }
    const SDL_MessageBoxData box = {kind, nullptr, kTitle, text.c_str(), static_cast<int>(data.size()), data.data(), nullptr};
    int pressed = -1;
    if (!SDL_ShowMessageBox(&box, &pressed))
    {
        log("no message box (" + std::string(SDL_GetError()) + "): " + text);
        return -1;
    }
    return pressed;
}

void tell(SDL_MessageBoxFlags kind, const std::string &text) { ask(kind, text, {"OK"}); }

struct FileChoice
{
    std::atomic<bool> done{false};
    std::string path; // empty: cancelled
};

void SDLCALL fileChosen(void *user, const char *const *files, int)
{
    auto *choice = static_cast<FileChoice *>(user);
    if (!files)
        log(std::string("file dialog failed: ") + SDL_GetError());
    else if (files[0])
        choice->path = files[0];
    choice->done = true;
}

std::string chooseFile()
{
    ensureSdl();
    static const SDL_DialogFileFilter filters[] = {{"PS2 disc images (.iso, .bin, .cue)", "iso;bin;cue;ISO;BIN;CUE"}, {"All files", "*"}};
    FileChoice choice;
#ifdef _WIN32
    const char *home = std::getenv("USERPROFILE");
#else
    const char *home = std::getenv("HOME");
#endif
    SDL_ShowOpenFileDialog(fileChosen, &choice, nullptr, filters, 2, home, false);
    while (!choice.done)
    {
        SDL_Event e;
        SDL_WaitEventTimeout(&e, 50);
    }
    return choice.path;
}

// A small window with a message and a progress bar while `work` runs on another thread.
template <typename Work>
bool withProgress(const std::string &message, std::atomic<float> &progress, Work work)
{
    ensureSdl();
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_CreateWindowAndRenderer(kTitle, 560, 120, 0, &window, &renderer);
    std::atomic<bool> finished{false};
    bool result = false;
    std::thread worker([&] { result = work(); finished = true; });
    while (!finished)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        if (renderer)
        {
            SDL_SetRenderDrawColor(renderer, 24, 26, 32, 255);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawColor(renderer, 230, 230, 235, 255);
            SDL_SetRenderScale(renderer, 2.0f, 2.0f);
            SDL_RenderDebugText(renderer, 10, 12, message.c_str());
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);
            const SDL_FRect frame = {20, 64, 520, 24}, bar = {22, 66, 516 * std::clamp(progress.load(), 0.0f, 1.0f), 20};
            SDL_RenderRect(renderer, &frame);
            SDL_SetRenderDrawColor(renderer, 220, 70, 40, 255);
            SDL_RenderFillRect(renderer, &bar);
            SDL_RenderPresent(renderer);
        }
        SDL_Delay(30);
    }
    worker.join();
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    return result;
}

// ---- Setting a release up from a disc image ----
Config g_config; // the launcher's settings, loaded in main
fs::path isoLink(const Release &r) { return dataDir(r) / r.isoName; }   // where the game looks: beside game/
// The image the game reads: on Windows an .iso is read where it is (its path in the settings), else isoLink.
fs::path gameImage(const Release &r)
{
#ifdef _WIN32
    if (const auto it = g_config.find(std::string("image_") + r.id); it != g_config.end() && !it->second.empty())
        return fromUtf8(it->second);
#endif
    return isoLink(r);
}
fs::path elfPath(const Release &r) { return dataDir(r) / "game" / r.elf; }

std::string fileSha1(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return in.bad() || data.empty() ? std::string() : launcher::sha1Hex(data.data(), data.size());
}

// Whether the release is ready: its executable copied and unchanged, and the disc image still there and still it.
bool ready(const Release &r)
{
    std::error_code ec;
    if (!fs::exists(gameImage(r), ec) || fileSha1(elfPath(r)) != r.sha1)
        return false;
    DiscImage image(gameImage(r));
    return image.ok() && image.bootName() == r.elf;
}

// Sets up the release on the disc image; returns it, or nullptr after telling the player why not.
const Release *prepare(const std::string &chosen, Config &config)
{
    DiscImage image(fromUtf8(chosen)); // SDL gives UTF-8
    if (!image.ok())
    {
        tell(SDL_MESSAGEBOX_ERROR, "That file cannot be used:\n" + image.error());
        return nullptr;
    }
    const std::string boot = image.bootName();
    const Release *release = nullptr;
    for (const Release &r : kReleases)
        if (boot == r.elf)
            release = &r;
    if (!release)
    {
        tell(SDL_MESSAGEBOX_ERROR, boot.empty() ? "That file cannot be used:\n" + image.error()
                                                : "That disc is " + boot + ", not RC Revenge Pro.\nThe USA (SLUS-20153) and Europe (SLES-50077) releases are supported.");
        return nullptr;
    }
    const auto elf = image.readFile(release->elf);
    if (!elf || launcher::sha1Hex(elf->data(), elf->size()) != release->sha1)
    {
        tell(SDL_MESSAGEBOX_ERROR, std::string("This is RC Revenge Pro ") + release->label +
                                       ", but its executable differs from the release this port is made for (a different printing, or a damaged or modified image).");
        return nullptr;
    }
    const fs::path dir = dataDir(*release);
    std::error_code ec;
#ifdef _WIN32
    for (const char *sub : {"game", "logs"}) // the cards are folders inside game\ (below)
#else
    for (const char *sub : {"game", "mc0", "mc1", "logs"})
#endif
        fs::create_directories(dir / sub, ec);
    {
        std::ofstream out(elfPath(*release), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(elf->data()), static_cast<std::streamsize>(elf->size()));
        if (!out)
        {
            tell(SDL_MESSAGEBOX_ERROR, "Could not write " + elfPath(*release).string());
            return nullptr;
        }
    }
    // The game keeps its memory cards beside its executable: link them to the release's folder (shared with tools/play.sh).
#ifdef _WIN32
    for (const char *card : {"mc0", "mc1"}) // real folders there (links need privileges on Windows)
        fs::create_directories(dir / "game" / card, ec);
#else
    for (const char *card : {"mc0", "mc1"})
        if (const fs::path link = dir / "game" / card; !fs::exists(fs::symlink_status(link, ec)))
            fs::create_directory_symlink(fs::path("..") / card, link, ec);
#endif

    const fs::path iso = isoLink(*release);
    fs::remove(iso, ec);
    config[std::string("image_") + release->id] = "";
    if (!image.raw())
    {
#ifdef _WIN32
        config[std::string("image_") + release->id] = utf8(image.path());
#else
        fs::create_symlink(image.path(), iso, ec);
        if (ec)
        {
            tell(SDL_MESSAGEBOX_ERROR, "Could not link the disc image: " + ec.message());
            return nullptr;
        }
#endif
    }
    else // the game reads 2048-byte sectors: convert a raw .bin once
    {
        const uint64_t need = image.sectors() * 2048ull;
        const fs::space_info space = fs::space(dir, ec);
        if (!ec && space.available < need + (64ull << 20))
        {
            tell(SDL_MESSAGEBOX_ERROR, "A .bin image is converted to an .iso once, which needs " + std::to_string(need >> 20) +
                                           " MB free in\n" + dir.string() + "\n(" + std::to_string(space.available >> 20) + " MB free).");
            return nullptr;
        }
        std::atomic<float> progress{0.0f};
        const fs::path temp = iso.string() + ".part";
        const bool done = withProgress("Converting the disc image (once)...", progress, [&] { return image.writeIso(temp, progress); });
        if (!done || (fs::rename(temp, iso, ec), ec))
        {
            fs::remove(temp, ec);
            tell(SDL_MESSAGEBOX_ERROR, "Could not convert the disc image: " + (done ? ec.message() : image.error()));
            return nullptr;
        }
    }
    config[std::string("disc_") + release->id] = utf8(fs::absolute(fromUtf8(chosen)));
    config["version"] = release->id;
    saveConfig(config);
    log(std::string("set up ") + release->label + " from " + chosen);
    return release;
}

// ---- Starting the game ----
fs::path exeDir()
{
#ifdef _WIN32
    static wchar_t buffer[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, 32768);
    return n > 0 && n < 32768 ? fs::path(std::wstring(buffer, n)).parent_path() : fs::current_path();
#else
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : self.parent_path();
#endif
}

#ifdef _WIN32
constexpr const char *kExe = ".exe";
#else
constexpr const char *kExe = "";
#endif

std::optional<fs::path> gameBinary(const Release &r)
{
    if (const char *v = std::getenv("RC_GAME_BINARY"); v && *v)
        return fs::path(v);
    const fs::path here = exeDir();
    const fs::path candidates[] = {
        here / (std::string("rc_revenge_pro_") + r.id + kExe), // a release
        here.parent_path() / (std::string(r.id) == "eu" ? "build-eu" : "build-decomp") / (std::string("rc_revenge_pro") + kExe), // the source tree
    };
    for (const fs::path &p : candidates)
    {
#ifdef _WIN32
        std::error_code ec;
        if (fs::is_regular_file(p, ec))
            return p;
#else
        if (access(p.c_str(), X_OK) == 0)
            return p;
#endif
    }
    return std::nullopt;
}

// The discrete NVIDIA GPU through PRIME render offload when its driver is there and healthy (as tools/play.sh does).
bool chooseGpu()
{
#ifdef _WIN32
    return true; // Windows picks the GPU itself (the game exports NvOptimusEnablement for hybrid laptops)
#else
    if (const char *v = std::getenv("RC_GPU"); v && std::string(v) == "default")
        return true;
    if (std::system("command -v nvidia-smi >/dev/null 2>&1") != 0)
        return true;
    std::string out;
    int status = -1;
    if (std::FILE *p = popen("timeout 15 nvidia-smi 2>&1", "r"))
    {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0)
            out.append(buf, n);
        status = pclose(p);
    }
    std::string problem;
    if (status != 0)
        problem = "nvidia-smi failed";
    for (const char *bad : {"requires reset", "ERR!", "Unknown Error", "No devices were found", "has fallen off the bus"})
        if (out.find(bad) != std::string::npos)
            problem = "the driver reports errors";
    if (problem.empty())
    {
        setEnv("__NV_PRIME_RENDER_OFFLOAD", "1", false);
        setEnv("__VK_LAYER_NV_optimus", "NVIDIA_only", false);
        setEnv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", false);
        return true;
    }
    log("NVIDIA GPU not usable: " + problem);
    return ask(SDL_MESSAGEBOX_WARNING,
               "The NVIDIA graphics card is not working (" + problem + "). Restarting the computer usually resets it.\n\n"
               "The game can run on the other graphics adapter instead, probably more slowly.",
               {"Use the other adapter", "Quit"}) == 0;
#endif
}

#ifdef _WIN32
// Starts the game with its output in the log file and the release's folder as its working folder, and waits for it. A
// job object ties the game to the launcher: closing the launcher closes the game.
int runGame(const Release &r, const fs::path &binary, const fs::path &logFile)
{
    SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE out = CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    if (out != INVALID_HANDLE_VALUE)
    {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = out;
        si.hStdError = out;
    }
    std::wstring command = L"\"" + binary.wstring() + L"\" \"" + elfPath(r).wstring() + L"\"";
    const std::wstring folder = dataDir(r).wstring();
    PROCESS_INFORMATION pi = {};
    const BOOL started = CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &si, &pi);
    const DWORD startError = GetLastError();
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    if (!started)
    {
        tell(SDL_MESSAGEBOX_ERROR, "Could not start the game (Windows error " + std::to_string(startError) + ").");
        return 1;
    }
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr))
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(job, pi.hProcess); // the handle closes with the launcher, and the game with it
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code >= 0xC0000000u) // an exception status (access violation, stack overflow, ...), not an exit code
    {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(code));
        log("the game stopped with status " + std::string(hex));
        tell(SDL_MESSAGEBOX_ERROR, std::string("The game stopped unexpectedly (status ") + hex + ").\n\nIts log is in\n" +
                                       utf8(logFile) + "\nPlease include it when reporting the problem.");
        return 1;
    }
    return static_cast<int>(code);
}
#endif

int play(const Release &r)
{
    const auto binary = gameBinary(r);
    if (!binary)
    {
        tell(SDL_MESSAGEBOX_ERROR, std::string("The game program for the ") + r.label + " release is missing (rc_revenge_pro_" + r.id +
                                       " beside the launcher). Reinstalling should bring it back.");
        return 1;
    }
    if (!chooseGpu())
        return 1;
    // What tools/play.sh sets for playing; anything already in the environment wins.
    const fs::path shaders = cacheDir(r) / "shaders";
    std::error_code ec;
    fs::create_directories(shaders, ec);
    const std::pair<const char *, std::string> defaults[] = {
        {"PS2X_DEBUG_PANEL", "0"}, {"PS2X_GS_GPU", "1"}, {"PS2X_GS_HW", "1"}, {"PS2X_GS_THREAD", "1"}, {"PS2X_MTVU", "1"},
        {"PS2X_VU1_TIMING", "0"}, {"PS2X_EE_HOST_PACING", "1"}, {"PS2X_GS_SHADER_PRELOAD", "0"}, {"PS2X_GS_SHADER_CACHE", "1"},
        {"PS2X_GS_SHADER_CACHE_DIR", shaders.string()}, {"RC_TRACK_SHADER", "1"}, {"RC_VU1_REF", "0"},
    };
    for (const auto &[name, value] : defaults)
        setEnv(name, value, false);
#ifdef _WIN32
    setEnv("RC_DISC_IMAGE", utf8(gameImage(r)), true);
#endif

    const fs::path logs = dataDir(r) / "logs", logFile = logs / "session.log";
    fs::create_directories(logs, ec);
    fs::rename(logFile, logs / "session.prev.log", ec);
    log("starting " + utf8(*binary) + " (" + r.label + "), output in " + logFile.string());

    if (g_sdl)
    {
        SDL_Quit();
        g_sdl = false;
    }
#ifdef _WIN32
    return runGame(r, *binary, logFile);
#else
    const pid_t child = fork();
    if (child == 0)
    {
        if (const int fd = open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); fd >= 0)
        {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (chdir(dataDir(r).c_str()) != 0) {}
        const std::string elf = elfPath(r).string(), bin = binary->string();
        execl(bin.c_str(), bin.c_str(), elf.c_str(), static_cast<char *>(nullptr));
        std::perror("exec");
        _exit(127);
    }
    if (child < 0)
    {
        tell(SDL_MESSAGEBOX_ERROR, std::string("Could not start the game: ") + std::strerror(errno));
        return 1;
    }
    std::signal(SIGINT, SIG_IGN); // Ctrl+C in a terminal stops the game; the launcher then reports as usual
    g_child = child;               // stopping the launcher stops the game
    std::signal(SIGTERM, forwardSignal);
    std::signal(SIGHUP, forwardSignal);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (WIFSIGNALED(status) && WTERMSIG(status) != SIGINT && WTERMSIG(status) != SIGTERM && WTERMSIG(status) != SIGKILL)
    {
        log("the game stopped with signal " + std::to_string(WTERMSIG(status)));
        tell(SDL_MESSAGEBOX_ERROR, std::string("The game stopped unexpectedly (") + strsignal(WTERMSIG(status)) +
                                       ").\n\nIts log is in\n" + logFile.string() + "\nPlease include it when reporting the problem.");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
    // A windowed program has no console: use the one it was started from, if any, for the terminal messages.
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
    // The arguments as UTF-8 (argv is in the ANSI code page, which cannot hold every path).
    std::vector<std::string> utf8Args;
    std::vector<char *> utf8Argv;
    int wargc = 0;
    if (LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc))
    {
        for (int i = 0; i < wargc; ++i)
            utf8Args.push_back(utf8(fs::path(wargv[i])));
        LocalFree(wargv);
        for (std::string &a : utf8Args)
            utf8Argv.push_back(a.data());
        utf8Argv.push_back(nullptr);
        argc = static_cast<int>(utf8Args.size());
        argv = utf8Argv.data();
    }
#endif
    bool chooseDisc = false, setupOnly = false;
    std::string wanted, disc, image;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--choose-disc")
            chooseDisc = true;
        else if (a == "--version" && i + 1 < argc)
            wanted = argv[++i];
        else if (a == "--disc" && i + 1 < argc)
            disc = argv[++i];
        else if (a == "--setup-only")
            setupOnly = true;
        else if (!a.empty() && a[0] != '-' && image.empty())
            image = a; // a disc image to play, as a launcher such as Quiver may pass the player's game copy
        else
        {
            std::cout << "usage: " << argv[0] << " [--choose-disc | --disc <image> [--setup-only]] [--version usa|eu] [<disc image>]\n";
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    g_config = loadConfig();
    Config &config = g_config;
    const Release *release = releaseById(wanted.empty() ? config["version"] : wanted);
    if (!wanted.empty() && !release)
    {
        std::cerr << "--version is usa or eu\n";
        return 2;
    }
    if (!image.empty() && disc.empty())
    {
        // Already set up from this image and still ready: play it; else set it up (with dialogs: a player started this).
        const std::string wantedImage = utf8(fs::absolute(fromUtf8(image)));
        const Release *same = nullptr;
        for (const Release &r : kReleases)
            if (const auto it = config.find(std::string("disc_") + r.id); it != config.end() && it->second == wantedImage && ready(r))
                same = &r;
        release = same ? same : prepare(image, config);
        if (!release)
            return 1;
        if (same && config["version"] != same->id)
        {
            config["version"] = same->id;
            saveConfig(config);
        }
        return play(*release);
    }
    if (!disc.empty())
    {
        g_quiet = true;
        release = prepare(disc, config);
        if (!release)
            return 1;
        if (setupOnly)
            return 0;
    }
    else if (chooseDisc || !release || !ready(*release))
    {
        std::string intro = "Welcome to RC Revenge Pro: Re-Amped.\n\nIt plays from your own copy of RC Revenge Pro for the PlayStation 2: "
                            "a disc image (.iso, or .bin/.cue) of the USA or Europe release.\nNothing is copied from it except the game's "
                            "executable, kept with your saves.";
        if (release && !chooseDisc)
            intro = std::string("The ") + release->label + " disc image set up before is missing or has changed:\n" +
                    config[std::string("disc_") + release->id] + "\n\nChoose it again, or another image.";
        for (release = nullptr; !release;)
        {
            if (ask(SDL_MESSAGEBOX_INFORMATION, intro, {"Choose disc image...", "Quit"}) != 0)
                return 0;
            const std::string chosen = chooseFile();
            if (chosen.empty())
                continue;
            release = prepare(chosen, config);
            intro = "Choose your RC Revenge Pro disc image (.iso, or .bin/.cue), USA or Europe release.";
        }
    }
    return play(*release);
}
