#pragma once

// RC (renderer plan P1 step 4, RENDERER_FRAME_RECORD.md section 4.1): the frame record's view of the graphics machine.
//
// Null by default; then every site below is one test of a null pointer and nothing else happens. A program installs a
// table before the runtime starts (RC's render/record/capture.cpp does it from a static initializer when
// RC_FRAME_RECORD is set); the oracle installs nothing. The runtime never drops or changes anything because of a hook:
// the hooks only observe.
//
// Threads: kickBegin/kickTouch/kickEnd/kickDone, hwStore, setCrt and gameFrame are called on the thread making the guest
// store (the EE executor). gifPacket and odd can come from the VU1/GIF threads when PS2X_MTVU=1 or the GS thread.
//
// This header is deliberately not included by ps2_runtime.h, so changing it rebuilds only the runtime files that use it.

#include <cstdint>
#include <vector>

class PS2Runtime;
class GS;

namespace ps2x::capture
{
    // Kinds for `odd`: traffic the GS survey found never happens (RENDERER_SURVEY.md, "Traffic outside the planned kicks").
    enum OddKind : int
    {
        OddVif0Kick = 1,       // a VIF0 DMA start (address: the channel base, value: CHCR)
        OddFifoStore = 2,      // an EE store into a VIF0/VIF1/GIF FIFO (address)
        OddGsDirectWrite = 3,  // GS::writeRegister from a runtime stub (address: the register, value)
        OddNativeGifPath = 4,  // a native GIF fast path (tryProcessNativeGif*) took a channel 2 chain (address: TADR)
        OddPath2 = 5,          // a PATH2 (VIF1 DIRECT/DIRECTHL) packet (value: bytes)
        OddLocalToHost = 6,    // a local->host GS transfer was started (value: TRXDIR)
        OddForeignThread = 7,  // a hook that should run on the EE thread ran on another one (address: what)
    };

    struct Hooks
    {
        // PS2Runtime::initialize finished: the runtime the hooks observe.
        void (*attach)(PS2Runtime *runtime);
        // PS2Memory::writeIORegister, a CHCR store with STR on channel 0/1/2, after the DMAC-enable test. The registers
        // are the values the store found (TADR, ASR0/1 before the walk updates them). callerRa: g_storeCallerRa.
        void (*kickBegin)(uint32_t channelBase, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t asr0,
                          uint32_t asr1, uint32_t callerRa);
        // Each byte range the stream is made of, in stream order. physAddr: offset into RDRAM, or into the scratchpad
        // when `scratchpad`. isTag: a 16-byte DMA tag (of which only the upper 8 bytes enter the stream, and only with
        // CHCR.TTE on a VIF channel; the recorder needs all 16 for a replay's walk).
        void (*kickTouch)(uint32_t physAddr, uint32_t bytes, bool scratchpad, bool isTag);
        // After the walk, before processPendingTransfers. flat: the chain as flattened (null for normal mode).
        void (*kickEnd)(const uint8_t *flat, uint32_t flatBytes, uint32_t dmaTags);
        // After processPendingTransfers returned.
        void (*kickDone)();
        // PS2Memory::submitGifPacket (path: 1 VU1 XGKICK, 2 VIF1 DIRECT, 3 GIF DMA). Any thread.
        void (*gifPacket)(int path, const uint8_t *data, uint32_t bytes);
        // A store to a GS privileged register (value: the register after the store), a VIF1 register (0x10003c00..)
        // or a GIF register (0x10003000..). width: 32 or 64.
        void (*hwStore)(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa);
        void (*setCrt)(int interlaced, int mode, int ffmd);
        void (*odd)(int kind, uint32_t address, uint64_t value);
        void (*stateLoaded)();
        void (*hotkey)(); // F10, from either host loop
        // From the game's GfxP2_SwapBuffers override (RC's overrides): a game frame starts. frame: since boot;
        // levelFrame: since the last level start (as RC_RAMDUMP_FRAME counts); scene: the GS survey's scene id.
        void (*gameFrame)(uint64_t frame, uint64_t levelFrame, uint32_t scene);
    };

    inline const Hooks *g_hooks = nullptr;
    // The guest return address of the hardware store being made (PS2Runtime::Store32/Store64 set it while hooks are
    // installed).
    inline uint32_t g_storeCallerRa = 0u;

    inline void install(const Hooks *hooks) { g_hooks = hooks; }

    // The GS register file as last written, indexed by GS register address (0x00..0x62), in the register's own
    // encoding (PACKED vertex data re-encoded as the A+D form would carry it). Kept by the GS front end
    // (gs_frontend.cpp) at every register write whether or not hooks are installed; one GS per process.
    constexpr uint32_t kGsRegCount = 0x63u;
    extern uint64_t g_gsRawRegs[kGsRegCount];

    // For keyframes (defined in ps2_save_state.cpp, beside the save-state code that already reaches the GS's
    // internals): waits for the VU1/GIF threads and the backend, then copies GS local memory (4 MB, as the backend
    // stores it) and the register file above. False when the GS has no memory yet.
    bool snapshotGs(PS2Runtime &runtime, std::vector<uint8_t> &vram, uint64_t regs[kGsRegCount]);
}
