#pragma once

// Live 2D under RC_RENDERER=vk (renderer plan P1 step 7 batch 5; RENDERER_2D_DESIGN.md section 11 item 5 and section
// 20, RENDERER_3D_DESIGN.md section 12, RENDERER_FRAME_RECORD.md section 8 "The vk host loop").
//
// Reader (EE thread): an rc::rec::Consumer in the capture's live list (render/record/capture.h addLiveConsumer). It
// drives step 8's FrameReader (VIF1 reader + the C++ VU1 references + the GIF reader: one GS register file across PATH1
// and PATH3, started from reset with the game) with PATH1's primitives left out of the list (K2 is step 8's to draw;
// its register writes are kept, so K1 inherits ZBUF_1, SCISSOR_1, TEST_1... as the GS does), and step 6's FrameBlit (is
// the displayed picture upload-only, and its pixels). At each cycle close the cycle's draw list (the flip's clear,
// TEXTURE uploads, K1) is published to a Mailbox with DISPFB2 and, when FrameBlit says the picture is upload-only
// (logos, movie pictures, the loading splash), that picture.
//
// Presenter (main thread, the vk host's present): every list published since the last frame, in order, is recorded
// through Gs2dVk (render/gs2d) into its two targets (FBP 0x0, 0x50) in the caller's command list; the picture to show is
// target[DISPFB2.FBP] (the top rows of 640 x 256), or the upload-only picture. Never waits for the GPU (the lists of two
// frames may be in flight: Gs2dVk keeps one record buffer a frame slot).
//
// Neither half includes a runtime header; rc_replay's live2d consumer runs the same two classes on a dump.

#include "../blit/frame_blit.h"
#include "../frame/frame_reader.h"
#include "../gs2d/gs2d_vk.h"
#include "../gs2d/gs_state.h"

#include "rhi.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace rc::live
{
    // One closed display cycle.
    struct Published
    {
        uint64_t cycle = 0;
        uint64_t dispfb2 = 0;
        uint32_t fbp = 0, width = 640, height = 240; // the picture: DISPFB2's FBP, 640 x the version's lines
        rc::rd::DrawList list;
        bool uploadOnly = false;
        std::vector<uint8_t> picture; // RGBA8 as stored (alpha raw), width x height, when uploadOnly
        double readerMs = 0.0;        // the reader's time on the EE thread in this cycle
    };

    // The EE thread pushes, the presenter takes everything at once. Nothing is dropped that a later list depends on:
    // past kMaxPending lists the oldest is reduced to its TEXTURE commands, carried into the next one.
    class Mailbox
    {
    public:
        static constexpr size_t kMaxPending = 16;
        void push(Published &&p);
        void take(std::vector<Published> &out); // appends, oldest first
        uint64_t pushed() const { return m_pushed; }
        uint64_t stripped() const { return m_stripped; }

    private:
        std::mutex m_mutex;
        std::deque<Published> m_queue;
        uint64_t m_pushed = 0, m_stripped = 0;
    };

    class Reader final : public rc::rec::Consumer
    {
    public:
        struct Stats
        {
            uint64_t cycles = 0, path1Kicks = 0, path3Kicks = 0, uploadOnly = 0, guarded = 0;
            double ns = 0.0, path1Ns = 0.0, maxCycleNs = 0.0, maxPath1Ns = 0.0;
        };
        // live: start from reset (the game's live reader); false: from the dump's first keyframe (rc_replay).
        Reader(Mailbox &box, bool live);

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        const Stats &stats() const { return m_stats; }
        rc::rd::FrameReader &frame() { return m_frame; }
        // Live: a line on stderr every `every` cycles (0: never) with the reader's time and the K2 kicks.
        void setReportEvery(uint64_t every) { m_reportEvery = every; }

    private:
        using Clock = std::chrono::steady_clock;
        Mailbox &m_box;
        rc::rd::FrameReader m_frame;
        rc::rd::FrameBlit m_blit;
        bool m_live;
        uint32_t m_height = 240;
        double m_cycleNs = 0.0;
        Stats m_stats, m_window;
        uint64_t m_reportEvery = 0, m_windowFirst = 0;
    };

    class Presenter
    {
    public:
        struct Options
        {
            rc::rd::Gs2dVk::BlendMode blend = rc::rd::Gs2dVk::kBlendAuto; // RC_VK_BLEND=ff | read
            bool timestamps = true;
        };
        // The picture to present: rows [0, height) of `texture` (texHeight rows), width 640.
        struct View
        {
            RhiTexture texture{};
            uint32_t width = 0, height = 0, texHeight = 0;
            uint64_t cycle = 0;
            bool upload = false, valid = false;
        };
        struct Stats
        {
            uint64_t frames = 0, lists = 0, framesWithLists = 0, uploadsShown = 0, otherFbp = 0, failures = 0;
            uint64_t gpuSamples = 0, readPrims = 0, barriers = 0, draws = 0;
            double gpuMs = 0.0, maxGpuMs = 0.0;
        };

        Presenter() = default;
        ~Presenter();
        Presenter(const Presenter &) = delete;
        Presenter &operator=(const Presenter &) = delete;

        // Makes Gs2dVk and fills both targets and the depth with zeros: GS memory at the machine's reset.
        bool create(const Options &o, std::string *why);
        // (rc_replay) The targets and depth from a picture of GS memory (a keyframe's GSVR): FBP 0x0 and 0x50 CT32, ZBP 0xa0
        // Z32, 640 x 256. Outside a frame; waits for the GPU.
        bool seedFromMemory(const uint8_t *gsMemory, uint32_t bytes);
        void destroy();
        // In the caller's frame (after rhi_wait_frame): collects the GPU time of the frame slot just recycled, records
        // `lists` (oldest first) into cl and puts the picture to show in a sampled state.
        View record(RhiCommandList cl, std::vector<Published> &lists);
        const View &view() const { return m_view; }
        // The shown picture read back as RGBA8 (width x height). Waits for the GPU; outside a frame (it starts its own).
        bool readShown(std::vector<uint8_t> &rgba);
        const Stats &stats() const { return m_stats; }
        rc::rd::Gs2dVk &gs2d() { return m_vk; }

    private:
        rc::rd::Gs2dVk m_vk;
        Options m_opt;
        View m_view;
        uint32_t m_viewTarget = 0;
        Stats m_stats;
        rc::rd::DrawList m_merged;
        RhiTexture m_picture{};
        uint32_t m_pictureW = 0, m_pictureH = 0;
        RhiState m_pictureState = RHI_STATE_UNDEFINED;
        std::vector<uint8_t> m_pictureCpu; // the shown upload-only picture
        bool m_stamped[RHI_FRAMES_IN_FLIGHT] = {};
        bool m_created = false;
    };

    // Appends `from`'s commands (payloads rebased) to `to`; textureOnly: TEXTURE commands only.
    void appendList(rc::rd::DrawList &to, const rc::rd::DrawList &from, bool textureOnly = false);
}
