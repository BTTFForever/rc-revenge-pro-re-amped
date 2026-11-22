#pragma once

// The frame reader (renderer plan P1 step 8; RENDERER_3D_DESIGN.md sections 2, 7, 8): one rc::rec::Consumer that turns
// a display cycle's kicks into one draw list. Channel 1 kicks (K2) go through the VIF1 reader (render/vif), whose
// XGKICK packets feed the GIF reader of step 7 as PATH1; channel 2 kicks (K1, uploads) go to the GIF reader as PATH3.
// So there is one GS register file and one list per cycle: K2's triangles, then K1's 2D. The live reader (step 8
// batch 4) and rc_replay's vif and gif2d consumers use it.
//
// Off-line it seeds the VU1 and VIF1 state from the first keyframe (and one after a save-state load), as the GIF
// reader seeds the shadow of GS memory; live it starts from reset with the game. A checker can sit between the VIF1
// reader and the GIF reader (Path1Tap: sees each run and packet, may drop a packet or replace the state after a run),
// and can take channel 1 kicks out of kick() to hand them over itself (deferPath1: rc_replay runs them once the
// emulated machine has run the same kick).
//
// Uploads into the frame buffers are the GIF reader's IMAGE_BLIT commands (Gif2dReader::Options::frameUploads, step 7
// batch 5), in list order with the draws; the chained replay (step 8 batch 3, rc_replay --consumer vk) draws them with
// everything else, so step 6's FrameBlit stays outside this class (RENDERER_3D_DESIGN.md 19.1 item 4).

#include "../gs2d/gif2d_reader.h"
#include "../vif/vif1_reader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rc::rd
{
    struct Path1Tap
    {
        virtual void runBegin(const Vu1Run &) {}
        // One XGKICK packet of the reader's; false: not passed on to the GIF reader (nor collected).
        virtual bool packet(const uint8_t *data, uint32_t bytes) = 0;
        virtual void runEnd(const Vu1Run &, uint8_t * /*vuData*/, float (*/*vf*/)[4]) {}
        // (P2 step 9 batch 3) True: the tap does not need the fast paths' triangle packets as packets (they go into the
        // list as nine quadwords without passing packet()). False (the default): every packet is made and passed.
        virtual bool fastTriangles() const { return false; }
        // (P2 step 9 batch 5) True (the default): runBegin and runEnd read VU data memory, so the VIF1 reader keeps no TOPS
        // views in the kick. False: they read the registers only.
        virtual bool readsVuMemory() const { return true; }
        virtual ~Path1Tap() = default;
    };

    class FrameReader : public rc::rec::Consumer
    {
    public:
        struct Options
        {
            Gif2dReader::Options gif;
            bool deferPath1 = false;   // kick() leaves channel 1 kicks to kickPath1()
            bool collectPath1 = false; // keep each channel 1 kick's PATH1 bytes (path1Bytes())
            // (P2 step 9 batch 2, RENDERER_P2_DESIGN.md 4) The fast reader: the lean references (Vif1Reader::Options::library),
            // the direct entries (directEntries), triangle packets straight into the list and flush packets dropped
            // (Gif2dReader::Options::path1Direct), the CLUT and state-packet memos; no PATH1 hash (path1Hash() stays 0:
            // D-P2-3). Its lists are the strict reader's byte for byte (check (e), rc_replay --consumer lists). Off: the
            // strict reader of P1, unchanged.
            bool fast = false;
            // (P2 step 9 batch 3, RENDERER_P2_DESIGN.md 4.2) With fast: the fast paths (render/shape; Vif1Reader::Options::
            // fastPaths), their quick triangles straight into the list. Off: batch 2's fast reader (to time the paths).
            bool fastPaths = true;
            rc::shape::FastFault fastFault = rc::shape::FastFault::None; // planted (check (e)), from fastFaultCycle on
            uint8_t fastFaultPrograms = 0xFF;                           // ... in these programs' paths (bit per rc::vu1::Program)
            int64_t fastFaultCycle = 0;
            // (P2 step 9 batch 5, RENDERER_P2_DESIGN.md 4.5) With fast and fastPaths: TOPS views (Vif1Reader::Options::views)
            // and no hash of the VIF1 stream in the chain walk. Off: batch 4's fast reader (to time the views).
            bool views = true;
        };
        struct Stats
        {
            uint64_t path1Kicks = 0, path3Kicks = 0, guardedKicks = 0, seeds = 0, seedMissing = 0;
            uint64_t packetsDropped = 0; // by the planted fault
        };

        FrameReader();
        explicit FrameReader(const Options &options);

        // (step 7 batch 5) Live: start from the machine's reset state with the game (RENDERER_3D_DESIGN.md 5.1), no
        // keyframe: VU1 and VIF1 reset, GS memory blank, every register unknown until written.
        void startLive();

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        // A channel 1 kick through the VIF1 reader into the GIF reader. False: a guard stopped it (guardText()).
        bool kickPath1(const rc::rec::KickView &k, const rc::rec::RamView &ram, uint32_t mxcsr);
        void setPath1Tap(Path1Tap *tap) { m_tap = tap; }

        // The last channel 1 kick: its PATH1 bytes as passed on (hash, count; the bytes and packet sizes when collected).
        uint64_t path1Hash() const { return m_path1HashValue; }
        uint64_t path1ByteCount() const { return m_path1Count; }
        const std::vector<uint8_t> &path1Bytes() const { return m_path1Bytes; }
        const std::vector<uint32_t> &path1Packets() const { return m_path1Packets; }
        const std::string &guardText() const { return m_vif.lastGuardText(); }

        // Planted faults (the VIF1 reader's, and VifFault::Packet here).
        void setFault(VifFault f, int64_t cycle, uint32_t n);

        Gif2dReader &gif() { return m_gif; }
        const Gif2dReader &gif() const { return m_gif; }
        Vif1Reader &vif() { return m_vif; }
        const Vif1Reader &vif() const { return m_vif; }
        const DrawList &list() const { return m_gif.list(); }
        // (step 8 batch 4) The cycle's channel 1 kicks as command ranges of list(): begin, end pairs, in kick order (the
        // live presenter times K2's draws on the GPU by them).
        const std::vector<uint32_t> &path1Ranges() const { return m_path1Ranges; }
        const Stats &stats() const { return m_stats; }
        bool started() const { return m_started; }

    private:
        struct Bridge final : Path1Sink
        {
            FrameReader *self = nullptr;
            void runBegin(const Vu1Run &run) override;
            void packet(const uint8_t *data, uint32_t bytes) override;
            bool triangle(const uint32_t *nine) override;
            void runEnd(const Vu1Run &run, uint8_t *vuData, float (*vf)[4]) override;
            bool readsVuMemory() const override;
        } m_bridge;

        static Options resolve(Options o); // fast: the GIF reader's options it implies
        Options m_opt;
        Gif2dReader m_gif;
        Vif1Reader m_vif;
        Path1Tap *m_tap = nullptr;
        bool m_started = false;
        uint64_t m_cycle = 0;
        rc::rec::Hash64 m_path1Hash;
        uint64_t m_path1HashValue = 0, m_path1Count = 0;
        std::vector<uint8_t> m_path1Bytes;
        std::vector<uint32_t> m_path1Packets;
        std::vector<uint32_t> m_path1Ranges;
        // The kick's side tags in stream order, placed as scope commands before the run at or after their offset.
        const rc::rec::KickView *m_kick = nullptr;
        std::vector<uint32_t> m_spanOrder;
        uint32_t m_nextSpan = 0;
        void scopesUntil(uint32_t offset);
        VifFault m_fault = VifFault::None;
        int64_t m_faultCycle = -1;
        uint32_t m_faultN = 0, m_faultSeen = 0;
        Stats m_stats;
    };
}
