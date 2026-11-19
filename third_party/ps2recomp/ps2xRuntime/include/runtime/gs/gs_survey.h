#ifndef PS2_GS_SURVEY_H
#define PS2_GS_SURVEY_H

// GS usage survey (RC renderer plan, P0 step 1). Off unless RC_GS_SURVEY=<file> is set; then every DMA kick, every
// GIF packet (by path), every GS privileged register write and every GS register write that bypasses the GIF are
// parsed and summarised into <file> (a stream of kick, register and per-frame lines) and <file>.agg (distinct
// register values, draw states, textures and uploads per scene and path, rewritten every few seconds).
// tools/gs_survey.py reads both. When the variable is unset every hook is one test of a false global.
//
//   RC_GS_SURVEY_DETAIL=<n>  write per-kick lines for the first <n> frames of each scene and every <n>th frame after
//                            (default 30; 0 = every frame)

#include <atomic>
#include <cstdint>

namespace ps2x::gssurvey
{
    namespace detail
    {
        extern bool g_enabled;
    }

    inline bool enabled() { return detail::g_enabled; }

    // Where GS data came from. Path1 = VU1 XGKICK, Path2 = VIF1 DIRECT/DIRECTHL, Path3 = GIF DMA (channel 2),
    // EeDirect = a runtime stub writing a GS register itself (no GIF transfer at all).
    enum Path : int
    {
        Path1 = 1,
        Path2 = 2,
        Path3 = 3,
        EeDirect = 4,
        kPaths = 5
    };

    // A GIF packet entering the GS through the arbiter (or a native fast path).
    void gifPacket(int path, const uint8_t *data, uint32_t sizeBytes);
    // An image upload the runtime decoded itself (native GIF fast path): its four registers and the pixel bytes.
    void nativeImage(int path, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir, uint32_t bytes);
    // GS::writeRegister called by a runtime stub (not through a GIF packet).
    void eeRegisterWrite(uint8_t reg, uint64_t value);

    // A DMA channel start (CHCR.STR written). Call chainTag for each tag the runtime's chain walk reads first.
    void chainTag(uint32_t id, uint32_t addr, uint32_t qwc);
    void dmaKick(uint32_t channelBase, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t bytes);

    // A store to a GS privileged register (0x12000000..): the register's offset and its new 64-bit value.
    void privWrite(uint32_t regOffset, uint64_t value);
    // An EE store straight into a FIFO (VIF0 0x10004000, VIF1 0x10005000, GIF 0x10006000).
    void fifoWrite(uint32_t address);
    // SetGsCrt (the kernel's display mode call).
    void setCrt(int interlaced, int videoMode, int frameMode);
    // The guest return address of the next hardware store (for labelling kicks).
    void noteStoreCaller(uint32_t ra);

    // From the game: a frame boundary (its buffer swap), with a scene id and the game's own buffer counters.
    // Counters are written as given, in order, and named by the caller once through setCounterNames.
    void frameBoundary(uint32_t frame, uint32_t scene, const uint32_t *counters, int counterCount);
    void setCounterNames(const char *names); // comma-separated
    // A named guest address range; DMA call tags into it are counted per kick (e.g. the state packet ring).
    // Index 0..15; ranges named k1buf* label channel 2 kicks from them K1, a range named chain labels channel 1 K2.
    void setRange(int index, const char *name, uint32_t lo, uint32_t hi);
    // A movie picture was decoded (sets the scene's movie flag for a few fields).
    void markMovie();
    // The vertical blank counter, read for each record's field number.
    void setFieldCounter(const std::atomic<uint64_t> *counter);
}

#endif
