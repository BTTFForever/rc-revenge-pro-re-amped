// The offline oracle: runs the random test of decompiled functions (rc::fuzzCase, the same code as RC_SHADOW_FUZZ) on
// RAM dumps taken from the running game (RC_RAMDUMP), without starting the game. The recompiled originals and the
// native functions are both linked in, so no interpreter is needed. No game thread, window, interrupt or timer runs,
// so results are deterministic.
//
// Each (dump, function) pair runs in a child process forked from the loaded dump, so a function whose original needs
// hardware or a guest wait (which cannot work offline) only ends its own child: it is reported with a reason.
//
//   rc_oracle --elf <SLUS_201.53> --dump <file> [--dump <file>...] [--only a,b] [--cases N] [--seed S] [--jobs J]
//             [--timeout seconds] [--verbose]
// Prints one line per function and a summary; exits 0 if nothing failed, 1 otherwise. tools/oracle.sh wraps it.

#include "rc_decomp.h"
#include "ram_dump.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct Options
{
    std::string elf;
    std::vector<std::string> dumps;
    std::string only;
    int cases = 300;
    uint64_t seed = 0;
    int jobs = std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)) - 1);
    int timeout = 20;
    bool verbose = false;
    std::string known; // file of functions whose differences are known artefacts, one "name reason" per line
};

struct Dump
{
    std::string path;
    std::string label;
    rc::RamDumpHeader header{};
    R5900Context context{};
};

// What one child found for one function on one dump.
enum class Outcome { Ok, Fail, Skip };
struct JobResult
{
    Outcome outcome = Outcome::Skip;
    int cases = 0;
    uint64_t mismatches = 0;
    std::string detail; // first differences, or why it was skipped
    double seconds = 0;
};

bool loadDump(PS2Runtime &runtime, Dump &dump)
{
    std::ifstream in(dump.path, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(&dump.header), sizeof(dump.header)) ||
        std::memcmp(dump.header.magic, rc::kRamDumpMagic, sizeof(rc::kRamDumpMagic)) != 0)
    {
        std::fprintf(stderr, "oracle: %s is not a RAM dump\n", dump.path.c_str());
        return false;
    }
    if (dump.header.contextSize != sizeof(R5900Context))
    {
        std::fprintf(stderr, "oracle: %s was written by a build with another R5900Context layout; capture it again\n", dump.path.c_str());
        return false;
    }
    PS2Memory &memory = runtime.memory();
    in.read(reinterpret_cast<char *>(&dump.context), sizeof(R5900Context));
    in.read(reinterpret_cast<char *>(memory.getRDRAM()), PS2_RAM_SIZE);
    in.read(reinterpret_cast<char *>(memory.getScratchpad()), PS2_SCRATCHPAD_SIZE);
    in.read(reinterpret_cast<char *>(memory.getVU0Code()), PS2_VU0_CODE_SIZE);
    in.read(reinterpret_cast<char *>(memory.getVU0Data()), PS2_VU0_DATA_SIZE);
    if (!in)
    {
        std::fprintf(stderr, "oracle: %s is truncated\n", dump.path.c_str());
        return false;
    }
    // The dump's thread becomes the running main thread, so native code's runningContext() (guest_call, GuestFrame,
    // stale_stack) sees its registers, as in the game.
    EeScheduler &scheduler = runtime.eeScheduler();
    scheduler.reset(memory.getRDRAM(), dump.context);
    scheduler.bindMainContextForSyscall(dump.context, memory.getRDRAM());
    return true;
}

// --- The child: one function on the loaded dump. ---

// Which run a crash or timeout happened in, for the report. Set by the trampolines around the entry's two functions.
volatile sig_atomic_t g_phase = 0; // 0 setup/harness, 1 original, 2 native
rc::RecompFn g_realOriginal = nullptr, g_realNative = nullptr;

void originalTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_phase = 1;
    g_realOriginal(rdram, ctx, runtime);
    g_phase = 0;
}

void nativeTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_phase = 2;
    g_realNative(rdram, ctx, runtime);
    g_phase = 0;
}

void writeRaw(const char *text)
{
    ssize_t ignored = write(STDERR_FILENO, text, std::strlen(text));
    (void)ignored;
}

void onFatalSignal(int signal)
{
    static const char *const kPhases[] = {"setup", "original", "native"};
    char line[96];
    std::snprintf(line, sizeof(line), "\n@@PHASE %s %d\n", kPhases[g_phase], signal);
    writeRaw(line);
    _exit(3);
}

[[noreturn]] void runChild(rc::Entry &entry, PS2Runtime &runtime, const Options &options, const std::string &logPath)
{
    const int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(log, STDERR_FILENO);
    dup2(log, STDOUT_FILENO);
    close(log);
    // A runaway log (a cut-short original's resume loop) stops growing at 4 MB instead of filling the disk.
    const rlimit size{4u << 20, 4u << 20};
    setrlimit(RLIMIT_FSIZE, &size);
    std::signal(SIGXFSZ, SIG_IGN);
    for (const int signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGALRM, SIGTRAP})
        std::signal(signal, onFatalSignal);
    alarm(static_cast<unsigned>(options.timeout));

    g_realOriginal = entry.original;
    g_realNative = entry.native;
    entry.original = &originalTrampoline;
    entry.native = &nativeTrampoline;

    uint8_t *rdram = runtime.memory().getRDRAM();
    const R5900Context live = runtime.eeScheduler().runningContext();
    std::unique_ptr<uint8_t[]> backup;
    if (entry.fuzzWholeRam)
    {
        backup.reset(new uint8_t[PS2_RAM_SIZE]);
        std::memcpy(backup.get(), rdram, PS2_RAM_SIZE);
    }
    const int inputs = entry.fuzzWholeRam ? std::min(options.cases, rc::kFuzzWholeRamInputs) : options.cases;
    // The live test's generator start, so a seed of 0 gives the inputs RC_SHADOW_FUZZ gives.
    uint64_t state = 0x5EEDu + entry.address + options.seed * 0x9E3779B97F4A7C15ull;
    uint64_t mismatches = 0, cutShort = 0, nonZero = 0, wrote = 0;
    int done = 0;
    try
    {
        for (; done < inputs; ++done)
        {
            const rc::FuzzCaseResult result = rc::fuzzCase(entry, rdram, live, &runtime, state, backup.get(), done, mismatches);
            if (result.cutShort)
            {
                ++cutShort;
                break; // Offline nothing can cut an original short but a wait, which no later input will avoid.
            }
            mismatches += result.same ? 0u : 1u;
            nonZero += result.returnedNonZero ? 1u : 0u;
            wrote += result.wroteMemory ? 1u : 0u;
        }
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "\n@@PHASE %s exception: %s\n", g_phase == 2 ? "native" : g_phase == 1 ? "original" : "setup", error.what());
        std::fflush(stderr);
        _exit(3);
    }
    catch (...)
    {
        std::fprintf(stderr, "\n@@PHASE %s exception\n", g_phase == 2 ? "native" : g_phase == 1 ? "original" : "setup");
        std::fflush(stderr);
        _exit(3);
    }
    std::fprintf(stderr, "\n@@RESULT %d %llu %llu %llu %llu\n", done, (unsigned long long)mismatches, (unsigned long long)cutShort,
                 (unsigned long long)nonZero, (unsigned long long)wrote);
    std::fflush(stderr);
    _exit(0);
}

// --- The parent. ---

std::string readLog(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

// The lines that explain a failure: the harness's FUZZ lines, else the last runtime message before a crash.
std::string explain(const std::string &log, bool crashed)
{
    std::string out;
    size_t lines = 0, pos = 0;
    std::string lastOther;
    while (pos < log.size())
    {
        size_t end = log.find('\n', pos);
        if (end == std::string::npos)
            end = log.size();
        std::string line = log.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty() || line.rfind("@@", 0) == 0)
            continue;
        if (line.find("FUZZ ") != std::string::npos)
        {
            if (lines++ < 3)
                out += "\n      " + line.substr(0, 220);
        }
        else
            lastOther = line.substr(0, 200);
    }
    if (crashed && !lastOther.empty())
        out += "\n      last message: " + lastOther;
    return out;
}

JobResult parseResult(const std::string &log, int status, int cases)
{
    JobResult result;
    const size_t at = log.rfind("@@RESULT ");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && at != std::string::npos)
    {
        unsigned long long mismatches = 0, cut = 0, nonZero = 0, wrote = 0;
        int done = 0;
        std::sscanf(log.c_str() + at, "@@RESULT %d %llu %llu %llu %llu", &done, &mismatches, &cut, &nonZero, &wrote);
        result.cases = done;
        result.mismatches = mismatches;
        if (cut != 0u)
        {
            // An original that waits (a semaphore, vsync, an IOP reply) cannot finish without the game's threads.
            result.outcome = Outcome::Skip;
            result.detail = "original did not return (waits on another thread or the hardware)";
        }
        else if (mismatches != 0u)
        {
            result.outcome = Outcome::Fail;
            result.detail = std::to_string(mismatches) + " of " + std::to_string(done) + " inputs differ" + explain(log, false);
        }
        else
            result.outcome = Outcome::Ok;
        (void)cases;
        return result;
    }
    // Crashed, timed out or threw. Where decides the verdict: in the setup or the original, it cannot run offline; in
    // the native function, while the original ran fine, it is a failure.
    std::string phase = "setup", what;
    const size_t mark = log.rfind("@@PHASE ");
    if (mark != std::string::npos)
    {
        const size_t end = log.find('\n', mark);
        const std::string line = log.substr(mark + 8, end == std::string::npos ? std::string::npos : end - mark - 8);
        phase = line.substr(0, line.find(' '));
        const std::string rest = line.substr(line.find(' ') + 1);
        const int signal = std::atoi(rest.c_str());
        what = rest.rfind("exception", 0) == 0 ? rest
               : signal == SIGALRM            ? "timed out"
               : signal != 0                  ? std::string(strsignal(signal))
                                              : rest;
    }
    else if (WIFSIGNALED(status))
        what = strsignal(WTERMSIG(status));
    else
        what = "exited " + std::to_string(WEXITSTATUS(status));
    result.outcome = phase == "native" ? Outcome::Fail : Outcome::Skip;
    result.detail = (phase == "native" ? "native " : phase == "original" ? "original " : "setup ") + what + explain(log, true);
    return result;
}

std::vector<std::string> split(const std::string &list)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size())
    {
        const size_t end = std::min(list.find(',', pos), list.size());
        if (end > pos)
            out.push_back(list.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

void usage()
{
    std::fprintf(stderr, "usage: rc_oracle --elf <SLUS_201.53> --dump <file> [--dump <file>...] [--only a,b] [--cases N] "
                         "[--seed S] [--jobs J] [--timeout s] [--verbose]\n");
    std::exit(2);
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string
        {
            if (i + 1 >= argc)
                usage();
            return argv[++i];
        };
        if (arg == "--elf")
            options.elf = value();
        else if (arg == "--dump")
            options.dumps.push_back(value());
        else if (arg == "--only")
            options.only = value();
        else if (arg == "--cases")
            options.cases = std::max(1, std::atoi(value().c_str()));
        else if (arg == "--seed")
            options.seed = std::strtoull(value().c_str(), nullptr, 0);
        else if (arg == "--jobs")
            options.jobs = std::max(1, std::atoi(value().c_str()));
        else if (arg == "--timeout")
            options.timeout = std::max(1, std::atoi(value().c_str()));
        else if (arg == "--known")
            options.known = value();
        else if (arg == "--verbose")
            options.verbose = true;
        else
            usage();
    }
    if (options.elf.empty() || options.dumps.empty())
        usage();
    const auto started = std::chrono::steady_clock::now();

    // The runtime without a window, audio, game thread or kernel boot: memory, the GS/VIF plumbing the hardware
    // registers lead to, and the ELF with the game's overrides. The dump then replaces all of guest memory.
    // Its messages are only shown with --verbose.
    std::fflush(stdout);
    const int savedOut = dup(STDOUT_FILENO), savedErr = dup(STDERR_FILENO);
    if (!options.verbose)
    {
        const int null = open("/dev/null", O_WRONLY);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        close(null);
    }
    static PS2Runtime runtime;
    const bool ready = runtime.memory().initialize() && runtime.syncCoreSubsystems() && runtime.loadELF(options.elf);
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    dup2(savedOut, STDOUT_FILENO);
    dup2(savedErr, STDERR_FILENO);
    close(savedOut);
    close(savedErr);
    if (!ready)
    {
        std::fprintf(stderr, "oracle: could not set up the runtime\n");
        return 2;
    }
    rc::installForOracle(runtime);

    std::vector<rc::Entry *> chosen, noSetup;
    const std::vector<std::string> only = split(options.only);
    for (rc::Entry *entry : rc::registeredEntries())
    {
        if (!only.empty() && std::find(only.begin(), only.end(), entry->name) == only.end())
            continue;
        (entry->randomiseArgs ? chosen : noSetup).push_back(entry);
    }
    for (const std::string &name : only)
    {
        const auto known = [&](const rc::Entry *entry) { return name == entry->name; };
        if (std::none_of(chosen.begin(), chosen.end(), known) && std::none_of(noSetup.begin(), noSetup.end(), known))
        {
            std::fprintf(stderr, "oracle: no registered function %s\n", name.c_str());
            return 2;
        }
    }

    const std::filesystem::path logDir = std::filesystem::temp_directory_path() / ("rc_oracle." + std::to_string(getpid()));
    std::filesystem::create_directories(logDir);
    std::map<rc::Entry *, std::vector<std::pair<size_t, JobResult>>> results;

    // One queue of (dump, function) jobs. A child keeps the memory it was forked with, so the next dump is loaded as soon
    // as the last job of the previous one has started: a slow function does not hold up the dumps after it.
    struct Running
    {
        rc::Entry *entry;
        size_t dump;
        std::chrono::steady_clock::time_point started;
    };
    std::vector<std::string> labels;
    for (const std::string &path : options.dumps)
        labels.push_back(std::filesystem::path(path).stem().string());
    std::map<pid_t, Running> running;
    const auto logFor = [&](const rc::Entry *entry, size_t dump)
    { return (logDir / (labels[dump] + "." + std::string(entry->name) + ".log")).string(); };
    size_t dumpIndex = 0, next = chosen.size(); // next == chosen.size(): load the next dump first
    bool loaded = false;
    for (;;)
    {
        while (running.size() < static_cast<size_t>(options.jobs))
        {
            if (next >= chosen.size())
            {
                if (loaded)
                    ++dumpIndex;
                if (dumpIndex >= options.dumps.size())
                    break;
                Dump dump;
                dump.path = options.dumps[dumpIndex];
                if (!loadDump(runtime, dump))
                    return 2;
                loaded = true;
                next = 0;
                if (options.verbose)
                    std::fprintf(stderr, "oracle: %s level %u frame %llu sp 0x%x gp 0x%x\n", labels[dumpIndex].c_str(), dump.header.level,
                                 (unsigned long long)dump.header.frame, getRegU32(&dump.context, 29), getRegU32(&dump.context, 28));
                std::fflush(stdout);
                std::fflush(stderr);
                if (chosen.empty())
                    continue;
            }
            rc::Entry *entry = chosen[next++];
            const pid_t pid = fork();
            if (pid == 0)
                runChild(*entry, runtime, options, logFor(entry, dumpIndex));
            if (pid < 0)
            {
                std::perror("fork");
                return 2;
            }
            running[pid] = {entry, dumpIndex, std::chrono::steady_clock::now()};
        }
        if (running.empty())
            break;
        int status = 0;
        const pid_t done = waitpid(-1, &status, 0);
        if (done < 0)
            break;
        const auto found = running.find(done);
        if (found == running.end())
            continue;
        const Running job = found->second;
        running.erase(found);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count();
        const std::string log = readLog(logFor(job.entry, job.dump));
        results[job.entry].emplace_back(job.dump, parseResult(log, status, options.cases));
        results[job.entry].back().second.seconds = seconds;
        if (options.verbose)
            std::fprintf(stderr, "oracle: %s %s %.2f s\n", labels[job.dump].c_str(), job.entry->name, seconds);
        std::filesystem::remove(logFor(job.entry, job.dump));
    }
    std::filesystem::remove_all(logDir);

    // One line per function: ok with the inputs per dump, or the first problem.
    // Known artefacts still print their differences, as "known", but do not fail the run.
    std::map<std::string, std::string> known;
    if (!options.known.empty())
    {
        std::ifstream in(options.known);
        for (std::string line; std::getline(in, line);)
        {
            if (line.empty() || line[0] == '#')
                continue;
            const size_t space = line.find(' ');
            known[line.substr(0, space)] = space == std::string::npos ? "" : line.substr(space + 1);
        }
    }
    int ok = 0, failed = 0, skipped = 0, knownCount = 0;
    for (rc::Entry *entry : chosen)
    {
        auto &list = results[entry];
        std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        const auto worst = [&](Outcome outcome)
        {
            return std::find_if(list.begin(), list.end(), [&](const auto &item) { return item.second.outcome == outcome; });
        };
        if (const auto fail = worst(Outcome::Fail); fail != list.end())
        {
            const auto isKnown = known.find(entry->name);
            ++(isKnown != known.end() ? knownCount : failed);
            std::printf("%s %-40s %s: %s\n", isKnown != known.end() ? "known" : "FAIL ", entry->name, labels[fail->first].c_str(),
                        fail->second.detail.substr(0, isKnown != known.end() ? fail->second.detail.find('\n') : std::string::npos).c_str());
            if (isKnown != known.end())
            {
                std::printf("      (known: %s)\n", isKnown->second.c_str());
                continue;
            }
            for (const auto &[label, result] : list)
                if (&result != &fail->second && result.outcome == Outcome::Fail)
                    std::printf("     %-40s %s: %s\n", "", labels[label].c_str(), result.detail.substr(0, result.detail.find('\n')).c_str());
        }
        else if (const auto skip = worst(Outcome::Skip); skip != list.end())
        {
            ++skipped;
            std::printf("skip %-40s %s: %s\n", entry->name, labels[skip->first].c_str(), skip->second.detail.c_str());
        }
        else
        {
            ++ok;
            int total = 0;
            for (const auto &item : list)
                total += item.second.cases;
            std::printf("ok   %-40s %d inputs over %zu dump%s\n", entry->name, total, list.size(), list.size() == 1 ? "" : "s");
        }
    }
    if (!noSetup.empty())
    {
        std::printf("no random setup (live checks only):");
        for (const rc::Entry *entry : noSetup)
            std::printf(" %s", entry->name);
        std::printf("\n");
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("summary: %d ok, %d FAIL, %d known, %d skipped (cannot run offline), %zu without a setup; %zu dump%s, %.1f s\n", ok,
                failed, knownCount, skipped, noSetup.size(), options.dumps.size(), options.dumps.size() == 1 ? "" : "s", seconds);
    std::fflush(stdout);
    std::_Exit(failed != 0 ? 1 : 0);
}
