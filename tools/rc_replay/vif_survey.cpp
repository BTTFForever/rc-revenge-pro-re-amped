// rc_replay's vifstat consumer: see vif_survey.h. Measurement for RENDERER_3D_DESIGN.md; not the reader.
#include "vif_survey.h"

#include "chain_walk.h"
#include "format.h"
#include "picture.h"

#include "../../render/gs2d/gs_model.h"
#include "../../render/gs2d/gs_plan.h"
#include "../../render/gs2d/gs_state.h"
#include "../../render/tex/gif_walk.h"

#include "ps2_runtime.h"
#if RC_HAVE_VU1_CODE
#include "ps2_vu1_impl.inl"
#include "vu1_ref.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <xmmintrin.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#if RC_HAVE_VU1_CODE
#define RC_VIFSTAT_IMAGES(X) \
    X(0x6EC8B9FF8FA44221ull) X(0xBAC581270F262987ull) X(0xAB0E7743A9BA7393ull) X(0xEC08B22243D4DC8Cull) X(0xE96770E4143D2DA7ull) \
    X(0xC3952C0FAEB52EADull)
#define RC_VIFSTAT_DECLARE(hash) \
    template <> VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<hash>(VU1Interpreter &, const Vu1RunArgs &);
RC_VIFSTAT_IMAGES(RC_VIFSTAT_DECLARE)
#endif

namespace GSRaster
{
    std::array<uint64_t, 8> &FallbackCounters(); // the software GS's counters of draws that left its fast paths
}

namespace rc::replay
{
    using namespace rc::rec;
    using namespace rc::rd;

    namespace
    {
        std::string fmt(const char *f, ...)
        {
            char buf[512];
            va_list ap;
            va_start(ap, f);
            std::vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        uint64_t fnv1a64(const uint8_t *data, size_t size) // the runtime's image hash (ps2_vu1_core.cpp)
        {
            uint64_t hash = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= data[i];
                hash *= 0x100000001B3ull;
            }
            return hash;
        }

        struct Program
        {
            uint64_t hash;
            const char *name;
            uint32_t entry1, entry2; // besides 0 (decomp/vu1/vu1_check.cpp)
        };
        const Program kPrograms[6] = {
            {0xC3952C0FAEB52EADull, "TRdr_VUShapeProg", 0xFFFFFFFFu, 0x2050u},
            {0xAB0E7743A9BA7393ull, "EnvRdr_VUShapeProg", 0x78u, 0x2370u},
            {0xE96770E4143D2DA7ull, "Rdr_VUWaterProg", 0xFFFFFFFFu, 0xFFFFFFFFu},
            {0xEC08B22243D4DC8Cull, "Rdr_VUSkinProg", 0xFFFFFFFFu, 0xFFFFFFFFu},
            {0xBAC581270F262987ull, "TERdr_VUShapeProg", 0xFFFFFFFFu, 0x2198u},
            {0x6EC8B9FF8FA44221ull, "Rdr_VUShapeProg", 0xFFFFFFFFu, 0xFFFFFFFFu},
        };
        // The runtime's lookup (ps2_vu1_core.cpp, findCompiledVu1ProgramInRegistry): a program is the longest prefix of
        // micro memory whose FNV-1a hash, continued over zeros to 16 KB, is a registered image's. Returns the index in
        // kPrograms or -1, and the prefix length in bytes.
        int matchProgram(const uint8_t *code, uint32_t &prefixBytes)
        {
            constexpr uint64_t kPrime = 0x100000001B3ull;
            constexpr size_t pairs = 0x4000u / 8u;
            static std::vector<uint64_t> prefixes(pairs);
            uint64_t hash = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < pairs; ++i)
            {
                for (size_t b = 0; b < 8u; ++b)
                {
                    hash ^= code[i * 8u + b];
                    hash *= kPrime;
                }
                prefixes[i] = hash;
            }
            uint64_t zeroPow = 1ull;
            for (size_t i = pairs; i > 0u; --i)
            {
                const uint64_t h = prefixes[i - 1u] * zeroPow;
                for (int p = 0; p < 6; ++p)
                    if (kPrograms[p].hash == h)
                    {
                        prefixBytes = static_cast<uint32_t>(i * 8u);
                        return p;
                    }
                for (size_t b = 0; b < 8u; ++b)
                    zeroPow *= kPrime;
            }
            prefixBytes = 0;
            return -1;
        }

        const char *bucket(uint64_t v)
        {
            static const char *const names[] = {"b00 0", "b01 1", "b02 2-4", "b03 5-8", "b04 9-16", "b05 17-32", "b06 33-64", "b07 65-128",
                                                "b08 129-256", "b09 257-512", "b10 513-1024", "b11 1025-2048", "b12 2049-4096",
                                                "b13 4097-8192", "b14 8193-16384", "b15 16385-32768", "b16 32769+"};
            if (v == 0)
                return names[0];
            if (v == 1)
                return names[1];
            int i = 2;
            uint64_t hi = 4;
            while (v > hi && i < 16)
            {
                hi *= 2;
                ++i;
            }
            return names[i];
        }

        struct Box
        {
            int x0, y0, x1, y1;
            bool disjoint(const Box &o) const { return x1 < o.x0 || o.x1 < x0 || y1 < o.y0 || o.y1 < y0; }
        };

        // render/gs2d/gs2d_vk.cpp's rule: two triangles that are the halves of one axis-aligned rectangle.
        bool rectangleHalves(const GsVertex *a, const GsVertex *b)
        {
            uint16_t xs[2], ys[2];
            uint32_t nx = 0, ny = 0;
            const GsVertex *v[6] = {a, a + 1, a + 2, b, b + 1, b + 2};
            for (const GsVertex *p : v)
            {
                bool hx = false, hy = false;
                for (uint32_t i = 0; i < nx; ++i)
                    hx |= xs[i] == p->x;
                for (uint32_t i = 0; i < ny; ++i)
                    hy |= ys[i] == p->y;
                if (!hx)
                {
                    if (nx == 2)
                        return false;
                    xs[nx++] = p->x;
                }
                if (!hy)
                {
                    if (ny == 2)
                        return false;
                    ys[ny++] = p->y;
                }
            }
            if (nx != 2 || ny != 2)
                return false;
            uint32_t shared = 0;
            for (int i = 0; i < 3; ++i)
            {
                for (int j = i + 1; j < 3; ++j)
                    if ((a[i].x == a[j].x && a[i].y == a[j].y) || (b[i].x == b[j].x && b[i].y == b[j].y))
                        return false;
                for (int j = 0; j < 3; ++j)
                    shared += a[i].x == b[j].x && a[i].y == b[j].y;
            }
            return shared == 2;
        }

        constexpr int kW = 640, kH = 256;
        constexpr uint32_t kPoisonMask = 0xFFFF00FFu, kPoisonBits = 0x4000005Au;
        uint32_t poison(int reg, int lane) { return kPoisonBits | static_cast<uint32_t>(reg * 4 + lane) << 8; }

        struct StateKey
        {
            uint64_t alpha, test, tex1, clamp;
            uint16_t prim;
            uint8_t pabe, zmsk, zpsm, fba;
            uint32_t texFormat; // PSM, TW, TH, TCC, TFX
            bool operator<(const StateKey &o) const { return std::memcmp(this, &o, sizeof(*this)) < 0; }
        };
        struct StateCount
        {
            uint64_t prims = 0, boxPixels = 0, pixels = 0;
            GsPlan plan{};
        };

        // One kick's drawing, for the barrier rules.
        struct KickDraw
        {
            uint32_t prims = 0, culled = 0, batches = 0, readPrims = 0, readBatches = 0, splitPrims = 0;
            uint32_t groups0 = 0, barriers0 = 0, draws0 = 0; // as built (gs2d_vk.cpp)
            uint32_t barriersT = 0, barriersX = 0, barriersP = 0, drawsP = 0; // dirty tiles, dirty boxes, dirty pixels
            uint32_t barriersRun = 0; // one per run of consecutive reading batches whose primitives are pairwise disjoint
            uint32_t barriersC[2] = {}, tileTests[2] = {}; // dirty tiles by conservative coverage, 8x8 and 4x4
            uint64_t readPixels = 0, pixels = 0;
            uint32_t batchesTexOnly = 0; // batches that differ from the one before only in the texture (TEX0, MIPTBP)
            uint64_t trackedPixels = 0; // covered pixels of the primitives from the kick's first reading one on
            uint32_t trackedPrims = 0;
            bool nonSprite = false;
        };
    }

    struct VifSurvey::Impl final : GifSink
    {
        PS2Runtime *rt = nullptr;
        FileHeader header{};
        // The experiment of RENDERER_3D_DESIGN.md U1: the machine's PATH1 packets are held back while a VU1 program
        // runs (round toward zero) and handed to the GS afterwards under the kick's own rounding mode.
        bool roundNearest = false;
        std::vector<uint8_t> picture;
        uint64_t picDiff[4][7] = {}, picPixels = 0, picPixelsDiffer = 0, picCycles = 0, picCyclesDiffer = 0;
#if RC_HAVE_VU1_CODE
        vu1_impl::Vu1PacketSink sink;
        PS2Memory *sinkMemory = nullptr;
        void flushSink()
        {
            if (sink.packets.empty() || !sinkMemory)
                return;
            const uint32_t csr = _mm_getcsr();
            _mm_setcsr(csr & ~0x6000u);
            for (const std::vector<uint8_t> &p : sink.packets)
                sinkMemory->submitGifPacket(GifPathId::Path1, p.data(), static_cast<uint32_t>(p.size()));
            _mm_setcsr(csr);
            sink.packets.clear();
        }
#endif
        std::map<std::pair<std::string, std::string>, std::array<uint64_t, 2>> sums;
        std::map<std::pair<std::string, std::string>, uint64_t> maxes;
        void add(const std::string &c, const std::string &k, uint64_t n = 1, uint64_t x = 0)
        {
            auto &e = sums[{c, k}];
            e[0] += n;
            e[1] += x;
        }
        void addMax(const std::string &c, const std::string &k, uint64_t v)
        {
            uint64_t &e = maxes[{c, k}];
            e = std::max(e, v);
        }
        void dist(const std::string &name, uint64_t v)
        {
            add(name, bucket(v), 1, v);
            add("total", name, 1, v);
            addMax("max", name, v);
        }

        // ---- the VIF1 model ----
        struct Vif
        {
            uint32_t cycle = 0, mode = 0, mask = 0, base = 0, ofst = 0, tops = 0, itops = 0, top = 0, itop = 0;
            bool dbf = false;
        } vif;
        bool vifKnown = false;
        std::vector<uint8_t> micro = std::vector<uint8_t>(0x4000u, 0);
        bool microKnown = false, microDirty = true;
        uint64_t microHash = 0;
        int microProgram = -1;
        uint32_t microPrefix = 0;
        uint64_t opCount[128] = {}, opBytes[128] = {};
        struct UnpackCount
        {
            uint64_t codes = 0, vectors = 0, bytes = 0;
        };
        std::unordered_map<uint64_t, UnpackCount> unpacks;
        std::unordered_map<uint32_t, uint64_t> stcycl, stmod, stmask, baseV, offsetV, itopV;
        std::map<std::string, std::array<uint64_t, 2>> mscals, mpgs;
        uint64_t destAbs[32] = {}, destRel[32] = {};
        struct Piece
        {
            uint32_t start, bytes, phys;
            bool isTag, scratch;
        };
        std::vector<uint8_t> stream;
        std::vector<Piece> pieces;
        uint64_t vifKicks = 0;

        // ---- the GS side ----
        uint64_t regs[0x80] = {};
        bool haveRegs = false, regsAfterKick = false;
        int path = 0;
        GifWalker walker[4];
        uint64_t regWrites[4][0x80] = {};
        std::unordered_map<uint64_t, uint64_t> regValues[4][0x80];
        GsVertex queue[3] = {};
        uint32_t queued = 0;
        uint64_t rgbaq = 0, st = 0, uv = 0;
        bool stateDirty = true;
        uint64_t stateSerial = 0;
        GsState state{};
        int statePath = -1;
        GsPlan plan{};
        StateCount *cur = nullptr;
        std::map<StateKey, StateCount> states[4];
        std::string texKey, envKey;
        uint64_t *texCount = nullptr, *envCount = nullptr;
        std::map<std::string, uint64_t> texFormats[4], envs[4];
        struct Geo
        {
            uint64_t tris = 0, lines = 0, points = 0, sprites = 0, primType[8] = {};
            uint64_t qNotOne = 0, qDiffer = 0, zDiffer = 0, aDiffer = 0, rgbDiffer = 0, subpixel = 0, fst = 0, flatIip = 0;
            uint64_t zeroArea = 0, boxEmpty = 0, clippedByScissor = 0, outside[6] = {}, stRange[6] = {}, zAbove24 = 0, tme0 = 0, iip0 = 0;
            uint64_t backFacing = 0;
            uint32_t zMin = 0xFFFFFFFFu, zMax = 0;
            uint64_t boxArea[8] = {};
        } geo[4];
        std::map<std::string, std::array<uint64_t, 2>> gifTags[4];
        uint32_t lastCsr[4] = {};
        std::array<uint64_t, 2> *csrCount[4] = {};
        uint64_t lastTagLo = 0, lastTagHi = 0;
        uint32_t lastTagBytes = 0;
        std::array<uint64_t, 2> *lastTagCount = nullptr;

        // the kick in progress
        KickDraw kd;
        uint64_t batchSerial = ~0ull;
        GsState batchState{};
        uint32_t batchIndex = 0;
        bool batchReads = false, batchSplit = false, anyDrawn0 = false, prevValid = false, runOpen = false;
        std::vector<Box> group, dirtyBoxes, runBoxes;
        Box prevBox{};
        GsVertex prevTri[3] = {};
        uint64_t tiles[kH / 8][2] = {};
        // Dirty tiles by conservative coverage: a tile is touched when no edge of the triangle rejects it.
        struct TileLever
        {
            int shift = 3, words = 2, rows = 32;
            std::vector<uint64_t> bits, before;
            void init(int sh)
            {
                shift = sh;
                words = ((kW >> sh) + 63) / 64;
                rows = kH >> sh;
                bits.assign(static_cast<size_t>(words) * rows, 0);
                before = bits;
            }
            void clear()
            {
                std::fill(bits.begin(), bits.end(), 0);
                std::fill(before.begin(), before.end(), 0);
            }
            // Visits the tiles of `box` the primitive may cover; f(index, bit) returns true to stop.
            template <typename F> bool visit(const Box &box, const GsTri *t, uint32_t &tests, F f) const
            {
                const int size = 1 << shift;
                for (int ty = box.y0 >> shift; ty <= box.y1 >> shift; ++ty)
                    for (int tx = box.x0 >> shift; tx <= box.x1 >> shift; ++tx)
                    {
                        ++tests;
                        if (t)
                        {
                            const int x0 = std::max(tx * size, box.x0), x1 = std::min(tx * size + size - 1, box.x1);
                            const int y0 = std::max(ty * size, box.y0), y1 = std::min(ty * size + size - 1, box.y1);
                            bool out = false;
                            for (int i = 0; i < 3 && !out; ++i)
                                out = gsTriEdge(*t, i, t->a[i] > 0 ? x1 : x0, t->b[i] > 0 ? y1 : y0) + t->bias[i] <= 0;
                            if (out)
                                continue;
                        }
                        if (f(static_cast<size_t>(ty) * words + (tx >> 6), 1ull << (tx & 63)))
                            return true;
                    }
                return false;
            }
        } lever[2];
        std::vector<uint32_t> pix = std::vector<uint32_t>(static_cast<size_t>(kW) * kH, 0u);
        uint32_t pixGen = 1;
        uint32_t kickPackets[4] = {};
        uint64_t kickGifBytes[4] = {};
        uint32_t kickMscals = 0;
        uint64_t kickRefNs = 0;
        // per cycle
        uint64_t cycBarriers0 = 0, cycBarriersP = 0, cycBarriersT = 0, cycDraws0 = 0, cycPrims = 0, cycReadPrims = 0, cycRefNs = 0;
        bool cycAny = false;

        // ---- the VU1 runs ----
        struct Run
        {
            uint64_t runs = 0, entries[3] = {}, continued = 0, packets = 0, packetMismatch = 0, countMismatch = 0, memoryMismatch = 0;
            uint64_t cutShort = 0, ns = 0, leakRuns = 0, computedRuns = 0, countDiffRuns = 0, regsMoved = 0;
            uint64_t leak[128] = {}, leakFrom[128][7] = {};
            std::map<uint32_t, uint64_t> leakValues[128];
            uint32_t vfChanged[32] = {}; // lanes
            uint32_t viChanged = 0, accChanged = 0;
            bool qChanged = false, pChanged = false, iChanged = false;
        } run[6];
        std::vector<std::vector<uint8_t>> expected, poisoned;
        size_t nextPacket = 0;
        bool runActive = false;
        int runProgram = -1;
        std::vector<uint8_t> refMem = std::vector<uint8_t>(0x4000u), poisonMem = std::vector<uint8_t>(0x4000u);
        float vfBefore[32][4] = {}, vfAfterLast[32][4] = {};
        int32_t viBefore[16] = {};
        float accBefore[4] = {}, qBefore = 0, pBefore = 0, iBefore = 0;
        bool haveAfter = false;
        uint8_t lastWriter[128]; // program index + 1 that last changed the lane; 0: the keyframe
        uint64_t path1Outside = 0;

        Impl() { std::memset(lastWriter, 0, sizeof(lastWriter)); }

        // ---------------------------------------------------------------------------------------------------------
        void loadVif(const Vif1Regs &r)
        {
            vif.cycle = r.cycle;
            vif.mode = r.mode;
            vif.mask = r.mask;
            vif.base = r.base;
            vif.ofst = r.ofst;
            vif.tops = r.tops;
            vif.itops = r.itops;
            vif.top = r.top;
            vif.itop = r.itop;
            vif.dbf = (r.stat >> 7) & 1u;
        }

        const Piece *pieceAt(uint32_t pos, size_t &hint) const
        {
            while (hint + 1 < pieces.size() && pieces[hint + 1].start <= pos)
                ++hint;
            return hint < pieces.size() ? &pieces[hint] : nullptr;
        }

        void vifKick(const KickView &k, const RamView &ram)
        {
            ++vifKicks;
            stream.clear();
            pieces.clear();
            struct Collect final : WalkVisitor
            {
                Impl *self;
                bool tte;
                uint32_t lastId = 8;
                void range(const WalkRange &r) override
                {
                    if (r.isTag)
                    {
                        uint32_t w;
                        std::memcpy(&w, r.data, 4);
                        lastId = (w >> 28) & 7u;
                        self->add("dma tag", fmt("id %u", lastId), 1, 0);
                        if (tte)
                        {
                            self->pieces.push_back({static_cast<uint32_t>(self->stream.size()), 8u, r.physAddr + 8u, true, r.scratch});
                            self->stream.insert(self->stream.end(), r.data + 8, r.data + 16);
                        }
                        return;
                    }
                    if (!r.bytes)
                        return;
                    self->add("dma payload", lastId < 8 ? fmt("after tag id %u", lastId) : std::string("normal mode"), 1, r.bytes);
                    self->pieces.push_back({static_cast<uint32_t>(self->stream.size()), r.bytes, r.physAddr, false, r.scratch});
                    self->stream.insert(self->stream.end(), r.data, r.data + r.bytes);
                }
            } collect;
            collect.self = this;
            collect.tte = k.tte;
            const WalkResult wr = walkChain(k, ram, &collect);
            if (wr.absentPageReads || wr.untranslatable)
                add("vif walk", "absent or untranslatable", 1, wr.absentPageReads + wr.untranslatable);
            if (stream.size() != k.streamBytes)
                add("vif walk", "flattened bytes differ from the recorded streamBytes", 1, 0);
            add("vif kick", fmt("mode %u tte %u", k.mode, k.tte ? 1 : 0), 1, stream.size());

            bool sawStcycl = false, sawBase = false, sawOffset = false, sawStmod = false, sawStmask = false, sawItop = false, sawMpg = false;
            uint32_t codes = 0, mscalCount = 0, unpackCodes = 0;
            const uint8_t *data = stream.data();
            const uint32_t size = static_cast<uint32_t>(stream.size());
            uint32_t pos = 0;
            size_t hint = 0;
            auto span = [&](uint32_t codeAt, uint32_t end, const char *what)
            {
                // Where a code's data lies relative to the code's own piece.
                const Piece *p = pieceAt(codeAt, hint);
                if (!p || end <= p->start + p->bytes)
                    return;
                size_t h2 = hint;
                const Piece *q = pieceAt(end - 1u, h2);
                if (p->isTag && q && !q->isTag && h2 == hint + 1)
                    add("vif code span", fmt("%s: code in a DMA tag, data in that tag's payload", what), 1, 0);
                else
                    add("vif code span", fmt("%s: data crosses %zu piece boundaries (code in %s)", what, h2 - hint, p->isTag ? "tag" : "payload"), 1, 0);
            };
            while (pos + 4u <= size)
            {
                const uint32_t codeAt = pos;
                uint32_t cmd;
                std::memcpy(&cmd, data + pos, 4);
                pos += 4u;
                const uint8_t op = (cmd >> 24) & 0x7Fu;
                const uint16_t imm = cmd & 0xFFFFu;
                const uint8_t num = (cmd >> 16) & 0xFFu;
                ++codes;
                ++opCount[op];
                if (cmd & 0x80000000u)
                    add("vif", "code with the interrupt bit", 1, 0);
                uint32_t dataBytes = 0;
                switch (op)
                {
                case 0x00: // NOP
                    break;
                case 0x01:
                    vif.cycle = imm;
                    ++stcycl[imm];
                    sawStcycl = true;
                    break;
                case 0x02:
                    vif.ofst = imm & 0x3FFu;
                    vif.tops = vif.base & 0x3FFu;
                    vif.dbf = false;
                    ++offsetV[imm & 0x3FFu];
                    sawOffset = true;
                    break;
                case 0x03:
                    vif.base = imm & 0x3FFu;
                    ++baseV[imm & 0x3FFu];
                    sawBase = true;
                    break;
                case 0x04:
                    vif.itops = imm & 0x3FFu;
                    ++itopV[imm & 0x3FFu];
                    sawItop = true;
                    break;
                case 0x05:
                    vif.mode = imm & 3u;
                    ++stmod[imm & 3u];
                    sawStmod = true;
                    break;
                case 0x06: // MSKPATH3
                    add("vif", fmt("MSKPATH3 0x%04x", imm), 1, 0);
                    break;
                case 0x07: // MARK
                case 0x10: // FLUSHE
                case 0x11: // FLUSH
                case 0x13: // FLUSHA
                    break;
                case 0x14: // MSCAL
                case 0x15: // MSCALF
                case 0x17: // MSCNT
                {
                    if (microDirty)
                    {
                        microHash = fnv1a64(micro.data(), micro.size());
                        microProgram = matchProgram(micro.data(), microPrefix);
                        microDirty = false;
                        add("vif program", microProgram >= 0 ? fmt("%s, %u bytes of micro memory (FNV-1a %016" PRIx64 ")%s", kPrograms[microProgram].name, microPrefix,
                                                                 fnv1a64(micro.data(), microPrefix),
                                                                 kPrograms[microProgram].hash == microHash ? ", zeros after it" : ", another program's tail after it")
                                                             : fmt("unknown image %016" PRIx64, microHash),
                            1, 0);
                    }
                    const std::string prog = microProgram >= 0 ? std::string(kPrograms[microProgram].name) : fmt("unknown image %016" PRIx64, microHash);
                    auto &e = mscals[op == 0x17 ? fmt("%s MSCNT", prog.c_str()) : fmt("%s %s pc 0x%04x", prog.c_str(), op == 0x14 ? "MSCAL" : "MSCALF", imm * 8u)];
                    ++e[0];
                    if (!sawMpg)
                        ++e[1]; // the program was uploaded by an earlier kick
                    if (!sawBase || !sawOffset)
                        add("vif inherit", "MSCAL with BASE or OFFSET from an earlier kick", 1, 0);
                    if (!sawItop)
                        add("vif inherit", "MSCAL with ITOPS from an earlier kick", 1, 0);
                    if (!microKnown)
                        add("vif inherit", "MSCAL with micro memory unknown", 1, 0);
                    add("vif top", fmt("TOP 0x%03x at MSCAL (BASE 0x%03x OFST 0x%03x DBF %u)", vif.tops, vif.base, vif.ofst, vif.dbf ? 1 : 0), 1, 0);
                    vif.top = vif.tops;
                    vif.itop = vif.itops;
                    vif.tops = vif.dbf ? (vif.base & 0x3FFu) : ((vif.base + vif.ofst) & 0x3FFu);
                    vif.dbf = !vif.dbf;
                    ++mscalCount;
                    break;
                }
                case 0x20: // STMASK
                    if (pos + 4u <= size)
                    {
                        std::memcpy(&vif.mask, data + pos, 4);
                        ++stmask[vif.mask];
                    }
                    dataBytes = 4;
                    sawStmask = true;
                    break;
                case 0x30: // STROW
                case 0x31: // STCOL
                    dataBytes = 16;
                    break;
                case 0x4A: // MPG
                {
                    const uint32_t dest = static_cast<uint32_t>(imm) * 8u, bytes = (num ? num : 256u) * 8u;
                    dataBytes = bytes;
                    if (pos + bytes <= size && dest < 0x4000u)
                    {
                        const uint32_t n = std::min(bytes, 0x4000u - dest);
                        std::memcpy(micro.data() + dest, data + pos, n);
                        microDirty = true;
                        size_t h = hint;
                        const Piece *p = pieceAt(codeAt, h);
                        auto &e = mpgs[fmt("dest 0x%04x, %u bytes, payload %016" PRIx64 ", code at %s0x%07x", dest, bytes, fnv1a64(data + pos, bytes),
                                           p && p->scratch ? "scratchpad " : "", p ? p->phys + (codeAt - p->start) : 0u)];
                        ++e[0];
                    }
                    sawMpg = true;
                    span(codeAt, pos + bytes, "MPG");
                    break;
                }
                case 0x50:
                case 0x51:
                    dataBytes = (imm ? imm : 65536u) * 16u;
                    add("vif", "DIRECT or DIRECTHL (PATH2)", 1, dataBytes);
                    break;
                default:
                    if ((op & 0x60u) == 0x60u)
                    {
                        const uint32_t vn = (op >> 2) & 3u, vl = op & 3u;
                        const bool maskOn = (op & 0x10u) != 0u, flg = (imm & 0x8000u) != 0u, usn = (imm & 0x4000u) != 0u;
                        const uint32_t components = vn + 1u;
                        const uint32_t bits = vl == 0 ? 32u : vl == 1 ? 16u : vl == 2 ? 8u : (vn == 3 ? 4u : 16u);
                        const uint32_t bitsPerVector = (vl == 3 && vn == 3) ? 16u : components * bits;
                        const uint32_t bytesPerVector = (bitsPerVector + 7u) / 8u;
                        const uint32_t writes = num ? num : 256u;
                        uint32_t cl = vif.cycle & 0xFFu, wl = (vif.cycle >> 8) & 0xFFu;
                        cl = cl ? cl : 1u;
                        wl = wl ? wl : 1u;
                        uint32_t source = writes;
                        if (cl < wl)
                        {
                            const uint32_t full = writes / wl;
                            const uint32_t rem = std::min(writes % wl, cl);
                            source = full * cl + rem;
                        }
                        dataBytes = (source * bytesPerVector + 3u) & ~3u;
                        const uint32_t addr = ((imm & 0x3FFu) + (flg ? vif.tops : 0u)) & 0x3FFu;
                        const uint64_t key = static_cast<uint64_t>(op & 0x1Fu) | (usn ? 0x20u : 0u) | (flg ? 0x40u : 0u) |
                                             static_cast<uint64_t>(vif.cycle & 0xFFFFu) << 8 | static_cast<uint64_t>(vif.mode & 3u) << 24 |
                                             (maskOn ? static_cast<uint64_t>(vif.mask) << 32 : 0u);
                        UnpackCount &u = unpacks[key];
                        ++u.codes;
                        u.vectors += writes;
                        u.bytes += dataBytes;
                        ++unpackCodes;
                        // The quadwords written (skipping write: cl > wl leaves gaps).
                        const uint32_t lastOffset = ((writes - 1u) / wl) * cl + (writes - 1u) % wl;
                        uint64_t *dest = flg ? destRel : destAbs;
                        const uint32_t first = flg ? (imm & 0x3FFu) : addr;
                        for (uint32_t b = first / 32u; b <= std::min(1023u, first + lastOffset) / 32u; ++b)
                            ++dest[b];
                        if (addr + lastOffset > 0x3FFu)
                            add("vif unpack", "wraps past the end of VU memory", 1, 0);
                        if (!sawStcycl)
                            add("vif inherit", fmt("UNPACK with CYCLE 0x%04x from an earlier kick", vif.cycle), 1, 0);
                        if (maskOn && !sawStmask)
                            add("vif inherit", "masked UNPACK with MASK from an earlier kick", 1, 0);
                        if (!sawStmod)
                            add("vif inherit", fmt("UNPACK with MODE %u from an earlier kick", vif.mode), 1, 0);
                        if (flg && (!sawBase || !sawOffset))
                            add("vif inherit", "TOPS-relative UNPACK with BASE or OFFSET from an earlier kick", 1, 0);
                        span(codeAt, pos + dataBytes, "UNPACK");
                    }
                    else
                        add("vif", fmt("unknown code 0x%02x", op), 1, 0);
                    break;
                }
                opBytes[op] += dataBytes;
                if (pos + dataBytes > size)
                {
                    add("vif walk", fmt("code 0x%02x's data runs past the end of the stream", op), 1, 0);
                    pos = size;
                    break;
                }
                pos += dataBytes;
            }
            if (pos != size)
                add("vif walk", "bytes left after the last code", 1, size - pos);
            dist("k2 vif codes", codes);
            dist("k2 mscal", mscalCount);
            dist("k2 unpack codes", unpackCodes);
            dist("k2 stream KB", size / 1024u);
            dist("k2 dma tags", wr.dmaTags);
        }

        // ---------------------------------------------------------------------------------------------------------
        void beginKick()
        {
            kd = KickDraw{};
            batchSerial = ~0ull;
            batchIndex = 0;
            anyDrawn0 = false;
            prevValid = false;
            runOpen = false;
            group.clear();
            dirtyBoxes.clear();
            runBoxes.clear();
            std::memset(tiles, 0, sizeof(tiles));
            if (lever[0].bits.empty())
            {
                lever[0].init(3);
                lever[1].init(2);
            }
            lever[0].clear();
            lever[1].clear();
            ++pixGen;
            std::memset(kickPackets, 0, sizeof(kickPackets));
            std::memset(kickGifBytes, 0, sizeof(kickGifBytes));
            kickMscals = 0;
            kickRefNs = 0;
            queued = 0;
        }

        void rebuildState()
        {
            GsState s;
            std::memset(&s, 0, sizeof(s));
            const bool ac = regs[kGsPrmodecont] & 1u;
            uint64_t prim = regs[kGsPrim] & 0x7FFu;
            if (!ac)
                prim = (regs[kGsPrmode] & 0x7F8u) | (prim & 7u);
            static const uint16_t kClass[8] = {kPrimPoint, kPrimLine, kPrimLine, kPrimTriangle, kPrimTriangle, kPrimTriangle, kPrimSprite, 7};
            s.prim = static_cast<uint16_t>((prim & ~7ull) | kClass[prim & 7u]);
            const uint8_t c = static_cast<uint8_t>(s.ctxt());
            s.frame = regs[kGsFrame + c];
            s.zbuf = regs[kGsZbuf + c];
            s.scissor = regs[kGsScissor + c];
            s.xyoffset = regs[kGsXyoffset + c];
            s.test = regs[kGsTest + c];
            if (s.abe())
                s.alpha = regs[kGsAlpha + c];
            s.pabe = regs[kGsPabe] & 1u;
            s.fba = regs[kGsFba + c] & 1u;
            s.colclamp = regs[kGsColclamp] & 1u;
            s.dthe = regs[kGsDthe] & 1u;
            s.texture = 0xFFFFFFFFu;
            uint32_t texFormat = 0;
            if (s.tme())
            {
                s.texture = 0; // the survey decodes no texture; the plan only asks whether there is one
                s.tex0 = regs[kGsTex0 + c];
                s.tex1 = regs[kGsTex1 + c];
                s.clamp = regs[kGsClamp + c];
                if ((s.tex1 >> 2) & 7u)
                {
                    s.miptbp1 = regs[kGsMiptbp1 + c];
                    s.miptbp2 = regs[kGsMiptbp2 + c];
                }
                s.texa = regs[kGsTexa];
                const uint32_t psm = (s.tex0 >> 20) & 0x3Fu, tw = (s.tex0 >> 26) & 0xFu, th = (s.tex0 >> 30) & 0xFu;
                const uint32_t tcc = (s.tex0 >> 34) & 1u, tfx = (s.tex0 >> 35) & 3u;
                texFormat = psm | tw << 8 | th << 12 | tcc << 16 | tfx << 17 | 1u << 20;
                texKey = fmt("PSM 0x%02x %ux%u TCC %u TFX %u CPSM 0x%x CSM %u CSA %u", psm, 1u << tw, 1u << th, tcc, tfx,
                             static_cast<unsigned>((s.tex0 >> 51) & 0xFu), static_cast<unsigned>((s.tex0 >> 55) & 1u),
                             static_cast<unsigned>((s.tex0 >> 56) & 0x1Fu));
            }
            else
                texKey = "untextured";
            texCount = &texFormats[path][texKey];
            envKey = fmt("FRAME 0x%" PRIx64 " SCISSOR %u..%u x %u..%u XYOFFSET %u,%u (%u,%u /16) COLCLAMP %u DTHE %u PRMODECONT %u TEXA 0x%" PRIx64,
                         s.frame, static_cast<unsigned>(s.scissor & 0x7FFu), static_cast<unsigned>((s.scissor >> 16) & 0x7FFu),
                         static_cast<unsigned>((s.scissor >> 32) & 0x7FFu), static_cast<unsigned>((s.scissor >> 48) & 0x7FFu),
                         static_cast<unsigned>(s.xyoffset & 0xFFFFu) >> 4, static_cast<unsigned>((s.xyoffset >> 32) & 0xFFFFu) >> 4,
                         static_cast<unsigned>(s.xyoffset & 0xFu), static_cast<unsigned>((s.xyoffset >> 32) & 0xFu), s.colclamp, s.dthe, ac ? 1 : 0,
                         s.tme() ? s.texa : 0ull);
            envCount = &envs[path][envKey];
            const bool changed = std::memcmp(&s, &state, sizeof(s)) != 0 || statePath != path;
            state = s;
            statePath = path;
            plan = planState(s);
            StateKey key;
            std::memset(&key, 0, sizeof(key));
            key.alpha = s.alpha;
            key.test = s.test;
            key.tex1 = s.tex1;
            key.clamp = s.clamp;
            key.prim = static_cast<uint16_t>(prim);
            key.pabe = s.pabe;
            key.zmsk = (s.zbuf >> 32) & 1u;
            key.zpsm = (s.zbuf >> 24) & 0xFu;
            key.fba = s.fba;
            key.texFormat = texFormat;
            cur = &states[path][key];
            cur->plan = plan;
            if (changed)
                ++stateSerial;
            stateDirty = false;
        }

        bool tileHit(const Box &b) const
        {
            for (int ty = b.y0 / 8; ty <= b.y1 / 8; ++ty)
                for (int tx = b.x0 / 8; tx <= b.x1 / 8; ++tx)
                    if (tiles[ty][tx >> 6] >> (tx & 63) & 1u)
                        return true;
            return false;
        }
        void tileMark(const Box &b)
        {
            for (int ty = b.y0 / 8; ty <= b.y1 / 8; ++ty)
                for (int tx = b.x0 / 8; tx <= b.x1 / 8; ++tx)
                    tiles[ty][tx >> 6] |= 1ull << (tx & 63);
        }

        void primitive(const GsVertex *v, uint32_t n, uint32_t rawType)
        {
            if (stateDirty)
                rebuildState();
            Geo &g = geo[path];
            ++g.primType[rawType & 7u];
            ++cur->prims;
            ++*texCount;
            ++*envCount;
            ++kd.prims;
            const GsState &s = state;
            const uint32_t ofx = static_cast<uint32_t>(s.xyoffset & 0xFFFFu), ofy = static_cast<uint32_t>((s.xyoffset >> 32) & 0xFFFFu);
            const int sx0 = static_cast<int>(s.scissor & 0x7FFu), sx1 = static_cast<int>((s.scissor >> 16) & 0x7FFu);
            const int sy0 = static_cast<int>((s.scissor >> 32) & 0x7FFu), sy1 = static_cast<int>((s.scissor >> 48) & 0x7FFu);
            const Box sbox{std::max(sx0, 0), std::max(sy0, 0), std::min(sx1, kW - 1), std::min(sy1, kH - 1)};
            if (s.primType() != kPrimSprite)
                kd.nonSprite = true;
            Box box{};
            GsTri t;
            bool tri = false;
            if (n == 3)
            {
                ++g.tris;
                if (v[0].q != 1.0f || v[1].q != 1.0f || v[2].q != 1.0f)
                    ++g.qNotOne;
                if (v[0].q != v[1].q || v[0].q != v[2].q)
                    ++g.qDiffer;
                if (v[0].z != v[1].z || v[0].z != v[2].z)
                    ++g.zDiffer;
                if (v[0].a != v[1].a || v[0].a != v[2].a)
                    ++g.aDiffer;
                const bool rgb = std::memcmp(&v[0].r, &v[1].r, 3) || std::memcmp(&v[0].r, &v[2].r, 3);
                if (rgb && s.iip())
                    ++g.rgbDiffer;
                if (rgb && !s.iip())
                    ++g.flatIip;
                if (!s.iip())
                    ++g.iip0;
                if (!s.tme())
                    ++g.tme0;
                if ((v[0].x | v[0].y | v[1].x | v[1].y | v[2].x | v[2].y) & 0xFu)
                    ++g.subpixel;
                if (s.fst())
                    ++g.fst;
                int far = 0;
                for (int i = 0; i < 3; ++i)
                {
                    g.zMin = std::min(g.zMin, v[i].z);
                    g.zMax = std::max(g.zMax, v[i].z);
                    if (v[i].z >= 0x1000000u)
                        ++g.zAbove24;
                    const int px = (static_cast<int>(v[i].x) - static_cast<int>(ofx)) >> 4, py = (static_cast<int>(v[i].y) - static_cast<int>(ofy)) >> 4;
                    const int dx = px < 0 ? -px : px >= kW ? px - kW + 1 : 0, dy = py < 0 ? -py : py >= static_cast<int>(header.height) ? py - static_cast<int>(header.height) + 1 : 0;
                    far = std::max(far, std::max(dx, dy));
                }
                ++g.outside[far == 0 ? 0 : far <= 64 ? 1 : far <= 512 ? 2 : far <= 1024 ? 3 : far <= 2048 ? 4 : 5];
                if (s.tme() && !s.fst())
                {
                    float m = 0.0f;
                    for (int i = 0; i < 3; ++i)
                    {
                        const float q = v[i].q != 0.0f ? v[i].q : 1.0f;
                        const float a = std::max(std::fabs(v[i].s / q), std::fabs(v[i].t / q));
                        const float lo = std::min(v[i].s / q, v[i].t / q);
                        m = std::max(m, lo < 0.0f ? std::max(a, 1.0001f) : a);
                    }
                    ++g.stRange[!(m > 1.0f) ? 0 : m <= 2.0f ? 1 : m <= 8.0f ? 2 : m <= 64.0f ? 3 : m <= 1024.0f ? 4 : 5];
                }
                if (!gsTriSetup(v, ofx, ofy, t))
                {
                    ++g.zeroArea;
                    ++kd.culled;
                    return;
                }
                // Winding before gsTriSetup normalised it.
                const int64_t ax = static_cast<int64_t>(v[1].x) - v[0].x, ay = static_cast<int64_t>(v[1].y) - v[0].y;
                const int64_t bx = static_cast<int64_t>(v[2].x) - v[0].x, by = static_cast<int64_t>(v[2].y) - v[0].y;
                if (ax * by - ay * bx < 0)
                    ++g.backFacing;
                if (t.minX < sbox.x0 || t.maxX > sbox.x1 || t.minY < sbox.y0 || t.maxY > sbox.y1)
                    ++g.clippedByScissor;
                box = {std::max(t.minX, sbox.x0), std::max(t.minY, sbox.y0), std::min(t.maxX, sbox.x1), std::min(t.maxY, sbox.y1)};
                tri = true;
            }
            else
            {
                if (n == 1)
                    ++g.points;
                else if (s.primType() == kPrimSprite)
                    ++g.sprites;
                else
                    ++g.lines;
                int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
                for (uint32_t i = 0; i < n; ++i)
                {
                    const int px = (static_cast<int>(v[i].x) - static_cast<int>(ofx)) >> 4, py = (static_cast<int>(v[i].y) - static_cast<int>(ofy)) >> 4;
                    x0 = std::min(x0, px);
                    x1 = std::max(x1, px);
                    y0 = std::min(y0, py);
                    y1 = std::max(y1, py);
                }
                if (s.primType() == kPrimSprite)
                {
                    --x1;
                    --y1;
                }
                box = {std::max(x0, sbox.x0), std::max(y0, sbox.y0), std::min(x1, sbox.x1), std::min(y1, sbox.y1)};
            }
            if (box.x1 < box.x0 || box.y1 < box.y0)
            {
                ++g.boxEmpty;
                ++kd.culled;
                return;
            }
            const uint64_t area = static_cast<uint64_t>(box.x1 - box.x0 + 1) * static_cast<uint64_t>(box.y1 - box.y0 + 1);
            cur->boxPixels += area;
            ++g.boxArea[area <= 1 ? 0 : area <= 16 ? 1 : area <= 256 ? 2 : area <= 4096 ? 3 : area <= 16384 ? 4 : area <= 65536 ? 5 : 6];

            const bool reads = plan.blend != kBlendOff && !plan.exactBlend && !plan.unsupported;
            const bool newBatch = batchSerial != stateSerial;
            if (newBatch)
            {
                GsState a = state, b = batchState;
                a.tex0 = b.tex0 = 0;
                a.miptbp1 = b.miptbp1 = a.miptbp2 = b.miptbp2 = 0;
                if (kd.batches && std::memcmp(&a, &b, sizeof(a)) == 0)
                    ++kd.batchesTexOnly;
                batchState = state;
                batchSerial = stateSerial;
                batchIndex = 0;
                batchReads = reads;
                batchSplit = plan.split != 0;
                ++kd.batches;
                ++kd.drawsP;
                group.clear();
                if (!reads && !plan.split)
                {
                    ++kd.draws0;
                    anyDrawn0 = true;
                }
                if (reads)
                    ++kd.readBatches;
                else
                    runOpen = false;
            }
            const bool pair = tri && (batchIndex & 1u) && prevValid && !newBatch && rectangleHalves(prevTri, v);
            // (L0) as built: groups of pairwise box-disjoint primitives inside a batch of a reading or split plan.
            if (batchReads || batchSplit)
            {
                bool conflict = group.empty();
                for (size_t i = 0; i < group.size() && !conflict; ++i)
                    if (!box.disjoint(group[i]) && !(i + 1 == group.size() && pair))
                        conflict = true;
                if (conflict)
                {
                    group.clear();
                    kd.draws0 += batchSplit ? 2u : 1u;
                    if (batchReads)
                    {
                        ++kd.groups0;
                        if (anyDrawn0)
                            ++kd.barriers0;
                    }
                    anyDrawn0 = true;
                }
                group.push_back(box);
            }
            if (batchSplit)
                ++kd.splitPrims;
            // (run rule) a group continues across consecutive reading batches while the boxes stay pairwise disjoint;
            // a batch that does not read ends it.
            if (reads)
            {
                bool conflict = !runOpen;
                for (size_t i = 0; i < runBoxes.size() && !conflict; ++i)
                    if (!box.disjoint(runBoxes[i]) && !(i + 1 == runBoxes.size() && pair))
                        conflict = true;
                if (conflict)
                {
                    runBoxes.clear();
                    ++kd.barriersRun;
                    runOpen = true;
                }
                runBoxes.push_back(box);
            }
            // (tiles) a barrier only when the primitive's box touches an 8x8 tile drawn to since the last barrier.
            if (!pair && prevValid)
                tileMark(prevBox);
            if (reads && tileHit(box))
            {
                ++kd.barriersT;
                std::memset(tiles, 0, sizeof(tiles));
            }
            else if (pair)
                tileMark(prevBox);
            // (tiles by coverage) the same with the tiles the triangle can cover, 8x8 and 4x4.
            for (int l = 0; l < 2; ++l)
            {
                TileLever &tl = lever[l];
                const GsTri *tp = tri ? &t : nullptr;
                if (reads)
                {
                    const std::vector<uint64_t> &seen = pair ? tl.before : tl.bits;
                    if (tl.visit(box, tp, kd.tileTests[l], [&](size_t i, uint64_t bit) { return (seen[i] & bit) != 0; }))
                    {
                        ++kd.barriersC[l];
                        tl.clear();
                    }
                }
                tl.before = tl.bits;
                tl.visit(box, tp, kd.tileTests[l], [&](size_t i, uint64_t bit)
                         {
                             tl.bits[i] |= bit;
                             return false;
                         });
            }
            // (boxes) the same with the exact boxes.
            if (reads)
            {
                bool hit = false;
                for (size_t i = 0; i < dirtyBoxes.size() && !hit; ++i)
                    if (!box.disjoint(dirtyBoxes[i]) && !(i + 1 == dirtyBoxes.size() && pair))
                        hit = true;
                if (hit)
                {
                    ++kd.barriersX;
                    dirtyBoxes.clear();
                }
            }
            dirtyBoxes.push_back(box);
            // (pixels) the same with the reference's coverage: the fewest barriers any in-order rule can issue.
            uint64_t covered = 0;
            if (tri)
            {
                bool hit = false;
                for (int pass = reads ? 0 : 1; pass < 2; ++pass)
                {
                    for (int py = box.y0; py <= box.y1; ++py)
                    {
                        uint32_t *row = pix.data() + static_cast<size_t>(py) * kW;
                        int64_t e[3] = {gsTriEdge(t, 0, box.x0, py), gsTriEdge(t, 1, box.x0, py), gsTriEdge(t, 2, box.x0, py)};
                        for (int px = box.x0; px <= box.x1; ++px)
                        {
                            if (gsTriCovers(t, e))
                            {
                                if (pass == 0)
                                    hit |= row[px] == pixGen;
                                else
                                {
                                    row[px] = pixGen;
                                    ++covered;
                                }
                            }
                            e[0] += t.a[0] * 16;
                            e[1] += t.a[1] * 16;
                            e[2] += t.a[2] * 16;
                        }
                        if (pass == 0 && hit)
                            break;
                    }
                    if (pass == 0 && hit)
                    {
                        ++kd.barriersP;
                        ++pixGen;
                        if (!newBatch)
                            ++kd.drawsP;
                    }
                }
            }
            else
            {
                bool hit = false;
                if (reads)
                    for (int py = box.y0; py <= box.y1 && !hit; ++py)
                        for (int px = box.x0; px <= box.x1 && !hit; ++px)
                            hit = pix[static_cast<size_t>(py) * kW + px] == pixGen;
                if (hit)
                {
                    ++kd.barriersP;
                    ++pixGen;
                    if (!newBatch)
                        ++kd.drawsP;
                }
                for (int py = box.y0; py <= box.y1; ++py)
                    for (int px = box.x0; px <= box.x1; ++px)
                        pix[static_cast<size_t>(py) * kW + px] = pixGen;
                covered = area;
            }
            cur->pixels += covered;
            kd.pixels += covered;
            if (reads || kd.readPrims)
            {
                kd.trackedPixels += covered;
                ++kd.trackedPrims;
            }
            if (reads)
            {
                ++kd.readPrims;
                kd.readPixels += covered;
            }
            prevBox = box;
            prevValid = true;
            if (tri)
                std::memcpy(prevTri, v, sizeof(prevTri));
            ++batchIndex;
        }

        void vertex(uint8_t addr, uint64_t value)
        {
            GsVertex v{};
            v.x = static_cast<uint16_t>(value & 0xFFFFu);
            v.y = static_cast<uint16_t>((value >> 16) & 0xFFFFu);
            const bool f = addr == kGsXyzf2 || addr == kGsXyzf3;
            v.z = f ? static_cast<uint32_t>((value >> 32) & 0xFFFFFFu) : static_cast<uint32_t>(value >> 32);
            v.r = rgbaq & 0xFFu;
            v.g = (rgbaq >> 8) & 0xFFu;
            v.b = (rgbaq >> 16) & 0xFFu;
            v.a = (rgbaq >> 24) & 0xFFu;
            const uint32_t qb = static_cast<uint32_t>(rgbaq >> 32), sb = static_cast<uint32_t>(st), tb = static_cast<uint32_t>(st >> 32);
            std::memcpy(&v.q, &qb, 4);
            std::memcpy(&v.s, &sb, 4);
            std::memcpy(&v.t, &tb, 4);
            v.u = uv & 0x3FFFu;
            v.v = (uv >> 16) & 0x3FFFu;
            if (f)
                add(fmt("path%d", path), "XYZF vertex", 1, 0);
            const bool draw = addr == kGsXyz2 || addr == kGsXyzf2;
            const uint32_t type = regs[kGsPrim] & 7u;
            const uint32_t need = type == 0 ? 1u : (type == 1 || type == 2 || type == 6) ? 2u : 3u;
            if (queued < need)
                queue[queued++] = v;
            else
            {
                if (type == 5) // fan: the first vertex stays
                    queue[1] = queue[2], queue[2] = v;
                else
                {
                    for (uint32_t i = 0; i + 1 < need; ++i)
                        queue[i] = queue[i + 1];
                    queue[need - 1] = v;
                }
            }
            if (queued < need)
                return;
            if (draw)
            {
                GsVertex out[3] = {queue[0], queue[1], queue[2]};
                primitive(out, need, type);
            }
            else
                add(fmt("path%d", path), "primitive completed by a vertex without the drawing kick", 1, 0);
            // Lists start again; strips and fans keep sliding.
            if (type == 0 || type == 1 || type == 3 || type == 6)
                queued = 0;
        }

        void reg(uint8_t addr, uint64_t value) override
        {
            if (addr >= 0x80)
                return;
            ++regWrites[path][addr];
            switch (addr)
            {
            case kGsRgbaq:
                rgbaq = value;
                regs[addr] = value;
                return;
            case kGsSt:
                st = value;
                regs[addr] = value;
                return;
            case kGsUv:
                uv = value;
                regs[addr] = value;
                return;
            case kGsXyz2:
            case kGsXyzf2:
            case kGsXyz3:
            case kGsXyzf3:
                regs[addr] = value;
                vertex(addr, value);
                return;
            default:
                break;
            }
            if (addr == kGsPrim)
                queued = 0;
            auto &vals = regValues[path][addr];
            if (vals.size() < 48 || vals.count(value))
                ++vals[value];
            else
                ++vals[~0ull];
            regs[addr] = value;
            stateDirty = true;
        }
        void image(const uint8_t *, uint32_t) override {}

        void packet(int p, const uint8_t *data, uint32_t bytes)
        {
            if (path != (p & 3))
                stateDirty = true;
            path = p & 3;
            // The MXCSR in force while the machine hands this packet to its GS (the software GS rasterises in floats
            // on the calling thread, or carries the caller's MXCSR to its own).
            const uint32_t csr = _mm_getcsr();
            if (csr != lastCsr[path] || !csrCount[path])
            {
                lastCsr[path] = csr;
                csrCount[path] = &sums[{fmt("path%d mxcsr at the packet", path), fmt("0x%04x (rounding %s)", csr, (csr & 0x6000u) == 0 ? "to nearest" : (csr & 0x6000u) == 0x6000u ? "toward zero" : "directed")}];
            }
            ++(*csrCount[path])[0];
            ++kickPackets[path];
            kickGifBytes[path] += bytes;
            if (path == 1 && bytes >= 16u)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, data, 8);
                std::memcpy(&hi, data + 8, 8);
                if (!lastTagCount || lo != lastTagLo || hi != lastTagHi || bytes != lastTagBytes)
                {
                    const uint32_t nreg = (lo >> 60) & 0xFu;
                    lastTagCount = &gifTags[1][fmt("first tag NLOOP %u EOP %u PRE %u PRIM 0x%03x FLG %u NREG %u REGS 0x%" PRIx64 ", packet %u bytes",
                                                   static_cast<unsigned>(lo & 0x7FFFu), static_cast<unsigned>((lo >> 15) & 1u),
                                                   static_cast<unsigned>((lo >> 46) & 1u), static_cast<unsigned>((lo >> 47) & 0x7FFu),
                                                   static_cast<unsigned>((lo >> 58) & 3u), nreg,
                                                   nreg ? hi & ((1ull << (nreg * 4u)) - 1u) : hi, bytes)];
                    lastTagLo = lo;
                    lastTagHi = hi;
                    lastTagBytes = bytes;
                }
                ++(*lastTagCount)[0];
                (*lastTagCount)[1] += bytes;
            }
            walker[path].feed(data, bytes, *this);
            if (path == 1 && walker[1].tagOpen())
                add("path1", "packet ends inside a GIF tag", 1, 0);
            if (path == 1)
                comparePacket(data, bytes);
        }

        // ---------------------------------------------------------------------------------------------------------
        void comparePacket(const uint8_t *data, uint32_t bytes)
        {
            if (!runActive || runProgram < 0)
            {
                ++path1Outside;
                return;
            }
            Run &r = run[runProgram];
            if (nextPacket >= expected.size())
            {
                ++path1Outside;
                return;
            }
            const std::vector<uint8_t> &e = expected[nextPacket++];
            ++r.packets;
            if (e.size() != bytes || std::memcmp(e.data(), data, bytes) != 0)
            {
                if (r.packetMismatch++ < 4)
                {
                    uint32_t at = 0;
                    while (at < bytes && at < e.size() && e[at] == data[at])
                        ++at;
                    std::printf("vifstat: %s packet %zu differs at byte %u (sizes %u emulated, %zu reference)\n", kPrograms[runProgram].name, nextPacket - 1,
                                at, bytes, e.size());
                }
            }
        }

#if RC_HAVE_VU1_CODE
        using RunFn = void (*)(uint8_t *, const float[32][4], uint32_t, uint32_t, std::vector<std::vector<uint8_t>> &);

        void runBegin(int prog, const VU1State &st, const uint8_t *vuData, RunFn fn)
        {
            Run &r = run[prog];
            const Program &p = kPrograms[prog];
            const bool entry = st.pc == 0u || st.pc == p.entry1 || st.pc == p.entry2;
            if (!entry)
            {
                ++r.continued;
                add("vu1 continued", fmt("%s re-entered at pc 0x%04x", p.name, st.pc), 1, 0);
                return;
            }
            if (runActive && nextPacket != expected.size())
                ++run[runProgram].countMismatch;
            ++r.runs;
            ++r.entries[st.pc == 0u ? 0 : st.pc == p.entry1 ? 1 : 2];
            ++kickMscals;
            add("vu1 run", fmt("%s pc 0x%04x", p.name, st.pc), 1, 0);
            if (haveAfter && std::memcmp(vfAfterLast, st.vf, sizeof(vfAfterLast)) != 0)
                ++r.regsMoved;
            std::memcpy(vfBefore, st.vf, sizeof(vfBefore));
            std::memcpy(viBefore, st.vi, sizeof(viBefore));
            std::memcpy(accBefore, st.acc, sizeof(accBefore));
            qBefore = st.q;
            pBefore = st.p;
            iBefore = st.i;
            std::memcpy(refMem.data(), vuData, 0x4000u);
            expected.clear();
            nextPacket = 0;
            const auto t0 = std::chrono::steady_clock::now();
            fn(refMem.data(), st.vf, st.pc, st.top, expected);
            const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            r.ns += ns;
            kickRefNs += ns;
            runActive = true;
            runProgram = prog;

            // The same run with every register but VF0 marked: which lanes of an earlier run's registers reach the GS.
            float marked[32][4];
            std::memcpy(marked, st.vf, sizeof(marked));
            for (int reg = 1; reg < 32; ++reg)
                for (int lane = 0; lane < 4; ++lane)
                {
                    const uint32_t b = poison(reg, lane);
                    std::memcpy(&marked[reg][lane], &b, 4);
                }
            std::memcpy(poisonMem.data(), vuData, 0x4000u);
            poisoned.clear();
            fn(poisonMem.data(), marked, st.pc, st.top, poisoned);
            bool leak = false, computed = false;
            if (poisoned.size() != expected.size())
                ++r.countDiffRuns;
            for (size_t i = 0; i < std::min(poisoned.size(), expected.size()); ++i)
            {
                const std::vector<uint8_t> &a = expected[i], &b = poisoned[i];
                if (a.size() != b.size())
                {
                    computed = true;
                    continue;
                }
                for (size_t at = 0; at + 4u <= a.size(); at += 4u)
                {
                    uint32_t x, y;
                    std::memcpy(&x, a.data() + at, 4);
                    std::memcpy(&y, b.data() + at, 4);
                    if (x == y)
                        continue;
                    const uint32_t index = (y >> 8) & 0xFFu;
                    if ((y & kPoisonMask) == kPoisonBits && index >= 4 && index < 128)
                    {
                        leak = true;
                        ++r.leak[index];
                        ++r.leakFrom[index][lastWriter[index]];
                        uint32_t actual;
                        std::memcpy(&actual, &st.vf[index / 4][index & 3], 4);
                        auto &lv = r.leakValues[index];
                        if (lv.size() < 12 || lv.count(actual))
                            ++lv[actual];
                        else
                            ++lv[0xFFFFFFFFu];
                    }
                    else
                        computed = true;
                }
            }
            if (std::memcmp(poisonMem.data(), refMem.data(), 0x4000u) != 0)
                add("vu1 marked", fmt("%s: final VU memory depends on the incoming registers", p.name), 1, 0);
            if (leak)
                ++r.leakRuns;
            if (computed)
                ++r.computedRuns;
        }

        void runEnd(int prog, VU1InterpreterAccess::StepResult result, const VU1State &st, const uint8_t *vuData)
        {
            Run &r = run[prog];
            if (result != VU1InterpreterAccess::StepResult::Ended && result != VU1InterpreterAccess::StepResult::Budget)
                return;
            // What the program changed in the registers (a proxy for what it wrote).
            for (int reg = 0; reg < 32; ++reg)
                for (int lane = 0; lane < 4; ++lane)
                    if (std::memcmp(&st.vf[reg][lane], &vfBefore[reg][lane], 4) != 0)
                    {
                        r.vfChanged[reg] |= 1u << lane;
                        lastWriter[reg * 4 + lane] = static_cast<uint8_t>(prog + 1);
                    }
            for (int i = 0; i < 16; ++i)
                if (st.vi[i] != viBefore[i])
                    r.viChanged |= 1u << i;
            for (int i = 0; i < 4; ++i)
                if (std::memcmp(&st.acc[i], &accBefore[i], 4) != 0)
                    r.accChanged |= 1u << i;
            r.qChanged |= std::memcmp(&st.q, &qBefore, 4) != 0;
            r.pChanged |= std::memcmp(&st.p, &pBefore, 4) != 0;
            r.iChanged |= std::memcmp(&st.i, &iBefore, 4) != 0;
            std::memcpy(vfAfterLast, st.vf, sizeof(vfAfterLast));
            haveAfter = true;
            if (!runActive || runProgram != prog)
                return;
            if (result == VU1InterpreterAccess::StepResult::Budget)
            {
                ++r.cutShort;
                runActive = false;
                return;
            }
            if (nextPacket != expected.size())
                ++r.countMismatch;
            if (std::memcmp(vuData, refMem.data(), 0x4000u) != 0)
            {
                if (r.memoryMismatch++ < 4)
                {
                    uint32_t at = 0;
                    while (at < 0x4000u && vuData[at] == refMem[at])
                        ++at;
                    std::printf("vifstat: %s final VU memory differs at 0x%04x\n", kPrograms[prog].name, at);
                }
            }
            runActive = false;
        }
#endif
    };

#if RC_HAVE_VU1_CODE
    namespace
    {
        VifSurvey::Impl *g_survey = nullptr;
    }
    struct VifSurveyAccess
    {
        template <uint64_t Hash, int Prog>
        static VU1InterpreterAccess::StepResult wrapped(VU1Interpreter &vu, const Vu1RunArgs &args)
        {
            static constexpr VifSurvey::Impl::RunFn kFns[6] = {&rc::vu1::runTrdrShape, &rc::vu1::runEnvShape, nullptr, &rc::vu1::runSkinShape,
                                                               &rc::vu1::runTerdrShape, &rc::vu1::runRdrShape};
            VifSurvey::Impl *s = g_survey;
            if (s && s->roundNearest)
            {
                s->sinkMemory = args.memory;
                s->flushSink();
            }
            if (s)
            {
                const VU1State st = VU1InterpreterAccess::effectiveState(vu);
                VifSurvey::Impl::RunFn fn = kFns[Prog];
                if (Prog == 2)
                    fn = [](uint8_t *memory, const float registers[32][4], uint32_t pc, uint32_t top, std::vector<std::vector<uint8_t>> &kicks)
                    { rc::vu1::runWaterShape(memory, registers, pc, top, kicks); };
                s->runBegin(Prog, st, args.vuData, fn);
            }
            const auto result = VU1InterpreterAccess::runImage<Hash>(vu, args);
            if (s)
                s->runEnd(Prog, result, VU1InterpreterAccess::effectiveState(vu), args.vuData);
            return result;
        }
    };
#endif

    VifSurvey::VifSurvey(PS2Runtime *runtime) : m(std::make_unique<Impl>()) { m->rt = runtime; }
    VifSurvey::~VifSurvey()
    {
#if RC_HAVE_VU1_CODE
        if (g_survey == m.get())
            g_survey = nullptr;
#endif
    }

    void VifSurvey::setRoundNearest(bool on) { m->roundNearest = on; }

    void VifSurvey::installVu1Wrappers()
    {
#if RC_HAVE_VU1_CODE
        g_survey = m.get();
        registerVu1CompiledProgram(0xC3952C0FAEB52EADull, &VifSurveyAccess::wrapped<0xC3952C0FAEB52EADull, 0>);
        registerVu1CompiledProgram(0xAB0E7743A9BA7393ull, &VifSurveyAccess::wrapped<0xAB0E7743A9BA7393ull, 1>);
        registerVu1CompiledProgram(0xE96770E4143D2DA7ull, &VifSurveyAccess::wrapped<0xE96770E4143D2DA7ull, 2>);
        registerVu1CompiledProgram(0xEC08B22243D4DC8Cull, &VifSurveyAccess::wrapped<0xEC08B22243D4DC8Cull, 3>);
        registerVu1CompiledProgram(0xBAC581270F262987ull, &VifSurveyAccess::wrapped<0xBAC581270F262987ull, 4>);
        registerVu1CompiledProgram(0x6EC8B9FF8FA44221ull, &VifSurveyAccess::wrapped<0x6EC8B9FF8FA44221ull, 5>);
#else
        std::printf("vifstat: built without the generated VU1 programs: no reference comparison\n");
#endif
    }

    void VifSurvey::open(const FileInfo &info) { m->header = info.header; }

    void VifSurvey::keyframe(const Keyframe &kf)
    {
        if (kf.header.flags & kKeyFinal)
            return;
        if (const StateChunk *c = kf.find(kStateVif1); c && c->bytes >= sizeof(Vif1Regs))
        {
            Vif1Regs r;
            std::memcpy(&r, c->data, sizeof(r));
            if (m->vifKnown && !(kf.header.flags & kKeyAfterStateLoad))
            {
                const Impl::Vif &v = m->vif;
                const bool same = v.cycle == r.cycle && v.mode == r.mode && v.mask == r.mask && v.base == r.base && v.ofst == r.ofst && v.tops == r.tops &&
                                  v.itops == r.itops && v.dbf == static_cast<bool>((r.stat >> 7) & 1u);
                m->add("vif model", same ? "equals the next keyframe's VIF1 chunk" : "differs from the next keyframe's VIF1 chunk", 1, 0);
                if (!same)
                    std::printf("vifstat: VIF1 model differs at keyframe (cycle %" PRIu64 "): CYCLE %x/%x MODE %x/%x MASK %x/%x BASE %x/%x OFST %x/%x TOPS %x/%x ITOPS %x/%x DBF %u/%u\n",
                                kf.header.cycle, v.cycle, r.cycle, v.mode, r.mode, v.mask, r.mask, v.base, r.base, v.ofst, r.ofst, v.tops, r.tops, v.itops,
                                r.itops, v.dbf ? 1 : 0, static_cast<unsigned>((r.stat >> 7) & 1u));
            }
            m->loadVif(r);
            m->vifKnown = true;
        }
        else
            m->add("keyframe", "without a VIF1 chunk", 1, 0);
        if (const StateChunk *c = kf.find(kStateVu1Memory); c && c->bytes >= 0x8000u)
        {
            if (m->microKnown && !(kf.header.flags & kKeyAfterStateLoad))
                m->add("vif model", std::memcmp(m->micro.data(), c->data, 0x4000u) == 0 ? "micro memory equals the next keyframe's VU1M" : "micro memory differs from the next keyframe's VU1M", 1, 0);
            std::memcpy(m->micro.data(), c->data, 0x4000u);
            m->microKnown = true;
            m->microDirty = true;
        }
        else
            m->add("keyframe", "without a VU1M chunk", 1, 0);
        m->add("keyframe", kf.find(kStateVu1Regs) ? "with a VU1R chunk" : "without a VU1R chunk", 1, 0);
        m->haveRegs = false;
        m->haveAfter = false;
    }

    void VifSurvey::cycleBegin(uint64_t)
    {
        m->cycBarriers0 = m->cycBarriersP = m->cycBarriersT = m->cycDraws0 = m->cycPrims = m->cycReadPrims = m->cycRefNs = 0;
        m->cycAny = false;
    }

    void VifSurvey::hw(uint32_t address, uint32_t, uint64_t value, uint32_t)
    {
        if (address >= 0x10003C00u && address < 0x10003D00u)
        {
            m->add("vif1 register store", fmt("0x%08x = 0x%" PRIx64, address, value), 1, 0);
            // FBRST.RST resets the VIF: the model follows the runtime only as far as the registers the walk uses.
            if (address == 0x10003C10u && (value & 1u))
                m->add("vif1 register store", "FBRST with RST", 1, 0);
        }
    }

    void VifSurvey::kick(const KickView &k, const RamView &ram)
    {
        // The register file the emulated GS had before this kick: check the walk of the previous kick's packets, then
        // start from it.
        if (k.regs)
        {
            if (m->haveRegs && m->regsAfterKick)
            {
                static const uint8_t kSkip[] = {kGsRgbaq, kGsSt, kGsUv, kGsXyzf2, kGsXyz2, kGsXyzf3, kGsXyz3, kGsFog};
                bool same = true;
                for (uint32_t a = 0; a < kGsRegCount; ++a)
                {
                    if (std::find(std::begin(kSkip), std::end(kSkip), a) != std::end(kSkip))
                        continue;
                    if (m->regs[a] != k.regs[a])
                    {
                        same = false;
                        m->add("register file", fmt("register 0x%02x differs after a kick's packets", a), 1, 0);
                    }
                }
                m->add("register file", same ? "equal to the next kick's recorded file" : "differs from the next kick's recorded file", 1, 0);
            }
            std::memcpy(m->regs, k.regs, kGsRegCount * 8u);
            m->haveRegs = true;
            m->regsAfterKick = false;
        }
        m->stateDirty = true;
        m->rgbaq = m->regs[kGsRgbaq];
        m->st = m->regs[kGsSt];
        m->uv = m->regs[kGsUv];
        m->beginKick();
#if RC_HAVE_VU1_CODE
        if (m->roundNearest && k.channel == 1u)
        {
            m->sink.forward = false;
            vu1_impl::packetSink = &m->sink;
        }
#endif
        m->add("kick mxcsr", fmt("channel %u: 0x%04x", k.channel, k.mxcsr), 1, 0);
        if (k.channel == 1u)
            m->vifKick(k, ram);
    }

    void VifSurvey::gifPacket(int path, const uint8_t *data, uint32_t bytes) { m->packet(path, data, bytes); }

    void VifSurvey::afterKick(const KickView &k)
    {
        Impl &s = *m;
#if RC_HAVE_VU1_CODE
        if (s.roundNearest && k.channel == 1u)
        {
            s.flushSink();
            vu1_impl::packetSink = nullptr;
        }
#endif
        s.regsAfterKick = true;
        const KickDraw &d = s.kd;
        const bool k2 = k.channel == 1u, k1 = k.channel == 2u && d.prims && d.nonSprite;
        if (k2)
        {
            s.dist("k2 path1 packets", s.kickPackets[1]);
            s.dist("k2 path1 KB", s.kickGifBytes[1] / 1024u);
            s.dist("k2 vu1 runs", s.kickMscals);
            s.dist("k2 reference us", s.kickRefNs / 1000u);
            s.cycRefNs += s.kickRefNs;
            if (s.kickPackets[2] || s.kickPackets[3])
                s.add("path1", "a VIF1 kick produced PATH2 or PATH3 packets", 1, 0);
        }
        if (k2 || k1)
        {
            const char *n = k2 ? "k2" : "k1";
            s.dist(fmt("%s prims", n), d.prims);
            s.dist(fmt("%s prims culled (zero area or outside the scissor)", n), d.culled);
            s.dist(fmt("%s prims drawn", n), d.prims - d.culled);
            s.dist(fmt("%s batches (state runs)", n), d.batches);
            s.dist(fmt("%s batches that change only the texture", n), d.batchesTexOnly);
            s.dist(fmt("%s reading prims", n), d.readPrims);
            s.dist(fmt("%s reading batches", n), d.readBatches);
            s.dist(fmt("%s split prims", n), d.splitPrims);
            s.dist(fmt("%s reading groups, as built", n), d.groups0);
            s.dist(fmt("%s barriers, as built", n), d.barriers0);
            s.dist(fmt("%s draws, as built", n), d.draws0);
            s.dist(fmt("%s barriers, groups across reading batches", n), d.barriersRun);
            s.dist(fmt("%s barriers, dirty 8x8 tiles", n), d.barriersT);
            s.dist(fmt("%s barriers, dirty boxes", n), d.barriersX);
            s.dist(fmt("%s barriers, dirty 8x8 tiles by coverage", n), d.barriersC[0]);
            s.dist(fmt("%s barriers, dirty 4x4 tiles by coverage", n), d.barriersC[1]);
            s.dist(fmt("%s tile tests, 8x8", n), d.tileTests[0]);
            s.dist(fmt("%s tile tests, 4x4", n), d.tileTests[1]);
            s.dist(fmt("%s barriers, dirty pixels", n), d.barriersP);
            s.dist(fmt("%s draws, dirty pixels", n), d.drawsP);
            s.dist(fmt("%s covered Kpixels", n), d.pixels / 1000u);
            s.dist(fmt("%s reading Kpixels", n), d.readPixels / 1000u);
            s.dist(fmt("%s Kpixels from the first reading prim on", n), d.trackedPixels / 1000u);
            s.dist(fmt("%s prims from the first reading prim on", n), d.trackedPrims);
            s.cycBarriers0 += d.barriers0;
            s.cycBarriersP += d.barriersP;
            s.cycBarriersT += d.barriersC[0];
            s.cycDraws0 += d.draws0;
            s.cycPrims += d.prims;
            s.cycReadPrims += d.readPrims;
            s.cycAny = true;
        }
    }

    void VifSurvey::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        Impl &s = *m;
        // RC_VIFSTAT_PICTURES=<file>: a plain run writes every cycle's picture to the file; a --vif-round-nearest run
        // reads them back as the reference (the dumps carry a picture only every so many cycles, a hash otherwise).
        static std::FILE *pictures = nullptr;
        static std::vector<uint8_t> stored;
        if (s.rt && !pictures)
            if (const char *path = std::getenv("RC_VIFSTAT_PICTURES"))
                pictures = std::fopen(path, s.roundNearest ? "rb" : "wb");
        if (s.rt && pictures)
        {
            const uint32_t w = info.end.refWidth ? info.end.refWidth : s.header.width, h = info.end.refHeight ? info.end.refHeight : s.header.height;
            s.picture.resize(static_cast<size_t>(w) * h * 4u);
            readPicture(s.rt->memory().getGSVRAM(), kGsMemBytes, static_cast<uint32_t>(s.rt->memory().gs().dispfb2 & 0x1FFu), 10u, w, h, s.picture.data());
            if (!s.roundNearest)
            {
                std::fwrite(s.picture.data(), 1, s.picture.size(), pictures);
                refRgba = nullptr;
            }
            else
            {
                stored.resize(s.picture.size());
                refRgba = std::fread(stored.data(), 1, stored.size(), pictures) == stored.size() ? stored.data() : nullptr;
            }
        }
        if (s.rt && refRgba)
        {
            // The machine's picture against the reference, per channel.
            const uint32_t w = info.end.refWidth ? info.end.refWidth : s.header.width, h = info.end.refHeight ? info.end.refHeight : s.header.height;
            s.picture.resize(static_cast<size_t>(w) * h * 4u);
            readPicture(s.rt->memory().getGSVRAM(), kGsMemBytes, static_cast<uint32_t>(s.rt->memory().gs().dispfb2 & 0x1FFu), 10u, w, h, s.picture.data());
            ++s.picCycles;
            uint64_t differ = 0;
            for (size_t i = 0; i < s.picture.size(); i += 4)
            {
                bool d = false;
                for (int c = 0; c < 4; ++c)
                {
                    const int delta = static_cast<int>(s.picture[i + c]) - static_cast<int>(refRgba[i + c]);
                    ++s.picDiff[c][std::clamp(delta, -3, 3) + 3];
                    d |= delta != 0;
                }
                differ += d;
            }
            s.picPixels += s.picture.size() / 4u;
            s.picPixelsDiffer += differ;
            s.picCyclesDiffer += differ != 0;
        }
        if (!s.cycAny)
            return;
        s.dist("cycle barriers, as built", s.cycBarriers0);
        s.dist("cycle barriers, dirty 8x8 tiles by coverage", s.cycBarriersT);
        s.dist("cycle barriers, dirty pixels", s.cycBarriersP);
        s.dist("cycle draws, as built", s.cycDraws0);
        s.dist("cycle prims", s.cycPrims);
        s.dist("cycle reference us", s.cycRefNs / 1000u);
    }

    void VifSurvey::summary() const
    {
        Impl &s = *m;
        auto opName = [](uint32_t op) -> std::string
        {
            switch (op)
            {
            case 0x00: return "NOP";
            case 0x01: return "STCYCL";
            case 0x02: return "OFFSET";
            case 0x03: return "BASE";
            case 0x04: return "ITOP";
            case 0x05: return "STMOD";
            case 0x06: return "MSKPATH3";
            case 0x07: return "MARK";
            case 0x10: return "FLUSHE";
            case 0x11: return "FLUSH";
            case 0x13: return "FLUSHA";
            case 0x14: return "MSCAL";
            case 0x15: return "MSCALF";
            case 0x17: return "MSCNT";
            case 0x20: return "STMASK";
            case 0x30: return "STROW";
            case 0x31: return "STCOL";
            case 0x4A: return "MPG";
            case 0x50: return "DIRECT";
            case 0x51: return "DIRECTHL";
            default: break;
            }
            if ((op & 0x60u) == 0x60u)
            {
                static const char *const vl[4] = {"32", "16", "8", "5"};
                return fmt("UNPACK %s%u-%s%s", ((op >> 2) & 3u) == 3 && (op & 3u) == 3 ? "V" : ((op >> 2) & 3u) ? "V" : "S", ((op >> 2) & 3u) + 1u, vl[op & 3u],
                           (op & 0x10u) ? " masked" : "");
            }
            return fmt("code 0x%02x", op);
        };
        for (uint32_t op = 0; op < 128; ++op)
            if (s.opCount[op])
                s.add("vif code", opName(op), s.opCount[op], s.opBytes[op]);
        for (const auto &[key, u] : s.unpacks)
        {
            const uint32_t op = 0x60u | static_cast<uint32_t>(key & 0x1Fu);
            const uint32_t cycle = (key >> 8) & 0xFFFFu, mode = (key >> 24) & 3u;
            std::string k = fmt("%s%s%s CL %u WL %u MODE %u", opName(op).c_str(), (key & 0x20u) ? " unsigned" : " signed", (key & 0x40u) ? " TOPS+" : " absolute",
                                cycle & 0xFFu, (cycle >> 8) & 0xFFu, mode);
            if (op & 0x10u)
                k += fmt(" MASK 0x%08x", static_cast<uint32_t>(key >> 32));
            s.add("vif unpack", k, u.codes, u.vectors);
        }
        for (const auto &[v, n] : s.stcycl)
            s.add("vif STCYCL", fmt("CL %u WL %u", v & 0xFFu, (v >> 8) & 0xFFu), n, 0);
        for (const auto &[v, n] : s.stmod)
            s.add("vif STMOD", fmt("%u", v), n, 0);
        for (const auto &[v, n] : s.stmask)
            s.add("vif STMASK", fmt("0x%08x", v), n, 0);
        for (const auto &[v, n] : s.baseV)
            s.add("vif BASE", fmt("0x%03x", v), n, 0);
        for (const auto &[v, n] : s.offsetV)
            s.add("vif OFFSET", fmt("0x%03x", v), n, 0);
        s.add("vif ITOP", "distinct values", s.itopV.size(), 0);
        for (const auto &[v, n] : s.itopV)
            if (s.itopV.size() <= 24)
                s.add("vif ITOP", fmt("0x%03x", v), n, 0);
        for (const auto &[k, e] : s.mscals)
            s.add("vif mscal (n; m: program uploaded by an earlier kick)", k, e[0], e[1]);
        for (const auto &[k, e] : s.mpgs)
            s.add("vif mpg", k, e[0], e[1]);
        for (int b = 0; b < 32; ++b)
        {
            if (s.destAbs[b])
                s.add("vif unpack dest", fmt("absolute 0x%03x-0x%03x", b * 32, b * 32 + 31), s.destAbs[b], 0);
            if (s.destRel[b])
                s.add("vif unpack dest", fmt("TOPS + 0x%03x-0x%03x", b * 32, b * 32 + 31), s.destRel[b], 0);
        }
        for (int p = 1; p <= 3; ++p)
        {
            const std::string pn = fmt("path%d", p);
            const GifWalker::Stats &w = s.walker[p].stats();
            if (w.tags)
            {
                s.add(pn + " gif", "tags", w.tags, 0);
                s.add(pn + " gif", "PACKED tags", w.packed, 0);
                s.add(pn + " gif", "REGLIST tags", w.reglist, 0);
                s.add(pn + " gif", "IMAGE tags", w.image, w.imageBytes);
                s.add(pn + " gif", "tags with EOP", w.eop, 0);
                s.add(pn + " gif", "bytes dropped by the walker", w.droppedBytes, 0);
            }
            for (const auto &[k, e] : s.gifTags[p])
                s.add(pn + " packet", k, e[0], e[1]);
            for (uint32_t a = 0; a < 0x80; ++a)
            {
                if (!s.regWrites[p][a])
                    continue;
                s.add(pn + " reg writes", fmt("0x%02x", a), s.regWrites[p][a], 0);
                if (a == kGsTex0 || a == kGsTex0 + 1 || a == kGsMiptbp1 || a == kGsMiptbp2 || a == kGsBitbltbuf || a == kGsTrxpos || a == kGsTrxreg)
                {
                    s.add(pn + " reg values", fmt("0x%02x distinct (48 = or more)", a), s.regValues[p][a].size(), 0);
                    continue;
                }
                for (const auto &[v, n] : s.regValues[p][a])
                    s.add(pn + " reg values", v == ~0ull ? fmt("0x%02x other values", a) : fmt("0x%02x = 0x%" PRIx64, a, v), n, 0);
            }
            for (const auto &[key, c] : s.states[p])
            {
                const GsPlan &pl = c.plan;
                const bool reads = pl.blend != kBlendOff && !pl.exactBlend && !pl.unsupported;
                std::string k = fmt("PRIM 0x%03x ALPHA 0x%02x FIX 0x%02x TEST 0x%05x PABE %u ZMSK %u ZPSM %u FBA %u TEX1 0x%" PRIx64 " CLAMP 0x%" PRIx64
                                    " | blend %s%s%s%s ztst %u atst %u/0x%02x guards 0x%x%s",
                                    key.prim, static_cast<unsigned>(key.alpha & 0xFFu), static_cast<unsigned>((key.alpha >> 32) & 0xFFu),
                                    static_cast<unsigned>(key.test & 0x7FFFFu), key.pabe, key.zmsk, key.zpsm, key.fba, key.tex1, key.clamp,
                                    pl.blend == kBlendOff ? "OFF" : pl.blend == kBlendMix ? "MIX" : "MIXSUB", reads ? " READ" : pl.blend != kBlendOff ? " exact" : "",
                                    pl.split ? " split" : "", pl.alphaByBlend ? " alphaByBlend" : "", pl.depthCompare, pl.atst, pl.aref, pl.guards,
                                    pl.unsupported ? " UNSUPPORTED" : "");
                s.add(pn + " draw state (prims, pixels)", k, c.prims, c.pixels);
                s.add(pn + " plan (prims, pixels)", fmt("%s%s%s ztst %u zwrite %u/%u", pl.blend == kBlendOff ? "OFF" : pl.blend == kBlendMix ? "MIX" : "MIXSUB",
                                                        reads ? " READ" : pl.blend != kBlendOff ? " exact" : "", pl.split ? " split" : "", pl.depthCompare,
                                                        pl.passDepthWrite, pl.failDepthWrite),
                      c.prims, c.pixels);
                for (uint32_t gbit = 1; gbit < kGuardCount; ++gbit)
                    if (pl.guards >> gbit & 1u)
                        s.add(pn + " guard (prims, pixels)", gsGuardName(gbit), c.prims, c.pixels);
            }
            for (const auto &[k, n] : s.texFormats[p])
                s.add(pn + " texture format", k, n, 0);
            for (const auto &[k, n] : s.envs[p])
                s.add(pn + " environment", k, n, 0);
            const Impl::Geo &g = s.geo[p];
            if (!(g.tris + g.lines + g.points + g.sprites))
                continue;
            const std::string gn = pn + " geometry";
            static const char *const types[8] = {"point", "line", "line strip", "triangle", "triangle strip", "triangle fan", "sprite", "type 7"};
            for (int i = 0; i < 8; ++i)
                if (g.primType[i])
                    s.add(pn + " prim type", types[i], g.primType[i], 0);
            s.add(gn, "triangles", g.tris, 0);
            s.add(gn, "triangles with a Q other than 1", g.qNotOne, 0);
            s.add(gn, "triangles with different Q at the vertices", g.qDiffer, 0);
            s.add(gn, "triangles with different Z at the vertices", g.zDiffer, 0);
            s.add(gn, "triangles with different alpha at the vertices", g.aDiffer, 0);
            s.add(gn, "triangles with Gouraud RGB (IIP 1)", g.rgbDiffer, 0);
            s.add(gn, "triangles with IIP 0", g.iip0, 0);
            s.add(gn, "triangles with IIP 0 and different vertex colours", g.flatIip, 0);
            s.add(gn, "triangles untextured", g.tme0, 0);
            s.add(gn, "triangles with a subpixel vertex", g.subpixel, 0);
            s.add(gn, "triangles with FST", g.fst, 0);
            s.add(gn, "triangles of zero area", g.zeroArea, 0);
            s.add(gn, "triangles clockwise in screen space as sent", g.backFacing, 0);
            s.add(gn, "primitives wholly outside the scissor", g.boxEmpty, 0);
            s.add(gn, "triangles cut by the scissor", g.clippedByScissor, 0);
            s.add(gn, "vertices with Z at or above 2^24", g.zAbove24, 0);
            static const char *const outside[6] = {"all vertices inside the picture", "a vertex up to 64 pixels outside", "up to 512", "up to 1024", "up to 2048", "more than 2048"};
            for (int i = 0; i < 6; ++i)
                s.add(pn + " vertex distance", outside[i], g.outside[i], 0);
            static const char *const stn[6] = {"S/Q and T/Q within 0..1", "up to 2 (or negative)", "up to 8", "up to 64", "up to 1024", "more"};
            for (int i = 0; i < 6; ++i)
                s.add(pn + " texture coordinate range", stn[i], g.stRange[i], 0);
            static const char *const areas[7] = {"1 pixel", "2-16", "17-256", "257-4096", "4097-16384", "16385-65536", "more"};
            for (int i = 0; i < 7; ++i)
                s.add(pn + " scissored box area", areas[i], g.boxArea[i], 0);
            s.addMax("max", pn + " largest Z", g.zMax);
            if (g.zMin != 0xFFFFFFFFu)
                s.addMax("max", pn + " (0xffffffff - smallest Z)", 0xFFFFFFFFu - g.zMin);
            s.add(gn, "lines", g.lines, 0);
            s.add(gn, "points", g.points, 0);
            s.add(gn, "sprites", g.sprites, 0);
        }
        s.add("path1", "packets outside a reference's run", s.path1Outside, 0);
        for (int p = 0; p < 6; ++p)
        {
            const Impl::Run &r = s.run[p];
            if (!r.runs && !r.continued)
                continue;
            const std::string n = fmt("vu1 %s", kPrograms[p].name);
            s.add(n, "runs", r.runs, 0);
            s.add(n, "runs entered at pc 0", r.entries[0], 0);
            if (kPrograms[p].entry1 != 0xFFFFFFFFu)
                s.add(n, fmt("runs entered at pc 0x%x", kPrograms[p].entry1), r.entries[1], 0);
            if (kPrograms[p].entry2 != 0xFFFFFFFFu)
                s.add(n, fmt("runs entered at pc 0x%x", kPrograms[p].entry2), r.entries[2], 0);
            s.add(n, "re-entered at another pc (a run continued)", r.continued, 0);
            s.add(n, "packets compared", r.packets, 0);
            s.add(n, "packets differing from the reference", r.packetMismatch, 0);
            s.add(n, "runs with a different packet count", r.countMismatch, 0);
            s.add(n, "runs with different final VU memory", r.memoryMismatch, 0);
            s.add(n, "runs the emulated VU1 cut short at its cycle budget", r.cutShort, 0);
            s.add(n, "reference time, microseconds", r.ns / 1000u, 0);
            s.add(n, "runs whose packets carry an incoming register lane unchanged", r.leakRuns, 0);
            s.add(n, "runs whose packets depend on incoming registers otherwise", r.computedRuns, 0);
            s.add(n, "runs whose packet count depends on incoming registers", r.countDiffRuns, 0);
            s.add(n, "runs starting with registers other than the previous run left", r.regsMoved, 0);
            static const char *const lanes = "xyzw";
            for (int i = 4; i < 128; ++i)
            {
                if (!r.leak[i])
                    continue;
                s.add(n + " leaks", fmt("VF%02d.%c", i / 4, lanes[i & 3]), r.leak[i], 0);
                for (const auto &[v, c] : r.leakValues[i])
                    s.add(n + " leak value", v == 0xFFFFFFFFu && r.leakValues[i].size() > 12 ? fmt("VF%02d.%c other values", i / 4, lanes[i & 3])
                                                                                            : fmt("VF%02d.%c = 0x%08x", i / 4, lanes[i & 3], v),
                          c, 0);
                for (int w = 0; w < 7; ++w)
                    if (r.leakFrom[i][w])
                        s.add(n + " leak source", fmt("VF%02d.%c last changed by %s", i / 4, lanes[i & 3], w ? kPrograms[w - 1].name : "nothing since the keyframe"),
                              r.leakFrom[i][w], 0);
            }
            std::string changed;
            for (int reg = 0; reg < 32; ++reg)
                if (r.vfChanged[reg])
                {
                    changed += fmt(" VF%02d.", reg);
                    for (int lane = 0; lane < 4; ++lane)
                        if (r.vfChanged[reg] >> lane & 1u)
                            changed += lanes[lane];
                }
            s.add(n + " changes", fmt("VF:%s | VI mask 0x%04x | ACC mask 0x%x | Q %u P %u I %u", changed.c_str(), r.viChanged, r.accChanged, r.qChanged ? 1 : 0,
                                      r.pChanged ? 1 : 0, r.iChanged ? 1 : 0),
                  1, 0);
        }
        s.add("total", "channel 1 kicks walked", s.vifKicks, 0);
        if (s.picCycles)
        {
            const std::string c = s.roundNearest ? "picture, PATH1 rasterised rounding to nearest, against REFI" : "picture against REFI";
            s.add(c, "cycles compared (n), differing (m)", s.picCycles, s.picCyclesDiffer);
            s.add(c, "pixels compared (n), differing (m)", s.picPixels, s.picPixelsDiffer);
            static const char *const ch[4] = {"R", "G", "B", "A"};
            static const char *const dn[7] = {"-3 or less", "-2", "-1", "0", "+1", "+2", "+3 or more"};
            for (int i = 0; i < 4; ++i)
                for (int d = 0; d < 7; ++d)
                    if (s.picDiff[i][d])
                        s.add(c, fmt("%s difference %s", ch[i], dn[d]), s.picDiff[i][d], 0);
        }
        if (s.rt)
        {
            static const char *const fb[8] = {"sprite 0", "sprite 1", "sprite 2", "sprite 3", "sprite 4", "triangle: texture setup failed",
                                              "triangle: the texture overlaps the pixels written", "triangle: texture cache not prepared"};
            const std::array<uint64_t, 8> &c = GSRaster::FallbackCounters();
            for (int i = 0; i < 8; ++i)
                s.add("reference rasteriser fallbacks (whole replay)", fb[i], c[i], 0);
        }
        for (const auto &[k, e] : s.sums)
            std::printf("V %s\t%s\t%" PRIu64 "\t%" PRIu64 "\n", k.first.c_str(), k.second.c_str(), e[0], e[1]);
        for (const auto &[k, e] : s.maxes)
            std::printf("X %s\t%s\t%" PRIu64 "\n", k.first.c_str(), k.second.c_str(), e);
    }
}
