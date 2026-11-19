// rc_replay's tex consumer: see tex.h and RENDERER_FRAME_RECORD.md section 17.
#include "tex.h"

#include "png.h"
#include "../../render/gfx/tags.h"
#include "../../render/tex/tex_cache.h"
#include "../../render/tex/tex_shadow.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace rc::rec;
using namespace rc::rd;

namespace rc::replay
{
    namespace
    {
        constexpr uint32_t kTexFirstByte = GsMemory::kTextureFirstBlock * kGsBlockBytes; // 0x1e0000

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        const char *psmName(uint8_t psm)
        {
            switch (psm)
            {
            case kPsmCt32: return "CT32";
            case kPsmCt24: return "CT24";
            case kPsmCt16: return "CT16";
            case kPsmCt16s: return "CT16S";
            case kPsmT8: return "T8";
            case kPsmT4: return "T4";
            default: return "?";
            }
        }

        bool writePngRgba(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height)
        {
            std::vector<uint8_t> raw;
            raw.reserve(static_cast<size_t>(height) * (width * 4u + 1u));
            for (uint32_t y = 0; y < height; ++y)
            {
                raw.push_back(0);
                const uint8_t *row = rgba + static_cast<size_t>(y) * width * 4u;
                for (uint32_t x = 0; x < width; ++x)
                {
                    raw.insert(raw.end(), row + x * 4u, row + x * 4u + 3u);
                    raw.push_back(static_cast<uint8_t>(std::min(255u, row[x * 4u + 3u] * 255u / 128u)));
                }
            }
            uLongf packedBytes = compressBound(static_cast<uLong>(raw.size()));
            std::vector<uint8_t> packed(packedBytes);
            if (compress2(packed.data(), &packedBytes, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
                return false;
            std::FILE *f = std::fopen(path.c_str(), "wb");
            if (!f)
                return false;
            static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            std::fwrite(sig, 1, 8, f);
            const uint8_t ihdr[13] = {static_cast<uint8_t>(width >> 24), static_cast<uint8_t>(width >> 16), static_cast<uint8_t>(width >> 8),
                                      static_cast<uint8_t>(width), static_cast<uint8_t>(height >> 24), static_cast<uint8_t>(height >> 16),
                                      static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height), 8, 6, 0, 0, 0};
            pngChunk(f, "IHDR", ihdr, 13);
            pngChunk(f, "IDAT", packed.data(), static_cast<uint32_t>(packedBytes));
            pngChunk(f, "IEND", nullptr, 0);
            return std::fclose(f) == 0;
        }

        // A draw state as far as blending and the alpha test go.
        struct StateKey
        {
            uint8_t path = 0, pabe = 0;
            uint16_t prim = 0; // PRIM's attribute bits as in force (PRMODE when PRMODECONT is 0)
            uint64_t alpha = 0, test = 0;
            bool operator<(const StateKey &o) const
            {
                if (path != o.path) return path < o.path;
                if (prim != o.prim) return prim < o.prim;
                if (alpha != o.alpha) return alpha < o.alpha;
                if (test != o.test) return test < o.test;
                return pabe < o.pabe;
            }
        };
        struct Combo
        {
            uint64_t prims = 0;
            uint64_t byVertexAlpha[256] = {}; // primitives by their largest vertex alpha
            uint8_t texAlphaMax = 0;          // the texture's largest alpha over the draws (as decoded at each draw)
        };
        // A texture as the draws saw it: its alpha summary at the times it was drawn with. (Memory under a cache entry
        // can later hold something else, a new level's image for one; that is not this texture any more.)
        struct TexUse
        {
            uint64_t prims = 0;
            uint8_t alphaMin = 0xFF, alphaMax = 0;
            uint32_t above80 = 0, texels = 0;
        };

        // Does a pixel with source alpha `a` reach the colour output under TEST (the alpha test passes, or fails with
        // FB_ONLY / RGB_ONLY)?
        bool reachesColour(uint64_t test, uint32_t a)
        {
            if (!(test & 1u))
                return true;
            const uint32_t atst = static_cast<uint32_t>((test >> 1) & 7u), aref = static_cast<uint32_t>((test >> 4) & 0xFFu),
                           afail = static_cast<uint32_t>((test >> 12) & 3u);
            bool pass = false;
            switch (atst)
            {
            case 0: pass = false; break;
            case 1: pass = true; break;
            case 2: pass = a < aref; break;
            case 3: pass = a <= aref; break;
            case 4: pass = a == aref; break;
            case 5: pass = a >= aref; break;
            case 6: pass = a > aref; break;
            default: pass = a != aref; break;
            }
            return pass || afail == 1u || afail == 3u;
        }

        std::string testText(uint64_t test)
        {
            static const char *const atst[8] = {"never", "always", "A<", "A<=", "A==", "A>=", "A>", "A!="};
            static const char *const afail[4] = {"KEEP", "FB_ONLY", "ZB_ONLY", "RGB_ONLY"};
            char buf[64];
            if (!(test & 1u))
                return "off";
            const uint32_t t = static_cast<uint32_t>((test >> 1) & 7u);
            if (t <= 1u)
                std::snprintf(buf, sizeof(buf), "%s %s", atst[t], afail[(test >> 12) & 3u]);
            else
                std::snprintf(buf, sizeof(buf), "%s0x%02x %s", atst[t], static_cast<unsigned>((test >> 4) & 0xFFu), afail[(test >> 12) & 3u]);
            return buf;
        }
    }

    struct TexCheck::Impl final : GifSink
    {
        TexOptions opt;
        PS2Runtime *rt = nullptr;
        FileHeader header{};
        bool exact = false, started = false;
        TexShadow shadow;
        TextureCache cache{shadow.memory()};
        uint64_t failures = 0;
        uint64_t cycle = 0;
        bool faultDone = false;

        // Checks.
        uint64_t keyChecked = 0, keyOk = 0, finalChecked = 0, finalOk = 0, keyLoaded = 0, keyInexact = 0, keyInexactOk = 0;
        uint64_t regsChecked = 0, regsOk = 0;
        uint64_t kickCompared = 0, kickEqual = 0;
        uint64_t sampled = 0, sampledOk = 0, sampledTexels = 0, sampledSkipped = 0;

        // The usage scan: one GS register file over every path, in order.
        uint64_t regs[0x63] = {};
        int path = 3;
        GifWalker pathWalker[4];
        std::vector<uint8_t> pending;                         // PATH1/2 packets of the kick the emu consumer just ran
        std::vector<std::pair<int, uint32_t>> pendingPackets; // path, bytes
        bool comboDirty = true;
        Combo *combo = nullptr;
        uint32_t vertexCount = 0;
        uint8_t vertexAlpha[3] = {};
        std::map<std::pair<StateKey, const CachedTexture *>, Combo> combos;
        std::map<const CachedTexture *, TexUse> uses;
        const CachedTexture *comboTex = nullptr;
        TexUse *texUse = nullptr;
        struct MipUse
        {
            uint32_t mxl = 0;
            uint64_t tex1 = 0, miptbp1 = 0, miptbp2 = 0, prims = 0;
        };
        std::map<const CachedTexture *, MipUse> mips;
        MipUse *mipUse = nullptr;
        uint64_t draws = 0, tex2Writes = 0, noKickVertices = 0, undecodable = 0;
        // The GS survey's vertex-alpha count (gs_survey.cpp), kept to compare with it: a primitive's range there starts
        // from the RGBAQ in force when the previous primitive was drawn, whatever path and state that one had.
        uint32_t surveyMaxA = 0;
        uint64_t surveyHigh = 0, surveyHighPabe = 0;
        std::set<uint32_t> scenes;

        // Per cycle, for the cost figures.
        struct CycleCost
        {
            uint64_t applyNs = 0, bytes = 0, transfers = 0, changing = 0, blocks = 0, redecoded = 0, first = 0, texels = 0, decodeNs = 0;
        };
        TextureCache::Stats cacheAtCycle{};
        std::vector<CycleCost> costs;
        TexShadow::Stats shadowAtCycle{};
        GsMemory::Stats memAtCycle{};
        uint64_t fullDecodeNs = 0, fullDecodeTexels = 0, fullDecodeTextures = 0;

        void fail(const std::string &what)
        {
            if (++failures <= 40)
                std::printf("FAIL %s\n", what.c_str());
        }

        // ---- the usage scan ----
        void reg(uint8_t addr, uint64_t v) override
        {
            switch (addr)
            {
            case 0x00:
                regs[0] = v;
                vertexCount = 0;
                comboDirty = true;
                return;
            case 0x01: regs[1] = v; return;
            case 0x04:
            case 0x05: vertex(true); return;
            case 0x0C:
            case 0x0D:
                ++noKickVertices;
                vertex(false);
                return;
            case 0x16:
            case 0x17:
            {
                // TEX2: the format and palette fields of TEX0.
                constexpr uint64_t mask = (0x3Full << 20) | (0x3FFFull << 37) | (0xFull << 51) | (1ull << 55) | (0x1Full << 56) | (7ull << 61);
                regs[addr - 0x10] = (regs[addr - 0x10] & ~mask) | (v & mask);
                ++tex2Writes;
                comboDirty = true;
                return;
            }
            default: break;
            }
            if (addr >= 0x63)
                return;
            regs[addr] = v;
            switch (addr)
            {
            case 0x06: case 0x07: case 0x14: case 0x15: case 0x34: case 0x35: case 0x36: case 0x37: case 0x42: case 0x43:
            case 0x47: case 0x48: case 0x49: case 0x3B: case 0x1A: case 0x1B:
                comboDirty = true;
                break;
            default: break;
            }
        }
        void image(const uint8_t *, uint32_t) override {}

        uint32_t attributes() const { return static_cast<uint32_t>((regs[0x1A] & 1u) ? regs[0] : ((regs[0] & 7u) | (regs[0x1B] & 0x7F8u))) & 0x7FFu; }

        void vertex(bool kick)
        {
            vertexAlpha[0] = vertexAlpha[1];
            vertexAlpha[1] = vertexAlpha[2];
            vertexAlpha[2] = static_cast<uint8_t>(regs[1] >> 24);
            surveyMaxA = std::max<uint32_t>(surveyMaxA, vertexAlpha[2]);
            ++vertexCount;
            const uint32_t attrs = attributes();
            bool complete = false;
            uint32_t corners = 1;
            switch (attrs & 7u)
            {
            case 0: complete = true; vertexCount = 0; break;
            case 1: corners = 2; if (vertexCount == 2) { complete = true; vertexCount = 0; } break;
            case 2: corners = 2; complete = vertexCount >= 2; break;
            case 3: corners = 3; if (vertexCount == 3) { complete = true; vertexCount = 0; } break;
            case 4:
            case 5: corners = 3; complete = vertexCount >= 3; break;
            case 6: corners = 1; if (vertexCount == 2) { complete = true; vertexCount = 0; } break; // a sprite takes its second vertex's colour
            default: break;
            }
            if (!complete || !kick)
                return;
            if (comboDirty)
                resolve(attrs);
            uint8_t a = vertexAlpha[2];
            if (attrs & 8u) // Gouraud: any corner's alpha can reach a pixel
                for (uint32_t i = 1; i < corners; ++i)
                    a = std::max(a, vertexAlpha[2 - i]);
            if (surveyMaxA > 0x80u)
            {
                ++surveyHigh;
                surveyHighPabe += (regs[0x49] & 1u) && (attrs & 0x40u);
            }
            surveyMaxA = vertexAlpha[2];
            ++draws;
            ++combo->prims;
            ++combo->byVertexAlpha[a];
            if (comboTex)
            {
                combo->texAlphaMax = std::max(combo->texAlphaMax, comboTex->alphaMax);
                TexUse &u = *texUse;
                ++u.prims;
                u.alphaMin = std::min(u.alphaMin, comboTex->alphaMin);
                u.alphaMax = std::max(u.alphaMax, comboTex->alphaMax);
                u.above80 = std::max(u.above80, comboTex->alphaAbove80);
            }
            if (mipUse)
                ++mipUse->prims;
        }

        void resolve(uint32_t attrs)
        {
            const uint32_t ctxt = (attrs >> 9) & 1u;
            const bool tme = (attrs >> 4) & 1u, abe = (attrs >> 6) & 1u;
            StateKey key;
            key.path = static_cast<uint8_t>(path);
            key.prim = static_cast<uint16_t>(attrs);
            key.alpha = abe ? regs[0x42 + ctxt] : 0u;
            key.pabe = abe ? static_cast<uint8_t>(regs[0x49] & 1u) : 0u;
            key.test = regs[0x47 + ctxt] & 0xFFFFu; // the alpha test and DATE; the depth test is not this step's
            const CachedTexture *tex = nullptr;
            mipUse = nullptr;
            if (tme)
            {
                const Tex0 t = Tex0::unpack(regs[0x06 + ctxt]);
                const Texa texa = Texa::unpack(regs[0x3B]);
                const CachedTexture &e = cache.get(t, texa);
                tex = &e;
                if (!e.valid)
                    ++undecodable;
                const uint64_t tex1 = regs[0x14 + ctxt];
                const uint32_t mxl = static_cast<uint32_t>((tex1 >> 2) & 7u);
                if (mxl)
                {
                    MipUse &m = mips[tex];
                    m.mxl = mxl;
                    m.tex1 = tex1;
                    m.miptbp1 = regs[0x34 + ctxt];
                    m.miptbp2 = regs[0x36 + ctxt];
                    for (uint32_t level = 1; level <= mxl && level <= 6u; ++level)
                        cache.get(t, mipLevel(t, m.miptbp1, m.miptbp2, level), texa);
                    mipUse = &m;
                }
            }
            combo = &combos[{key, tex}];
            comboTex = tex;
            texUse = tex ? &uses[tex] : nullptr;
            comboDirty = false;
        }

        void feedPath(int p, const uint8_t *data, uint32_t bytes)
        {
            path = p;
            comboDirty = true;
            pathWalker[p & 3].feed(data, bytes, *this);
        }

        // ---- checks ----
        void plantSwizzle(uint32_t bits)
        {
            LayoutTables &t = layoutTablesForTests();
            switch (bits)
            {
            case 32: std::swap(t.column32[0][0], t.column32[0][1]); break;
            case 16: std::swap(t.column16[0][0], t.column16[0][1]); break;
            case 4: std::swap(t.column4[0][0], t.column4[0][1]); break;
            default: bits = 8; std::swap(t.column8[0][0], t.column8[0][1]); break;
            }
            faultDone = true;
            std::printf("fault planted: the %u-bit column table's first two entries swapped (cycle %" PRIu64 ")\n", bits, cycle);
        }

        // The texture area against another image of GS memory. Returns the number of differing bytes.
        uint64_t compareArea(const uint8_t *other, uint32_t &firstAt, uint32_t &blocks) const
        {
            const uint8_t *mine = shadow.memory().data();
            firstAt = 0;
            blocks = 0;
            if (std::memcmp(mine + kTexFirstByte, other + kTexFirstByte, kGsBytes - kTexFirstByte) == 0)
                return 0;
            uint64_t diff = 0;
            for (uint32_t b = GsMemory::kTextureFirstBlock; b < kGsBlocks; ++b)
            {
                const uint8_t *p = mine + b * kGsBlockBytes, *q = other + b * kGsBlockBytes;
                if (std::memcmp(p, q, kGsBlockBytes) == 0)
                    continue;
                ++blocks;
                for (uint32_t i = 0; i < kGsBlockBytes; ++i)
                    if (p[i] != q[i] && diff++ == 0)
                        firstAt = b * kGsBlockBytes + i;
            }
            return diff;
        }

        void sampleCheck();
        void report() const;
        void writePngs() const;
    };

    TexCheck::TexCheck(TexOptions options, PS2Runtime *runtime) : m(new Impl)
    {
        m->opt = std::move(options);
        m->rt = runtime;
    }
    TexCheck::~TexCheck() = default;

    bool TexCheck::failed() const { return m->failures != 0; }

    void TexCheck::open(const FileInfo &info)
    {
        m->header = info.header;
        m->exact = (info.header.flags & kFileExact) != 0;
        // PATH3's other register writes go to the scan in order with the transfers. Alone, the 2D packet's textures are
        // all the scan can see; behind emu, PATH1's packets come through gifPacket().
        m->shadow.setObserver(m.get());
        if (m->opt.faultKind == "swizzle" && m->opt.faultCycle <= 0)
            m->plantSwizzle(m->opt.faultN);
    }

    void TexCheck::keyframe(const Keyframe &kf)
    {
        Impl &s = *m;
        const StateChunk *vram = kf.find(kStateGsMemory);
        const bool final = (kf.header.flags & kKeyFinal) != 0, afterLoad = (kf.header.flags & kKeyAfterStateLoad) != 0;
        const bool vramExact = (kf.header.flags & kKeyVramExact) != 0;
        if (s.started && !afterLoad && vram && vram->bytes == kGsBytes)
        {
            uint32_t firstAt = 0, blocks = 0;
            const uint64_t diff = s.compareArea(vram->data, firstAt, blocks);
            // The transfer registers as the keyframe's register file has them.
            uint32_t regDiff = 0;
            if (const StateChunk *regs = kf.find(kStateGsRegs); regs && regs->bytes >= kGsRegCount * 8u && s.exact)
            {
                uint64_t r[4];
                std::memcpy(r, regs->data + kRegBitbltbuf * 8u, sizeof(r));
                const GsMemory &mem = s.shadow.memory();
                regDiff = (r[0] != mem.bitbltbuf()) + (r[1] != mem.trxpos()) + (r[2] != mem.trxreg()) + (r[3] != mem.trxdir());
            }
            const bool ok = diff == 0 && regDiff == 0;
            if (vramExact)
            {
                (final ? s.finalChecked : s.keyChecked)++;
                if (ok)
                    (final ? s.finalOk : s.keyOk)++;
            }
            else
            {
                ++s.keyInexact;
                s.keyInexactOk += ok;
            }
            if (!ok)
            {
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                              "%s keyframe at cycle %" PRIu64 "%s: the shadow differs from GSVR in %" PRIu64 " bytes of %u blocks of texture memory "
                              "(first at 0x%06x, block 0x%04x: shadow %02x, GSVR %02x); %u transfer registers differ",
                              final ? "final" : "later", kf.header.cycle, vramExact ? "" : " (GSVR not exact: a GPU backend's image)", diff, blocks,
                              firstAt, firstAt >> 8, s.shadow.memory().data()[firstAt], vram->data[firstAt], regDiff);
                s.fail(buf);
            }
            if (!ok && !final)
            {
                // Start the next segment clean, so each is judged on its own.
                s.shadow.loadKeyframe(kf);
            }
            return;
        }
        if (final)
            return;
        if (!s.shadow.loadKeyframe(kf))
        {
            s.fail("keyframe at cycle " + std::to_string(kf.header.cycle) + " has no GS memory");
            return;
        }
        // The scan's register file starts as the GS's was.
        if (const StateChunk *regs = kf.find(kStateGsRegs); regs && regs->bytes >= sizeof(s.regs))
            std::memcpy(s.regs, regs->data, sizeof(s.regs));
        s.comboDirty = true;
        s.vertexCount = 0;
        for (GifWalker &w : s.pathWalker)
            w.reset();
        s.started = true;
        ++s.keyLoaded;
        if (afterLoad)
            std::printf("keyframe at cycle %" PRIu64 " follows a save-state load: loaded, not compared\n", kf.header.cycle);
    }

    void TexCheck::cycleBegin(uint64_t cycle)
    {
        Impl &s = *m;
        s.cycle = cycle;
        s.shadowAtCycle = s.shadow.stats();
        s.memAtCycle = s.shadow.memory().stats();
        s.cacheAtCycle = s.cache.stats();
        if (!s.faultDone && s.opt.faultKind == "swizzle" && static_cast<int64_t>(cycle) >= s.opt.faultCycle)
            s.plantSwizzle(s.opt.faultN);
        if (!s.faultDone && s.opt.faultKind == "transfer" && static_cast<int64_t>(cycle) == s.opt.faultCycle)
        {
            s.shadow.memory().dropTransferForTests(s.shadow.memory().stats().transfers + s.opt.faultN + 1u);
            s.faultDone = true;
            std::printf("fault planted: transfer %u of cycle %" PRIu64 " loses its data\n", s.opt.faultN, cycle);
        }
    }

    void TexCheck::tag(const TagView &t)
    {
        if (t.kind == rc::gfx::KindScene && t.payloadBytes >= sizeof(rc::gfx::SceneTag))
        {
            rc::gfx::SceneTag scene;
            std::memcpy(&scene, t.payload, sizeof(scene));
            m->scenes.insert(scene.scene);
        }
    }

    void TexCheck::gifPacket(int path, const uint8_t *data, uint32_t bytes)
    {
        // PATH3 comes from the shadow's own walk (in order with its transfers); the other paths from the emulated VIF1.
        if (path == 3 || !data || !bytes)
            return;
        m->pending.insert(m->pending.end(), data, data + bytes);
        m->pendingPackets.push_back({path, bytes});
    }

    void TexCheck::kick(const KickView &k, const RamView &ram)
    {
        Impl &s = *m;
        if (!s.started)
            return;
        const GsMemory &mem = s.shadow.memory();
        if (s.opt.verify && s.exact && k.regs)
        {
            // The transfer registers before the kick. The shadow follows PATH3 only: a difference means a transfer
            // register was written on another path, or a PATH3 write was missed.
            ++s.regsChecked;
            const uint64_t *r = k.regs + kRegBitbltbuf;
            if (r[0] == mem.bitbltbuf() && r[1] == mem.trxpos() && r[2] == mem.trxreg() && r[3] == mem.trxdir())
                ++s.regsOk;
            else if (s.regsChecked - s.regsOk > 3)
            {
                ++s.failures;
                s.shadow.memory().setTransferRegisters(r[0], r[1], r[2], r[3]);
            }
            else
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "cycle %" PRIu64 " kick %u: transfer registers before the kick: recorded %016" PRIx64 " %016" PRIx64 " %016" PRIx64
                              " %" PRIx64 ", shadow %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %" PRIx64,
                              k.cycle, k.index, r[0], r[1], r[2], r[3], mem.bitbltbuf(), mem.trxpos(), mem.trxreg(), mem.trxdir());
                s.fail(buf);
                s.shadow.memory().setTransferRegisters(r[0], r[1], r[2], r[3]);
            }
        }
        // The emulated machine's PATH1 output for this kick (it ran just before this call).
        size_t at = 0;
        for (const auto &[p, bytes] : s.pendingPackets)
        {
            s.feedPath(p, s.pending.data() + at, bytes);
            at += bytes;
        }
        s.pending.clear();
        s.pendingPackets.clear();

        if (!s.faultDone && s.opt.faultKind == "kick" && static_cast<int64_t>(k.cycle) == s.opt.faultCycle && k.index == s.opt.faultN)
        {
            s.faultDone = true;
            std::printf("fault planted: the shadow skips cycle %" PRIu64 " kick %u (ch%u, %u bytes)\n", k.cycle, k.index, k.channel, k.streamBytes);
        }
        else
        {
            s.path = 3;
            s.comboDirty = true;
            s.shadow.kick(k, ram);
        }
        if (s.rt && s.opt.verify)
        {
            // Behind the emu consumer: texture memory after every kick, PATH1's included (draws must not write it).
            ++s.kickCompared;
            uint32_t firstAt = 0, blocks = 0;
            const uint64_t diff = s.compareArea(s.rt->memory().getGSVRAM(), firstAt, blocks);
            if (diff == 0)
                ++s.kickEqual;
            else if (s.kickCompared - s.kickEqual <= 3)
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "cycle %" PRIu64 " kick %u (ch%u): the shadow differs from the emulated GS's memory in %" PRIu64 " bytes of %u blocks "
                              "(first at 0x%06x, block 0x%04x)",
                              k.cycle, k.index, k.channel, diff, blocks, firstAt, firstAt >> 8);
                s.fail(buf);
            }
            else
                ++s.failures;
        }
    }

    void TexCheck::cycleEnd(const CycleInfo &info, const uint8_t *)
    {
        Impl &s = *m;
        if (!s.started)
            return;
        Impl::CycleCost c;
        const TexShadow::Stats &sh = s.shadow.stats();
        const GsMemory::Stats &ms = s.shadow.memory().stats();
        c.applyNs = sh.applyNs - s.shadowAtCycle.applyNs;
        c.bytes = ms.bytes - s.memAtCycle.bytes;
        c.transfers = ms.transfers - s.memAtCycle.transfers;
        c.blocks = ms.blockMarks - s.memAtCycle.blockMarks;
        c.changing = ms.transfersChanging - s.memAtCycle.transfersChanging;
        // Textures are decoded when a draw names them (the cache validates at each TEX0 a draw follows).
        const TextureCache::Stats &cs = s.cache.stats();
        c.redecoded = cs.redecodes - s.cacheAtCycle.redecodes;
        c.first = (cs.decodes - s.cacheAtCycle.decodes) - c.redecoded;
        c.texels = cs.texels - s.cacheAtCycle.texels;
        c.decodeNs = cs.decodeNs - s.cacheAtCycle.decodeNs;
        s.costs.push_back(c);
        if (!s.opt.quiet)
            std::printf("cycle %" PRIu64 ": %" PRIu64 " transfers (%" PRIu64 " changing memory), %" PRIu64 " bytes, %" PRIu64 " blocks changed, %.3f ms; of %zu "
                        "textures %" PRIu64 " new and %" PRIu64 " decoded again (%" PRIu64 " texels in %.3f ms)\n",
                        info.end.cycle, c.transfers, c.changing, c.bytes, c.blocks, c.applyNs / 1e6, s.cache.size(), c.first, c.redecoded, c.texels,
                        c.decodeNs / 1e6);
    }

    void TexCheck::Impl::sampleCheck()
    {
        // Each decoded texture against the emulated GS's own sampling: a point-sampled DECAL sprite of the texture drawn
        // one texel to a pixel into the emulated frame buffer at FBP 0, read back through the CT32 layout. The emulated
        // machine is not used again after this.
        if (opt.faultKind == "decode" && !faultDone)
            plantSwizzle(opt.faultN);
        const uint64_t t0 = nowNs();
        cache.decodeAll();
        fullDecodeNs = nowNs() - t0;
        std::vector<const CachedTexture *> entries;
        cache.forEach([&](const CachedTexture &e) { entries.push_back(&e); });
        fullDecodeTextures = entries.size();
        for (const CachedTexture *e : entries)
            fullDecodeTexels += static_cast<uint64_t>(e->key.width) * e->key.height;
        if (!rt || !opt.verify)
            return;
        std::sort(entries.begin(), entries.end(), [](const CachedTexture *a, const CachedTexture *b)
                  { return a->key.tbp != b->key.tbp ? a->key.tbp < b->key.tbp : a->key.width < b->key.width; });
        GS &gs = rt->gs();
        const uint8_t *vram = rt->memory().getGSVRAM();
        const LayoutTables &lt = layoutTables();
        for (const CachedTexture *e : entries)
        {
            const uint32_t w = e->key.width, h = e->key.height;
            if (!e->valid || w > 512u || h > 256u || (w & (w - 1u)) || (h & (h - 1u)))
            {
                ++sampledSkipped;
                continue;
            }
            Tex0 t = e->tex0;
            t.tbp0 = e->key.tbp;
            t.tbw = e->key.tbw;
            t.tw = t.th = 0;
            while ((1u << t.tw) < w)
                ++t.tw;
            while ((1u << t.th) < h)
                ++t.th;
            t.tcc = 1;
            t.tfx = 1; // DECAL: the texel's colour and alpha pass through unchanged
            t.cld = 1;
            const uint32_t texa = e->key.texa & 0x7FFFFFFFu;
            std::vector<uint64_t> p;
            auto ad = [&](uint8_t r, uint64_t v)
            {
                p.push_back(v);
                p.push_back(r);
            };
            p.push_back(0);
            p.push_back(0xE);
            ad(0x1A, 1);                                  // PRMODECONT
            ad(0x4C, 0xA0000);                            // FRAME_1: FBP 0, FBW 10, CT32, no mask
            ad(0x4E, 0x1010000A0ull);                     // ZBUF_1: masked
            ad(0x18, 0);                                  // XYOFFSET_1
            ad(0x40, 639ull << 16 | 255ull << 48);        // SCISSOR_1
            ad(0x47, 0x30000);                            // TEST_1: no alpha test, depth always
            ad(0x49, 0);                                  // PABE
            ad(0x4A, 0);                                  // FBA_1
            ad(0x46, 1);                                  // COLCLAMP
            ad(0x45, 0);                                  // DTHE
            ad(0x3B, (texa & 0xFFu) | static_cast<uint64_t>((texa >> 8) & 1u) << 15 | static_cast<uint64_t>((texa >> 16) & 0xFFu) << 32); // TEXA
            ad(0x14, 1);                                  // TEX1_1: level 0, nearest
            ad(0x08, 5);                                  // CLAMP_1
            ad(0x3F, 0);                                  // TEXFLUSH
            ad(0x06, t.pack());                           // TEX0_1 (loads the CLUT)
            ad(0x00, 0x116);                              // PRIM: sprite, textured, UV
            ad(0x01, 0x3F80000080808080ull);              // RGBAQ
            ad(0x03, 8u | 8u << 16);                      // UV: the first texel's centre
            ad(0x05, 0);                                  // XYZ2
            ad(0x03, (w * 16u + 8u) | static_cast<uint64_t>(h * 16u + 8u) << 16);
            ad(0x05, (w * 16u) | static_cast<uint64_t>(h * 16u) << 16);
            p[0] = (p.size() / 2u - 1u) | 0x8000u | 1ull << 60;
            gs.processGIFPacket(reinterpret_cast<const uint8_t *>(p.data()), static_cast<uint32_t>(p.size() * 8u));
            uint64_t diff = 0;
            uint32_t fx = 0, fy = 0, mineAt = 0, gsAt = 0;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    uint32_t a, b;
                    std::memcpy(&a, e->rgba.data() + (static_cast<size_t>(y) * w + x) * 4u, 4);
                    std::memcpy(&b, vram + (gsAddr32(lt, 0, 10, x, y) & (kGsBytes - 1u)), 4);
                    if (a != b && diff++ == 0)
                        fx = x, fy = y, mineAt = a, gsAt = b;
                }
            ++sampled;
            sampledTexels += static_cast<uint64_t>(w) * h;
            if (diff == 0)
                ++sampledOk;
            else
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "texture %s %ux%u at 0x%04x (TBW %u, CBP 0x%04x): %" PRIu64 " texels differ from the emulated GS's sampling (first at %u,%u: "
                              "decoded %08x, sampled %08x)",
                              psmName(e->key.psm), w, h, e->key.tbp, e->key.tbw, e->key.cbp, diff, fx, fy, mineAt, gsAt);
                fail(buf);
            }
        }
    }

    void TexCheck::Impl::writePngs() const
    {
        uint32_t n = 0;
        cache.forEach([&](const CachedTexture &e)
                      {
                          if (!e.valid)
                              return;
                          char name[160];
                          std::snprintf(name, sizeof(name), "%s/tex_%04x_%s_%ux%u_c%04x.png", opt.pngDir.c_str(), e.key.tbp, psmName(e.key.psm), e.key.width,
                                        e.key.height, e.key.cbp);
                          n += writePngRgba(name, e.rgba.data(), e.key.width, e.key.height);
                      });
        std::printf("wrote %u texture PNGs to %s\n", n, opt.pngDir.c_str());
    }

    void TexCheck::Impl::report() const
    {
        if (combos.empty())
            return;
        // Textures, as the draws saw them.
        std::map<const CachedTexture *, std::set<std::string>> statesOf;
        for (const auto &[key, c] : combos)
            if (key.second && c.prims)
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "P%u ALPHA 0x%" PRIx64 "%s PABE %u TEST %s", key.first.path, key.first.alpha & 0xFFu,
                              (key.first.prim & 0x40u) ? "" : " (ABE off)", key.first.pabe, testText(key.first.test).c_str());
                statesOf[key.second].insert(buf);
            }
        uint64_t byFormat[64] = {}, aboveByFormat[64] = {}, texAbove = 0, texAt80Only = 0, texZeroOnly = 0, texDrawn = 0;
        std::vector<const CachedTexture *> above;
        for (const auto &[e, u] : uses)
        {
            if (!u.prims)
                continue;
            ++texDrawn;
            ++byFormat[e->key.psm & 63u];
            if (u.alphaMax > 0x80u)
            {
                ++texAbove;
                ++aboveByFormat[e->key.psm & 63u];
                above.push_back(e);
            }
            else if (u.alphaMax == 0u)
                ++texZeroOnly;
            else if (u.alphaMin == 0x80u)
                ++texAt80Only;
        }
        std::printf("\ntextures drawn with: %" PRIu64 " (cache entries with their mip levels: %zu)\n", texDrawn, cache.size());
        for (uint32_t f = 0; f < 64; ++f)
            if (byFormat[f])
                std::printf("  %-5s %4" PRIu64 " textures, %" PRIu64 " with a texel alpha above 0x80\n", psmName(static_cast<uint8_t>(f)), byFormat[f],
                            aboveByFormat[f]);
        std::printf("  alpha all zero: %" PRIu64 "; all 0x80: %" PRIu64 "; some texel above 0x80: %" PRIu64 "\n", texZeroOnly, texAt80Only, texAbove);
        std::sort(above.begin(), above.end(), [](const CachedTexture *a, const CachedTexture *b) { return a->key.tbp < b->key.tbp; });
        for (const CachedTexture *e : above)
        {
            const TexUse &u = uses.at(e);
            std::printf("  above 0x80: %-4s %3ux%-3u TBP 0x%04x CBP 0x%04x: %u of %u texels (alpha %02x..%02x), %" PRIu64 " primitives:", psmName(e->key.psm),
                        e->key.width, e->key.height, e->key.tbp, e->key.cbp, u.above80, static_cast<uint32_t>(e->key.width) * e->key.height, u.alphaMin,
                        u.alphaMax, u.prims);
            for (const std::string &st : statesOf[e])
                std::printf(" [%s]", st.c_str());
            std::printf("\n");
        }
        for (const auto &[e, mu] : mips)
            std::printf("  mipmapped: %-4s %ux%u TBP 0x%04x, MXL %u, TEX1 0x%" PRIx64 ", MIPTBP1 0x%" PRIx64 " MIPTBP2 0x%" PRIx64 ", %" PRIu64 " primitives\n",
                        psmName(e->key.psm), e->key.width, e->key.height, e->key.tbp, mu.mxl, mu.tex1, mu.miptbp1, mu.miptbp2, mu.prims);

        // Draw states: how many primitives can put a source alpha at or above 0x80 on the colour output.
        struct Row
        {
            uint64_t prims = 0, vertexAbove = 0, texAbove = 0, reachGe = 0, reachGt = 0;
            uint32_t asMax = 0;
        };
        std::map<StateKey, Row> rows;
        Row pabe;
        for (const auto &[key, c] : combos)
        {
            const StateKey &st = key.first;
            const CachedTexture *e = key.second;
            Row &r = rows[st];
            // The largest alpha below which nothing at or above 0x80 (0x81) reaches the colour output.
            bool reach[256];
            for (uint32_t a = 0; a < 256; ++a)
                reach[a] = reachesColour(st.test, a);
            const uint32_t tfx = e ? e->tex0.tfx : 0u, tcc = e ? e->tex0.tcc : 0u;
            for (uint32_t av = 0; av < 256; ++av)
            {
                const uint64_t n = c.byVertexAlpha[av];
                if (!n)
                    continue;
                uint32_t asMax = av;
                if (e && tcc)
                {
                    const uint32_t at = c.texAlphaMax;
                    asMax = tfx == 0u ? std::min(255u, (at * av) >> 7) : tfx == 2u ? std::min(255u, at + av) : at;
                }
                r.prims += n;
                r.asMax = std::max(r.asMax, asMax);
                if (av > 0x80u)
                    r.vertexAbove += n;
                if (e && c.texAlphaMax > 0x80u)
                    r.texAbove += n;
                bool ge = false, gt = false;
                for (uint32_t a = 0x80; a <= asMax; ++a)
                    if (reach[a])
                    {
                        ge = true;
                        gt = gt || a > 0x80u;
                    }
                r.reachGe += ge ? n : 0u;
                r.reachGt += gt ? n : 0u;
            }
        }
        std::printf("\ndraw states (primitives; 'As >= 0x80 out': primitives whose largest possible source alpha, from the texture's largest\n"
                    "alpha and the vertices', is at or above 0x80 at a pixel the alpha test lets reach the colour output):\n");
        std::printf("  %-4s %-5s %-10s %-4s %-16s %12s %12s %12s %8s %14s %14s\n", "path", "PRIM", "ALPHA", "PABE", "alpha test", "primitives", "vertex A>80",
                    "texture A>80", "max As", "As>=0x80 out", "As>0x80 out");
        for (const auto &[st, r] : rows)
        {
            char alpha[24];
            if (st.prim & 0x40u)
                std::snprintf(alpha, sizeof(alpha), "0x%02x/%02x", static_cast<unsigned>(st.alpha & 0xFFu), static_cast<unsigned>((st.alpha >> 32) & 0xFFu));
            else
                std::snprintf(alpha, sizeof(alpha), "-");
            std::printf("  %-4u 0x%03x %-10s %-4u %-16s %12" PRIu64 " %12" PRIu64 " %12" PRIu64 "     0x%02x %14" PRIu64 " %14" PRIu64 "\n", st.path, st.prim, alpha,
                        st.pabe, testText(st.test).c_str(), r.prims, r.vertexAbove, r.texAbove, r.asMax, r.reachGe, r.reachGt);
            if (st.pabe)
            {
                pabe.prims += r.prims;
                pabe.vertexAbove += r.vertexAbove;
                pabe.texAbove += r.texAbove;
                pabe.reachGe += r.reachGe;
                pabe.reachGt += r.reachGt;
            }
        }
        std::string sceneList;
        for (const uint32_t sc : scenes)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%s%04x", sceneList.empty() ? "" : ",", sc);
            sceneList += buf;
        }
        std::printf("TEXSUM %s scenes=%s draws=%" PRIu64 " textures=%zu texAbove80=%" PRIu64 " pabePrims=%" PRIu64 " pabeVertexAbove80=%" PRIu64
                    " pabeTexAbove80=%" PRIu64 " pabeAsGe80Out=%" PRIu64 " pabeAsGt80Out=%" PRIu64 " mips=%zu\n",
                    header.gameVersion ? "eu" : "usa", sceneList.empty() ? "-" : sceneList.c_str(), draws, static_cast<size_t>(texDrawn), texAbove, pabe.prims,
                    pabe.vertexAbove, pabe.texAbove, pabe.reachGe, pabe.reachGt, mips.size());
    }

    void TexCheck::close()
    {
        Impl &s = *m;
        if (!s.started)
            return;
        s.sampleCheck();
        if (!s.opt.pngDir.empty())
            s.writePngs();
        s.report();
    }

    void TexCheck::summary() const
    {
        const Impl &s = *m;
        const GsMemory::Stats &ms = s.shadow.memory().stats();
        const GifWalker::Stats &ws = s.shadow.walker().stats();
        const TexShadow::Stats &sh = s.shadow.stats();
        std::printf("\ntex: %zu cycles, %" PRIu64 " PATH3 kicks (%.1f MB), %" PRIu64 " GIF tags (%" PRIu64 " image), %" PRIu64 " transfers, %.2f MB of "
                    "pixel data; %" PRIu64 " transfers changed %" PRIu64 " pixels in %" PRIu64 " blocks; %" PRIu64 " keyframes loaded\n",
                    s.costs.size(), sh.kicks, sh.streamBytes / 1048576.0, ws.tags, ws.image, ms.transfers, ms.bytes / 1048576.0, ms.transfersChanging,
                    ms.pixelsChanged, ms.blockMarks, s.keyLoaded);
        std::printf("  transfers by format:");
        for (uint32_t f = 0; f < 64; ++f)
            if (ms.byPsm[f])
                std::printf(" %s %" PRIu64 " (%.2f MB)", psmName(static_cast<uint8_t>(f)), ms.byPsm[f], ms.bytesByPsm[f] / 1048576.0);
        std::printf("; into frame memory %" PRIu64 "\n", ms.intoFrame);
        std::printf("  odd: unsupported %" PRIu64 ", local->host %" PRIu64 ", local->local %" PRIu64 ", unfinished %" PRIu64 ", image data with no transfer "
                    "%" PRIu64 " bytes, dropped packet bytes %" PRIu64 ", PRE outside PACKED %" PRIu64 ", absent pages %" PRIu64 ", untranslatable %" PRIu64 "\n",
                    ms.unsupported, ms.localToHost, ms.localToLocal, ms.unfinished, ms.excessBytes, ws.droppedBytes, ws.preOutsidePacked, sh.absentPages,
                    sh.untranslatable);
        if (s.draws)
            std::printf("  scan: %" PRIu64 " primitives, %zu state and texture pairs, %" PRIu64 " TEX2 writes, %" PRIu64 " vertices without a kick, %" PRIu64
                        " draws with a texture format the decoder lacks; counted the GS survey's way, %" PRIu64 " primitives have a vertex alpha "
                        "above 0x80 (%" PRIu64 " in PABE states)\n",
                        s.draws, s.combos.size(), s.tex2Writes, s.noKickVertices, s.undecodable, s.surveyHigh, s.surveyHighPabe);
        // Cost.
        if (!s.costs.empty())
        {
            auto column = [&](uint64_t Impl::CycleCost::*f)
            {
                std::vector<uint64_t> v;
                for (const Impl::CycleCost &c : s.costs)
                    v.push_back(c.*f);
                std::sort(v.begin(), v.end());
                return v;
            };
            auto line = [&](const char *name, uint64_t Impl::CycleCost::*f, double scale, const char *unit)
            {
                const std::vector<uint64_t> v = column(f);
                uint64_t sum = 0;
                for (uint64_t x : v)
                    sum += x;
                std::printf("  %-28s median %9.3f  99%% %9.3f  max %9.3f  total %10.3f %s\n", name, v[v.size() / 2] * scale, v[v.size() * 99 / 100] * scale,
                            v.back() * scale, sum * scale, unit);
            };
            std::printf("  per cycle:\n");
            line("transfers", &Impl::CycleCost::transfers, 1.0, "");
            line("pixel data", &Impl::CycleCost::bytes, 1.0 / 1024.0, "KB");
            line("transfers changing memory", &Impl::CycleCost::changing, 1.0, "");
            line("blocks changed", &Impl::CycleCost::blocks, 1.0, "");
            line("walk and apply", &Impl::CycleCost::applyNs, 1e-6, "ms");
            line("textures decoded first", &Impl::CycleCost::first, 1.0, "");
            line("textures decoded again", &Impl::CycleCost::redecoded, 1.0, "");
            line("texels decoded", &Impl::CycleCost::texels, 1.0, "");
            line("decode", &Impl::CycleCost::decodeNs, 1e-6, "ms");
            std::printf("  full decode of the %" PRIu64 " cached textures (%" PRIu64 " texels, %.2f MB of RGBA): %.3f ms\n", s.fullDecodeTextures,
                        s.fullDecodeTexels, s.fullDecodeTexels * 4.0 / 1048576.0, s.fullDecodeNs / 1e6);
        }
        if (s.opt.verify)
        {
            std::printf("verify: texture memory (blocks 0x1e00 up, %u bytes) at later keyframes %" PRIu64 "/%" PRIu64 ", final %" PRIu64 "/%" PRIu64, kGsBytes - kTexFirstByte,
                        s.keyOk, s.keyChecked, s.finalOk, s.finalChecked);
            if (s.keyInexact)
                std::printf("; against inexact GSVR (a GPU backend's image) %" PRIu64 "/%" PRIu64, s.keyInexactOk, s.keyInexact);
            std::printf("; transfer registers before kicks %" PRIu64 "/%" PRIu64, s.regsOk, s.regsChecked);
            if (s.rt)
                std::printf("; against the emulated GS after every kick %" PRIu64 "/%" PRIu64 "; textures against the emulated GS's sampling %" PRIu64
                            "/%" PRIu64 " (%" PRIu64 " texels; %" PRIu64 " skipped)",
                            s.kickEqual, s.kickCompared, s.sampledOk, s.sampled, s.sampledTexels, s.sampledSkipped);
            std::printf("\n");
        }
    }
}
