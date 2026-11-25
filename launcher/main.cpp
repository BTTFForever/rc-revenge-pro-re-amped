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
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

void log(const std::string &line) { std::cerr << "[launcher] " << line << std::endl; }

// ---- Folders and the launcher's settings ----
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
volatile pid_t g_child = 0;
void forwardSignal(int sig)
{
    if (g_child > 0)
        kill(g_child, sig);
}
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
    const char *home = std::getenv("HOME");
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
fs::path isoLink(const Release &r) { return dataDir(r) / r.isoName; }   // where the game looks: beside game/
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
    if (!fs::exists(isoLink(r), ec) || fileSha1(elfPath(r)) != r.sha1)
        return false;
    DiscImage image(isoLink(r));
    return image.ok() && image.bootName() == r.elf;
}

// Sets up the release on the disc image; returns it, or nullptr after telling the player why not.
const Release *prepare(const std::string &chosen, Config &config)
{
    DiscImage image(chosen);
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
    for (const char *sub : {"game", "mc0", "mc1", "logs"})
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
    for (const char *card : {"mc0", "mc1"})
        if (const fs::path link = dir / "game" / card; !fs::exists(fs::symlink_status(link, ec)))
            fs::create_directory_symlink(fs::path("..") / card, link, ec);

    const fs::path iso = isoLink(*release);
    fs::remove(iso, ec);
    if (!image.raw())
    {
        fs::create_symlink(image.path(), iso, ec);
        if (ec)
        {
            tell(SDL_MESSAGEBOX_ERROR, "Could not link the disc image: " + ec.message());
            return nullptr;
        }
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
    config[std::string("disc_") + release->id] = fs::absolute(chosen).string();
    config["version"] = release->id;
    saveConfig(config);
    log(std::string("set up ") + release->label + " from " + chosen);
    return release;
}

// ---- Starting the game ----
fs::path exeDir()
{
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : self.parent_path();
}

std::optional<fs::path> gameBinary(const Release &r)
{
    if (const char *v = std::getenv("RC_GAME_BINARY"); v && *v)
        return fs::path(v);
    const fs::path here = exeDir();
    const fs::path candidates[] = {
        here / (std::string("rc_revenge_pro_") + r.id),                                                 // a release
        here.parent_path() / (std::string(r.id) == "eu" ? "build-eu" : "build-decomp") / "rc_revenge_pro", // the source tree
    };
    for (const fs::path &p : candidates)
        if (access(p.c_str(), X_OK) == 0)
            return p;
    return std::nullopt;
}

// The discrete NVIDIA GPU through PRIME render offload when its driver is there and healthy (as tools/play.sh does).
bool chooseGpu()
{
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
        setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
        setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 0);
        setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
        return true;
    }
    log("NVIDIA GPU not usable: " + problem);
    return ask(SDL_MESSAGEBOX_WARNING,
               "The NVIDIA graphics card is not working (" + problem + "). Restarting the computer usually resets it.\n\n"
               "The game can run on the other graphics adapter instead, probably more slowly.",
               {"Use the other adapter", "Quit"}) == 0;
}

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
        setenv(name, value.c_str(), 0);

    const fs::path logs = dataDir(r) / "logs", logFile = logs / "session.log";
    fs::create_directories(logs, ec);
    fs::rename(logFile, logs / "session.prev.log", ec);
    log("starting " + binary->string() + " (" + r.label + "), output in " + logFile.string());

    if (g_sdl)
    {
        SDL_Quit();
        g_sdl = false;
    }
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
}

} // namespace

int main(int argc, char **argv)
{
    bool chooseDisc = false, setupOnly = false;
    std::string wanted, disc;
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
        else
        {
            std::cout << "usage: " << argv[0] << " [--choose-disc | --disc <image> [--setup-only]] [--version usa|eu]\n";
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    Config config = loadConfig();
    const Release *release = releaseById(wanted.empty() ? config["version"] : wanted);
    if (!wanted.empty() && !release)
    {
        std::cerr << "--version is usa or eu\n";
        return 2;
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
