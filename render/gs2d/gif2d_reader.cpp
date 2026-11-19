#include "gif2d_reader.h"

#include "../gfx/tags.h"
#include "../record/chain_walk.h"

#include <algorithm>
#include <cstring>

namespace rc::rd
{
    using namespace rc::rec;

    bool path1Writes(uint8_t addr)
    {
        // Measured on the 81 dumps of step 7 batch 1 (RENDERER_2D_DESIGN.md section 14): the registers whose value
        // differs before and after a VIF1 kick are PRIM, RGBAQ, ST, XYZ2, XYZ3, TEX0_1, TEX1_1, TEXA, SCISSOR_1,
        // ALPHA_1, TEST_1, PABE and ZBUF_1. The other vertex registers (UV, XYZF2, XYZF3, FOG) are added: they are
        // vertex data like the measured ones. Check (a) compares every other register after a VIF1 kick, so a scene
        // in which PATH1 changes one more fails there.
        switch (addr)
        {
        case kGsPrim: case kGsRgbaq: case kGsSt: case kGsUv: case kGsXyzf2: case kGsXyz2: case kGsXyzf3: case kGsXyz3: case kGsFog:
        case kGsTex0: case kGsTex1: case kGsTexa: case kGsScissor: case kGsAlpha: case kGsTest: case kGsPabe: case kGsZbuf:
            return true;
        default:
            return false;
        }
    }

    namespace
    {
        // The defaults of RENDERER_2D_DESIGN.md 3.1 for a register a draw needs and the reader does not know.
        bool defaultOf(uint8_t addr, uint64_t &v)
        {
            switch (addr)
            {
            case kGsZbuf: case kGsZbuf + 1: v = 0x10000A0u; return true;
            case kGsPabe: v = 0; return true;
            case kGsFba: case kGsFba + 1: v = 0; return true;
            case kGsColclamp: v = 1; return true;
            case kGsDthe: v = 0; return true;
            case kGsPrmodecont: v = 1; return true;
            case kGsTex1: case kGsTex1 + 1: v = 0x60u; return true;
            case kGsClamp: case kGsClamp + 1: v = 5u; return true;
            case kGsTexa: v = 0x80u; return true;
            default: return false;
            }
        }
        bool isVertexReg(uint8_t a) { return a == kGsXyz2 || a == kGsXyzf2 || a == kGsXyz3 || a == kGsXyzf3; }
        bool isLatchReg(uint8_t a) { return a == kGsRgbaq || a == kGsSt || a == kGsUv || a == kGsFog; }
        bool usesTexa(const Tex0 &t)
        {
            if (t.psm == kPsmCt24 || t.psm == kPsmCt16 || t.psm == kPsmCt16s)
                return true;
            return t.indexed() && (t.cpsm == kPsmCt16 || t.cpsm == kPsmCt16s);
        }
        // Two triangles whose six vertices are the four corners of an axis-aligned rectangle.
        bool rectanglePair(const GsVertex *a, const GsVertex *b)
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
            // Each triangle has three distinct corners.
            for (const GsVertex *t : {a, b})
                for (int i = 0; i < 3; ++i)
                    for (int j = i + 1; j < 3; ++j)
                        if (t[i].x == t[j].x && t[i].y == t[j].y)
                            return false;
            return true;
        }
    }

    Gif2dReader::Gif2dReader() : Gif2dReader(Options{}) {}

    Gif2dReader::Gif2dReader(Options options) : m_opt(options), m_cache(m_shadow.memory())
    {
        m_shadow.setObserver(this);
    }

    void Gif2dReader::open(const FileInfo &) {}

    void Gif2dReader::keyframe(const Keyframe &kf)
    {
        // The first keyframe, and one after a save-state load, start the shadow; later ones hold the same texture memory.
        if (m_started && !(kf.header.flags & kKeyAfterStateLoad))
            return;
        if (!m_shadow.loadKeyframe(kf))
            return;
        std::memset(m_prov, 0, sizeof(m_prov));
        if (const StateChunk *regs = kf.find(kStateGsRegs); m_opt.seed && regs && regs->bytes >= kGsRegFile * 8u)
        {
            std::memcpy(m_regs, regs->data, kGsRegFile * 8u);
            for (uint32_t a = 0; a < kGsRegFile; ++a)
                if (a != kGsHwreg)
                    m_prov[a] = kProvSeeded;
        }
        m_queued = 0;
        m_stateDirty = true;
        m_started = true;
        ++m_stats.keyframesLoaded;
    }

    void Gif2dReader::cycleBegin(uint64_t cycle)
    {
        m_list.clear();
        m_cycle = cycle;
        m_cycleRegWrites = 0;
        m_primsOpen = false;
        m_haveListState = false;
        m_cyclePrims = 0;
        m_kickFirstCmd = 0;
    }

    void Gif2dReader::cycleEnd(const CycleInfo &, const uint8_t *)
    {
        closePrims();
        ++m_stats.cycles;
    }

    void Gif2dReader::feed(const uint8_t *data, uint32_t bytes)
    {
        if (bytes)
            m_shadow.path3(data, bytes);
    }

    void Gif2dReader::startBlank()
    {
        std::memset(m_prov, 0, sizeof(m_prov));
        m_queued = 0;
        m_stateDirty = true;
        m_started = true;
    }

    void Gif2dReader::feedPath3(const uint8_t *data, uint32_t bytes)
    {
        if (!m_started)
            return;
        closePrims();
        m_kickFirstCmd = static_cast<uint32_t>(m_list.cmds.size());
        m_kickPrims = m_kickDropped = m_kickTransfers = 0;
        ++m_stats.kicks;
        feed(data, bytes);
        closePrims();
    }

    void Gif2dReader::kick(const KickView &k, const RamView &ram)
    {
        if (!m_started)
            return;
        closePrims();
        m_kickFirstCmd = static_cast<uint32_t>(m_list.cmds.size());
        m_kickPrims = m_kickDropped = m_kickTransfers = 0;
        if (k.channel == 1u)
        {
            // PATH1 (step 8 reads it): the registers it writes are no longer known.
            ++m_stats.path1Kicks;
            for (uint32_t a = 0; a < kGsRegFile; ++a)
                if (path1Writes(static_cast<uint8_t>(a)))
                    m_prov[a] = kProvUnknown;
            m_queued = 0;
            m_stateDirty = true;
            return;
        }
        if (k.channel != 2u)
            return;
        ++m_stats.kicks;
        if (m_opt.seed && k.regs)
            for (uint32_t a = 0; a < kGsRegFile; ++a)
                if (m_prov[a] == kProvUnknown && a != kGsHwreg)
                {
                    m_regs[a] = k.regs[a];
                    m_prov[a] = kProvSeeded;
                    m_stateDirty = true;
                }
        const GsMemory &mem = m_shadow.memory();
        const uint64_t transfersBefore = mem.stats().transfers, imageBefore = m_shadow.walker().stats().imageBytes;
        const uint64_t droppedBefore = m_shadow.walker().stats().droppedBytes;
        const uint64_t trx[4] = {mem.bitbltbuf(), mem.trxpos(), mem.trxreg(), mem.trxdir()};

        // Spans in stream order; each becomes a scope command where the walk passes its offset.
        std::vector<uint32_t> order(k.spanCount);
        for (uint32_t i = 0; i < k.spanCount; ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&k](uint32_t a, uint32_t b) { return k.spans[a].streamOffset < k.spans[b].streamOffset; });
        struct Feed final : WalkVisitor
        {
            Gif2dReader *self;
            const KickView *k;
            const std::vector<uint32_t> *order;
            uint32_t next = 0;
            void scope(const Span &s)
            {
                if (!s.tag || (s.op != rc::gfx::OpBegin && s.op != rc::gfx::OpEnd))
                    return;
                self->closePrims();
                DrawCmd &c = self->m_list.add(s.op == rc::gfx::OpBegin ? kOpScopeBegin : kOpScopeEnd, s.tag->payload, s.tag->payloadBytes);
                c.a = s.tag->kind;
                c.b = s.tag->seq;
                c.c = s.guestAddr;
                ++self->m_stats.scopeCmds;
            }
            void until(uint32_t offset)
            {
                while (next < order->size() && k->spans[(*order)[next]].streamOffset <= offset)
                    scope(k->spans[(*order)[next++]]);
            }
            void range(const WalkRange &r) override
            {
                if (r.isTag || !r.bytes)
                {
                    until(r.streamOffset);
                    return;
                }
                uint32_t at = 0;
                while (true)
                {
                    until(r.streamOffset + at);
                    // The next span inside this range: feed up to it (spans sit on quadwords).
                    uint32_t end = r.bytes;
                    if (next < order->size())
                    {
                        const uint32_t so = k->spans[(*order)[next]].streamOffset;
                        if (so < r.streamOffset + r.bytes)
                            end = (so - r.streamOffset) & ~15u;
                    }
                    if (end <= at)
                        end = std::min(r.bytes, at + 16u);
                    self->feed(r.data + at, end - at);
                    at = end;
                    if (at >= r.bytes)
                        break;
                }
            }
        } feed;
        feed.self = this;
        feed.k = &k;
        feed.order = &order;
        m_kick = &k;
        const WalkResult w = walkChain(k, ram, &feed);
        feed.until(~0u);
        m_kick = nullptr;
        closePrims();
        m_stats.absentPages += w.absentPageReads;
        m_stats.walkerDropped += m_shadow.walker().stats().droppedBytes - droppedBefore;

        const uint64_t trxAfter[4] = {mem.bitbltbuf(), mem.trxpos(), mem.trxreg(), mem.trxdir()};
        for (uint32_t i = 0; i < 4; ++i)
        {
            m_regs[kGsBitbltbuf + i] = trxAfter[i];
            if (trxAfter[i] != trx[i])
                m_prov[kGsBitbltbuf + i] = kProvStream;
        }
        m_kickTransfers = static_cast<uint32_t>((mem.stats().transfers - transfersBefore) + (m_shadow.walker().stats().imageBytes != imageBefore));
        if (m_kickTransfers && (m_kickPrims || m_kickDropped))
            ++m_stats.kicksWithTransfersAndDraws;
    }

    void Gif2dReader::reg(uint8_t addr, uint64_t value)
    {
        if (static_cast<int64_t>(m_cycle) == m_opt.faultCycle && addr == m_opt.faultN)
        {
            ++m_cycleRegWrites;
            return; // planted fault: every write of this register in the cycle is lost
        }
        ++m_stats.regWrites;
        if (addr >= 0x80u)
            return;
        m_regs[addr] = value;
        m_prov[addr] = kProvStream;
        if (isVertexReg(addr))
        {
            vertex(addr, value);
            return;
        }
        if (addr == kGsPrim)
            m_queued = 0;
        if (addr == kGsScanmsk || addr == kGsDimx || addr == kGsTexclut || addr == kGsFogcol || addr == kGsFog || addr == kGsTex2 ||
            addr == kGsTex2 + 1)
            ++m_stats.sideRegs[addr];
        if (!isLatchReg(addr))
            m_stateDirty = true;
    }

    void Gif2dReader::vertex(uint8_t addr, uint64_t value)
    {
        ++m_stats.vertices;
        GsVertex v{};
        v.x = static_cast<uint16_t>(value & 0xFFFFu);
        v.y = static_cast<uint16_t>((value >> 16) & 0xFFFFu);
        const bool fogForm = addr == kGsXyzf2 || addr == kGsXyzf3;
        if (fogForm)
        {
            v.z = static_cast<uint32_t>((value >> 32) & 0xFFFFFFu);
            v.fog = static_cast<uint8_t>(value >> 56);
            v.flags = kVertexXyzf;
            ++m_stats.xyzfVertices;
        }
        else
            v.z = static_cast<uint32_t>(value >> 32);
        const uint64_t c = m_regs[kGsRgbaq];
        v.r = static_cast<uint8_t>(c);
        v.g = static_cast<uint8_t>(c >> 8);
        v.b = static_cast<uint8_t>(c >> 16);
        v.a = static_cast<uint8_t>(c >> 24);
        const uint32_t qb = static_cast<uint32_t>(c >> 32), sb = static_cast<uint32_t>(m_regs[kGsSt]), tb = static_cast<uint32_t>(m_regs[kGsSt] >> 32);
        std::memcpy(&v.q, &qb, 4);
        std::memcpy(&v.s, &sb, 4);
        std::memcpy(&v.t, &tb, 4);
        v.u = static_cast<uint16_t>(m_regs[kGsUv] & 0x3FFFu);
        v.v = static_cast<uint16_t>((m_regs[kGsUv] >> 16) & 0x3FFFu);
        const bool kick = addr == kGsXyz2 || addr == kGsXyzf2;
        if (!kick)
            ++m_stats.verticesNoKick;

        // Primitive assembly as the GS (gs_frontend.cpp vertexKick): the queue fills, a drawing vertex draws once
        // enough are there, then the queue advances by the type's rule whether it drew or not.
        const uint32_t type = static_cast<uint32_t>(m_regs[kGsPrim] & 7u);
        static const uint32_t kNeeded[8] = {1, 2, 2, 3, 3, 3, 2, 0};
        const uint32_t needed = kNeeded[type];
        if (!needed)
            return; // reserved type: the GS ignores the vertex
        m_queue[m_queued < 3 ? m_queued : 2] = v;
        ++m_queued;
        if (m_queued < needed)
            return;
        if (kick)
            primitive(m_queue, needed);
        switch (type)
        {
        case kPrimLineStrip:
            m_queue[0] = m_queue[1];
            m_queued = 1;
            break;
        case kPrimTriStrip:
            m_queue[0] = m_queue[1];
            m_queue[1] = m_queue[2];
            m_queued = 2;
            break;
        case kPrimTriFan:
            m_queue[1] = m_queue[2];
            m_queued = 2;
            break;
        default:
            m_queued = 0;
            break;
        }
    }

    uint64_t Gif2dReader::value(uint8_t addr, uint16_t group, uint16_t &needSeeded, uint16_t &needDefault, bool &missing)
    {
        switch (m_prov[addr])
        {
        case kProvStream: return m_regs[addr];
        case kProvSeeded:
        {
            needSeeded |= group;
            uint64_t d;
            if (!defaultOf(addr, d) || d != m_regs[addr])
                m_needSeededOff |= group; // live, without the recorded file, the default would be wrong or missing
            return m_regs[addr];
        }
        case kProvDefault: needDefault |= group; return m_regs[addr];
        default: break;
        }
        uint64_t v;
        if (defaultOf(addr, v))
        {
            m_regs[addr] = v;
            m_prov[addr] = kProvDefault;
            needDefault |= group;
            return v;
        }
        missing = true;
        return 0;
    }

    bool Gif2dReader::buildState(GsState &s, uint16_t &needSeeded, uint16_t &needDefault, uint32_t &dropReason)
    {
        std::memset(&s, 0, sizeof(s));
        needSeeded = needDefault = 0;
        m_needSeededOff = 0;
        dropReason = 0;
        bool missing = false;
        const bool ac = value(kGsPrmodecont, kGrpPrim, needSeeded, needDefault, missing) & 1u;
        uint64_t prim = value(kGsPrim, kGrpPrim, needSeeded, needDefault, missing) & 0x7FFu;
        if (!ac)
            prim = (value(kGsPrmode, kGrpPrim, needSeeded, needDefault, missing) & 0x7F8u) | (prim & 7u);
        static const uint16_t kClass[8] = {kPrimPoint, kPrimLine, kPrimLine, kPrimTriangle, kPrimTriangle, kPrimTriangle, kPrimSprite, 7};
        s.prim = static_cast<uint16_t>((prim & ~7ull) | kClass[prim & 7u]);
        const uint8_t c = static_cast<uint8_t>(s.ctxt());
        s.frame = value(kGsFrame + c, kGrpFrame, needSeeded, needDefault, missing);
        s.zbuf = value(kGsZbuf + c, kGrpZbuf, needSeeded, needDefault, missing);
        s.scissor = value(kGsScissor + c, kGrpScissor, needSeeded, needDefault, missing);
        s.xyoffset = value(kGsXyoffset + c, kGrpXyoffset, needSeeded, needDefault, missing);
        s.test = value(kGsTest + c, kGrpTest, needSeeded, needDefault, missing);
        if (s.abe())
            s.alpha = value(kGsAlpha + c, kGrpAlpha, needSeeded, needDefault, missing);
        s.pabe = static_cast<uint8_t>(value(kGsPabe, kGrpPabe, needSeeded, needDefault, missing) & 1u);
        s.fba = static_cast<uint8_t>(value(kGsFba + c, kGrpFba, needSeeded, needDefault, missing) & 1u);
        s.colclamp = static_cast<uint8_t>(value(kGsColclamp, kGrpColclamp, needSeeded, needDefault, missing) & 1u);
        s.dthe = static_cast<uint8_t>(value(kGsDthe, kGrpDthe, needSeeded, needDefault, missing) & 1u);
        s.texture = 0xFFFFFFFFu;
        m_stateTexture = nullptr;
        if (s.tme())
        {
            s.tex0 = value(kGsTex0 + c, kGrpTex0, needSeeded, needDefault, missing);
            s.tex1 = value(kGsTex1 + c, kGrpTex1, needSeeded, needDefault, missing);
            s.clamp = value(kGsClamp + c, kGrpClamp, needSeeded, needDefault, missing);
            if ((s.tex1 >> 2) & 7u)
            {
                s.miptbp1 = value(kGsMiptbp1 + c, kGrpMiptbp, needSeeded, needDefault, missing);
                s.miptbp2 = value(kGsMiptbp2 + c, kGrpMiptbp, needSeeded, needDefault, missing);
            }
            const Tex0 t = Tex0::unpack(s.tex0);
            if (usesTexa(t))
                s.texa = value(kGsTexa, kGrpTexa, needSeeded, needDefault, missing);
            if (t.indexed() && t.cld != 1u)
                ++m_stats.cldOther;
            if (!missing)
            {
                const CachedTexture &e = m_cache.get(t, m_opt.faultIgnoreTexa ? Texa::unpack(0x80ull << 32) : Texa::unpack(s.texa));
                if (e.valid)
                {
                    s.texture = textureId(&e);
                    s.textureGeneration = e.generation;
                    s.texAlphaMin = e.alphaMin;
                    s.texAlphaMax = e.alphaMax;
                    m_stateTexture = &e;
                }
            }
        }
        // Groups known from the stream: every group the state reads that was neither seeded nor defaulted.
        uint16_t reads = kGrpFrame | kGrpZbuf | kGrpScissor | kGrpXyoffset | kGrpTest | kGrpPrim | kGrpPabe | kGrpFba | kGrpColclamp | kGrpDthe;
        if (s.abe())
            reads |= kGrpAlpha;
        if (s.tme())
        {
            reads |= kGrpTex0 | kGrpTex1 | kGrpClamp;
            if ((s.tex1 >> 2) & 7u)
                reads |= kGrpMiptbp;
            if (usesTexa(Tex0::unpack(s.tex0)))
                reads |= kGrpTexa;
        }
        s.known = static_cast<uint16_t>(reads & ~(needSeeded | needDefault));
        if (missing)
            dropReason = 2;
        else if (((s.frame >> 16) & 0x3Fu) == 0u)
            dropReason = 1;
        return dropReason == 0;
    }

    GsState Gif2dReader::currentState()
    {
        GsState s;
        uint16_t a, b;
        uint32_t r;
        buildState(s, a, b, r);
        return s;
    }

    uint32_t Gif2dReader::textureId(const CachedTexture *t)
    {
        auto it = m_textureIds.find(t);
        if (it != m_textureIds.end())
            return it->second;
        const uint32_t id = static_cast<uint32_t>(m_textureIds.size());
        m_textureIds.emplace(t, id);
        return id;
    }

    void Gif2dReader::closePrims()
    {
        if (!m_primsOpen)
            return;
        DrawCmd &c = m_list.cmds.back();
        if (c.a != kKindTriangles || (c.b & 1u))
            c.c &= ~kPrimsRectPairs;
        m_primsOpen = false;
    }

    void Gif2dReader::emitState(const GsState &s, const GsPlan &p)
    {
        if (m_haveListState && s == m_listState)
            return;
        closePrims();
        if (m_opt.textures && s.tme() && m_stateTexture && s.texture != 0xFFFFFFFFu)
        {
            if (m_sentGeneration.size() <= s.texture)
                m_sentGeneration.resize(s.texture + 1u, 0u);
            if (m_sentGeneration[s.texture] != s.textureGeneration)
            {
                const CachedTexture &e = *m_stateTexture;
                DrawCmd &t = m_list.add(kOpTexture, e.rgba.data(), static_cast<uint32_t>(e.rgba.size()));
                t.a = s.texture;
                t.b = s.textureGeneration;
                t.c = static_cast<uint32_t>(e.key.width) | static_cast<uint32_t>(e.key.height) << 16;
                m_sentGeneration[s.texture] = s.textureGeneration;
                ++m_stats.textureCmds;
                m_stats.textureBytes += e.rgba.size();
            }
        }
        DrawCmd &c = m_list.add(kOpState, &s, sizeof(GsState));
        c.a = p.unsupported ? 1u : 0u;
        m_listState = s;
        m_haveListState = true;
        ++m_stats.stateCmds;
    }

    void Gif2dReader::primitive(const GsVertex *v, uint32_t n)
    {
        ++m_stats.prims;
        if (m_stateDirty || m_shadow.memory().serial() != m_stateSerial)
        {
            buildState(m_state, m_needSeeded, m_needDefault, m_dropReason);
            m_plan = planState(m_state);
            m_stateSerial = m_shadow.memory().serial();
            m_stateDirty = false;
        }
        const uint32_t type = m_state.primType();
        const uint32_t kind = type == kPrimPoint ? kKindPoints : type == kPrimLine ? kKindLines : type == kPrimSprite ? kKindSprites : kKindTriangles;
        ++m_stats.primsByKind[kind];
        if (m_needSeeded | m_needDefault)
        {
            if (!m_seenClear)
                ++m_stats.neededBeforeFirstClear;
            else
                for (uint32_t g = 0; g < kGsGroupCount; ++g)
                {
                    if (m_needSeeded & (1u << g))
                        ++m_stats.neededSeeded[g];
                    if (m_needSeededOff & (1u << g))
                        ++m_stats.neededSeededNotDefault[g];
                    if (m_needDefault & (1u << g))
                        ++m_stats.neededDefault[g];
                }
        }
        if (m_dropReason)
        {
            ++m_stats.dropped;
            ++(m_dropReason == 1 ? m_stats.droppedFbw0 : m_stats.droppedUnknown);
            ++m_kickDropped;
            return;
        }
        for (uint32_t g = 1; g < kGuardCount; ++g)
            if (m_plan.guards & (1u << g))
                ++m_stats.guardPrims[g];
        const uint32_t zpsm = static_cast<uint32_t>((m_state.zbuf >> 24) & 0xFu);
        if (zpsm != 0u)
            for (uint32_t i = 0; i < n; ++i)
                if (v[i].z >= (1u << 24))
                {
                    ++m_stats.zAbove24;
                    break;
                }
        if ((m_regs[kGsScanmsk] & 3u) || (m_state.tme() && Tex0::unpack(m_state.tex0).csm))
            ++m_stats.sideRegsNonZero;

        ++m_kickPrims;
        const bool ate = m_state.test & 1u;
        const uint32_t ztst = static_cast<uint32_t>((m_state.test >> 17) & 3u);
        if (kind == kKindSprites && !m_state.tme() && !m_state.abe() && !ate && ztst == kDepthAlways)
        {
            emitState(m_state, m_plan);
            DrawCmd &c = m_list.add(kOpClear, v, 2u * sizeof(GsVertex));
            c.a = static_cast<uint32_t>(v[1].r) | static_cast<uint32_t>(v[1].g) << 8 | static_cast<uint32_t>(v[1].b) << 16 |
                  static_cast<uint32_t>(v[1].a) << 24;
            c.b = v[1].z;
            c.d = m_cyclePrims++;
            ++m_stats.clears;
            m_seenClear = true;
            return;
        }
        if (!m_primsOpen || m_primsKind != kind || !m_haveListState || m_listState != m_state)
        {
            closePrims();
            emitState(m_state, m_plan);
            DrawCmd &c = m_list.add(kOpScreenPrims);
            c.a = kind;
            c.b = 0;
            c.c = kPrimsOneColour | (kind == kKindTriangles ? kPrimsRectPairs : 0u);
            c.d = m_cyclePrims;
            m_primsOpen = true;
            m_primsKind = kind;
            ++m_stats.primCmds;
        }
        m_list.append(v, n * static_cast<uint32_t>(sizeof(GsVertex)));
        DrawCmd &c = m_list.cmds.back();
        ++c.b;
        for (uint32_t i = 1; i < n; ++i)
            if (std::memcmp(&v[i].r, &v[0].r, 4) != 0)
                c.c &= ~kPrimsOneColour;
        if (kind == kKindTriangles && !(c.b & 1u) && (c.c & kPrimsRectPairs))
        {
            const GsVertex *all = m_list.vertices(c);
            if (!rectanglePair(all + (c.b - 2u) * 3u, all + (c.b - 1u) * 3u))
                c.c &= ~kPrimsRectPairs;
        }
        ++m_cyclePrims;
    }
}
