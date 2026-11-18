#pragma once

#include "ps2x/iop/iop_pad.h"

#include <atomic>
#include <cstdint>

namespace ps2_host_input
{
    constexpr uint32_t kPadPorts = 2u;

    void setInputFrameCounter(const std::atomic<uint64_t> *counter); // The clock PS2X_PAD_RECORD and PS2X_PAD_REPLAY use.

    void setFieldCounter(const std::atomic<uint64_t> *counter);
    void sample();
    void releaseMouse();
    [[nodiscard]] ps2x::iop::PadInputState padState(uint32_t port);
}
