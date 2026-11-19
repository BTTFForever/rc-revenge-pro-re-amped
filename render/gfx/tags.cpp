// The side-tag channel (RENDERER_FRAME_RECORD.md section 5; tags.h). Linked into the game through rc_record; it does
// nothing until a consumer attaches (render/record/capture.cpp, when RC_FRAME_RECORD is set). All calls come from the
// EE executor thread (native game code), so no lock is taken.
#include "tags.h"

#include "../record/chain_walk.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace rc::gfx
{
    const char *nameTable()
    {
        return "kind 1 Scene\nkind 2 View\nkind 3 Pass\nkind 4 Draw\nkind 5 Hud\nkind 6 Kick\nkind 7 Label\nkind 8 Cut\n"
               "op 0 set\nop 1 begin\nop 2 end\n"
               "draw 1 shape\ndraw 2 skin\ndraw 3 hud\ndraw 4 font\n"
               "hud 1 panel\nhud 2 top-left\nhud 3 top-right\nhud 4 bottom-right\nhud 5 bottom-left\nhud 6 middle\n"
               "anchor 1 left\nanchor 2 right\nanchor 3 top\nanchor 4 bottom\nanchor 5 centre\nanchor 6 stretch\n"
               "anchor 7 top-left\nanchor 8 top-right\nanchor 9 bottom-left\nanchor 10 bottom-right\n"
               "kick 1 K1\nkick 2 K2\nkick 3 texture-upload\nkick 4 animated-texture\nkick 5 backdrop\nkick 6 movie\nkick 7 flip\n";
    }

#if RC_GFX_TAGS
    namespace detail
    {
        bool g_live = false;
        bool g_enabled = false;
        bool g_comparing = false;

        namespace
        {
            Sink *g_sink = nullptr;
            uint32_t (*g_readers[4])() = {};
            uint32_t g_seq = 0;

            struct Open
            {
                uint32_t seq;
                uint16_t kind;
                AnchorKind anchorKind;
                uint32_t address; // beginAt's
            };
            std::vector<Open> g_open; // open spans in begin order (ends need not come in reverse order: guest threads)

            uint32_t readAnchor(AnchorKind kind, uint32_t address)
            {
                if (kind == AnchorKind::Address)
                    return physical(address);
                const auto reader = g_readers[static_cast<int>(kind) & 3];
                return reader ? physical(reader()) : 0u;
            }

            uint32_t emit(uint16_t kind, uint8_t op, AnchorKind anchorKind, uint32_t anchor, uint32_t parent, const void *payload,
                          uint32_t bytes)
            {
                Event e;
                e.seq = ++g_seq;
                e.kind = kind;
                e.op = op;
                e.depth = static_cast<uint8_t>(g_open.size() < 255u ? g_open.size() : 255u);
                e.anchor = anchor;
                e.parent = op == OpEnd ? parent : (g_open.empty() ? 0u : g_open.back().seq);
                e.anchorKind = anchorKind;
                e.payload = payload;
                e.payloadBytes = bytes;
                g_sink->tag(e);
                return e.seq;
            }

            uint32_t begin(uint16_t kind, AnchorKind anchorKind, uint32_t address, const void *payload, uint32_t bytes)
            {
                if (!g_live || !g_sink)
                    return 0u;
                const uint32_t anchor = anchorKind == AnchorKind::None ? 0u : readAnchor(anchorKind, address);
                const uint32_t seq = emit(kind, OpBegin, anchorKind, anchor, 0u, payload, bytes);
                g_open.push_back({seq, kind, anchorKind, address});
                return seq;
            }

            void finish(uint32_t token)
            {
                if (!token || !g_sink)
                    return;
                for (size_t i = g_open.size(); i-- > 0;)
                {
                    if (g_open[i].seq != token)
                        continue;
                    const Open o = g_open[i];
                    g_open.erase(g_open.begin() + static_cast<std::ptrdiff_t>(i));
                    const uint32_t anchor = o.anchorKind == AnchorKind::None ? 0u : readAnchor(o.anchorKind, o.address);
                    emit(o.kind, OpEnd, o.anchorKind, anchor, token, nullptr, 0u);
                    return;
                }
            }

            void set(uint16_t kind, const void *payload, uint32_t bytes)
            {
                if (g_live && g_sink)
                    emit(kind, OpSet, AnchorKind::None, 0u, 0u, payload, bytes);
            }
        }

        void attach(Sink *sink)
        {
            g_sink = sink;
            g_enabled = sink != nullptr;
            g_live = g_enabled && !g_comparing;
            g_open.clear();
        }

        void setAnchorReader(AnchorKind kind, uint32_t (*reader)()) { g_readers[static_cast<int>(kind) & 3] = reader; }

        uint32_t physical(uint32_t guestAddress)
        {
            uint32_t offset = 0;
            bool scratch = false;
            if (!rc::rec::translateDmaAddress(guestAddress, offset, scratch))
                return 0u;
            return scratch ? 0x70000000u + offset : offset;
        }
    }

    void scene(const SceneTag &t) { detail::set(KindScene, &t, sizeof(t)); }
    void view(const ViewTag &t) { detail::set(KindView, &t, sizeof(t)); }
    void pass(uint32_t p)
    {
        const PassTag t{p, 0u};
        detail::set(KindPass, &t, sizeof(t));
    }

    uint32_t begin2D(const DrawKey &key, const HudTag *hud)
    {
        if (hud)
        {
            const HudPayload p{key, *hud};
            return detail::begin(KindHud, AnchorKind::Buffer2D, 0u, &p, sizeof(p));
        }
        const DrawTag d{key, 0u, 0u};
        return detail::begin(KindDraw, AnchorKind::Buffer2D, 0u, &d, sizeof(d));
    }

    uint32_t beginChain(const DrawKey &key, const float *matrix3x4)
    {
        uint8_t buffer[sizeof(DrawTag) + 12 * sizeof(float)];
        const DrawTag d{key, 0u, matrix3x4 ? 1u : 0u};
        std::memcpy(buffer, &d, sizeof(d));
        uint32_t bytes = sizeof(d);
        if (matrix3x4)
        {
            std::memcpy(buffer + sizeof(d), matrix3x4, 12 * sizeof(float));
            bytes += 12 * sizeof(float);
        }
        return detail::begin(KindDraw, AnchorKind::ChainSlot, 0u, buffer, bytes);
    }

    uint32_t beginAt(uint32_t guestAddress, const DrawKey &key)
    {
        const DrawTag d{key, 0u, 0u};
        return detail::begin(KindDraw, AnchorKind::Address, guestAddress, &d, sizeof(d));
    }

    void end(uint32_t token) { detail::finish(token); }

    uint32_t beginKicks(const KickLabel &k) { return detail::begin(KindKick, AnchorKind::None, 0u, &k, sizeof(k)); }
    void endKicks(uint32_t token) { detail::finish(token); }

    void label(const char *text)
    {
        if (!detail::g_live || !text)
            return;
        char buffer[128] = {};
        const size_t n = std::min<size_t>(std::strlen(text), sizeof(buffer) - 1u);
        std::memcpy(buffer, text, n);
        const uint32_t bytes = static_cast<uint32_t>((n + 1u + 7u) & ~size_t{7});
        detail::set(KindLabel, buffer, bytes);
    }
#endif
}
