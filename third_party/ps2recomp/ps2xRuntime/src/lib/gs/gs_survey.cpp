// GS usage survey: see gs_survey.h. Parses GIF data on its own (it never touches the GS front end's state), so what
// it counts is what a host reader at the kicks would see.
#include "runtime/gs/gs_survey.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::gssurvey
{
    namespace detail
    {
        bool g_enabled = false;
    }

    namespace
    {
        constexpr int kRegCount = 128;
        constexpr int kRanges = 16;

        enum : uint8_t
        {
            R_PRIM = 0x00,
            R_XYZF2 = 0x04,
            R_XYZ2 = 0x05,
            R_TEX0_1 = 0x06,
            R_CLAMP_1 = 0x08,
            R_TEX1_1 = 0x14,
            R_PRMODECONT = 0x1a,
            R_PRMODE = 0x1b,
            R_TEXCLUT = 0x1c,
            R_MIPTBP1_1 = 0x34,
            R_MIPTBP2_1 = 0x36,
            R_TEXA = 0x3b,
            R_FOGCOL = 0x3d,
            R_ALPHA_1 = 0x42,
            R_DIMX = 0x44,
            R_DTHE = 0x45,
            R_COLCLAMP = 0x46,
            R_TEST_1 = 0x47,
            R_PABE = 0x49,
            R_FBA_1 = 0x4a,
            R_FRAME_1 = 0x4c,
            R_ZBUF_1 = 0x4e,
            R_BITBLTBUF = 0x50,
            R_TRXPOS = 0x51,
            R_TRXREG = 0x52,
            R_TRXDIR = 0x53,
            R_HWREG = 0x54,
        };

        const char *regName(uint32_t r)
        {
            switch (r)
            {
            case 0x00: return "PRIM";
            case 0x01: return "RGBAQ";
            case 0x02: return "ST";
            case 0x03: return "UV";
            case 0x04: return "XYZF2";
            case 0x05: return "XYZ2";
            case 0x06: return "TEX0_1";
            case 0x07: return "TEX0_2";
            case 0x08: return "CLAMP_1";
            case 0x09: return "CLAMP_2";
            case 0x0a: return "FOG";
            case 0x0c: return "XYZF3";
            case 0x0d: return "XYZ3";
            case 0x14: return "TEX1_1";
            case 0x15: return "TEX1_2";
            case 0x16: return "TEX2_1";
            case 0x17: return "TEX2_2";
            case 0x18: return "XYOFFSET_1";
            case 0x19: return "XYOFFSET_2";
            case 0x1a: return "PRMODECONT";
            case 0x1b: return "PRMODE";
            case 0x1c: return "TEXCLUT";
            case 0x22: return "SCANMSK";
            case 0x34: return "MIPTBP1_1";
            case 0x35: return "MIPTBP1_2";
            case 0x36: return "MIPTBP2_1";
            case 0x37: return "MIPTBP2_2";
            case 0x3b: return "TEXA";
            case 0x3d: return "FOGCOL";
            case 0x3f: return "TEXFLUSH";
            case 0x40: return "SCISSOR_1";
            case 0x41: return "SCISSOR_2";
            case 0x42: return "ALPHA_1";
            case 0x43: return "ALPHA_2";
            case 0x44: return "DIMX";
            case 0x45: return "DTHE";
            case 0x46: return "COLCLAMP";
            case 0x47: return "TEST_1";
            case 0x48: return "TEST_2";
            case 0x49: return "PABE";
            case 0x4a: return "FBA_1";
            case 0x4b: return "FBA_2";
            case 0x4c: return "FRAME_1";
            case 0x4d: return "FRAME_2";
            case 0x4e: return "ZBUF_1";
            case 0x4f: return "ZBUF_2";
            case 0x50: return "BITBLTBUF";
            case 0x51: return "TRXPOS";
            case 0x52: return "TRXREG";
            case 0x53: return "TRXDIR";
            case 0x54: return "HWREG";
            case 0x60: return "SIGNAL";
            case 0x61: return "FINISH";
            case 0x62: return "LABEL";
            default: return nullptr;
            }
        }

        const char *privName(uint32_t off)
        {
            switch (off)
            {
            case 0x000: return "PMODE";
            case 0x010: return "SMODE1";
            case 0x020: return "SMODE2";
            case 0x030: return "SRFSH";
            case 0x040: return "SYNCH1";
            case 0x050: return "SYNCH2";
            case 0x060: return "SYNCV";
            case 0x070: return "DISPFB1";
            case 0x080: return "DISPLAY1";
            case 0x090: return "DISPFB2";
            case 0x0a0: return "DISPLAY2";
            case 0x0b0: return "EXTBUF";
            case 0x0c0: return "EXTDATA";
            case 0x0d0: return "EXTWRITE";
            case 0x0e0: return "BGCOLOR";
            case 0x1000: return "CSR";
            case 0x1010: return "IMR";
            case 0x1040: return "BUSDIR";
            case 0x1080: return "SIGLBLID";
            default: return nullptr;
            }
        }

        bool isVertexReg(uint32_t r)
        {
            return r == 0x01 || r == 0x02 || r == 0x03 || r == 0x04 || r == 0x05 || r == 0x0a || r == 0x0c || r == 0x0d;
        }

        // Registers whose every distinct value is kept (TRXPOS and HWREG vary per transfer; uploads keep TRXPOS).
        bool keepDistinct(uint32_t r) { return !isVertexReg(r) && r != R_TRXPOS && r != R_HWREG; }

        // Kick labels.
        enum Label : int
        {
            L_K1,         // channel 2, normal mode, from the 2D packet buffers
            L_K2,         // channel 1, chain, from the frame's tag array
            L_GifChain,   // channel 2, chain (other)
            L_GifNormal,  // channel 2, normal (other)
            L_Vif1Other,  // channel 1, not the frame chain
            L_Vif0,       // channel 0
            kLabels
        };
        const char *const kLabelNames[kLabels] = {"K1", "K2", "gif-chain", "gif-normal", "vif1-other", "vif0"};

        struct PathAcc
        {
            uint64_t packets = 0, bytes = 0, maxPacket = 0, tags = 0, regWrites = 0, verts = 0, prims = 0;
            uint64_t imageBytes = 0;
            std::vector<std::pair<uint32_t, uint64_t>> fbp; // FRAME FBP in force -> primitives drawn
            std::array<uint32_t, kRegCount> regCount{};
        };

        struct Acc
        {
            PathAcc path[kPaths];
            uint64_t kickCount[kLabels]{};
            uint64_t kickBytes[kLabels]{};
            uint64_t uploads = 0, fbUploads = 0;
            uint32_t uploadMinDbp = 0xffffffffu, uploadMaxDbp = 0;
            uint64_t privWrites = 0, fifoWrites = 0, dispfbWrites = 0;
            int64_t lastDispFbp = -1; // FBP of the last DISPFB1/2 write
            std::vector<std::pair<uint32_t, uint64_t>> labelFbp[kLabels + 1]; // by the kick the data came with
            bool any = false;

            void clear() { *this = Acc{}; }
        };

        struct ParseState
        {
            bool active = false;
            uint8_t fmt = 0;
            uint32_t loopsLeft = 0;
            uint32_t nreg = 0;
            uint8_t regs[16]{};
            uint32_t regIndex = 0;
            uint32_t imageLeft = 0;
            uint32_t totalRegs = 0;
        };

        struct DrawKey
        {
            uint32_t scene, path;
            uint64_t v[15];
            bool operator==(const DrawKey &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
        };
        struct TexKey
        {
            uint32_t scene, path;
            uint64_t v[7];
            bool operator==(const TexKey &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
        };
        struct RegKey
        {
            uint32_t scene, path, reg, pad;
            uint64_t value;
            bool operator==(const RegKey &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
        };
        struct UpKey
        {
            uint32_t scene, path, dir, pad;
            uint64_t bitblt, trxpos, trxreg;
            bool operator==(const UpKey &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
        };
        struct BytesHash
        {
            template <typename T>
            size_t operator()(const T &k) const
            {
                const auto *p = reinterpret_cast<const uint8_t *>(&k);
                uint64_t h = 1469598103934665603ull;
                for (size_t i = 0; i < sizeof(T); ++i)
                    h = (h ^ p[i]) * 1099511628211ull;
                return static_cast<size_t>(h);
            }
        };
        struct DrawStats
        {
            uint64_t prims = 0, verts = 0;
            uint32_t minA = 0xffu, maxA = 0u; // vertex alpha (RGBAQ.A) over the primitives drawn
            uint64_t highA = 0;               // primitives with a vertex alpha above 0x80
        };

        struct State
        {
            std::mutex mutex;
            FILE *out = nullptr;
            std::string aggPath;
            int detailEvery = 30;
            const std::atomic<uint64_t> *field = nullptr;

            uint64_t seq = 0;
            uint32_t baseScene = 0;
            uint64_t lastMovieField = ~0ull;
            uint32_t lastScene = ~0u;

            // Segments run from one frame boundary to the next (or 30 fields without one).
            uint64_t segIndex = 0, segInScene = 0, segStartField = 0;
            uint32_t segFrame = 0;
            bool segDetail = true;
            Acc seg;

            // Kick intervals inside a detail segment, merged when identical.
            Acc iv;
            std::string ivHead;
            std::string pendingLine;
            uint64_t pendingRepeat = 0;

            // GS register shadow, in submission order.
            uint64_t regs[kRegCount]{};
            bool drawDirty = true;
            DrawStats *drawCur = nullptr;
            DrawStats *texCur = nullptr;
            uint32_t vtxInPrim = 0;
            uint32_t primMinA = 0xffu, primMaxA = 0u;
            ParseState parse[kPaths];

            // Kick being walked.
            std::array<uint64_t, 8> tagIds{};
            std::array<uint64_t, kRanges + 1> calls{};
            uint64_t tagCount = 0;

            struct Range
            {
                std::string name;
                uint32_t lo = 0, hi = 0;
            };
            Range ranges[kRanges];
            std::string counterNames;
            uint32_t storeCaller = 0;
            int curLabel = kLabels; // the kick whose data is being parsed (kLabels: none yet)

            // Aggregates.
            std::unordered_map<DrawKey, DrawStats, BytesHash> draws;
            std::unordered_map<TexKey, DrawStats, BytesHash> textures;
            std::unordered_map<RegKey, uint64_t, BytesHash> regValues;
            std::unordered_map<UpKey, uint64_t, BytesHash> uploads;
            std::map<std::string, uint64_t> events;
            uint64_t privLast[0x110]{};
            bool privSeen[0x110]{};

            // Maxima.
            uint64_t maxKickBytes[kLabels]{};
            uint64_t maxKickTags[kLabels]{};
            uint64_t maxKickCalls[kLabels][kRanges + 1]{};
            uint64_t maxSegPathBytes[kPaths]{};
            uint64_t maxPathPacket[kPaths]{};
            uint64_t maxSegKicks[kLabels]{};
            std::vector<uint64_t> maxCounters;
            std::chrono::steady_clock::time_point lastAgg = std::chrono::steady_clock::now();
        };

        State *g_state = nullptr;

        uint64_t fieldNow(State &s) { return s.field ? s.field->load(std::memory_order_relaxed) : 0u; }

        uint32_t sceneNow(State &s)
        {
            const uint64_t f = fieldNow(s);
            const bool movie = s.lastMovieField != ~0ull && f - s.lastMovieField < 16u;
            return s.baseScene | (movie ? 1u : 0u);
        }

        void event(State &s, const std::string &text)
        {
            uint64_t &count = s.events[text];
            if (count++ == 0 && s.out)
                std::fprintf(s.out, "E seq=%llu fld=%llu seg=%llu scene=%x %s\n", (unsigned long long)s.seq, (unsigned long long)fieldNow(s),
                             (unsigned long long)s.segIndex, sceneNow(s), text.c_str());
        }

        // Writes the accumulator compactly: per path pk/bytes/maxpk/tags/regw/verts/prims/img, FBP use, register counts.
        std::string formatAcc(const Acc &a, bool regs)
        {
            std::string line;
            char buf[256];
            for (int l = 0; l < kLabels; ++l)
                if (a.kickCount[l])
                {
                    std::snprintf(buf, sizeof(buf), " k.%s=%llu/%llu", kLabelNames[l], (unsigned long long)a.kickCount[l], (unsigned long long)a.kickBytes[l]);
                    line += buf;
                }
            for (int p = 1; p < kPaths; ++p)
            {
                const PathAcc &pa = a.path[p];
                if (!pa.packets && !pa.regWrites)
                    continue;
                std::snprintf(buf, sizeof(buf), " p%d=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu", p, (unsigned long long)pa.packets, (unsigned long long)pa.bytes,
                              (unsigned long long)pa.maxPacket, (unsigned long long)pa.tags, (unsigned long long)pa.regWrites, (unsigned long long)pa.verts,
                              (unsigned long long)pa.prims, (unsigned long long)pa.imageBytes);
                line += buf;
                if (!pa.fbp.empty())
                {
                    std::snprintf(buf, sizeof(buf), " fb%d=", p);
                    line += buf;
                    for (size_t i = 0; i < pa.fbp.size(); ++i)
                    {
                        std::snprintf(buf, sizeof(buf), "%s%x:%llu", i ? "," : "", pa.fbp[i].first, (unsigned long long)pa.fbp[i].second);
                        line += buf;
                    }
                }
                if (regs)
                {
                    std::string list;
                    for (int r = 0; r < kRegCount; ++r)
                        if (pa.regCount[r])
                        {
                            const char *name = regName(r);
                            if (name)
                                std::snprintf(buf, sizeof(buf), "%s%s:%u", list.empty() ? "" : ",", name, pa.regCount[r]);
                            else
                                std::snprintf(buf, sizeof(buf), "%sR%02x:%u", list.empty() ? "" : ",", r, pa.regCount[r]);
                            list += buf;
                        }
                    if (!list.empty())
                    {
                        std::snprintf(buf, sizeof(buf), " r%d=", p);
                        line += buf + list;
                    }
                }
            }
            for (int l = 0; l <= kLabels; ++l)
                if (!a.labelFbp[l].empty())
                {
                    line += std::string(" lf.") + (l < kLabels ? kLabelNames[l] : "none") + "=";
                    for (size_t i = 0; i < a.labelFbp[l].size(); ++i)
                    {
                        std::snprintf(buf, sizeof(buf), "%s%x:%llu", i ? "," : "", a.labelFbp[l][i].first, (unsigned long long)a.labelFbp[l][i].second);
                        line += buf;
                    }
                }
            if (a.lastDispFbp >= 0)
            {
                std::snprintf(buf, sizeof(buf), " disp=%x", static_cast<unsigned>(a.lastDispFbp));
                line += buf;
            }
            if (a.uploads)
            {
                std::snprintf(buf, sizeof(buf), " up=%llu/%llu/%x-%x", (unsigned long long)a.uploads, (unsigned long long)a.fbUploads, a.uploadMinDbp, a.uploadMaxDbp);
                line += buf;
            }
            if (a.privWrites)
            {
                std::snprintf(buf, sizeof(buf), " priv=%llu/%llu", (unsigned long long)a.privWrites, (unsigned long long)a.dispfbWrites);
                line += buf;
            }
            if (a.fifoWrites)
            {
                std::snprintf(buf, sizeof(buf), " fifo=%llu", (unsigned long long)a.fifoWrites);
                line += buf;
            }
            return line;
        }

        void flushPending(State &s)
        {
            if (s.pendingLine.empty())
                return;
            if (s.pendingRepeat > 1)
                std::fprintf(s.out, "%s x%llu\n", s.pendingLine.c_str(), (unsigned long long)s.pendingRepeat);
            else
                std::fprintf(s.out, "%s\n", s.pendingLine.c_str());
            s.pendingLine.clear();
            s.pendingRepeat = 0;
        }

        // Closes the current kick interval (detail segments only): "I <what opened it> <what it carried>".
        void closeInterval(State &s)
        {
            if (s.segDetail && (s.iv.any || !s.ivHead.empty()))
            {
                std::string line = "I " + (s.ivHead.empty() ? std::string("start") : s.ivHead) + formatAcc(s.iv, true);
                if (line == s.pendingLine)
                    ++s.pendingRepeat;
                else
                {
                    flushPending(s);
                    s.pendingLine = line;
                    s.pendingRepeat = 1;
                }
            }
            s.iv.clear();
            s.ivHead.clear();
        }

        void writeAgg(State &s);

        void closeSegment(State &s, bool idle)
        {
            closeInterval(s);
            flushPending(s);
            const uint64_t f = fieldNow(s);
            std::fprintf(s.out, "S seg=%llu swap=%u fld=%llu-%llu scene=%x%s%s", (unsigned long long)s.segIndex, s.segFrame,
                         (unsigned long long)s.segStartField, (unsigned long long)f, s.lastScene == ~0u ? 0u : s.lastScene,
                         idle ? " idle" : "", formatAcc(s.seg, true).c_str());
            std::fputc('\n', s.out);
            for (int p = 1; p < kPaths; ++p)
                s.maxSegPathBytes[p] = std::max(s.maxSegPathBytes[p], s.seg.path[p].bytes);
            for (int l = 0; l < kLabels; ++l)
                s.maxSegKicks[l] = std::max(s.maxSegKicks[l], s.seg.kickCount[l]);
            std::fflush(s.out);
            s.seg.clear();
            ++s.segIndex;
            s.segStartField = f;
            const auto now = std::chrono::steady_clock::now();
            if (now - s.lastAgg > std::chrono::seconds(3))
            {
                writeAgg(s);
                s.lastAgg = now;
            }
        }

        // Starts the next segment: its scene, and whether it gets per-kick lines.
        void openSegment(State &s)
        {
            const uint32_t scene = sceneNow(s);
            if (scene != s.lastScene)
            {
                s.segInScene = 0;
                s.lastScene = scene;
            }
            else
                ++s.segInScene;
            s.segDetail = s.detailEvery == 0 || s.segInScene < static_cast<uint64_t>(s.detailEvery) ||
                          (s.segIndex % static_cast<uint64_t>(s.detailEvery)) == 0;
        }

        // Called at each hook: ends a segment that has gone 30 fields without a frame boundary (movies, loading).
        void touch(State &s)
        {
            ++s.seq;
            if (fieldNow(s) - s.segStartField >= 30u)
            {
                closeSegment(s, true);
                openSegment(s);
                s.segFrame = ~0u;
            }
        }

        PathAcc &segPath(State &s, int path) { return s.seg.path[path]; }

        void addFbp(std::vector<std::pair<uint32_t, uint64_t>> &list, uint32_t fbp)
        {
            for (auto &e : list)
                if (e.first == fbp)
                {
                    ++e.second;
                    return;
                }
            list.emplace_back(fbp, 1u);
        }

        uint64_t effectivePrim(State &s)
        {
            const uint64_t prim = s.regs[R_PRIM] & 0x7ffu;
            if ((s.regs[R_PRMODECONT] & 1u) != 0u)
                return prim;
            return (prim & 7u) | (s.regs[R_PRMODE] & 0x7f8u);
        }

        void drawPrim(State &s, int path)
        {
            const uint64_t prim = effectivePrim(s);
            const int c = (prim >> 9) & 1u;
            const uint32_t fbp = static_cast<uint32_t>(s.regs[R_FRAME_1 + c] & 0x1ffu);
            addFbp(s.seg.path[path].fbp, fbp);
            addFbp(s.seg.labelFbp[path == EeDirect ? kLabels : s.curLabel], fbp);
            ++s.seg.path[path].prims;
            if (s.segDetail)
            {
                addFbp(s.iv.path[path].fbp, fbp);
                ++s.iv.path[path].prims;
            }
            const uint32_t scene = sceneNow(s);
            const bool tme = (prim >> 4) & 1u;
            if (s.drawDirty || !s.drawCur)
            {
                DrawKey k{};
                k.scene = scene;
                k.path = static_cast<uint32_t>(path);
                const bool fge = (prim >> 5) & 1u;
                const bool abe = (prim >> 6) & 1u;
                k.v[0] = prim;
                k.v[1] = abe ? s.regs[R_ALPHA_1 + c] : 0u;
                k.v[2] = s.regs[R_TEST_1 + c];
                k.v[3] = s.regs[R_FBA_1 + c];
                k.v[4] = s.regs[R_PABE];
                // TEX0 without its addresses and sizes: PSM, TCC, TFX, CPSM, CSM, CSA, CLD.
                k.v[5] = tme ? (s.regs[R_TEX0_1 + c] & ((0x3full << 20) | (0x7ull << 34) | (0xfull << 51) | (0x1ull << 55) | (0x1full << 56) | (0x7ull << 61))) : 0u;
                k.v[6] = tme ? s.regs[R_TEX1_1 + c] : 0u;
                k.v[7] = tme ? (s.regs[R_CLAMP_1 + c] & 0xfu) : 0u;
                k.v[8] = s.regs[R_FRAME_1 + c];
                k.v[9] = s.regs[R_ZBUF_1 + c];
                k.v[10] = s.regs[R_COLCLAMP];
                k.v[11] = s.regs[R_DTHE];
                k.v[12] = tme ? s.regs[R_TEXA] : 0u;
                k.v[13] = fge ? s.regs[R_FOGCOL] : 0u;
                k.v[14] = (s.regs[R_DTHE] & 1u) ? s.regs[R_DIMX] : 0u;
                s.drawCur = &s.draws[k];
                if (tme)
                {
                    TexKey t{};
                    t.scene = scene;
                    t.path = static_cast<uint32_t>(path);
                    const uint64_t tex0 = s.regs[R_TEX0_1 + c];
                    const uint64_t tex1 = s.regs[R_TEX1_1 + c];
                    t.v[0] = tex0;
                    t.v[1] = tex1;
                    t.v[2] = s.regs[R_CLAMP_1 + c];
                    const uint32_t mxl = (tex1 >> 2) & 7u;
                    t.v[3] = mxl ? s.regs[R_MIPTBP1_1 + c] : 0u;
                    t.v[4] = mxl > 3 ? s.regs[R_MIPTBP2_1 + c] : 0u;
                    t.v[5] = s.regs[R_TEXA];
                    t.v[6] = ((tex0 >> 55) & 1u) ? s.regs[R_TEXCLUT] : 0u;
                    s.texCur = &s.textures[t];
                    const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3fffu);
                    if (tbp < 0x1e00u)
                    {
                        char buf[96];
                        std::snprintf(buf, sizeof(buf), "texture-below-0x1e00 path=%d tbp=%x psm=%x", path, tbp, static_cast<unsigned>((tex0 >> 20) & 0x3f));
                        event(s, buf);
                    }
                }
                else
                    s.texCur = nullptr;
                s.drawDirty = false;
            }
            ++s.drawCur->prims;
            s.drawCur->minA = std::min(s.drawCur->minA, s.primMinA);
            s.drawCur->maxA = std::max(s.drawCur->maxA, s.primMaxA);
            if (s.primMaxA > 0x80u)
                ++s.drawCur->highA;
            if (s.texCur)
                ++s.texCur->prims;
            const uint32_t a = static_cast<uint32_t>((s.regs[0x01] >> 24) & 0xffu);
            s.primMinA = s.primMaxA = a;
        }

        void vertexKick(State &s, int path)
        {
            ++s.seg.path[path].verts;
            if (s.segDetail)
                ++s.iv.path[path].verts;
            if (s.drawCur)
                ++s.drawCur->verts;
            const uint32_t a = static_cast<uint32_t>((s.regs[0x01] >> 24) & 0xffu);
            s.primMinA = std::min(s.primMinA, a);
            s.primMaxA = std::max(s.primMaxA, a);
            const uint32_t type = static_cast<uint32_t>(s.regs[R_PRIM] & 7u);
            const uint32_t n = ++s.vtxInPrim;
            bool emit = false;
            switch (type)
            {
            case 0: emit = true; s.vtxInPrim = 0; break;                  // point
            case 1: case 6: if (n == 2) { emit = true; s.vtxInPrim = 0; } break; // line, sprite
            case 2: emit = n >= 2; break;                                  // line strip
            case 3: if (n == 3) { emit = true; s.vtxInPrim = 0; } break;   // triangle
            case 4: case 5: emit = n >= 3; break;                          // strip, fan
            default: break;
            }
            if (emit)
                drawPrim(s, path);
        }

        void upload(State &s, int path)
        {
            const uint64_t bitblt = s.regs[R_BITBLTBUF];
            const uint32_t dir = static_cast<uint32_t>(s.regs[R_TRXDIR] & 3u);
            UpKey k{};
            k.scene = sceneNow(s);
            k.path = static_cast<uint32_t>(path);
            k.dir = dir;
            k.bitblt = dir == 0 ? (bitblt & 0xffffffff00000000ull) : dir == 1 ? (bitblt & 0xffffffffull) : bitblt;
            k.trxpos = s.regs[R_TRXPOS];
            k.trxreg = s.regs[R_TRXREG];
            ++s.uploads[k];
            const uint32_t dbp = static_cast<uint32_t>((bitblt >> 32) & 0x3fffu);
            for (Acc *a : {&s.seg, &s.iv})
            {
                if (a == &s.iv && !s.segDetail)
                    continue;
                ++a->uploads;
                a->any = true;
                if (dir == 0)
                {
                    a->uploadMinDbp = std::min(a->uploadMinDbp, dbp);
                    a->uploadMaxDbp = std::max(a->uploadMaxDbp, dbp);
                    if (dbp < 0x1e00u)
                        ++a->fbUploads;
                }
            }
            if (dir != 0)
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "transfer-dir-%u path=%d bitbltbuf=%llx trxpos=%llx trxreg=%llx", dir, path, (unsigned long long)bitblt,
                              (unsigned long long)s.regs[R_TRXPOS], (unsigned long long)s.regs[R_TRXREG]);
                event(s, buf);
            }
        }

        void regWrite(State &s, int path, uint32_t reg, uint64_t value)
        {
            reg &= 0x7fu;
            ++s.seg.path[path].regWrites;
            ++s.seg.path[path].regCount[reg];
            s.seg.any = true;
            if (s.segDetail)
            {
                ++s.iv.path[path].regWrites;
                ++s.iv.path[path].regCount[reg];
                s.iv.any = true;
            }
            if (reg == R_XYZ2 || reg == R_XYZF2)
            {
                s.regs[reg] = value;
                vertexKick(s, path);
                return;
            }
            if (isVertexReg(reg))
            {
                s.regs[reg] = value;
                return;
            }
            if (keepDistinct(reg))
            {
                RegKey k{};
                k.scene = sceneNow(s);
                k.path = static_cast<uint32_t>(path);
                k.reg = reg;
                k.value = value;
                ++s.regValues[k];
            }
            if (!regName(reg))
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "unknown-register path=%d reg=%02x", path, reg);
                event(s, buf);
            }
            s.regs[reg] = value;
            s.drawDirty = true;
            if (reg == R_PRIM)
                s.vtxInPrim = 0;
            else if (reg == R_TRXDIR)
                upload(s, path);
            else if (reg == R_FRAME_1 || reg == R_FRAME_1 + 1)
            {
                const uint32_t fbp = static_cast<uint32_t>(value & 0x1ffu);
                if (fbp != 0u && fbp != 0x50u)
                {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "frame-fbp path=%d reg=%s value=%llx", path, regName(reg), (unsigned long long)value);
                    event(s, buf);
                }
            }
        }

        void imageData(State &s, int path, uint32_t bytes)
        {
            s.seg.path[path].imageBytes += bytes;
            if (s.segDetail)
                s.iv.path[path].imageBytes += bytes;
        }

        void parse(State &s, int path, const uint8_t *data, uint32_t size)
        {
            ParseState &ps = s.parse[path];
            uint32_t off = 0;
            auto ld = [&](uint32_t o)
            {
                uint64_t v;
                std::memcpy(&v, data + o, 8);
                return v;
            };
            while (off < size)
            {
                if (!ps.active)
                {
                    if (off + 16 > size)
                        break;
                    const uint64_t lo = ld(off), hi = ld(off + 8);
                    off += 16;
                    ++s.seg.path[path].tags;
                    if (s.segDetail)
                        ++s.iv.path[path].tags;
                    ps = ParseState{};
                    ps.loopsLeft = static_cast<uint32_t>(lo & 0x7fffu);
                    ps.fmt = static_cast<uint8_t>((lo >> 58) & 3u);
                    ps.nreg = static_cast<uint32_t>((lo >> 60) & 0xfu);
                    if (ps.nreg == 0)
                        ps.nreg = 16;
                    for (uint32_t i = 0; i < ps.nreg; ++i)
                        ps.regs[i] = static_cast<uint8_t>((hi >> (i * 4)) & 0xfu);
                    if ((lo >> 46) & 1u)
                        regWrite(s, path, R_PRIM, (lo >> 47) & 0x7ffu);
                    if (ps.fmt >= 2)
                    {
                        ps.imageLeft = ps.loopsLeft * 16u;
                        ps.active = ps.imageLeft != 0;
                    }
                    else
                    {
                        ps.totalRegs = ps.loopsLeft * ps.nreg;
                        ps.active = ps.loopsLeft != 0;
                    }
                    continue;
                }
                if (ps.fmt >= 2)
                {
                    const uint32_t n = std::min(ps.imageLeft, size - off);
                    imageData(s, path, n);
                    ps.imageLeft -= n;
                    off += n;
                    ps.active = ps.imageLeft != 0;
                    continue;
                }
                const bool packed = ps.fmt == 0;
                const uint32_t item = packed ? 16u : 8u;
                if (off + item > size)
                    break; // the runtime drops a partial item too
                const uint32_t desc = ps.regs[ps.regIndex];
                const uint64_t lo = ld(off);
                if (packed)
                {
                    const uint64_t hi = ld(off + 8);
                    switch (desc)
                    {
                    case 0x0: regWrite(s, path, R_PRIM, lo & 0x7ffu); break;
                    case 0x1: // RGBAQ: the four colour bytes are 32 bits apart
                        regWrite(s, path, 0x01, (lo & 0xffu) | ((lo >> 32) & 0xffu) << 8 | (hi & 0xffu) << 16 | ((hi >> 32) & 0xffu) << 24);
                        break;
                    case 0x4: regWrite(s, path, ((hi >> 47) & 1u) ? 0x0c : R_XYZF2, lo); break;
                    case 0x5: regWrite(s, path, ((hi >> 47) & 1u) ? 0x0d : R_XYZ2, lo); break;
                    case 0xe: regWrite(s, path, static_cast<uint32_t>(hi & 0xffu), lo); break;
                    case 0xf: break;
                    default: regWrite(s, path, desc, lo); break;
                    }
                }
                else if (desc != 0xe && desc != 0xf)
                    regWrite(s, path, desc, lo);
                off += item;
                if (++ps.regIndex >= ps.nreg)
                {
                    ps.regIndex = 0;
                    if (--ps.loopsLeft == 0)
                    {
                        ps.active = false;
                        if (!packed && (ps.totalRegs & 1u))
                            off += 8; // REGLIST pads to a whole quadword
                    }
                }
            }
        }

        const char *psmName(uint32_t psm)
        {
            switch (psm)
            {
            case 0x00: return "CT32";
            case 0x01: return "CT24";
            case 0x02: return "CT16";
            case 0x0a: return "CT16S";
            case 0x13: return "T8";
            case 0x14: return "T4";
            case 0x1b: return "T8H";
            case 0x24: return "T4HL";
            case 0x2c: return "T4HH";
            case 0x30: return "Z32";
            case 0x31: return "Z24";
            case 0x32: return "Z16";
            case 0x3a: return "Z16S";
            default: return "?";
            }
        }

        void writeAgg(State &s)
        {
            const std::string tmp = s.aggPath + ".tmp";
            FILE *f = std::fopen(tmp.c_str(), "w");
            if (!f)
                return;
            std::fprintf(f, "# gs survey aggregates; scene = mode<<12 | level<<4 | split<<1 | movie\n");
            for (const auto &e : s.regValues)
                std::fprintf(f, "R %x %u %s %llx %llu\n", e.first.scene, e.first.path, regName(e.first.reg) ? regName(e.first.reg) : "?",
                             (unsigned long long)e.first.value, (unsigned long long)e.second);
            for (const auto &e : s.draws)
            {
                std::fprintf(f, "D %x %u", e.first.scene, e.first.path);
                for (uint64_t v : e.first.v)
                    std::fprintf(f, " %llx", (unsigned long long)v);
                std::fprintf(f, " %llu %llu %x %x %llu\n", (unsigned long long)e.second.prims, (unsigned long long)e.second.verts, e.second.minA, e.second.maxA,
                             (unsigned long long)e.second.highA);
            }
            for (const auto &e : s.textures)
            {
                std::fprintf(f, "T %x %u", e.first.scene, e.first.path);
                for (uint64_t v : e.first.v)
                    std::fprintf(f, " %llx", (unsigned long long)v);
                std::fprintf(f, " %llu %s\n", (unsigned long long)e.second.prims, psmName((e.first.v[0] >> 20) & 0x3f));
            }
            for (const auto &e : s.uploads)
                std::fprintf(f, "U %x %u %u %llx %llx %llx %llu\n", e.first.scene, e.first.path, e.first.dir, (unsigned long long)e.first.bitblt,
                             (unsigned long long)e.first.trxpos, (unsigned long long)e.first.trxreg, (unsigned long long)e.second);
            for (const auto &e : s.events)
                std::fprintf(f, "E %llu %s\n", (unsigned long long)e.second, e.first.c_str());
            for (int l = 0; l < kLabels; ++l)
            {
                std::fprintf(f, "M kick %s maxbytes=%llu maxtags=%llu maxperseg=%llu", kLabelNames[l], (unsigned long long)s.maxKickBytes[l],
                             (unsigned long long)s.maxKickTags[l], (unsigned long long)s.maxSegKicks[l]);
                for (int r = 0; r <= kRanges; ++r)
                    if (s.maxKickCalls[l][r])
                        std::fprintf(f, " calls.%s=%llu", r < kRanges ? s.ranges[r].name.c_str() : "other", (unsigned long long)s.maxKickCalls[l][r]);
                std::fputc('\n', f);
            }
            for (int p = 1; p < kPaths; ++p)
                std::fprintf(f, "M path %d maxpacket=%llu maxsegbytes=%llu\n", p, (unsigned long long)s.maxPathPacket[p], (unsigned long long)s.maxSegPathBytes[p]);
            if (!s.maxCounters.empty())
            {
                std::fprintf(f, "M counters %s", s.counterNames.c_str());
                for (uint64_t v : s.maxCounters)
                    std::fprintf(f, " %llu", (unsigned long long)v);
                std::fputc('\n', f);
            }
            std::fclose(f);
            std::rename(tmp.c_str(), s.aggPath.c_str());
        }

        // Created at start-up when RC_GS_SURVEY is set.
        const bool g_init = []
        {
            const char *path = std::getenv("RC_GS_SURVEY");
            if (!path || !*path)
                return false;
            auto *s = new State();
            s->out = std::fopen(path, "w");
            if (!s->out)
            {
                std::fprintf(stderr, "[gs survey] cannot write %s\n", path);
                delete s;
                return false;
            }
            std::setvbuf(s->out, nullptr, _IOFBF, 1 << 20);
            s->aggPath = std::string(path) + ".agg";
            if (const char *d = std::getenv("RC_GS_SURVEY_DETAIL"))
                s->detailEvery = std::atoi(d);
            s->regs[R_PRMODECONT] = 1;
            s->segDetail = true;
            std::fprintf(s->out, "# gs survey v1; detail every %d segments\n", s->detailEvery);
            std::fprintf(stderr, "[gs survey] writing %s\n", path);
            g_state = s;
            detail::g_enabled = true;
            return true;
        }();
    } // namespace

    void gifPacket(int path, const uint8_t *data, uint32_t sizeBytes)
    {
        State *s = g_state;
        if (!s || !data || path <= 0 || path >= kPaths)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        touch(*s);
        PathAcc &pa = segPath(*s, path);
        ++pa.packets;
        pa.bytes += sizeBytes;
        pa.maxPacket = std::max<uint64_t>(pa.maxPacket, sizeBytes);
        s->maxPathPacket[path] = std::max<uint64_t>(s->maxPathPacket[path], sizeBytes);
        s->seg.any = true;
        if (s->segDetail)
        {
            PathAcc &ia = s->iv.path[path];
            ++ia.packets;
            ia.bytes += sizeBytes;
            ia.maxPacket = std::max<uint64_t>(ia.maxPacket, sizeBytes);
            s->iv.any = true;
        }
        if (path == Path2)
            event(*s, "path2-packet");
        parse(*s, path, data, sizeBytes);
    }

    void nativeImage(int path, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir, uint32_t bytes)
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        touch(*s);
        event(*s, "native-image-fast-path");
        ++segPath(*s, path).packets;
        segPath(*s, path).bytes += bytes + 96u;
        regWrite(*s, path, R_BITBLTBUF, bitbltbuf);
        regWrite(*s, path, R_TRXPOS, trxpos);
        regWrite(*s, path, R_TRXREG, trxreg);
        regWrite(*s, path, R_TRXDIR, trxdir);
        imageData(*s, path, bytes);
    }

    void eeRegisterWrite(uint8_t reg, uint64_t value)
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        touch(*s);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "ee-direct-register %s", regName(reg) ? regName(reg) : "?");
        event(*s, buf);
        regWrite(*s, EeDirect, reg, value);
    }

    void chainTag(uint32_t id, uint32_t addr, uint32_t qwc)
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        (void)qwc;
        ++s->tagIds[id & 7u];
        ++s->tagCount;
        if ((id & 7u) == 5u) // call: count by target range
        {
            int r = 0;
            for (; r < kRanges; ++r)
                if (!s->ranges[r].name.empty() && addr >= s->ranges[r].lo && addr < s->ranges[r].hi)
                    break;
            ++s->calls[r];
        }
    }

    void dmaKick(uint32_t channelBase, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t bytes)
    {
        State *s = g_state;
        if (!s || channelBase > 0x1000a000u)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        touch(*s);
        const uint32_t mode = (chcr >> 2) & 3u;
        const auto inRange = [&](const char *prefix, uint32_t a, int &which)
        {
            for (int r = 0; r < kRanges; ++r)
                if (s->ranges[r].name.rfind(prefix, 0) == 0 && a >= s->ranges[r].lo && a < s->ranges[r].hi)
                {
                    which = r;
                    return true;
                }
            return false;
        };
        int which = -1;
        Label label;
        if (channelBase == 0x1000a000u)
            label = mode == 1 ? L_GifChain : (inRange("k1buf", madr & 0x0fffffffu, which) ? L_K1 : L_GifNormal);
        else if (channelBase == 0x10009000u)
            label = (mode == 1 && inRange("chain", tadr & 0x0fffffffu, which)) ? L_K2 : L_Vif1Other;
        else
            label = L_Vif0;
        if (mode != 1)
            bytes = qwc * 16u;
        closeInterval(*s);
        char head[512];
        int n = std::snprintf(head, sizeof(head), "K %s ch=%x mode=%u madr=%x qwc=%u tadr=%x bytes=%u ra=%x seq=%llu fld=%llu", kLabelNames[label],
                              (channelBase >> 12) & 0xfu, mode, madr, qwc, tadr, bytes, s->storeCaller, (unsigned long long)s->seq,
                              (unsigned long long)fieldNow(*s));
        if (mode == 1)
        {
            n += std::snprintf(head + n, sizeof(head) - n, " tags=%llu ids=", (unsigned long long)s->tagCount);
            for (int i = 0; i < 8; ++i)
                n += std::snprintf(head + n, sizeof(head) - n, "%s%llu", i ? "," : "", (unsigned long long)s->tagIds[i]);
            for (int r = 0; r <= kRanges; ++r)
                if (s->calls[r])
                {
                    n += std::snprintf(head + n, sizeof(head) - n, " calls.%s=%llu", r < kRanges ? s->ranges[r].name.c_str() : "other",
                                       (unsigned long long)s->calls[r]);
                    s->maxKickCalls[label][r] = std::max(s->maxKickCalls[label][r], s->calls[r]);
                }
            s->maxKickTags[label] = std::max(s->maxKickTags[label], s->tagCount);
        }
        s->ivHead = head; // written as the head of this kick's interval line (detail segments only)
        s->curLabel = label;
        s->maxKickBytes[label] = std::max<uint64_t>(s->maxKickBytes[label], bytes);
        ++s->seg.kickCount[label];
        s->seg.kickBytes[label] += bytes;
        s->seg.any = true;
        s->tagIds.fill(0);
        s->calls.fill(0);
        s->tagCount = 0;
    }

    void privWrite(uint32_t regOffset, uint64_t value)
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        touch(*s);
        ++s->seg.privWrites;
        s->seg.any = true;
        const uint32_t idx = regOffset >= 0x1000u ? 0x100u + ((regOffset - 0x1000u) >> 4) : regOffset >> 4;
        const bool dispfb = regOffset == 0x70u || regOffset == 0x90u;
        if (dispfb)
        {
            ++s->seg.dispfbWrites;
            s->seg.lastDispFbp = static_cast<int64_t>(value & 0x1ffu);
        }
        bool log = idx < 0x110u && (!s->privSeen[idx] || s->privLast[idx] != value);
        if (regOffset == 0x1000u)
            log = (value & 0x300u) != 0u; // CSR: only FLUSH and RESET
        if (dispfb)
            log = true;
        if (idx < 0x110u)
        {
            s->privSeen[idx] = true;
            s->privLast[idx] = value;
        }
        if (log)
        {
            if (dispfb)
            {
                closeInterval(*s);
                s->ivHead = std::string("P ") + privName(regOffset);
            }
            flushPending(*s);
            const char *name = privName(regOffset);
            std::fprintf(s->out, "P %s=%llx seq=%llu fld=%llu seg=%llu ra=%x\n", name ? name : "?", (unsigned long long)value,
                         (unsigned long long)s->seq, (unsigned long long)fieldNow(*s), (unsigned long long)s->segIndex, s->storeCaller);
        }
    }

    void fifoWrite(uint32_t address)
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        touch(*s);
        ++s->seg.fifoWrites;
        s->seg.any = true;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "ee-fifo-write %x", address & ~0xfffu);
        event(*s, buf);
    }

    void setCrt(int interlaced, int videoMode, int frameMode)
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        touch(*s);
        flushPending(*s);
        std::fprintf(s->out, "C SetGsCrt interlaced=%d mode=%d frame=%d fld=%llu\n", interlaced, videoMode, frameMode, (unsigned long long)fieldNow(*s));
    }

    void noteStoreCaller(uint32_t ra)
    {
        if (State *s = g_state)
            s->storeCaller = ra;
    }

    void frameBoundary(uint32_t frame, uint32_t scene, const uint32_t *counters, int counterCount)
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->seq;
        // Closes the segment that ends here (its scene is the one in force while it ran), then opens the next.
        closeSegment(*s, false);
        s->baseScene = scene;
        openSegment(*s);
        s->segFrame = frame;
        const std::vector<uint32_t> now(counters, counters + counterCount);
        std::fprintf(s->out, "F frame=%u scene=%x", frame, scene);
        for (uint32_t v : now)
            std::fprintf(s->out, " %u", v);
        std::fputc('\n', s->out);
        if (s->maxCounters.size() < now.size())
            s->maxCounters.resize(now.size());
        for (size_t i = 0; i < now.size(); ++i)
            s->maxCounters[i] = std::max<uint64_t>(s->maxCounters[i], now[i]);
    }

    void setCounterNames(const char *names)
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->counterNames != names)
        {
            s->counterNames = names;
            std::fprintf(s->out, "N %s\n", names);
        }
    }

    void setRange(int index, const char *name, uint32_t lo, uint32_t hi)
    {
        State *s = g_state;
        if (!s || index < 0 || index >= kRanges)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        auto &r = s->ranges[index];
        if (r.name != name || r.lo != lo || r.hi != hi)
        {
            r.name = name;
            r.lo = lo;
            r.hi = hi;
            std::fprintf(s->out, "G range %d %s %x %x\n", index, name, lo, hi);
        }
    }

    void markMovie()
    {
        State *s = g_state;
        if (!s)
            return;
        std::lock_guard<std::mutex> lock(s->mutex);
        s->lastMovieField = fieldNow(*s);
    }

    void setFieldCounter(const std::atomic<uint64_t> *counter)
    {
        if (State *s = g_state)
            s->field = counter;
    }
} // namespace ps2x::gssurvey
