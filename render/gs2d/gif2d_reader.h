#pragma once

// The GIF reader for the 2D packet (renderer plan P1 step 7; RENDERER_2D_DESIGN.md section 2.2): PATH3's packets ->
// one GS register file, primitive assembly as the GS does it, and a draw list per display cycle of STATE, TEXTURE,
// SCREEN_PRIMS, CLEAR and scope commands (gs_state.h).
//
// It is an rc::rec::Consumer on step 5's shadow of GS memory: it owns a TexShadow (so a texture's upload is applied
// before the TEX0 that samples it, whatever the packets' layout) and a TextureCache, walks each channel 2 kick with
// walkChain and feeds the payload to the shadow's GifWalker, which hands it every register write other than the
// transfers. Live it will sit in the capture's consumer list (step 7 batch 5); off-line rc_replay's gif2d consumer
// drives it. From step 8 on the same object takes PATH1's packets.
//
// Register file: 0x63 registers with a provenance each (unknown, default, seeded from a recorded file, from the
// stream). (Step 8, RENDERER_3D_DESIGN.md 7) PATH1's packets come in through beginPath1Kick / feedPath1 /
// endPath1Kick (the VIF1 reader's XGKICK packets, through a second GIF walker) into the same register file, vertex
// queue and draw list: there is one register file, and a channel 1 kick no longer makes anything unknown, except
// after a guard of the VIF1 reader (endPath1Kick(true): the registers PATH1 can write, path1Writes, become unknown).
// kick() ignores channel 1: FrameReader routes it. A draw that needs an unknown register takes the default of RENDERER_2D_DESIGN.md 3.1, or is
// dropped where there is none; with Options::seed (rc_replay on an exact dump) unknown registers are seeded from the
// kick's recorded register file (and the keyframe's GSRG) first. Draws that needed a seeded or default register are
// counted by register.
//
// The list is cleared at cycleBegin and complete after cycleEnd; kickFirstCommand() tells which commands the last
// kick made (check (b) re-encodes them).

#include "gs_plan.h"
#include "gs_state.h"

#include "../record/consumer.h"
#include "../tex/gif_walk.h"
#include "../tex/tex_cache.h"
#include "../tex/tex_shadow.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rc::rd
{
    // Register provenance.
    enum GsProvenance : uint8_t
    {
        kProvUnknown = 0,
        kProvDefault = 1, // a draw took the default of 3.1
        kProvSeeded = 2,  // from a recorded register file (a kick's or a keyframe's)
        kProvStream = 3,  // written by the packets the reader walked
    };

    // The registers PATH1 writes (RENDERER_3D_DESIGN.md 3.5, and the vertex registers): unknown after a guard of the
    // VIF1 reader stopped a kick.
    bool path1Writes(uint8_t addr);

    class Gif2dReader : public rc::rec::Consumer, private GifSink
    {
    public:
        struct Options
        {
            bool seed = false;          // seed unknown registers from recorded files (rc_replay on an exact dump)
            bool textures = true;       // emit TEXTURE commands (decode through the cache)
            int64_t faultCycle = -1;    // planted fault: every write of GS register faultN in this cycle lost (check (a)
            uint32_t faultN = 0;        // must fail where the register's value changed)
            bool faultIgnoreTexa = false; // planted fault (rc_gs2d_test T6): textures decoded as if TEXA were TA0 0, TA1 0x80, AEM 0
            // (step 8, RENDERER_3D_DESIGN.md 8) Leave out of the list a triangle of zero area and one whose bounding box
            // (the reference's gsTriangleSetup bounds) misses the scissor: the reference's own early exits.
            bool cullOffscreen = false;
            // (step 7 batch 5, live 2D) false: PATH1's primitives are left out of the list (counted), their register
            // writes kept. The live picture of RC_VK_PICTURE=native draws K1 only until step 8 draws K2; this spares the
            // EE thread K2's states, texture decodes and 0.6 MB of vertices a kick.
            bool path1Prims = true;
            // (step 7 batch 5, live 2D) host-to-local transfers into the frame buffers become IMAGE_BLIT commands (a: FBP,
            // b: x | y << 16, c: w | h << 16, payload w x h CT32 words as stored) in list order, so the targets hold what was
            // uploaded under what is drawn after it (the front end draws its menus over an uploaded picture, not a clear).
            // CT32 at FBW 10 from a page-row boundary, placed by its line in the two 640 x 256 buffers FBP 0x0 / 0x50 (a 640 x 512
            // upload at FBP 0x0 fills both); anything else is counted, not drawn.
            bool frameUploads = false;
            // (P2 step 9 batch 2, the fast reader: RENDERER_P2_DESIGN.md 4.4; FrameReader::Options::fast sets all three.)
            // path1Direct: feedPath1 recognises a triangle packet by its tag (one PACKED tag, NLOOP 3, NREG 3, REGS ST
            // RGBAQ XYZ2, 160 bytes) and hands it to path1Triangle instead of the walker, and drops a 16-byte packet whose
            // tag has NLOOP 0 (the flush), which writes nothing. clutMemo: a TEX0 write equal to the last TEX0 write, with
            // TEXCLUT as it was and no byte of GS memory changed since, does not reload the CLUT buffer (the load would read
            // the same bytes into the same entries). stateMemo: a PATH1 packet of register writes only (PRIM, TEX0, CLAMP,
            // A+D to registers other than RGBAQ, the vertex and the transfer registers) whose bytes equal the last such
            // packet's, with no register written and no memory changed since, is skipped (writing it again changes nothing).
            bool path1Direct = false;
            bool clutMemo = false;
            bool stateMemo = false;
            // Planted faults for check (e) (rc_replay --consumer lists --fault adc|clut-memo|state-memo:<cycle>), from
            // fastFaultCycle on: path1Triangle ignores ADC; the CLUT memo ignores memory writes; the state memo ignores
            // register writes and memory writes (bytes alone).
            enum FastFault : uint8_t
            {
                kFastFaultNone = 0,
                kFastFaultAdc,
                kFastFaultClutMemo,
                kFastFaultStateMemo,
                kFastFaultCull,    // (batch 5) gsTriBounds' first pixel rounded down instead of up (a wider box: fewer culled)
            };
            uint8_t fastFault = kFastFaultNone;
            int64_t fastFaultCycle = 0;
        };
        struct Stats
        {
            uint64_t cycles = 0, kicks = 0, path1Kicks = 0, keyframesLoaded = 0;
            uint64_t regWrites = 0, vertices = 0, verticesNoKick = 0, xyzfVertices = 0;
            uint64_t prims = 0, primsByKind[4] = {}, clears = 0;
            uint64_t dropped = 0, droppedFbw0 = 0, droppedUnknown = 0, droppedGuard = 0;
            uint64_t stateCmds = 0, textureCmds = 0, textureBytes = 0, primCmds = 0, scopeCmds = 0;
            uint64_t guardPrims[kGuardCount] = {};
            uint64_t zAbove24 = 0;        // vertices with Z >= 2^24 under a 24- or 16-bit Z buffer
            uint64_t neededSeeded[kGsGroupCount] = {}, neededDefault[kGsGroupCount] = {};
            uint64_t neededSeededNotDefault[kGsGroupCount] = {}; // ... seeded with a value the 3.1 default does not give
            uint64_t neededBeforeFirstClear = 0; // primitives that needed a seeded or default register before the dump's first clear
            uint64_t cldOther = 0;        // indexed textures drawn with TEX0.CLD other than 1 (the cache reads the palette from memory)
            uint64_t sideRegs[0x80] = {}; // writes of registers outside GsState a draw could read (SCANMSK, DIMX, TEXCLUT, FOGCOL, FOG, TEX2)
            uint64_t sideRegsNonZero = 0; // ... draws while SCANMSK, or DIMX with DTHE, were not neutral
            uint64_t walkerDropped = 0, absentPages = 0;
            uint64_t kicksWithTransfersAndDraws = 0;
            // (step 8) PATH1
            uint64_t path1Packets = 0, path1Bytes = 0, path1OpenTags = 0, path1Transfers = 0, path1Guarded = 0;
            uint64_t culledZeroArea = 0, culledOffscreen = 0;
            uint64_t clutLoads = 0; // TEX0 writes that loaded the CLUT buffer
            uint64_t vertexQNotRgbaq = 0; // vertices whose Q (the reference's) is not the RGBAQ register's
            uint64_t path1PrimsLeftOut = 0; // Options::path1Prims false: PATH1 primitives not put in the list
            uint64_t frameUploads = 0, frameUploadBlits = 0, frameUploadsOther = 0; // Options::frameUploads
            // (P2 step 9 batch 2) Options::path1Direct, clutMemo, stateMemo
            uint64_t path1Triangles = 0;       // triangle packets put into the list by path1Triangle
            uint64_t path1FastTriangles = 0;   // ... of them handed over by a fast path without a packet (feedPath1Triangle)
            uint64_t path1TrianglesWalked = 0; // ... recognised but sent through the walker (queue not empty, PRIM not a
                                               // triangle list, ADC on the third corner, a planted register fault)
            uint64_t path1Flushes = 0;         // NLOOP 0 packets dropped
            uint64_t path1Walked = 0;          // other packets, through the walker
            uint64_t clutMemoHits = 0, stateMemoHits = 0;
            uint64_t clutLoadsFromMemo = 0; // CLUT loads whose values the load memo held (the rest read memory)
        };

        Gif2dReader();
        explicit Gif2dReader(Options options);

        // Consumer
        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        const DrawList &list() const { return m_list; }
        uint32_t kickFirstCommand() const { return m_kickFirstCmd; }
        // The last kick: primitives it drew (or dropped), image bytes and transfers it carried.
        uint32_t kickPrims() const { return m_kickPrims; }
        uint32_t kickDropped() const { return m_kickDropped; }
        bool kickHadTransfers() const { return m_kickTransfers != 0; }

        const uint64_t *regs() const { return m_regs; }
        const uint8_t *provenance() const { return m_prov; }
        bool known(uint8_t addr) const { return m_prov[addr] != kProvUnknown; }
        const Stats &stats() const { return m_stats; }
        TexShadow &shadow() { return m_shadow; }
        TextureCache &cache() { return m_cache; }

        // Texture ids are kept for the life of the reader; sent generations too unless forgotten.
        void forgetSentTextures() { m_sentGeneration.clear(); }

        // The GsState the next draw would have (for tests).
        GsState currentState();

        // Tests (rc_gs2d_test): start on blank GS memory without a keyframe, and walk PATH3 bytes directly (a kick of
        // their own: register writes, transfers and draws, in the current cycle's list).
        void startBlank();
        void feedPath3(const uint8_t *data, uint32_t bytes);

        // (step 8) PATH1: one VIF1 kick's XGKICK packets, in order. `k` (or null) for the cycle and the counters.
        void beginPath1Kick();
        void feedPath1(const uint8_t *data, uint32_t bytes);
        // guarded: the VIF1 reader stopped the kick (the registers PATH1 writes become unknown).
        void endPath1Kick(bool guarded);
        // (P2 step 9 batch 2) One triangle packet's nine data quadwords (36 words: per corner ST, RGBAQ, XYZ2 as PACKED
        // data) straight into the register file, the vertex queue and the list, as the walker would put them (RENDERER_P2_DESIGN.md
        // 4.4). False, with nothing changed: the caller must send the packet through the walker (the vertex queue is not
        // empty, PRIM is not a triangle list, the third corner has ADC, or a planted register fault is on in this cycle).
        bool path1Triangle(const uint32_t *nine);
        // (P2 step 9 batch 3) A fast path's triangle (render/shape): the nine data quadwords of a packet whose tag the
        // path found to be the triangle tag with EOP (160 bytes), taken as feedPath1 would take that packet with
        // path1Direct. False, with nothing changed: the caller sends the 160-byte packet through feedPath1.
        bool feedPath1Triangle(const uint32_t *nine);
        // A side tag's begin or end as a scope command (PATH1's spans: FrameReader places them between the runs).
        void scope(const rc::rec::Span &s);
        // The current cycle (for a caller's planted faults).
        uint64_t cycle() const { return m_cycle; }

    private:
        // GifSink (through the shadow's observer)
        void reg(uint8_t addr, uint64_t value) override;
        void image(const uint8_t *, uint32_t) override {}
        void vertexQ(uint32_t q) override { m_vertexQ = q; }
        uint32_t m_vertexQ = 0x3F800000u; // the reference's Q for the next vertex (GifSink::vertexQ)

        void vertex(uint8_t addr, uint64_t value);
        void primitive(const GsVertex *v, uint32_t n);
        bool buildState(GsState &s, uint16_t &needSeeded, uint16_t &needDefault, uint32_t &dropReason);
        uint64_t value(uint8_t addr, uint16_t group, uint16_t &needSeeded, uint16_t &needDefault, bool &missing);
        void closePrims();
        void flushUploads(); // Options::frameUploads: the logged transfers as IMAGE_BLIT commands
        std::vector<GsMemory::TransferRect> m_xfers;
        void emitState(const GsState &s, const GsPlan &p);
        uint32_t textureId(const CachedTexture *t);
        void feed(const uint8_t *data, uint32_t bytes);

        Options m_opt;
        TexShadow m_shadow;
        TextureCache m_cache;
        Clut m_clut; // (step 8 batch 2) the GS's CLUT buffer, loaded at TEX0 writes
        ClutLoadMemo m_clutLoads; // (P2 step 9 batch 2) Options::clutMemo: palettes read since memory last changed
        DrawList m_list;
        uint64_t m_regs[0x80] = {};
        uint8_t m_prov[0x80] = {};
        bool m_started = false;
        uint64_t m_cycle = 0;
        uint32_t m_cycleRegWrites = 0;
        bool m_seenClear = false;

        // (step 8) PATH1's walker and the sink it feeds (register writes to reg(), transfers to the shadow's memory).
        struct Path1Feed final : GifSink
        {
            Gif2dReader *self = nullptr;
            void reg(uint8_t addr, uint64_t value) override;
            void image(const uint8_t *data, uint32_t bytes) override;
            void vertexQ(uint32_t q) override { self->m_vertexQ = q; }
        } m_path1Feed;
        GifWalker m_path1Walker;
        bool m_inPath1 = false; // between beginPath1Kick and endPath1Kick

        // Primitive assembly.
        GsVertex m_queue[3] = {};
        uint32_t m_queued = 0;

        // State cache: recomputed when a register a draw reads changed or memory changed (texture generations).
        bool m_stateDirty = true;
        uint64_t m_stateSerial = ~0ull;
        GsState m_state{};
        GsPlan m_plan{};
        const CachedTexture *m_stateTexture = nullptr; // the texture of m_state (TEXTURE payload)
        uint16_t m_needSeeded = 0, m_needDefault = 0, m_needSeededOff = 0;
        uint32_t m_dropReason = 0;     // 0, or 1 FBW 0, 2 unknown register, 3 guard
        bool m_haveListState = false;  // the list has a STATE
        GsState m_listState{};

        // The open SCREEN_PRIMS command.
        bool m_primsOpen = false;
        uint32_t m_primsKind = 0;
        uint32_t m_cyclePrims = 0;

        // Textures.
        std::unordered_map<const CachedTexture *, uint32_t> m_textureIds;
        std::vector<uint32_t> m_sentGeneration; // by id; 0 = not sent

        // The kick in progress.
        uint32_t m_kickFirstCmd = 0, m_kickPrims = 0, m_kickDropped = 0, m_kickTransfers = 0;
        const rc::rec::KickView *m_kick = nullptr;
        uint32_t m_nextSpan = 0;

        // (P2 step 9 batch 2) The memos. m_regSerial counts everything that can change the register file or the CLUT
        // buffer other than through the memoised writes themselves.
        uint64_t m_regSerial = 0;
        bool m_clutMemoValid = false;
        uint64_t m_clutMemoTex0 = 0, m_clutMemoTexclut = 0, m_clutMemoWrites = 0;
        bool m_stateMemoValid = false;
        uint64_t m_stateMemoRegSerial = 0, m_stateMemoWrites = 0;
        std::vector<uint8_t> m_stateMemo;
        bool fastFault(uint8_t f) const { return m_opt.fastFault == f && static_cast<int64_t>(m_cycle) >= m_opt.fastFaultCycle; }
        static bool stateOnlyPacket(const uint8_t *data, uint32_t bytes);

        Stats m_stats;
    };
}
