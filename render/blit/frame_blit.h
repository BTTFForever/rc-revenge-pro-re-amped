#pragma once

// Uploads into the frame buffer and the flip (renderer plan P1 step 6; RENDERER_FRAME_RECORD.md section 18): the
// displayed picture built from host-to-local transfers into GS memory and the display registers, with no GS
// rasteriser. RC shows pictures that no draw touched in three places, all uploads on DMA channel 2 (survey,
// "Uploads"): the logos (BackdropDisplayBMP24, two 640x128 CT32 halves), movie pictures and the loading splash
// (ldimage.cpp: one IMAGE per 16x16 CT32 tile, 600 tiles a 640x240 picture, into FBP 0x0 or 0x50), flipped through
// DISPFB2 by sceGsSwapDBuff or, for movies and the splash, sceGsPutDispEnv from the vertical blank handler.
//
// FrameBlit is an rc::rec::Consumer. It keeps the shadow of GS memory (render/tex TexShadow, GsMemory with word
// origins on) and DISPFB2, and at each cycle's close reads the rectangle DISPFB2 shows (FBP, FBW, PSM CT32, DBX/DBY;
// DISPLAY2's position and size are ignored: Europe's screen-adjust option moves it, movies set 512 lines) into RGBA8
// as stored (alpha raw, 0x80 opaque): the frame record's reference picture (section 3.5) when nothing drew there.
//
// Whether something drew there is answered without rasterising, conservatively, per 32-bit word of GS memory:
//   - a PATH3 vertex kick (XYZ2/XYZF2) marks the pages of the drawing context's frame buffer (FRAME, its scissor
//     rectangle, unless FBMSK masks every bit) and of its Z buffer (unless ZMSK) as drawn, the context taken from
//     PRIM or PRMODE as PRMODECONT says; when one of those registers is not known (not written on PATH3 since the
//     keyframe or since a VIF1 kick, which may have written it), the whole of GS memory;
//   - a VIF1 kick (K2, PATH1: its GIF output needs the VU1 programs, step 8) marks the whole of GS memory as drawn;
//   - a CT32 transfer makes the words it writes uploaded again (GsMemory::trackOrigins).
// A picture with no drawn word is "upload-only" and must equal the reference bit for bit; a picture with one is not
// this step's (steps 7-9 draw it). The live renderer can use the same answer to present an uploaded picture directly.
// Depends on rc_tex and rc_record's consumer interface; no Vulkan (blit_presenter.h is the GPU half).

#include "../record/consumer.h"
#include "../tex/gif_walk.h"
#include "../tex/gs_memory.h"
#include "../tex/tex_shadow.h"

#include <cstdint>
#include <vector>

namespace rc::rd
{
    // DISPFB1/2 decoded.
    struct DisplayBuffer
    {
        uint32_t fbp = 0, fbw = 0, psm = 0, dbx = 0, dby = 0;
        static DisplayBuffer decode(uint64_t v)
        {
            DisplayBuffer d;
            d.fbp = static_cast<uint32_t>(v & 0x1FFu);
            d.fbw = static_cast<uint32_t>((v >> 9) & 0x3Fu);
            d.psm = static_cast<uint32_t>((v >> 15) & 0x1Fu);
            d.dbx = static_cast<uint32_t>((v >> 32) & 0x7FFu);
            d.dby = static_cast<uint32_t>((v >> 43) & 0x7FFu);
            return d;
        }
    };

    class FrameBlit : public rc::rec::Consumer, private GifSink
    {
    public:
        struct Picture
        {
            uint32_t width = 0, height = 0;
            uint64_t dispfb2 = 0;
            DisplayBuffer fb;
            bool supported = false;          // DISPFB2 in a format the blit reads (CT32, CT24)
            std::vector<uint8_t> rgba;       // width * height * 4, R G B A as stored
            uint32_t drawnWords = 0, uploadedWords = 0, loadedWords = 0;
            bool uploadOnly() const { return supported && drawnWords == 0u; }
        };
        struct CycleCounts
        {
            uint32_t transfers = 0, transfersIntoFrame = 0;  // host-to-local transfers begun; of them into frame memory
            uint32_t path3Vertices = 0, path3BlindDraws = 0; // drawing vertex kicks; of them with an unknown context
            uint32_t vif1Kicks = 0;
            bool drew() const { return path3Vertices || vif1Kicks; }
        };
        struct Stats
        {
            uint64_t cycles = 0, keyframesLoaded = 0, path3Vertices = 0, path3BlindDraws = 0, vif1Kicks = 0;
            uint64_t regChecks = 0, regMismatches = 0; // tracked PATH3 registers against a kick's recorded file
        };

        FrameBlit();

        // Consumer
        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override; // builds picture()

        // The picture DISPFB2 shows now, width x height (cycleEnd calls it with the cycle's reference size).
        void buildPicture(uint32_t width, uint32_t height);
        const Picture &picture() const { return m_picture; }
        const CycleCounts &cycleCounts() const { return m_cycle; }
        const Stats &stats() const { return m_stats; }
        uint64_t dispfb2() const { return m_dispfb2; }
        bool started() const { return m_started; }

        GsMemory &memory() { return m_shadow.memory(); }
        const GsMemory &memory() const { return m_shadow.memory(); }
        TexShadow &shadow() { return m_shadow; }

        // For planted faults: the DISPFB2 value the next picture is read at.
        void setDispfb2ForTests(uint64_t v) { m_dispfb2 = v; }

    private:
        void reg(uint8_t addr, uint64_t value) override;     // PATH3 register writes other than the transfers
        void image(const uint8_t *, uint32_t) override {}
        void drawAt(); // a PATH3 vertex kick
        void markRect(uint32_t basePage, uint32_t fbw, uint32_t pageH, uint64_t scissor);

        TexShadow m_shadow;
        bool m_started = false;
        uint64_t m_dispfb2 = 0;
        uint32_t m_width = 640, m_height = 240;
        uint64_t m_regs[0x80] = {};
        bool m_known[0x80] = {};
        uint32_t m_transfersAtCycle = 0, m_intoFrameAtCycle = 0;
        CycleCounts m_cycle;
        Stats m_stats;
        Picture m_picture;
    };
}
