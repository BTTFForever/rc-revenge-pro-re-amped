#pragma once

// rc_replay's emu consumer (RENDERER_FRAME_RECORD.md section 7): re-submits every recorded store and kick to a
// headless runtime's own VIF1, VU1 and software GS, and with --verify must reproduce every reference picture, every
// kick's GIF output and GS memory at every later keyframe and the final one, bit for bit.

#include "consumer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class PS2Runtime;

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
        } m_t;
        uint64_t m_digest = 0; // all replayed picture hashes, folded: equal digests = identical replays
    };
}
