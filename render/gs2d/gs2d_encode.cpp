#include "gs2d_encode.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace rc::rd
{
    const char *gsFieldName(int field)
    {
        static const char *const names[kFieldCount] = {"frame", "zbuf", "scissor", "xyoffset", "test", "alpha", "tex0", "tex1", "clamp",
                                                       "miptbp1", "miptbp2", "texa", "prim", "pabe", "fba", "colclamp", "dthe"};
        return field >= 0 && field < kFieldCount ? names[field] : "?";
    }

    namespace
    {
        struct Encoder
        {
            std::vector<std::pair<uint8_t, uint64_t>> writes;
            uint64_t sim[kGsRegFile];
            void write(uint8_t addr, uint64_t value)
            {
                writes.emplace_back(addr, value);
                if (addr < kGsRegFile)
                    sim[addr] = value;
            }
            void state(const GsState &s, int zero)
            {
                const auto f = [zero](int field, uint64_t v) { return field == zero ? 0ull : v; };
                const uint8_t c = static_cast<uint8_t>(s.ctxt());
                write(kGsFrame + c, f(kFieldFrame, s.frame));
                write(kGsZbuf + c, f(kFieldZbuf, s.zbuf));
                write(kGsScissor + c, f(kFieldScissor, s.scissor));
                write(kGsXyoffset + c, f(kFieldXyoffset, s.xyoffset));
                write(kGsTest + c, f(kFieldTest, s.test));
                write(kGsAlpha + c, f(kFieldAlpha, s.alpha));
                write(kGsTex0 + c, f(kFieldTex0, s.tex0));
                write(kGsTex1 + c, f(kFieldTex1, s.tex1));
                write(kGsClamp + c, f(kFieldClamp, s.clamp));
                write(kGsMiptbp1 + c, f(kFieldMiptbp1, s.miptbp1));
                write(kGsMiptbp2 + c, f(kFieldMiptbp2, s.miptbp2));
                write(kGsTexa, f(kFieldTexa, s.texa));
                write(kGsPabe, f(kFieldPabe, s.pabe));
                write(kGsFba + c, f(kFieldFba, s.fba));
                write(kGsColclamp, f(kFieldColclamp, s.colclamp));
                write(kGsDthe, f(kFieldDthe, s.dthe));
                write(kGsPrmodecont, 1u); // the state's PRIM carries every attribute
            }
            void vertex(const GsVertex &v)
            {
                uint32_t sb, tb, qb;
                std::memcpy(&sb, &v.s, 4);
                std::memcpy(&tb, &v.t, 4);
                std::memcpy(&qb, &v.q, 4);
                write(kGsRgbaq, static_cast<uint64_t>(v.r) | static_cast<uint64_t>(v.g) << 8 | static_cast<uint64_t>(v.b) << 16 |
                                    static_cast<uint64_t>(v.a) << 24 | static_cast<uint64_t>(qb) << 32);
                write(kGsSt, static_cast<uint64_t>(sb) | static_cast<uint64_t>(tb) << 32);
                write(kGsUv, static_cast<uint64_t>(v.u) | static_cast<uint64_t>(v.v) << 16);
                if (v.flags & kVertexXyzf)
                    write(kGsXyzf2, static_cast<uint64_t>(v.x) | static_cast<uint64_t>(v.y) << 16 | static_cast<uint64_t>(v.z & 0xFFFFFFu) << 32 |
                                        static_cast<uint64_t>(v.fog) << 56);
                else
                    write(kGsXyz2, static_cast<uint64_t>(v.x) | static_cast<uint64_t>(v.y) << 16 | static_cast<uint64_t>(v.z) << 32);
            }
        };

        bool sideEffect(uint8_t a) { return a == kGsTrxdir || a == kGsHwreg || a == kGsSignal || a == kGsFinish || a == kGsLabel; }
        bool vertexReg(uint8_t a) { return a == kGsXyzf2 || a == kGsXyz2 || a == kGsXyzf3 || a == kGsXyz3; }
    }

    GsEncodeResult encodeDrawList(const DrawList &list, uint32_t first, uint32_t last, const uint64_t *before, const uint64_t *after,
                                  const bool *afterKnown, const GsEncodeFault &fault)
    {
        GsEncodeResult r;
        Encoder e;
        std::memcpy(e.sim, before, sizeof(e.sim));
        // The state in force at `first`: the list's last STATE before it.
        const GsState *cur = nullptr;
        for (uint32_t i = first; i-- > 0;)
            if (list.cmds[i].op == kOpState)
            {
                cur = &list.state(list.cmds[i]);
                break;
            }
        bool written = false;
        int64_t prim = 0;
        auto ensureState = [&]
        {
            if (cur && !written)
            {
                e.state(*cur, fault.zeroField);
                written = true;
                ++r.states;
            }
        };
        for (uint32_t i = first; i < last && i < list.cmds.size(); ++i)
        {
            const DrawCmd &c = list.cmds[i];
            switch (c.op)
            {
            case kOpState:
                cur = &list.state(c);
                written = false;
                break;
            case kOpClear:
            case kOpScreenPrims:
            {
                ensureState();
                const uint32_t kind = c.op == kOpClear ? static_cast<uint32_t>(kKindSprites) : c.a;
                const uint32_t count = c.op == kOpClear ? 1u : c.b, per = verticesPerPrim(kind);
                const GsVertex *v = list.vertices(c);
                const uint64_t primValue = cur ? (fault.zeroField == kFieldPrim ? 0u : cur->prim) : 0u;
                for (uint32_t p = 0; p < count; ++p, ++prim)
                {
                    if (prim == fault.dropPrim)
                        continue;
                    e.write(kGsPrim, primValue);
                    for (uint32_t k = 0; k < per; ++k)
                        e.vertex(v[p * per + k]);
                    ++r.prims;
                }
                break;
            }
            default:
                break; // TEXTURE and scopes carry nothing for the GS
            }
        }

        // Write back what the original stream left different.
        bool xyzDiffers = false;
        for (uint8_t a = 0; a < kGsRegFile; ++a)
            if (afterKnown[a] && vertexReg(a) && e.sim[a] != after[a])
                xyzDiffers = true;
        if (xyzDiffers)
        {
            e.write(kGsPrim, 7u); // reserved type: the reference takes the vertices into its registers and draws nothing
            for (uint8_t a = 0; a < kGsRegFile; ++a)
                if (afterKnown[a] && vertexReg(a) && e.sim[a] != after[a])
                {
                    e.write(a, after[a]);
                    ++r.restored;
                }
        }
        for (uint8_t a = 0; a < kGsRegFile; ++a)
        {
            if (!afterKnown[a] || vertexReg(a) || a == kGsPrim || a == kGsPrmodecont || e.sim[a] == after[a])
                continue;
            if (sideEffect(a))
            {
                if (!r.unrestorable++)
                    r.firstUnrestorable = a;
                continue;
            }
            e.write(a, after[a]);
            ++r.restored;
        }
        if (afterKnown[kGsPrim] && (xyzDiffers || e.sim[kGsPrim] != after[kGsPrim]))
        {
            e.write(kGsPrim, after[kGsPrim]);
            ++r.restored;
        }
        if (afterKnown[kGsPrmodecont] && e.sim[kGsPrmodecont] != after[kGsPrmodecont])
        {
            e.write(kGsPrmodecont, after[kGsPrmodecont]);
            ++r.restored;
        }

        // PACKED A+D tags.
        r.writes = static_cast<uint32_t>(e.writes.size());
        size_t at = 0;
        while (at < e.writes.size())
        {
            const size_t n = std::min<size_t>(0x7FFFu, e.writes.size() - at);
            const bool eop = at + n == e.writes.size();
            const uint64_t lo = static_cast<uint64_t>(n) | (eop ? 1ull << 15 : 0ull) | 1ull << 60; // FLG 0 PACKED, NREG 1
            const uint64_t hi = 0xEu;                                                             // A+D
            const size_t base = r.gif.size();
            r.gif.resize(base + 16u * (n + 1u));
            std::memcpy(&r.gif[base], &lo, 8);
            std::memcpy(&r.gif[base + 8], &hi, 8);
            for (size_t i = 0; i < n; ++i)
            {
                const uint64_t value = e.writes[at + i].second, addr = e.writes[at + i].first;
                std::memcpy(&r.gif[base + 16u * (i + 1u)], &value, 8);
                std::memcpy(&r.gif[base + 16u * (i + 1u) + 8], &addr, 8);
            }
            at += n;
        }
        return r;
    }
}
