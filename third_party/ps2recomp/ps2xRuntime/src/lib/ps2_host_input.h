#pragma once

#include "ps2x/iop/iop_pad.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace ps2_host_input
{
    constexpr uint32_t kPadPorts = 2u;

    void setInputFrameCounter(const std::atomic<uint64_t> *counter); // The clock PS2X_PAD_RECORD and PS2X_PAD_REPLAY use.

    void setFieldCounter(const std::atomic<uint64_t> *counter);
    void sample();
    void releaseMouse();
    [[nodiscard]] ps2x::iop::PadInputState padState(uint32_t port);
    // The game's DualShock 2 motors on a port (from the emulated pad or libpad's actuator call): to its gamepad.
    void setPadVibration(uint32_t port, uint8_t small, uint8_t large);
    // Test aid: a PS2X_PAD_LIVE line "shot <name>" asked for a screenshot (PS2X_SCREENSHOT_DIR/<name>.png).
    bool takeScreenshotRequest(std::string &name);
}
