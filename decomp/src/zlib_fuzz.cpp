// Builds the random-test inputs of the game's zlib (ADLER32.c, ZUTIL.c, UNCOMPR.c, INFLATE.c, INFBLOCK.c, INFCODES.c,
// INFFAST.c, INFTREES.c, INFUTIL.c). See zlib_fuzz.h.
//
// Real deflate streams (zlib_corpus.inc, from tools/gen_zlib_corpus.py: stored, fixed and dynamic blocks, many small
// blocks, flush points, runs, dictionaries, with and without the zlib header, windows of 512 to 4096 bytes) are made
// into inputs three ways: cut short, with bytes corrupted, or replaced by noise. States in the middle of a stream are
// made by running the ORIGINAL functions (guest_call reaches the recompiled functions in the oracle) over the stream in
// small input and output pieces, so every mode of inflate, of the block decoder and of the codes decoder is met, with
// real tables, a real window and real bit buffers; the function under test then takes the next step.
//
// Allocation. Most setups put zlib's zalloc and zfree (zcalloc and zcfree, which inflateInit2_ installs when the stream
// has none) on stand-ins that take blocks from a 20 KB arena in the scratch area, record every call in a log in
// scratch (so the two runs' calls are compared) and can be made to fail on the k-th call. Freeing the newest block
// gives its space back, so the arena holds a state of a 2048-byte window and a few block tables. The setups of the
// functions that call the allocator through the game's real heap (zcalloc, zcfree, uncompress, inflateInit_,
// inflateEnd, inflate_blocks_new, inflate_blocks_free) do not install the stand-ins, and compare all of RAM.
//
// Scratch layout: control words and log, the stream, small objects, the input (and a dictionary at +0x800 for a
// dictionary stream), the output, the arena.

#include "zlib_fuzz.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rc {

namespace fuzz {

namespace {

#include "zlib_corpus.inc"

constexpr uint32_t kCtl = 0x0u, kLog = 0x100u, kLogEntries = 120u, kLogStride = 0x14u, kZ = 0xb00u, kMisc = 0xc00u, kIn = 0x1000u,
                   kDictOffset = 0x800u, kOut = 0x2000u, kOutSize = 0x1000u, kArena = 0x3000u, kArenaEnd = 0x7ff0u;
constexpr uint32_t kZcalloc = sym::zcalloc, kZcfree = sym::zcfree;
constexpr int32_t Z_OK = 0, Z_STREAM_END = 1, Z_NEED_DICT = 2, Z_BUF_ERROR = -5;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

struct Rng
{
    uint64_t &state;
    uint32_t next() { return nextFuzz(state); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0u; }
    uint32_t between(uint32_t low, uint32_t high) { return low + below(high - low + 1u); }
    bool chance(uint32_t percent) { return below(100) < percent; }
    template <typename T, size_t N>
    T pick(const T (&values)[N]) { return values[below(static_cast<uint32_t>(N))]; }
    template <typename T>
    T of(std::initializer_list<T> values) { return *(values.begin() + below(static_cast<uint32_t>(values.size()))); }
};

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
uint32_t argOf(R5900Context *ctx, int index) { return getRegU32(ctx, 4 + index); }

// The stand-in allocator's control words (at the start of scratch) and log.
struct Ctl
{
    uint32_t top, count, failAt, logCount, depth, pad[3];
    uint32_t ptr[16], size[16];
};
Ctl &ctl() { return mem<Ctl>(kFuzzScratch + kCtl); }

void logCall(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    Ctl &c0 = ctl();
    if (c0.logCount < kLogEntries)
    {
        const uint32_t at = kFuzzScratch + kLog + c0.logCount * kLogStride;
        mem<uint32_t>(at) = kind;
        mem<uint32_t>(at + 4) = a;
        mem<uint32_t>(at + 8) = b;
        mem<uint32_t>(at + 12) = c;
        mem<uint32_t>(at + 16) = d;
    }
    c0.logCount++;
}

// zalloc(opaque, items, size): a block of the arena, or 0 when the arena is full or this is the call chosen to fail.
void standinAlloc(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    Ctl &c = ctl();
    const uint32_t opaque = argOf(ctx, 0), items = argOf(ctx, 1), size = argOf(ctx, 2);
    const uint32_t bytes = (items * size + 7u) & ~7u;
    c.count++;
    uint32_t result = 0;
    if (!(c.failAt != 0 && c.count == c.failAt) && c.depth < 16 && bytes <= kFuzzScratch + kArenaEnd - c.top && c.top + bytes >= c.top)
    {
        result = c.top;
        c.ptr[c.depth] = result;
        c.size[c.depth] = bytes;
        c.depth++;
        c.top += bytes;
    }
    logCall(1, opaque, items, size, result);
    SET_GPR_S32(ctx, 2, static_cast<int32_t>(result));
    ctx->pc = getRegU32(ctx, 31);
}

// zfree(opaque, block): the newest block gives its space back.
void standinFree(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    Ctl &c = ctl();
    const uint32_t opaque = argOf(ctx, 0), block = argOf(ctx, 1);
    bool popped = false;
    if (c.depth > 0 && c.ptr[c.depth - 1] == block)
    {
        c.depth--;
        c.top = block;
        popped = true;
    }
    logCall(2, opaque, block, popped ? 1u : 0u, 0);
    ctx->pc = getRegU32(ctx, 31);
}

void installStandins()
{
    if (!g_offline)
        return;
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    g_runtime->replaceFunction(kZcalloc, &standinAlloc);
    g_runtime->replaceFunction(kZcfree, &standinFree);
}

// One input: the generator, the scratch address, and the allocator in use.
struct Env
{
    Rng rng;
    uint32_t s;
    bool standin;
    bool tiny; // this input is advanced in the smallest pieces, to stop in the middle of table headers and symbols

    Env(uint64_t &state, uint32_t scratch, bool useStandin, R5900Context *ctx = nullptr) : rng{state}, s(scratch), standin(useStandin), tiny(false)
    {
        tiny = rng.chance(30);
        if (ctx != nullptr && rng.chance(80))
        {
            // The dead stack below $sp holds words of other calls, as it does in the game: the huffman table builder copies two
            // bytes of it into every table entry.
            const uint32_t sp = getRegU32(ctx, 29);
            const bool small = rng.chance(50);
            for (uint32_t offset = 0x800; offset >= 4; offset -= 4)
                mem<uint32_t>(sp - offset) = small ? (rng.next() & 0x0f0f0f0fu) : rng.next();
        }
        if (standin)
        {
            installStandins();
            std::memset(guest_host(s + kCtl), 0, 0x100);
            Ctl &c = mem<Ctl>(s + kCtl);
            c.top = s + kArena;
            // no log entry means "free": a zeroed log shows the calls made
            std::memset(guest_host(s + kLog), 0, kLogEntries * kLogStride);
        }
    }

    void resetArena()
    {
        if (!standin)
            return;
        Ctl &c = ctl();
        c.top = s + kArena;
        c.count = 0;
        c.depth = 0;
        c.logCount = 0;
        c.failAt = 0;
        std::memset(guest_host(s + kLog), 0, kLogEntries * kLogStride);
    }

    // The calls the setup made while building the state are not part of the input: the log and the call count restart.
    void finish()
    {
        if (!standin)
            return;
        Ctl &c = ctl();
        c.count = 0;
        c.logCount = 0;
        std::memset(guest_host(s + kLog), 0, kLogEntries * kLogStride);
        if (rng.chance(12))
            c.failAt = rng.between(1, 5);
    }

    uint32_t z() const { return s + kZ; }
    uint32_t in() const { return s + kIn; }
    uint32_t out() const { return s + kOut; }
};

ptr32<z_stream_s> zOf(uint32_t address) { return ptr32<z_stream_s>(address); }
uint32_t version() { return sym::interruptPC + 8u; }

struct Input
{
    const CorpusStream *stream = nullptr;
    uint32_t length = 0; // bytes in the input buffer
    uint32_t header = 0; // bytes of zlib header (0 for a raw stream)
    bool raw = false, dictionary = false;
};

Input craftInput(Env &env, bool withHeader);

// Puts a stream (or noise) in the input buffer. kind: 0 any, 1 only streams with a dictionary, 2 only with a header,
// 3 only dynamic-block ones (text, mixed, struct), without a dictionary.
Input loadInput(Env &env, int kind, bool allowDamage = true)
{
    Rng &rng = env.rng;
    constexpr uint32_t count = sizeof(kCorpus) / sizeof(kCorpus[0]);
    const CorpusStream *stream;
    for (int tries = 0;; ++tries)
    {
        stream = &kCorpus[rng.below(count)];
        const bool dict = stream->dictLength != 0;
        if (kind == 1 && !dict)
            continue;
        if (kind != 1 && dict && rng.chance(80))
            continue;
        if (kind == 2 && stream->raw)
            continue;
        if (kind == 3 && (dict || stream->length < 200 || stream->outLength < 400))
            continue;
        break;
    }
    if (kind != 1 && (kind == 3 ? rng.chance(25) : rng.chance(18)))
        return craftInput(env, kind == 2 || rng.chance(30));
    Input input;
    input.stream = stream;
    input.raw = stream->raw != 0;
    input.dictionary = stream->dictLength != 0;
    input.header = input.raw ? 0u : (input.dictionary ? 6u : 2u);
    input.length = stream->length;
    uint8_t *buffer = static_cast<uint8_t *>(guest_host(env.in()));
    std::memcpy(buffer, kCorpusBlob + stream->offset, stream->length);
    if (input.dictionary)
        std::memcpy(guest_host(env.in() + kDictOffset), kCorpusBlob + stream->dictOffset, stream->dictLength);
    if (!allowDamage)
        return input;
    const uint32_t damage = rng.below(100);
    if (damage < 10)
        input.length = rng.below(input.length + 1u); // cut short
    else if (damage < 22)
    {
        for (uint32_t i = 0, n = rng.between(1, 3); i < n && input.length > 0; ++i)
        {
            const uint32_t at = rng.chance(40) ? rng.below(std::min(input.length, 8u)) : rng.below(input.length);
            buffer[at] ^= static_cast<uint8_t>(1u << rng.below(8)) | (rng.chance(30) ? static_cast<uint8_t>(rng.next()) : 0);
        }
    }
    else if (damage < 26)
    {
        input.length = rng.between(1, 200);
        for (uint32_t i = 0; i < input.length; ++i)
            buffer[i] = static_cast<uint8_t>(rng.next());
    }
    else if (damage < 30 && input.header >= 2 && input.length >= 2)
    {
        buffer[rng.below(2)] = static_cast<uint8_t>(rng.next()); // a bad header
    }
    return input;
}

// Fills the z_stream: the input buffer, an output of outCap bytes, no allocator (inflateInit2_ makes it) or an explicit one.
void setupStream(Env &env, const Input &input, uint32_t outCap)
{
    Rng &rng = env.rng;
    z_stream_s &z = mem<z_stream_s>(env.z());
    z.next_in = ptr32<Bytef>(env.in());
    z.avail_in = input.length;
    z.total_in = rng.chance(15) ? rng.next() : 0;
    z.next_out = ptr32<Bytef>(env.out());
    z.avail_out = outCap;
    z.total_out = rng.chance(15) ? rng.next() : 0;
    z.msg = ptr32<char>(rng.chance(50) ? 0u : rng.next());
    const uint32_t allocator = rng.below(10);
    z.zalloc = ee_fnptr{allocator < 7 ? 0u : kZcalloc};
    z.zfree = ee_fnptr{allocator < 7 ? 0u : (allocator == 9 ? 0u : kZcfree)};
    z.opaque = ptr32<void>(rng.chance(70) ? 0u : rng.next());
}

// A window size for the state, in bits (negative: no zlib header), from the stream's.
int32_t windowBits(Env &env, const Input &input)
{
    Rng &rng = env.rng;
    int32_t w = input.stream->wbits;
    if (rng.chance(25))
        w += static_cast<int32_t>(rng.below(3));
    if (!input.raw && w > 9 && rng.chance(12))
        w -= 1; // smaller than the stream asks for: "invalid window size"
    if (env.standin && w > 12)
        w = 12;
    if (!env.standin && rng.chance(30))
        w = 15;
    return input.raw ? -w : w;
}

int32_t initStream(Env &env, const Input &input)
{
    return guest_call<int32_t>(sym::inflateInit2_, zOf(env.z()), windowBits(env, input), ptr32<char>(version()), 0x38);
}

uint32_t inputLeft(Env &env, const Input &input)
{
    const z_stream_s &z = mem<z_stream_s>(env.z());
    const uint32_t end = env.in() + input.length;
    return z.next_in.address >= env.in() && z.next_in.address <= end ? end - z.next_in.address : 0u;
}

uint32_t outputLeft(Env &env)
{
    const z_stream_s &z = mem<z_stream_s>(env.z());
    const uint32_t end = env.out() + kOutSize;
    return z.next_out.address >= env.out() && z.next_out.address <= end ? end - z.next_out.address : 0u;
}

uint32_t chunkIn(Rng &rng, uint32_t left, bool tiny = false)
{
    if (left == 0)
        return 0;
    if (tiny)
        return std::min(left, rng.between(1, 3));
    const uint32_t kind = rng.below(100);
    if (kind < 40)
        return std::min(left, rng.between(1, 4));
    if (kind < 60)
        return std::min(left, rng.between(5, 40));
    if (kind < 75)
        return left;
    return rng.between(1, left);
}

uint32_t chunkOut(Rng &rng, uint32_t left, bool tiny = false)
{
    if (left == 0)
        return 0;
    if (tiny)
        return std::min(left, rng.between(1, 40));
    const uint32_t kind = rng.below(100);
    if (kind < 30)
        return std::min(left, rng.between(1, 8));
    if (kind < 55)
        return std::min(left, rng.between(9, 300));
    if (kind < 75)
        return left;
    return rng.between(1, left);
}

// Runs the original inflate over the stream in pieces. stopAtDict: leave at Z_NEED_DICT (else answer it with the stream's
// dictionary, correct 85% of the time). Returns the last result.
int32_t advanceInflate(Env &env, const Input &input, uint32_t steps)
{
    Rng &rng = env.rng;
    int32_t result = Z_OK;
    for (uint32_t i = 0; i < steps; ++i)
    {
        z_stream_s &z = mem<z_stream_s>(env.z());
        const uint32_t left = inputLeft(env, input), room = outputLeft(env);
        if (left == 0 && rng.chance(70))
            break;
        if (room == 0)
            break;
        z.avail_in = chunkIn(rng, left, env.tiny);
        z.avail_out = chunkOut(rng, room, env.tiny && rng.chance(60));
        result = guest_call<int32_t>(sym::inflate, zOf(env.z()), rng.chance(8) ? 4 : (rng.chance(10) ? 1 : 0));
        if (result == Z_NEED_DICT && input.dictionary)
        {
            const uint32_t length = input.stream->dictLength;
            if (rng.chance(80))
            {
                const int32_t set = guest_call<int32_t>(sym::inflateSetDictionary, zOf(env.z()), ptr32<Bytef>(env.in() + kDictOffset), length);
                result = set == 0 ? Z_OK : set;
            }
            else
                break;
        }
        if (result != Z_OK && result != Z_BUF_ERROR)
            break;
    }
    return result;
}

// The block state of the stream, if it has one.
uint32_t blocksOf(Env &env)
{
    const uint32_t state = mem<z_stream_s>(env.z()).state.address;
    return state == 0 ? 0u : mem<uint32_t>(state + 0x14u);
}

// A stream with the header taken off, as the block decoder sees it: a nowrap state (the header is skipped by moving the
// input) or, for a zlib-wrapped stream, a state with the header's two (or six) bytes passed over.
void skipHeader(Env &env, Input &input)
{
    z_stream_s &z = mem<z_stream_s>(env.z());
    if (input.header != 0 && z.avail_in >= input.header)
    {
        z.next_in = ptr32<Bytef>(z.next_in.address + input.header);
        z.avail_in -= input.header;
        const uint32_t state = z.state.address;
        if (state != 0)
            mem<uint32_t>(state) = 7; // BLOCKS
    }
}

// Runs the original inflate_blocks over what is left in pieces.
int32_t advanceBlocks(Env &env, const Input &input, uint32_t steps, bool stopInCodes)
{
    Rng &rng = env.rng;
    int32_t result = Z_OK;
    for (uint32_t i = 0; i < steps; ++i)
    {
        const uint32_t blocks = blocksOf(env);
        if (blocks == 0)
            break;
        z_stream_s &z = mem<z_stream_s>(env.z());
        const uint32_t left = inputLeft(env, input), room = outputLeft(env);
        if (left == 0 && rng.chance(70))
            break;
        if (room == 0)
            break;
        z.avail_in = chunkIn(rng, left, env.tiny);
        z.avail_out = chunkOut(rng, room, env.tiny && rng.chance(60));
        result = guest_call<int32_t>(sym::inflate_blocks, ptr32<inflate_blocks_state__INFBLOCK>(blocks), zOf(env.z()), rng.chance(30) ? Z_BUF_ERROR : 0);
        if (stopInCodes && mem<uint32_t>(blocks) == 6 && rng.chance(75))
            break;
        if (result != Z_OK && result != Z_BUF_ERROR)
            break;
    }
    return result;
}

// Leaves the stream's input and output as the function under test will see them.
void finalPieces(Env &env, const Input &input)
{
    Rng &rng = env.rng;
    z_stream_s &z = mem<z_stream_s>(env.z());
    z.avail_in = rng.chance(20) ? inputLeft(env, input) : chunkIn(rng, inputLeft(env, input));
    z.avail_out = rng.chance(35) ? outputLeft(env) : chunkOut(rng, outputLeft(env));
}

// A stream in the middle of its decoding: initialised (nowrap, block decoder only, when blocksOnly), advanced. Returns
// false if there is no state (initialisation failed).
bool midStream(Env &env, Input &input, bool blocksOnly, uint32_t maxSteps, bool stopInCodes = false)
{
    setupStream(env, input, kOutSize);
    const int32_t init = initStream(env, input);
    if (init != 0)
        return false;
    int32_t last;
    if (blocksOnly)
    {
        skipHeader(env, input);
        last = advanceBlocks(env, input, env.rng.below(maxSteps + 1), stopInCodes);
    }
    else
        last = advanceInflate(env, input, env.rng.below(maxSteps + 1));
    // After Z_MEM_ERROR the block decoder keeps its mode and has freed (or not allocated) what it works on: the game never
    // goes on from there, and a retry reads a freed array whose place the work space then takes (zlib's own hazard), where
    // the original's and the native's order of reads and writes differ. The state is made a failed one instead.
    if (last == -4 && blocksOf(env) != 0)
        mem<uint32_t>(blocksOf(env)) = 9;
    return blocksOf(env) != 0;
}

// Positions the block state's window pointers away from the start, sometimes near the end, so wrapping is reached.
void moveWindow(Env &env, uint32_t blocks)
{
    Rng &rng = env.rng;
    const uint32_t window = mem<uint32_t>(blocks + 0x28u), end = mem<uint32_t>(blocks + 0x2cu);
    uint32_t &read = mem<uint32_t>(blocks + 0x30u), &write = mem<uint32_t>(blocks + 0x34u);
    if (end <= window)
        return;
    const uint32_t size = end - window;
    const uint32_t kind = rng.below(100);
    if (kind < 25)
    {
        read = write = window + rng.below(size);
    }
    else if (kind < 40)
    {
        read = write = end - std::min(size, rng.between(1, 600));
    }
    else if (kind < 50)
    {
        write = window + rng.below(size);
        read = window + rng.below(size);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Huffman code length sets, for huft_build and the tree functions.

// n code lengths for a complete code of `used` symbols no longer than maxLen, placed at random symbols.
void completeLengths(Rng &rng, std::vector<uint32_t> &lengths, uint32_t used, uint32_t maxLen)
{
    std::vector<uint32_t> leaves{0};
    while (leaves.size() < used)
    {
        std::vector<size_t> splittable;
        for (size_t i = 0; i < leaves.size(); ++i)
            if (leaves[i] < maxLen)
                splittable.push_back(i);
        if (splittable.empty())
            break;
        // favour the deepest leaves now and then, so that long codes (and sub-tables) occur
        size_t pick = splittable[rng.below(static_cast<uint32_t>(splittable.size()))];
        if (rng.chance(40))
            for (size_t i : splittable)
                if (leaves[i] > leaves[pick])
                    pick = i;
        const uint32_t depth = leaves[pick] + 1;
        leaves[pick] = depth;
        leaves.push_back(depth);
    }
    std::vector<size_t> slots(lengths.size());
    for (size_t i = 0; i < slots.size(); ++i)
        slots[i] = i;
    for (size_t i = slots.size(); i > 1; --i)
        std::swap(slots[i - 1], slots[rng.below(static_cast<uint32_t>(i))]);
    std::fill(lengths.begin(), lengths.end(), 0u);
    for (size_t i = 0; i < leaves.size() && i < slots.size(); ++i)
        lengths[slots[i]] = leaves[i];
}

// Damages a length set so the code is incomplete or oversubscribed, or leaves it.
void damageLengths(Rng &rng, std::vector<uint32_t> &lengths, uint32_t maxLen)
{
    const uint32_t kind = rng.below(100);
    std::vector<size_t> used;
    for (size_t i = 0; i < lengths.size(); ++i)
        if (lengths[i] != 0)
            used.push_back(i);
    if (used.empty() || kind < 45)
        return;
    const size_t at = used[rng.below(static_cast<uint32_t>(used.size()))];
    if (kind < 60)
        lengths[at] = std::min(maxLen, lengths[at] + 1); // incomplete
    else if (kind < 72 && lengths[at] > 1)
        lengths[at] -= 1; // oversubscribed
    else if (kind < 80)
        lengths[at] = 0; // incomplete
    else if (kind < 88)
    {
        std::fill(lengths.begin(), lengths.end(), 0u); // empty
    }
    else if (kind < 94)
    {
        std::fill(lengths.begin(), lengths.end(), 0u);
        lengths[at] = rng.between(1, maxLen); // one code
    }
    else
        lengths[rng.below(static_cast<uint32_t>(lengths.size()))] = rng.between(1, maxLen);
}

// The code lengths of one tree: its size, a set, damaged sometimes.
std::vector<uint32_t> makeLengths(Rng &rng, uint32_t n, uint32_t maxLen)
{
    std::vector<uint32_t> lengths(n, 0u);
    const uint32_t used = rng.chance(10) ? rng.between(1, std::min(n, 3u)) : rng.between(2, n);
    completeLengths(rng, lengths, std::min(used, n), maxLen);
    damageLengths(rng, lengths, maxLen);
    return lengths;
}

void putWords(uint32_t at, const std::vector<uint32_t> &values)
{
    for (size_t i = 0; i < values.size(); ++i)
        mem<uint32_t>(at + 4u * static_cast<uint32_t>(i)) = values[i];
}

// ---------------------------------------------------------------------------------------------------------------------
// Made-up deflate data, for the cases the real streams never reach: stored blocks of any length, tables with 30 or 31
// symbol counts, repeat codes at the start, a distance code of one symbol (its other code is invalid), long codes.

struct BitWriter
{
    uint8_t *buffer;
    uint32_t limit, bytes = 0, bits = 0;
    uint32_t cur = 0;
    void put(uint32_t value, uint32_t count) // low bit first
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            cur |= ((value >> i) & 1u) << bits;
            if (++bits == 8)
                flushByte();
        }
    }
    void flushByte()
    {
        if (bytes < limit)
            buffer[bytes++] = static_cast<uint8_t>(cur);
        cur = 0;
        bits = 0;
    }
    void align()
    {
        if (bits != 0)
            flushByte();
    }
    void putCode(uint32_t code, uint32_t length) // a huffman code goes high bit first
    {
        for (uint32_t i = length; i-- > 0;)
            put((code >> i) & 1u, 1);
    }
};

// Canonical codes (RFC 1951) of a length set.
std::vector<uint32_t> canonicalCodes(const std::vector<uint32_t> &lengths)
{
    uint32_t count[16] = {0}, next[16] = {0};
    for (uint32_t l : lengths)
        count[l & 15]++;
    count[0] = 0;
    uint32_t code = 0;
    for (uint32_t bits = 1; bits < 16; ++bits)
    {
        code = (code + count[bits - 1]) << 1;
        next[bits] = code;
    }
    std::vector<uint32_t> codes(lengths.size(), 0);
    for (size_t i = 0; i < lengths.size(); ++i)
        if (lengths[i] != 0)
            codes[i] = next[lengths[i] & 15]++;
    return codes;
}

uint32_t pickUsed(Rng &rng, const std::vector<uint32_t> &lengths)
{
    std::vector<uint32_t> used;
    for (uint32_t i = 0; i < lengths.size(); ++i)
        if (lengths[i] != 0)
            used.push_back(i);
    return used.empty() ? 0u : used[rng.below(static_cast<uint32_t>(used.size()))];
}

Input craftInput(Env &env, bool withHeader)
{
    Rng &rng = env.rng;
    static CorpusStream made;
    uint8_t *buffer = static_cast<uint8_t *>(guest_host(env.in()));
    BitWriter w{buffer, 0x7f0u};
    const uint32_t wbits = rng.between(9, 12);
    if (withHeader)
    {
        const uint32_t cmf = ((wbits - 8) << 4) | 8;
        uint32_t flg;
        // find a flag byte that makes the header a multiple of 31, without the dictionary bit
        for (flg = 0x80; flg < 0xa0; ++flg)
            if ((cmf * 256 + flg) % 31 == 0 && (flg & 0x20) == 0)
                break;
        w.put(cmf, 8);
        w.put(flg, 8);
    }
    const uint32_t blocks = rng.chance(60) ? 1 : rng.between(2, 3);
    for (uint32_t block = 0; block < blocks; ++block)
    {
        const bool last = block + 1 == blocks;
        w.put(last ? 1 : 0, 1);
        if (rng.chance(35))
        {
            // stored
            w.put(0, 2);
            w.align();
            static constexpr uint32_t kLengths[] = {0, 1, 2, 7, 100, 255, 256, 4095, 4096, 4097, 0x1000, 0x2345, 0x8000, 0xffff};
            const uint32_t length = rng.chance(60) ? rng.pick(kLengths) : rng.below(500);
            w.put(length & 0xffffu, 16);
            w.put(rng.chance(88) ? (~length & 0xffffu) : rng.below(0x10000), 16);
            for (uint32_t i = 0; i < length && i < rng.between(0, 300) && w.bytes < w.limit; ++i)
                w.put(rng.next() & 0xff, 8);
            continue;
        }
        w.put(2, 2); // dynamic
        const uint32_t hlit = rng.chance(20) ? rng.between(29, 31) : rng.chance(30) ? 0u : rng.below(29);
        const uint32_t hdist = rng.chance(20) ? rng.between(29, 31) : rng.chance(30) ? 0u : rng.below(29);
        const uint32_t hclen = 15;
        w.put(hlit, 5);
        w.put(hdist, 5);
        w.put(hclen, 4);
        // a complete code over all 19 code length symbols, so that every length can be written
        std::vector<uint32_t> clLengths(19, 0);
        completeLengths(rng, clLengths, 19, 7);
        if (rng.chance(8))
            clLengths[rng.below(19)] = rng.below(8); // not complete (or a symbol that cannot be written)
        static constexpr uint32_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        for (uint32_t i = 0; i < hclen + 4; ++i)
            w.put(clLengths[kOrder[i]], 3);
        const std::vector<uint32_t> clCodes = canonicalCodes(clLengths);
        const uint32_t total = 257 + hlit + 1 + hdist;
        std::vector<uint32_t> lit = makeLengths(rng, 257 + hlit, 15), dist = makeLengths(rng, 1 + hdist, 15);
        if (rng.chance(40))
        {
            std::fill(dist.begin(), dist.end(), 0u);
            dist[rng.below(static_cast<uint32_t>(dist.size()))] = 1; // one distance code: the other bit pattern is invalid
        }
        if (rng.chance(70) && lit.size() > 256 && lit[256] == 0)
            lit[256] = rng.between(1, 9);
        std::vector<uint32_t> all = lit;
        all.insert(all.end(), dist.begin(), dist.end());
        const auto writeSymbol = [&](uint32_t symbol) { w.putCode(clCodes[symbol], clLengths[symbol]); };
        for (uint32_t i = 0; i < total;)
        {
            const uint32_t value = all[i];
            uint32_t run = 1;
            while (i + run < total && all[i + run] == value)
                run++;
            if (i == 0 && rng.chance(30))
            {
                writeSymbol(16); // repeat the previous length with none before it
                w.put(rng.below(4), 2);
                i += 3;
                continue;
            }
            if (value == 0 && run >= 3 && rng.chance(80))
            {
                const uint32_t n = std::min(run, rng.chance(50) ? 138u : 10u);
                if (n >= 11)
                {
                    writeSymbol(18);
                    w.put(n - 11, 7);
                }
                else
                {
                    writeSymbol(17);
                    w.put(n - 3, 3);
                }
                i += n;
            }
            else if (value != 0 && run >= 4 && rng.chance(70))
            {
                writeSymbol(value);
                const uint32_t n = std::min(run - 1, 6u);
                writeSymbol(16);
                w.put(n - 3, 2);
                i += 1 + n;
            }
            else
            {
                writeSymbol(value);
                i++;
            }
            if (rng.chance(1))
                i = total + 3; // the repeats run past the end of the table
        }
        // the body: random symbols with their extra bits, with distances
        const std::vector<uint32_t> litCodes = canonicalCodes(lit), distCodes = canonicalCodes(dist);
        const uint32_t symbols = rng.between(0, 200);
        for (uint32_t i = 0; i < symbols && w.bytes < w.limit - 8; ++i)
        {
            const uint32_t symbol = pickUsed(rng, lit);
            if (lit[symbol] == 0)
                break;
            w.putCode(litCodes[symbol], lit[symbol]);
            if (symbol == 256)
                break;
            if (symbol > 256)
            {
                const uint32_t extra = symbol - 257 < 8 || symbol >= 285 ? 0 : std::min(5u, (symbol - 261) / 4);
                w.put(rng.next(), extra);
                const uint32_t d = pickUsed(rng, dist);
                if (dist[d] != 0)
                    w.putCode(distCodes[d], dist[d]);
                if (rng.chance(15))
                    w.put(rng.next(), 1); // a bit that may select the invalid distance code
                w.put(rng.next(), d >= 4 ? std::min(13u, (d - 2) / 2) : 0u);
            }
        }
        if (lit.size() > 256 && lit[256] != 0)
            w.putCode(litCodes[256], lit[256]);
    }
    w.align();
    made.wbits = static_cast<uint8_t>(wbits);
    made.raw = withHeader ? 0 : 1;
    made.offset = made.length = made.dictOffset = made.dictLength = 0;
    made.outLength = 3000;
    Input input;
    input.stream = &made;
    input.raw = !withHeader;
    input.header = withHeader ? 2u : 0u;
    input.length = w.bytes;
    return input;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

// adler32(adler, buf, len): the running value, a buffer in scratch, a length from nothing to three blocks of 5552.
void zAdler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, false, ctx);
    Rng &rng = env.rng;
    static constexpr uint32_t kSeeds[] = {1, 0, 0xffffffffu, 0xfff0fff0u, 0x0001fff0u, 0xffff0000u, 0x00010001u};
    uint32_t adler = rng.chance(50) ? 1u : (rng.chance(50) ? rng.pick(kSeeds) : rng.next());
    static constexpr uint32_t kLengths[] = {0, 1, 2, 3, 4, 5, 15, 16, 17, 31, 33, 5551, 5552, 5553, 11103, 11104, 11105};
    uint32_t length = rng.chance(50) ? rng.pick(kLengths) : rng.below(12000);
    const uint32_t at = scratch + 0x1000u + rng.below(0x40);
    if (rng.chance(30))
        for (uint32_t i = 0; i < length && i < 0x6000; ++i)
            mem<uint8_t>(at + i) = static_cast<uint8_t>(rng.chance(50) ? 0xff : rng.next());
    setArg(ctx, 0, adler);
    setArg(ctx, 1, rng.chance(5) ? 0u : at);
    setArg(ctx, 2, length);
}

// zError(err): the codes from Z_NEED_DICT to Z_VERSION_ERROR and a few either side.
void zErrorArg(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    setArg(ctx, 0, static_cast<uint32_t>(static_cast<int32_t>(rng.below(23)) - 12));
}

// zcalloc(opaque, items, size), on the real heap: small and large blocks, 0 bytes and more than the heap holds.
void zAlloc(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    uint32_t items = rng.between(1, 20);
    static constexpr uint32_t kSizes[] = {1, 4, 8, 0x18, 0x1c, 0x40, 0x5a0, 0x1000, 0x8000, 0x20000};
    uint32_t size = rng.pick(kSizes);
    if (rng.chance(8))
        items = 0;
    if (rng.chance(6))
        size = 0x40000000u;
    if (rng.chance(4))
        items = 0x10000u;
    setArg(ctx, 0, rng.chance(70) ? 0u : rng.next());
    setArg(ctx, 1, items);
    setArg(ctx, 2, size);
}

// zcfree(opaque, block): a block the real zcalloc made.
void zFree(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    const uint32_t items = rng.between(1, 8), size = rng.chance(50) ? 0x1000u : rng.between(1, 0x3000);
    const uint32_t block = guest_call<ptr32<void>>(kZcalloc, ptr32<void>(0u), items, size).address;
    setArg(ctx, 0, rng.chance(70) ? 0u : rng.next());
    setArg(ctx, 1, block);
}

// uncompress(dest, &destLen, source, sourceLen), on the real heap.
void zUncompress(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, false, ctx);
    Rng &rng = env.rng;
    const Input input = loadInput(env, rng.chance(15) ? 1 : 0);
    const uint32_t lengthAt = scratch + kMisc;
    uint32_t capacity;
    const uint32_t out = input.stream->outLength;
    const uint32_t kind = rng.below(100);
    if (kind < 45)
        capacity = out;
    else if (kind < 65)
        capacity = std::min(kOutSize, out + rng.between(1, 300));
    else if (kind < 90)
        capacity = rng.below(out + 1u);
    else
        capacity = rng.below(kOutSize);
    capacity = std::min(capacity, kOutSize);
    mem<uint32_t>(lengthAt) = capacity;
    // stale: the stream's own stack bytes and the fields inflateInit2_ does not set show through
    setArg(ctx, 0, env.out());
    setArg(ctx, 1, lengthAt);
    setArg(ctx, 2, env.in());
    setArg(ctx, 3, input.length);
}

// inflateInit_(z, version, stream_size) and inflateInit2_(z, w, version, stream_size).
namespace
{
void initArgs(Env &env, R5900Context *ctx, bool withWindow)
{
    Rng &rng = env.rng;
    const uint32_t z = rng.chance(3) ? 0u : env.z();
    z_stream_s &zs = mem<z_stream_s>(env.z());
    const uint32_t allocator = rng.below(10);
    zs.zalloc = ee_fnptr{allocator < 7 ? 0u : kZcalloc};
    zs.zfree = ee_fnptr{allocator < 7 ? 0u : (allocator == 9 ? 0u : kZcfree)};
    zs.opaque = ptr32<void>(rng.chance(70) ? 0u : rng.next());
    static constexpr int32_t kWindows[] = {8, 9, 10, 11, 12, 12, 12, 13, 14, 15, -8, -9, -12, -15, 7, 16, -7, -16, 0, 1, -1, 31};
    int32_t w = rng.chance(60) ? static_cast<int32_t>(rng.between(8, 15)) : rng.pick(kWindows);
    if (rng.chance(25))
        w = -w;
    if (env.standin && w > 12 && w <= 15 && rng.chance(70))
        w = 11;
    const uint32_t versionChoice = rng.below(100);
    uint32_t ver = version();
    const uint32_t other = env.s + kMisc + 0x40u;
    mem<char>(other) = static_cast<char>(rng.chance(50) ? '2' : '1');
    mem<char>(other + 1) = 0;
    if (versionChoice < 4)
        ver = 0;
    else if (versionChoice < 10)
        ver = other;
    else if (versionChoice < 14)
        ver = sym::mark_21 + 0x10u;
    const uint32_t size = rng.chance(88) ? 0x38u : rng.of<uint32_t>({0, 0x37, 0x39, 0x58, 0xffffffffu});
    int i = 0;
    setArg(ctx, i++, z);
    if (withWindow)
        setArg(ctx, i++, static_cast<uint32_t>(w));
    setArg(ctx, i++, ver);
    setArg(ctx, i++, size);
}
} // namespace

void zInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, false, ctx);
    initArgs(env, ctx, false);
}

void zInit2(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    initArgs(env, ctx, true);
    env.finish();
}

// inflateEnd(z): a state on the real heap, started and advanced; sometimes a stream without a state or free function.
void zEnd(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, false, ctx);
    Rng &rng = env.rng;
    Input input = loadInput(env, 0);
    uint32_t z = env.z();
    if (!midStream(env, input, false, 8))
    {
        // no state
    }
    z_stream_s &zs = mem<z_stream_s>(env.z());
    const uint32_t kind = rng.below(100);
    if (kind < 4)
        z = 0;
    else if (kind < 8)
        zs.state = ptr32<internal_state>(0u);
    else if (kind < 12)
        zs.zfree = ee_fnptr{0};
    setArg(ctx, 0, z);
}

namespace
{
// A stream in the middle of its decoding on the stand-in allocator, ready for a function that takes the stream.
Input prepared(Env &env, bool blocksOnly, uint32_t maxSteps, bool stopInCodes = false, int kind = 0)
{
    Input input = loadInput(env, kind);
    if (!midStream(env, input, blocksOnly, maxSteps, stopInCodes))
    {
        // no state: the stream is what the function sees
    }
    return input;
}
} // namespace

void zReset(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    prepared(env, false, 12);
    uint32_t z = env.z();
    const uint32_t kind = rng.below(100);
    if (kind < 4)
        z = 0;
    else if (kind < 8)
        mem<z_stream_s>(env.z()).state = ptr32<internal_state>(0u);
    setArg(ctx, 0, z);
    env.finish();
}

void zSyncPoint(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    prepared(env, false, 10);
    uint32_t z = env.z();
    const uint32_t blocks = blocksOf(env);
    const uint32_t kind = rng.below(100);
    if (kind < 5)
        z = 0;
    else if (kind < 10)
        mem<z_stream_s>(env.z()).state = ptr32<internal_state>(0u);
    else if (kind < 15 && mem<z_stream_s>(env.z()).state.address != 0)
        mem<uint32_t>(mem<z_stream_s>(env.z()).state.address + 0x14u) = 0;
    else if (kind < 50 && blocks != 0)
        mem<uint32_t>(blocks) = rng.below(11); // every block mode, and one that is none
    setArg(ctx, 0, z);
    env.finish();
}

// inflateSync(z): the input holds sync markers (00 00 ff ff) among bytes, or a real stream with flush points.
void zSync(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    Input input = loadInput(env, 0);
    if (rng.chance(45))
    {
        // a made-up input: bytes with zeros and ff's, and markers
        input.length = rng.between(1, 200);
        uint8_t *buffer = static_cast<uint8_t *>(guest_host(env.in()));
        for (uint32_t i = 0; i < input.length; ++i)
            buffer[i] = rng.chance(60) ? (rng.chance(50) ? 0 : 0xff) : static_cast<uint8_t>(rng.next());
        for (uint32_t i = 0, n = rng.below(3); i < n && input.length > 4; ++i)
        {
            const uint32_t at = rng.below(input.length - 3);
            buffer[at] = buffer[at + 1] = 0;
            buffer[at + 2] = buffer[at + 3] = 0xff;
        }
    }
    midStream(env, input, false, 4);
    z_stream_s &zs = mem<z_stream_s>(env.z());
    zs.next_in = ptr32<Bytef>(env.in() + (rng.chance(40) ? rng.below(std::max(1u, input.length)) : 0u));
    zs.avail_in = std::min(rng.chance(15) ? rng.below(5) : rng.between(0, 300), env.in() + input.length > zs.next_in.address ? env.in() + input.length - zs.next_in.address : 0u);
    uint32_t z = env.z();
    const uint32_t stateAt = zs.state.address;
    if (stateAt != 0)
    {
        const uint32_t kind = rng.below(100);
        if (kind < 45)
        {
            mem<uint32_t>(stateAt) = 13;
            mem<uint32_t>(stateAt + 4) = rng.below(5);
        }
        else if (kind < 55)
            mem<uint32_t>(stateAt) = rng.below(14);
        else if (kind < 62)
            mem<uint32_t>(stateAt + 4) = rng.below(6);
    }
    const uint32_t kind = rng.below(100);
    if (kind < 3)
        z = 0;
    else if (kind < 6)
        zs.state = ptr32<internal_state>(0u);
    setArg(ctx, 0, z);
    env.finish();
}

// inflateSetDictionary(z, dictionary, length): a stream stopped for its dictionary, with a good dictionary, a damaged
// one, or one whose checksum is made to match (so lengths past the window are reached).
void zSetDict(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    Input input = loadInput(env, rng.chance(80) ? 1 : 0);
    setupStream(env, input, kOutSize);
    mem<z_stream_s>(env.z()).avail_out = kOutSize;
    const int32_t init = initStream(env, input);
    uint32_t dictAt = env.in() + kDictOffset;
    uint32_t length = input.dictionary ? input.stream->dictLength : 0;
    if (init == 0)
    {
        z_stream_s &zs = mem<z_stream_s>(env.z());
        zs.avail_in = std::min(input.length, 6u + rng.below(30));
        const int32_t r = guest_call<int32_t>(sym::inflate, zOf(env.z()), 0);
        (void)r;
        const uint32_t stateAt = zs.state.address;
        const uint32_t kind = rng.below(100);
        if (kind < 8 && stateAt != 0)
            mem<uint32_t>(stateAt) = rng.below(14); // not stopped for a dictionary
        else if (kind < 45 || !input.dictionary)
        {
            // a made-up dictionary of any length, with its checksum
            length = rng.chance(30) ? rng.below(64) : rng.below(4200);
            if (rng.chance(30) && stateAt != 0)
            {
                const uint32_t wbits = mem<uint32_t>(stateAt + 0x10u);
                length = (1u << std::min(wbits, 15u)) + rng.below(3) - 1u;
            }
            dictAt = env.s + 0x3800u;
            for (uint32_t i = 0; i < length && i < 0x4000; ++i)
                mem<uint8_t>(dictAt + i) = static_cast<uint8_t>(rng.chance(50) ? rng.next() : 'a' + (i % 7));
            if (stateAt != 0 && rng.chance(85))
            {
                mem<uint32_t>(stateAt) = 6; // DICT0
                zs.adler = guest_call<uint32_t>(sym::adler32, 1u, ptr32<Bytef>(dictAt), length);
            }
        }
        else if (rng.chance(25))
            mem<uint8_t>(dictAt + rng.below(std::max(1u, length))) ^= 0x55; // a damaged dictionary
    }
    uint32_t z = env.z();
    const uint32_t kind = rng.below(100);
    if (kind < 3)
        z = 0;
    else if (kind < 6)
        mem<z_stream_s>(env.z()).state = ptr32<internal_state>(0u);
    setArg(ctx, 0, z);
    setArg(ctx, 1, dictAt);
    setArg(ctx, 2, length);
    env.finish();
}

// inflate(z, flush): a stream anywhere in its decoding; sometimes a state in a made-up mode or a stream with no input.
void zInflate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    Input input = loadInput(env, rng.chance(20) ? 1 : 0);
    midStream(env, input, false, 14);
    z_stream_s &zs = mem<z_stream_s>(env.z());
    finalPieces(env, input);
    const uint32_t stateAt = zs.state.address;
    if (stateAt != 0 && rng.chance(6))
    {
        mem<uint32_t>(stateAt) = rng.below(16);
        mem<uint32_t>(stateAt + 4) = rng.next() & (rng.chance(50) ? 0xffu : 0xffffffffu);
        mem<uint32_t>(stateAt + 8) = rng.next();
    }
    uint32_t z = env.z();
    const uint32_t kind = rng.below(100);
    if (kind < 2)
        z = 0;
    else if (kind < 4)
        zs.state = ptr32<internal_state>(0u);
    else if (kind < 6)
        zs.next_in = ptr32<Bytef>(0u);
    static constexpr uint32_t kFlush[] = {0, 0, 0, 4, 4, 1, 2, 3, 5, 0xffffffffu};
    setArg(ctx, 0, z);
    setArg(ctx, 1, rng.pick(kFlush));
    env.finish();
}

// inflate_blocks_new(z, checkfn, w), on the real heap.
void zBlocksNew(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, false, ctx);
    Rng &rng = env.rng;
    z_stream_s &zs = mem<z_stream_s>(env.z());
    zs.zalloc = ee_fnptr{kZcalloc};
    zs.zfree = ee_fnptr{kZcfree};
    zs.opaque = ptr32<void>(rng.chance(70) ? 0u : rng.next());
    static constexpr uint32_t kSizes[] = {1, 2, 256, 512, 1024, 2048, 4096, 32768, 0x10000, 1u << 20, 0x40000000u};
    setArg(ctx, 0, env.z());
    setArg(ctx, 1, rng.chance(60) ? sym::adler32 : 0u);
    setArg(ctx, 2, rng.chance(70) ? (1u << rng.between(8, 15)) : rng.pick(kSizes));
}

namespace
{
// z and s of a stream in the middle of its decoding in the block decoder, ready for a function of the block decoder.
struct Blocks
{
    Input input;
    uint32_t s = 0;
    uint32_t z = 0;
};

Blocks preparedBlocks(Env &env, uint32_t maxSteps, bool stopInCodes, int kind = 0)
{
    Blocks b;
    b.input = loadInput(env, kind);
    midStream(env, b.input, true, maxSteps, stopInCodes);
    b.s = blocksOf(env);
    b.z = env.z();
    return b;
}
} // namespace

// inflate_blocks(s, z, r)
void zBlocks(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    Blocks b = preparedBlocks(env, 14, false, rng.chance(8) ? 1 : 0);
    if (b.s != 0)
    {
        finalPieces(env, b.input);
        if (rng.chance(25))
            moveWindow(env, b.s);
        if (rng.chance(5))
            mem<uint32_t>(b.s) = rng.of<uint32_t>({0, 1, 2, 3, 7, 8, 9, 10, 11}); // a mode that needs nothing from the state's tables
        if (rng.chance(4))
            mem<uint32_t>(b.s + 0x1cu) = rng.below(40); // the bit buffer
    }
    static constexpr int32_t kResults[] = {0, 0, 0, Z_BUF_ERROR, 1};
    setArg(ctx, 0, b.s);
    setArg(ctx, 1, b.z);
    setArg(ctx, 2, static_cast<uint32_t>(rng.pick(kResults)));
    env.finish();
}

// inflate_blocks_reset(s, z, c)
void zBlocksReset(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    Blocks b = preparedBlocks(env, 14, false);
    if (b.s != 0 && rng.chance(10))
        mem<uint32_t>(b.s) = rng.below(10);
    setArg(ctx, 0, b.s);
    setArg(ctx, 1, b.z);
    setArg(ctx, 2, rng.chance(50) ? 0u : env.s + kMisc + 0x80u);
    env.finish();
}

// inflate_blocks_free(s, z), on the real heap.
void zBlocksFree(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, false, ctx);
    Blocks b = preparedBlocks(env, 14, false);
    setArg(ctx, 0, b.s);
    setArg(ctx, 1, b.z);
}

// inflate_set_dictionary(s, d, n): a dictionary of up to the window's size.
void zBlocksSetDictionary(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    Blocks b = preparedBlocks(env, 3, false);
    uint32_t length = 0;
    if (b.s != 0)
    {
        const uint32_t size = mem<uint32_t>(b.s + 0x2cu) - mem<uint32_t>(b.s + 0x28u);
        length = rng.chance(30) ? rng.below(std::min(size, 40u) + 1u) : rng.chance(20) ? size : rng.below(size + 1u);
    }
    const uint32_t dictAt = env.s + 0x3800u;
    for (uint32_t i = 0; i < 0x1000; ++i)
        mem<uint8_t>(dictAt + i) = static_cast<uint8_t>(rng.next());
    setArg(ctx, 0, b.s);
    setArg(ctx, 1, dictAt);
    setArg(ctx, 2, length);
    env.finish();
}

// inflate_blocks_sync_point(s)
void zBlocksSyncPoint(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    const uint32_t s = env.s + kMisc + 0x100u;
    mem<uint32_t>(s) = rng.chance(50) ? 1u : rng.below(12);
    setArg(ctx, 0, s);
}

namespace
{
// A stream in its block decoder's codes (inflate_codes' and inflate_fast's inputs): tries streams until one stops with
// the block state in CODES. Returns the block state, or 0 if none did.
Blocks inCodes(Env &env, bool atStart)
{
    Rng &rng = env.rng;
    Blocks b;
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        env.resetArena();
        b = preparedBlocks(env, 40, true, attempt < 25 ? 3 : 0);
        if (b.s != 0 && mem<uint32_t>(b.s) == 6 && mem<uint32_t>(b.s + 4) != 0)
            return b;
    }
    (void)atStart;
    return b;
}
} // namespace

// inflate_codes(s, z, r)
void zCodes(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    Blocks b = inCodes(env, false);
    if (b.s != 0)
    {
        finalPieces(env, b.input);
        if (rng.chance(40))
            moveWindow(env, b.s);
        const uint32_t codes = mem<uint32_t>(b.s + 4);
        if (rng.chance(6))
            mem<uint32_t>(codes) = rng.of<uint32_t>({0, 0, 7, 8, 9, 10}); // a codes mode that needs nothing from the sub fields
        if (rng.chance(20))
        {
            // at the start of a symbol, with exactly the room and input inflate_fast is entered at (258 and 10), or one less
            z_stream_s &zs = mem<z_stream_s>(env.z());
            const uint32_t window = mem<uint32_t>(b.s + 0x28u), end = mem<uint32_t>(b.s + 0x2cu);
            mem<uint32_t>(codes) = 0;
            if (end - window > 300)
            {
                const uint32_t m = 257 + rng.below(3);
                mem<uint32_t>(b.s + 0x34u) = end - m;
                mem<uint32_t>(b.s + 0x30u) = window;
                zs.avail_out = std::max(zs.avail_out, 300u);
            }
            zs.avail_in = std::min(inputLeft(env, b.input), 9 + rng.below(3));
        }
        if (rng.chance(40))
        {
            // plenty of room and input, so that inflate_fast is entered
            z_stream_s &zs = mem<z_stream_s>(env.z());
            zs.avail_in = std::max(zs.avail_in, 10u + rng.below(40));
            zs.avail_in = std::min(zs.avail_in, inputLeft(env, b.input) + 0u);
        }
    }
    static constexpr int32_t kResults[] = {0, 0, 0, Z_BUF_ERROR, 1};
    setArg(ctx, 0, b.s);
    setArg(ctx, 1, b.z);
    setArg(ctx, 2, static_cast<uint32_t>(rng.pick(kResults)));
    env.finish();
}

// inflate_fast(bl, bd, tl, td, s, z): at the start of a symbol with at least 258 bytes of room and 10 of input (the
// precondition), or without now and then.
void zFast(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    Blocks b = inCodes(env, false);
    uint32_t bl = 9, bd = 5, tl = 0, td = 0;
    if (b.s != 0)
    {
        const uint32_t codes = mem<uint32_t>(b.s + 4);
        bl = mem<uint8_t>(codes + 0x10u);
        bd = mem<uint8_t>(codes + 0x11u);
        tl = mem<uint32_t>(codes + 0x14u);
        td = mem<uint32_t>(codes + 0x18u);
        z_stream_s &zs = mem<z_stream_s>(env.z());
        // the room: the write pointer where at least 258 bytes (usually) are free
        const uint32_t window = mem<uint32_t>(b.s + 0x28u), end = mem<uint32_t>(b.s + 0x2cu);
        uint32_t &read = mem<uint32_t>(b.s + 0x30u), &write = mem<uint32_t>(b.s + 0x34u);
        if (end - window > 300)
        {
            if (rng.chance(75))
            {
                write = window + rng.below(end - window - 258u);
                if (rng.chance(60))
                    read = write;
                else
                    read = window + rng.below(end - window);
            }
            const uint32_t q = write;
            const uint32_t m = q < read ? read - q - 1 : end - q;
            if (m < 258 && rng.chance(85))
                read = write = window;
        }
        zs.avail_in = std::min(inputLeft(env, b.input), rng.chance(90) ? rng.between(10, 400) : rng.below(10));
        zs.avail_out = outputLeft(env);
        if (rng.chance(40))
            zs.avail_out = std::min(zs.avail_out, rng.between(1, 300));
        if (inputLeft(env, b.input) < 10 && rng.chance(80))
        {
            // too little input for the loop's own reads: the input buffer's noise past the stream
            zs.avail_in = std::min(zs.avail_in, inputLeft(env, b.input));
        }
    }
    setArg(ctx, 0, bl);
    setArg(ctx, 1, bd);
    setArg(ctx, 2, tl);
    setArg(ctx, 3, td);
    setArg(ctx, 4, b.s);
    setArg(ctx, 5, b.z);
    env.finish();
}

// inflate_codes_new(bl, bd, tl, td, z)
void zCodesNew(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    z_stream_s &zs = mem<z_stream_s>(env.z());
    zs.zalloc = ee_fnptr{kZcalloc};
    zs.zfree = ee_fnptr{kZcfree};
    zs.opaque = ptr32<void>(rng.chance(70) ? 0u : rng.next());
    setArg(ctx, 0, rng.chance(60) ? rng.between(1, 15) : rng.next());
    setArg(ctx, 1, rng.chance(60) ? rng.between(1, 15) : rng.next());
    setArg(ctx, 2, scratch + 0x4000u + 8u * rng.below(100));
    setArg(ctx, 3, scratch + 0x4400u + 8u * rng.below(100));
    setArg(ctx, 4, env.z());
    env.finish();
}

// inflate_codes_free(c, z): a block the stand-in made (the newest, so the arena takes it back, or an older one).
void zCodesFree(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    z_stream_s &zs = mem<z_stream_s>(env.z());
    zs.zalloc = ee_fnptr{kZcalloc};
    zs.zfree = ee_fnptr{rng.chance(10) ? 0x0u : kZcfree};
    zs.opaque = ptr32<void>(rng.chance(70) ? 0u : rng.next());
    if (zs.zfree.address == 0)
        zs.zfree = ee_fnptr{kZcfree};
    const uint32_t blocks = rng.between(1, 3);
    uint32_t block = 0;
    for (uint32_t i = 0; i < blocks; ++i)
        block = guest_call<ptr32<void>>(kZcalloc, zs.opaque, 1u, 0x1cu).address;
    setArg(ctx, 0, rng.chance(20) ? scratch + 0x4800u : block);
    setArg(ctx, 1, env.z());
    env.finish();
}

// inflate_flush(s, z, r): a block state with a window of its own (any size, the read and write pointers in every order,
// at the end of the window and beyond), a checksum function or none, and an output of any room.
void zFlush(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    Rng &rng = env.rng;
    const uint32_t s = scratch + kMisc + 0x100u;
    const uint32_t window = scratch + kArena;
    const uint32_t size = rng.chance(30) ? rng.between(1, 16) : rng.chance(50) ? rng.between(17, 600) : rng.between(1, 4096);
    const uint32_t end = window + size;
    auto position = [&]() -> uint32_t
    {
        const uint32_t kind = rng.below(100);
        if (kind < 12)
            return window;
        if (kind < 24)
            return end;
        if (kind < 30 && size > 1)
            return end - 1;
        return window + rng.below(size + 1);
    };
    uint32_t read = position(), write = position();
    if (rng.chance(8))
        write = read;
    mem<uint32_t>(s) = rng.below(10);
    mem<uint32_t>(s + 0x28u) = window;
    mem<uint32_t>(s + 0x2cu) = end;
    mem<uint32_t>(s + 0x30u) = read;
    mem<uint32_t>(s + 0x34u) = write;
    mem<uint32_t>(s + 0x38u) = rng.chance(55) ? sym::adler32 : 0u;
    mem<uint32_t>(s + 0x3cu) = rng.chance(50) ? 1u : rng.next();
    z_stream_s &zs = mem<z_stream_s>(env.z());
    zs.next_out = ptr32<Bytef>(env.out() + rng.below(0x100));
    const uint32_t kind = rng.below(100);
    zs.avail_out = kind < 15 ? 0u : kind < 35 ? rng.between(1, 8) : kind < 80 ? rng.between(1, 2000) : 0xe00u;
    zs.total_out = rng.next();
    zs.adler = rng.next();
    static constexpr int32_t kResults[] = {0, 0, Z_BUF_ERROR, Z_BUF_ERROR, 1, 2, -3};
    setArg(ctx, 0, s);
    setArg(ctx, 1, env.z());
    setArg(ctx, 2, static_cast<uint32_t>(rng.pick(kResults)));
    env.finish();
}

// ---------------------------------------------------------------------------------------------------------------------
// The table builders. The tables go in scratch at +0x3000 (the arena's place; 11520 bytes), the work space v after.

namespace
{
constexpr uint32_t kHufts = kArena, kV = 0x6000u, kHn = kMisc + 0x20u, kTreeOut = kMisc + 0x40u;

void fillLengths(uint32_t at, const std::vector<uint32_t> &lengths)
{
    putWords(at, lengths);
}
} // namespace

// huft_build(b, n, s, d, e, t, m, hp, hn, v): code lengths of a bit length tree (19 of 0-7), a literal/length tree (257 to
// 288 of 0-15, with the length bases and extra bits) or a distance tree (1 to 30 of 0-15); hn at 0 or part-filled.
void zHuftBuild(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, false, ctx);
    Rng &rng = env.rng;
    const uint32_t kind = rng.below(3);
    uint32_t n, sCount, d = 0, e = 0, maxLen;
    if (kind == 0)
    {
        n = 19;
        sCount = 19;
        maxLen = 7;
    }
    else if (kind == 1)
    {
        n = rng.chance(30) ? 288 : rng.between(257, 286);
        sCount = 257;
        d = sym::cplens;
        e = sym::cplext;
        maxLen = 15;
    }
    else
    {
        n = rng.between(1, 30);
        sCount = 0;
        d = sym::cpdist;
        e = sym::cpdext;
        maxLen = 15;
    }
    if (rng.chance(8))
        sCount = rng.of<uint32_t>({0, 1, 19, 256, 257, 288});
    if (kind == 1 && rng.chance(25))
    {
        // literal/length lengths with many long codes, to fill the table area
        maxLen = 15;
    }
    std::vector<uint32_t> lengths = makeLengths(rng, n, maxLen);
    const uint32_t b = scratch + kMisc + 0x200u;
    fillLengths(b, lengths);
    mem<uint32_t>(scratch + kHn) = rng.chance(70) ? 0u : rng.chance(50) ? rng.below(1440) : 1440u - rng.below(40);
    mem<uint32_t>(scratch + kTreeOut) = rng.next();                                     // *t
    static constexpr uint32_t kBits[] = {9, 6, 7, 5, 1, 2, 15, 8, 0, 20};
    mem<uint32_t>(scratch + kTreeOut + 4) = rng.chance(85) ? (kind == 2 ? 6u : (kind == 1 ? 9u : 7u)) : rng.pick(kBits); // *m
    const uint32_t sp = getRegU32(ctx, 29);
    mem<uint32_t>(sp) = scratch + kHn;
    mem<uint32_t>(sp + 8) = scratch + kV;
    setArg(ctx, 0, b);
    setArg(ctx, 1, n);
    setArg(ctx, 2, sCount);
    setArg(ctx, 3, d);
    setArg(ctx, 4, e);
    setArg(ctx, 5, scratch + kTreeOut);
    setArg(ctx, 6, scratch + kTreeOut + 4u);
    setArg(ctx, 7, scratch + kHufts);
}

// inflate_trees_bits(c, bb, tb, hp, z)
void zTreesBits(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    ctl().top = scratch + kArena + 0x2e00u; // the tables take the arena's first 11.5 KB
    Rng &rng = env.rng;
    std::vector<uint32_t> lengths = makeLengths(rng, 19, 7);
    const uint32_t c = scratch + kMisc + 0x200u;
    fillLengths(c, lengths);
    mem<uint32_t>(scratch + kTreeOut) = rng.next();
    mem<uint32_t>(scratch + kTreeOut + 4) = rng.chance(85) ? 7u : rng.below(20);
    z_stream_s &zs = mem<z_stream_s>(env.z());
    zs.zalloc = ee_fnptr{kZcalloc};
    zs.zfree = ee_fnptr{kZcfree};
    zs.opaque = ptr32<void>(rng.chance(70) ? 0u : rng.next());
    zs.msg = ptr32<char>(rng.chance(50) ? 0u : rng.next());
    setArg(ctx, 0, c);
    setArg(ctx, 1, scratch + kTreeOut + 4u);
    setArg(ctx, 2, scratch + kTreeOut);
    setArg(ctx, 3, scratch + kHufts);
    setArg(ctx, 4, env.z());
    env.finish();
}

// inflate_trees_dynamic(nl, nd, c, bl, bd, tl, td, hp, z): nl 257 to 286 literal/length lengths then nd 1 to 30 distance ones.
void zTreesDynamic(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Env env(state, scratch, true, ctx);
    ctl().top = scratch + kArena + 0x2e00u; // the tables take the arena's first 11.5 KB
    Rng &rng = env.rng;
    const uint32_t nl = rng.chance(20) ? 257u : rng.between(257, 286), nd = rng.chance(15) ? 1u : rng.between(1, 30);
    std::vector<uint32_t> literal = makeLengths(rng, nl, 15), distance = makeLengths(rng, nd, 15);
    if (rng.chance(15))
        std::fill(distance.begin(), distance.end(), 0u);
    if (rng.chance(8) && nl > 256)
        literal[256] = 0; // no end of block code
    const uint32_t c = scratch + kMisc + 0x200u;
    fillLengths(c, literal);
    fillLengths(c + 4u * nl, distance);
    const uint32_t out = scratch + kTreeOut;
    mem<uint32_t>(out) = rng.chance(85) ? 9u : rng.below(20);      // bl
    mem<uint32_t>(out + 4) = rng.chance(85) ? 6u : rng.below(20);  // bd
    mem<uint32_t>(out + 8) = rng.next();                           // tl
    mem<uint32_t>(out + 12) = rng.next();                          // td
    z_stream_s &zs = mem<z_stream_s>(env.z());
    zs.zalloc = ee_fnptr{kZcalloc};
    zs.zfree = ee_fnptr{kZcfree};
    zs.opaque = ptr32<void>(rng.chance(70) ? 0u : rng.next());
    zs.msg = ptr32<char>(rng.chance(50) ? 0u : rng.next());
    const uint32_t sp = getRegU32(ctx, 29);
    mem<uint32_t>(sp) = env.z();
    setArg(ctx, 0, nl);
    setArg(ctx, 1, nd);
    setArg(ctx, 2, c);
    setArg(ctx, 3, out);
    setArg(ctx, 4, out + 4u);
    setArg(ctx, 5, out + 8u);
    setArg(ctx, 6, out + 12u);
    setArg(ctx, 7, scratch + kHufts);
    env.finish();
}

// inflate_trees_fixed(bl, bd, tl, td, z)
void zTreesFixed(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t at = scratch + kMisc + 0x300u;
    setArg(ctx, 0, at);
    setArg(ctx, 1, at + 4u);
    setArg(ctx, 2, at + 8u);
    setArg(ctx, 3, at + 12u);
    setArg(ctx, 4, rng.chance(50) ? 0u : rng.next());
}

} // namespace fuzz

} // namespace rc
