#pragma once

// rc_replay's emu consumer (RENDERER_FRAME_RECORD.md section 7): re-submits every recorded store and kick to a
// headless runtime's own VIF1, VU1 and software GS, and with --verify must reproduce every reference picture, every
// kick's GIF output and GS memory at every later keyframe and the final one, bit for bit.

#include "consumer.h"
#include "emu_state.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class PS2Runtime;

namespace rc::rec
{
    class Writer;
}

namespace rc::replay
{
    struct EmuOptions
    {
        bool verify = false, quiet = false;
        uint64_t pngFirst = 0;      // --cycles a-b: pictures are written from cycle a on
        std::string pngDir;         // write the replayed pictures (and on a mismatch the reference and a difference)
        int64_t beforeCycle = -1;   // --picture-before <cycle>:<kick> <out.png>
        uint32_t beforeKick = 0;
        std::string beforeOut;
        int64_t path1Cycle = -1;    // --path1 <cycle>:<kick> <out.bin>
        uint32_t path1Kick = 0;
        std::string path1Out;
        bool kickHashes = false;    // --kick-hashes: print a GS memory hash after every kick (bisection)
        // --fault <kind>:<cycle>[:<n>] plants one fault, to show the checks fail at the right cycle: page (flip byte n,
        // default 2048, of the first page of the cycle's first PAGE event), kick (drop kick n), hw (the n-th store of
        // the cycle with its lowest bit flipped).
        std::string faultKind;
        int64_t faultCycle = -1;
        uint32_t faultN = 0;
        // (step 8 batch R) --rereference: keyframes after the first (except those after a save-state load) are not
        // loaded: the machine runs on, and its own state is taken there (Emu::keyState) for the re-referenced copy.
        bool rereference = false;
    };

    // Sets the environment the emu machine needs (bare software GS, no VU1/GIF or GS thread). Call before the runtime
    // is constructed.
    void prepareEmuEnvironment();

    class Emu final : public rc::rec::Consumer
    {
    public:
        Emu(PS2Runtime &runtime, EmuOptions options);
        ~Emu() override;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void pages(uint32_t firstPage, uint32_t count, const uint8_t *data) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;
        void close() override;

        void gifPacket(int path, const uint8_t *data, uint32_t bytes);
        // (step 5) Every GIF packet a replayed kick submits, with its path, for a consumer running behind this one.
        void setGifTap(std::function<void(int, const uint8_t *, uint32_t)> tap) { m_gifTap = std::move(tap); }
        // (step 7) A consumer in front of this one can replace a kick: when the function returns bytes, they are
        // submitted to the GS as PATH3 packets (under the kick's MXCSR) in place of the DMA transfer, and the kick's
        // recorded GIF hash is not checked (it is counted as substituted). Everything else is verified as usual.
        void setKickSubstitute(std::function<const std::vector<uint8_t> *(const rc::rec::KickView &)> f) { m_substitute = std::move(f); }
        // (step 7 batch 3, check (c)) Called after the machine has run a kick (substituted or not), so a consumer in
        // front of this one can read GS memory before (in its own kick()) and after it.
        void setAfterKick(std::function<void(const rc::rec::KickView &)> f) { m_afterKick = std::move(f); }
        // (step 8, RENDERER_3D_DESIGN.md 11.3) Called for every channel 1 kick once the machine has run its DMA (VIF1,
        // VU1), with the kick's RAM view. When it returns bytes they are submitted to the GS as PATH1 packets (one
        // submitGifPacket per packet size) under the kick's MXCSR, the mode the machine's VU1 hands its packets over in
        // since batch R (RENDERER_3D_DESIGN.md section 13), after whatever the machine itself sent (the caller diverts the machine's packets when it
        // substitutes). With checkHash the recorded GIF hash is checked against everything the GS received as usual;
        // otherwise the kick counts as substituted.
        struct Path1Substitute
        {
            const std::vector<uint8_t> *bytes = nullptr;
            const std::vector<uint32_t> *packets = nullptr; // sizes; null: one packet
            bool checkHash = true;
        };
        void setPath1Substitute(std::function<Path1Substitute(const rc::rec::KickView &, const rc::rec::RamView &)> f) { m_path1Sub = std::move(f); }

        // (batch R) What the replaying machine made, for the Rereference consumer behind this one: the register file
        // before the last kick and its GIF output; the last cycle's picture; the state at the last keyframe.
        const uint64_t *lastRegs() const { return m_lastRegs; }
        uint64_t lastGifHash() const { return m_lastGifHash; }
        uint64_t lastGifBytes() const { return m_lastGifBytes; }
        const std::vector<uint8_t> &lastPicture() const { return m_picture; }
        uint64_t lastPictureHash() const { return m_lastPictureHash; }
        uint32_t lastFbp() const { return m_lastFbp; }
        const rc::rec::KeyframeState *keyState() const { return m_keyCaptured ? &m_keyState : nullptr; }

        bool fatal() const { return !m_fatal.empty(); }
        const std::string &fatalError() const { return m_fatal; }
        bool failed() const { return m_failures != 0; }
        void summary() const;

    private:
        void fail(const std::string &what);
        void comparePictures(const rc::rec::CycleInfo &info, const uint8_t *replayed, const uint8_t *ref, uint32_t w, uint32_t h);

        PS2Runtime &m_rt;
        EmuOptions m_opt;
        rc::rec::FileHeader m_header{};
        bool m_exact = false, m_started = false;
        std::string m_fatal;
        uint64_t m_failures = 0;
        std::vector<uint8_t> m_picture;

        bool m_collect = false;
        std::function<void(int, const uint8_t *, uint32_t)> m_gifTap;
        std::function<const std::vector<uint8_t> *(const rc::rec::KickView &)> m_substitute;
        std::function<void(const rc::rec::KickView &)> m_afterKick;
        std::function<Path1Substitute(const rc::rec::KickView &, const rc::rec::RamView &)> m_path1Sub;
        uint64_t m_gifBytes = 0;
        rc::rec::Hash64 m_gif;
        bool m_path1 = false;
        std::vector<uint8_t> m_path1Bytes;

        uint64_t m_cycle = 0;
        uint32_t m_cyclePages = 0, m_cycleHw = 0;
        bool m_faultDone = false;
        uint64_t m_cycleFirstFail = ~0ull;
        struct Totals
        {
            uint64_t cycles = 0, kicks = 0, gifChecked = 0, gifOk = 0, gifBytes = 0, regsChecked = 0, regsOk = 0;
            uint64_t refHashChecked = 0, refHashOk = 0, picturesChecked = 0, picturesOk = 0;
            uint64_t keyframesChecked = 0, keyframesOk = 0, finalChecked = 0, finalOk = 0, keyframesLoaded = 0, keyframesAfterLoad = 0;
            uint64_t hwApplied = 0, pagesApplied = 0;
            uint64_t substituted = 0, substitutedBytes = 0, path1Substituted = 0, path1SubstitutedBytes = 0, path1HashChecked = 0;
        } m_t;
        uint64_t m_digest = 0; // all replayed picture hashes, folded: equal digests = identical replays
        uint64_t m_lastRegs[rc::rec::kGsRegCount]{};
        uint64_t m_lastGifHash = 0, m_lastGifBytes = 0, m_lastPictureHash = 0;
        uint32_t m_lastFbp = 0;
        rc::rec::KeyframeState m_keyState;
        bool m_keyCaptured = false;
    };

    // (step 8 batch R, RENDERER_3D_DESIGN.md section 13) rc_replay <dump> --consumer emu --rereference <out>: behind the
    // emu consumer, writes a copy of the dump in which everything that depends on the GS's output (or on VU1 runs)
    // is the replaying machine's: every KICK's register file and GIF hash and length, every END.refHash, every REFI
    // picture, the state chunks of every keyframe but the first and those after a save-state load (GSVR, GSRG, VU1M,
    // VU1R, VIF1, EMUS; GSPR is kept: stores and the vertical blank make it, not the GS). Pages, kicks, stores, tags,
    // CRT and odd events, TNAM and TAGP are copied; the header
    // gets kFileRereferenced and the first cycle a NOTE naming the build. Counts what changed.
    class Rereference final : public rc::rec::Consumer
    {
    public:
        Rereference(const Emu &emu, const std::string &out, const std::string &source);
        ~Rereference() override;
        bool ok() const;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void pages(uint32_t firstPage, uint32_t count, const uint8_t *data) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void crt(int interlaced, int mode, int ffmd) override;
        void tag(const rc::rec::TagView &tag) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void odd(int kind, uint32_t address, uint64_t value) override;
        void note(const std::string &text) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;
        void close() override;
        void summary() const;

    private:
        const Emu &m_emu;
        std::unique_ptr<rc::rec::Writer> m_writer;
        std::string m_source;
        bool m_noteDone = false;
        struct Totals
        {
            uint64_t kicks = 0, gifChanged = 0, gifBytesChanged = 0, regsChecked = 0, regsChanged = 0, regValuesChanged = 0;
            uint64_t cycles = 0, refHashes = 0, refHashChanged = 0, fbpChanged = 0, pictures = 0, picturesChanged = 0;
            uint64_t pixels = 0, pixelsChanged = 0;
            uint64_t hist[4][7] = {}; // by channel R, G, B, A: -3 or less, -2, -1, +1, +2, +3 or more, (unused)
            uint64_t keyframes = 0, keyframesReplaced = 0, keyframesKept = 0;
            uint64_t chunkChanged[8] = {}, gsvrBytesChanged = 0, gsprField[20] = {};
        } m_t;
    };
}
