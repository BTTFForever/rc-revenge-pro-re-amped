#pragma once

// The one interface live and off-line consumers of the kick record share (RENDERER_FRAME_RECORD.md section 4.2).
// All calls are made on one thread, in event order: the EE thread live (inside the runtime's hooks), the tool's
// thread off-line. A live consumer must not block on the GPU or another thread's work (but for rc::live::KickQueue's
// bounded wait of RENDERER_P2_DESIGN.md 8.3, and drain()).

#include "format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rc::rec
{
    struct FileInfo
    {
        FileHeader header{};
        std::string path;     // empty live
        std::string tagNames; // the TNAM text ("table value name" lines; batch 3), empty when the dump has none
    };

    struct StateChunk
    {
        uint32_t id = 0;
        const uint8_t *data = nullptr;
        uint64_t bytes = 0;
    };

    struct Keyframe
    {
        KeyframeHeader header{};
        std::vector<StateChunk> chunks; // GSVR, GSPR, GSRG, VU1M, VU1R, VIF1, EMUS, TAGP
        const StateChunk *find(uint32_t id) const
        {
            for (const StateChunk &c : chunks)
                if (c.id == id)
                    return &c;
            return nullptr;
        }
    };

    // Guest RAM as a consumer sees it. Live: guest RAM itself (present/generations null: every page is there).
    // Off-line: the image the reader rebuilt from PAGE events. At a kick it equals live RAM on every byte of that kick's
    // stream; other bytes are whatever they were when their page was last captured, and pages never captured are absent.
    struct RamView
    {
        const uint8_t *ram = nullptr;        // kRamBytes
        const uint8_t *scratchpad = nullptr; // kScratchpadBytes
        const uint8_t *present = nullptr;    // per page (kPageCount), or null = all present
        const uint32_t *generations = nullptr;
        bool has(uint32_t page) const { return page < kPageCount && (!present || present[page]); }
        uint32_t generation(uint32_t page) const { return generations && page < kPageCount ? generations[page] : 0u; }
        // A page index for an RDRAM offset or a scratchpad offset.
        static uint32_t pageOf(uint32_t offset, bool scratch) { return scratch ? kRamPages + offset / kPageBytes : offset / kPageBytes; }
    };

    struct TagView
    {
        uint32_t seq = 0;
        uint16_t kind = 0;
        uint8_t op = 0, depth = 0;
        uint32_t anchor = 0, parent = 0;
        const void *payload = nullptr;
        uint32_t payloadBytes = 0;
    };

    struct Span // an address-anchored tag resolved to a stream offset (begin or end)
    {
        uint32_t streamOffset = 0, guestAddr = 0;
        const TagView *tag = nullptr;
        uint8_t op = 0;
    };

    struct KickView
    {
        uint64_t cycle = 0;   // END.cycle of the cycle it belongs to
        uint32_t index = 0;   // within the cycle
        uint8_t channel = 0;  // 0 VIF0, 1 VIF1, 2 GIF
        uint8_t mode = 0;     // CHCR.MOD: 0 normal, 1 chain, 2 interleave
        bool tte = false;
        uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0, asr0 = 0, asr1 = 0, callerRa = 0, mxcsr = 0, fbrst = 0;
        uint32_t dmaTags = 0, streamBytes = 0, gifBytes = 0;
        uint64_t streamHash = 0, gifHash = 0, field = 0;
        bool hasGifHash = false;
        const Span *spans = nullptr;
        uint32_t spanCount = 0;
        const TagView *const *labels = nullptr;
        uint32_t labelCount = 0;
        const uint64_t *regs = nullptr; // GS register file before the kick (kGsRegCount) or null
    };

    struct CycleInfo
    {
        RecEndBody end{};
        uint32_t index = 0; // position in the dump (0-based)
    };

    struct Consumer
    {
        virtual void open(const FileInfo &) {}
        virtual void keyframe(const Keyframe &) {}
        virtual void cycleBegin(uint64_t /*cycle*/) {}
        // A run of captured pages (PAGE event), before the kick that needs them. Off-line the reader has already copied
        // them into the RAM view when this is called. (Added in batch 1: the file writer is a consumer and needs them.)
        virtual void pages(uint32_t /*firstPage*/, uint32_t /*count*/, const uint8_t * /*data*/) {}
        virtual void hw(uint32_t /*address*/, uint32_t /*width*/, uint64_t /*value*/, uint32_t /*callerRa*/) {}
        virtual void crt(int /*interlaced*/, int /*mode*/, int /*ffmd*/) {}
        virtual void tag(const TagView &) {}
        virtual void kick(const KickView &, const RamView &) {}
        virtual void odd(int /*kind*/, uint32_t /*address*/, uint64_t /*value*/) {}
        virtual void note(const std::string & /*text*/) {}
        virtual void cycleEnd(const CycleInfo &, const uint8_t * /*refRgba*/) {}
        // (step 8 batch 4) Off-line only: a chunk the reader does not interpret that follows a delivered cycle (DLST),
        // after that cycle's cycleEnd; its payload uncompressed.
        virtual void chunk(uint32_t /*id*/, const uint8_t * /*data*/, uint64_t /*bytes*/) {}
        virtual void close() {}
        // (P2 step 9 batch 6) Live only: return once every event delivered so far has been handled, including any work a
        // consumer hands to another thread (rc::live::KickQueue's reader thread). The capture calls it before it closes a
        // recording, so the lists of every recorded cycle reach the dump (DLST) before it is closed.
        virtual void drain() {}
        virtual ~Consumer() = default;
    };
}
