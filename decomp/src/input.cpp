// Decompiled from INPUT.c: the two pads (libpad, still called as guest code), their button bytes and the vibration.
//
// Each of the 32 keys has a byte in activeBuffer (written by InputHandler as the pads are read) and in inactiveBuffer
// (the copy InputSnapshot takes once a frame, which the game reads): bit 0 held, bit 1 went down since the snapshot
// (cleared by InputLatched), bit 5 hit (cleared by InputHit). Keys 0-15 are pad 1's, 16-31 pad 2's. InputHandler runs
// on the main thread (from InputSnapshot), not on an interrupt.
//
// Written from the instructions. InputSetVibrationFlags, InputHeld and InputPressureHit are in ai.cpp. Offline the
// setups (input_fuzz.cpp) put a scripted pad in place of the runtime's libpad stub, so the pads' answers are varied and
// every pad call and its arguments are logged into scratch and compared.

#include "input_fuzz.h"

#include <cstring>
#include <optional>

namespace rc {

namespace {

// libpad (the runtime's stubs) and printf.
constexpr uint32_t kScePadInit = 0x1c64f0u, kScePadEnd = 0x1c6698u, kScePadPortOpen = 0x1c6718u, kScePadPortClose = 0x1c6870u;
constexpr uint32_t kScePadRead = 0x1c6968u, kScePadGetState = 0x1c69f8u, kScePadInfoMode = 0x1c6dd8u;
constexpr uint32_t kScePadSetMainMode = 0x1c6ee0u, kScePadSetActDirect = 0x1c6f90u, kScePadSetActAlign = 0x1c7028u;
constexpr uint32_t kScePadInfoPressMode = 0x1c7218u, kScePadEnterPressMode = 0x1c7248u, kScePadExitPressMode = 0x1c7268u;
constexpr uint32_t kPrintf = 0x12e9e8u;
// printf formats: "scePadInit() failed\n" and the next five, the port open and close messages.
constexpr uint32_t kPadInitFailed = 0x3248f8u, kPort0OpenFailed = 0x324910u, kPort1OpenFailed = 0x324930u;
constexpr uint32_t kPort0CloseFailed = 0x324950u, kPort1CloseFailed = 0x324970u, kPadEndFailed = 0x324990u;
// The pads' DMA buffers, and the port tables: int32[2] (vibration), uint16[2] (main mode), int32[8] (by eInputPad).
constexpr uint32_t kPadDma0 = 0x1f4dc0u, kPadDma1 = 0x1f4ec0u;
constexpr uint32_t kVibratePorts = 0x328268u, kModePorts = 0x328270u, kPadPorts = 0x3249a8u;

constexpr int32_t kPadStable = 6, kPadFindCtp1 = 2;

inputClass &in() { return G(inputVarG); }
uint32_t inAddr(uint32_t offset) { return guest_addr(&G(inputVarG)) + offset; }

bool padReady(int32_t state) { return state == kPadStable || state == kPadFindCtp1; }

// One pad's 16 buttons (a bit clear is pressed) into its 16 key bytes.
void readButtons(uint8 *keys, uint16_t buttons)
{
    for (uint32_t i = 0; i < 16; ++i)
    {
        uint8 &key = keys[i];
        if ((buttons & (1u << i)) == 0)
        {
            if ((key & 1) == 0)
            {
                key = static_cast<uint8>(key | 0x20);
                key = static_cast<uint8>(key | 0x23);
            }
            else
                key = static_cast<uint8>(key | 3);
        }
        else
            key = static_cast<uint8>(key & 0xfe);
    }
}

// The pressure bytes of a pressure-mode pad (0x79): its buffer bytes 8-19 into the pad's 16 pressure keys.
void readPressure(uint8 *pressure, const uint8 *buffer)
{
    pressure[4] = buffer[10];
    pressure[6] = buffer[11];
    pressure[7] = buffer[9];
    pressure[5] = buffer[8];
    pressure[12] = buffer[12];
    pressure[14] = buffer[14];
    pressure[15] = buffer[15];
    pressure[13] = buffer[13];
    pressure[10] = buffer[16];
    pressure[8] = buffer[18];
    pressure[11] = buffer[17];
    pressure[9] = buffer[19];
    pressure[1] = 0;
    pressure[2] = 0;
    pressure[0] = 0;
    pressure[3] = 0;
}

// The port of an eInputPad, from the 8-entry table (the original copies it to its stack and reads the low half).
uint16_t padPort(eInputPad ePad)
{
    return *static_cast<uint16 *>(guest_host(kPadPorts + static_cast<uint32_t>(ePad) * 4u));
}

} // namespace

void InputSetVibrateMode(uint8 controllerID);
void InputSetPadMainMode(uint8 controllerID);

void InputInit()
{
    std::optional<GuestFrame> frame(std::in_place, 0x20);
    inputClass &v = in();
    std::memset(v.activeBuffer, 0, 0x20);
    std::memset(v.inactiveBuffer, 0, 0x20);
    std::memset(v.pressureBuffer, 0, 0x20);
    static const uint8 kVState[15] = {0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1};
    std::memcpy(v.vState, kVState, sizeof(kVState));
    v.vibrationFlags[0] = 0;
    v.vibrationFlags[1] = 0;
    v.motors[0].motor0 = 0;
    v.motors[0].motor1 = 0;
    v.userShockVals[0].motor0 = 0;
    v.userShockVals[0].motor1 = 0;
    v.send[0] = 0;
    v.pressureModeSet[0] = 0;
    v.motors[1].motor0 = 0;
    v.motors[1].motor1 = 0;
    v.userShockVals[1].motor0 = 0;
    v.userShockVals[1].motor1 = 0;
    v.send[1] = 0;
    v.pressureModeSet[1] = 0;
    v.align[0] = 0;
    v.padMode[1] = 1;
    v.align[5] = 0xff;
    v.align[1] = 1;
    v.align[2] = 0xff;
    v.align[3] = 0xff;
    v.align[4] = 0xff;
    v.analog1A = 0;
    v.analog1B = 0;
    v.analog1C = 0;
    v.analog1D = 0;
    v.analog2A = 0;
    v.analog2B = 0;
    v.analog2C = 0;
    v.analog2D = 0;
    v.padMode[0] = 1;
    if (guest_call<int32_t>(kScePadInit, 0) == 0)
        guest_call<int32_t>(kPrintf, kPadInitFailed);
    if (guest_call<int32_t>(kScePadPortOpen, 0, 0, kPadDma0) == 0)
        guest_call<int32_t>(kPrintf, kPort0OpenFailed);
    const bool failed = guest_call<int32_t>(kScePadPortOpen, 1, 0, kPadDma1) == 0;
    frame.reset(); // the last printf is a tail call
    if (failed)
        guest_call<int32_t>(kPrintf, kPort1OpenFailed);
}

void InputTerm()
{
    std::optional<GuestFrame> frame(std::in_place, 0x10);
    if (guest_call<int32_t>(kScePadPortClose, 0, 0) == 0)
        guest_call<int32_t>(kPrintf, kPort0CloseFailed);
    if (guest_call<int32_t>(kScePadPortClose, 1, 0) == 0)
        guest_call<int32_t>(kPrintf, kPort1CloseFailed);
    const bool failed = guest_call<int32_t>(kScePadEnd) == 0;
    frame.reset(); // the last printf is a tail call
    if (failed)
        guest_call<int32_t>(kPrintf, kPadEndFailed);
}

void InputHandler()
{
    GuestFrame frame(0xa0);
    inputClass &v = in();
    v.padState[0] = guest_call<int32_t>(kScePadGetState, 0, 0);
    v.padState[1] = guest_call<int32_t>(kScePadGetState, 1, 0);

    // Pad 1.
    if (!padReady(v.padState[0]) || guest_call<int32_t>(kScePadRead, 0, 0, inAddr(offsetof(inputClass, padBuffer1))) == 0)
        v.padBuffer1[0] = 1;
    if (v.padBuffer1[0] == 0)
    {
        const uint32_t id = v.padBuffer1[1] >> 4;
        const uint16_t buttons = static_cast<uint16_t>(v.padBuffer1[2] | (v.padBuffer1[3] << 8));
        if (id == 4)
        {
            readButtons(v.activeBuffer, buttons);
            v.signalSent[0] = 1;
        }
        else if (id == 5 || id == 7)
        {
            v.analog1B = static_cast<int8>(v.padBuffer1[7] + 0x80);
            v.analog1A = static_cast<int8>(v.padBuffer1[6] + 0x80);
            v.analog1C = static_cast<int8>(v.padBuffer1[4] + 0x80);
            v.analog1D = static_cast<int8>(v.padBuffer1[5] + 0x80);
            readButtons(v.activeBuffer, buttons);
            if (v.padBuffer1[1] == 0x79)
                readPressure(v.pressureBuffer, v.padBuffer1);
        }
    }

    // Pad 2.
    if (!padReady(v.padState[1]) || guest_call<int32_t>(kScePadRead, 1, 0, inAddr(offsetof(inputClass, padBuffer2))) == 0)
        v.padBuffer2[0] = 1;
    if (v.padBuffer2[0] == 0)
    {
        const uint32_t id = v.padBuffer2[1] >> 4;
        const uint16_t buttons = static_cast<uint16_t>(v.padBuffer2[2] | (v.padBuffer2[3] << 8));
        if (id == 4)
        {
            readButtons(v.activeBuffer + 16, buttons);
            v.signalSent[1] = 1;
        }
        else if (id == 5 || id == 7)
        {
            v.analog2B = static_cast<int8>(v.padBuffer2[7] + 0x80);
            v.analog2A = static_cast<int8>(v.padBuffer2[6] + 0x80);
            v.analog2C = static_cast<int8>(v.padBuffer2[4] + 0x80);
            v.analog2D = static_cast<int8>(v.padBuffer2[5] + 0x80);
            readButtons(v.activeBuffer + 16, buttons);
            if (v.padBuffer2[1] == 0x79)
                readPressure(v.pressureBuffer + 16, v.padBuffer2);
        }
    }

    InputSetVibrateMode(0);
    InputSetVibrateMode(1);
    InputSetPadMainMode(0);
    InputSetPadMainMode(1);

    // The motors: the small one follows vState through the user's count, the large one fades from the user's level.
    for (uint32_t pad = 0; pad < 2; ++pad)
    {
        sMotors &motors = v.motors[pad];
        sMotors &user = v.userShockVals[pad];
        if (v.vibrationFlags[pad] == 1)
        {
            if (user.motor0 != 0)
            {
                // vState is indexed by the count as it is (up to 255: past the table, as in the original).
                motors.motor0 = *static_cast<uint8 *>(guest_host(inAddr(offsetof(inputClass, vState)) + user.motor0));
                user.motor0 = static_cast<uint8>(user.motor0 - 1);
            }
            else
                motors.motor0 = 0;
            if (user.motor1 != 0)
            {
                motors.motor1 = static_cast<uint8>(user.motor1 * 3 + 0x50);
                user.motor1 = static_cast<uint8>(user.motor1 - 1);
            }
            else
                motors.motor1 = 0;
        }
        else
        {
            user.motor0 = 0;
            user.motor1 = 0;
            motors.motor0 = 0;
            motors.motor1 = 0;
            v.signalSent[pad] = 1;
        }
    }
}

// Steps one pad's actuator set-up: 0 waits for the pad, 1 sets the alignment, 2 waits for it, 3 drives the motors.
void InputSetVibrateMode(uint8 controllerID)
{
    GuestFrame frame(0x60);
    std::memcpy(frame.local<uint8_t>(0).get(), guest_host(kVibratePorts), 8); // the port table, copied as the original does
    inputClass &v = in();
    uint8 &send = v.send[controllerID];
    const int32_t port = *static_cast<int32 *>(guest_host(kVibratePorts + controllerID * 4u));
    const int32_t state = v.padState[controllerID];
    switch (send)
    {
    case 0:
        if (padReady(state))
            send = 1;
        return;
    case 1:
        if (guest_call<int32_t>(kScePadSetActAlign, port, 0, inAddr(offsetof(inputClass, align))) != 0)
            send = 2;
        else
            send = 0;
        return;
    case 2:
        if (state != 5)
            send = 3;
        return;
    case 3:
        if (!padReady(state))
        {
            send = 0;
            return;
        }
        if (guest_call<int32_t>(kScePadSetActDirect, port, 0, inAddr(offsetof(inputClass, motors)) + controllerID * 6u) != 0)
            v.signalSent[controllerID] = 1;
        else
            send = 0;
        return;
    default:
        return;
    }
}

// Applies the mode bits a pad has been asked for: 0x20 a main mode change (0x08 digital, 0x04 analog), 0x02 a lock,
// 0x10 pressure mode. A missing pad asks for 0x12 again.
void InputSetPadMainMode(uint8 controllerID)
{
    GuestFrame frame(0x50);
    std::memcpy(frame.local<uint8_t>(0).get(), guest_host(kModePorts), 4); // the port table, copied as the original does
    inputClass &v = in();
    const int32_t state = v.padState[controllerID];
    const uint16_t port = *static_cast<uint16 *>(guest_host(kModePorts + controllerID * 2u));
    uint8 &mode = v.padMode[controllerID];
    if (!padReady(state))
    {
        if (state == 0)
            mode = static_cast<uint8>(mode | 0x12);
        return;
    }
    const int32_t id = guest_call<int32_t>(kScePadInfoMode, port, 0, 2, 0);
    if (id == 0)
        return;
    const uint8 bits = mode;
    if ((bits & 0x20) != 0)
    {
        if ((bits & 8) != 0)
        {
            if (id == 4)
            {
                mode = static_cast<uint8>(bits & 0xdf);
                return;
            }
            guest_call<int32_t>(kScePadSetMainMode, port, 0, 0, 2);
            return;
        }
        if ((bits & 4) == 0)
            return;
        if (id == 7)
        {
            mode = static_cast<uint8>(bits & 0xdf);
            return;
        }
        guest_call<int32_t>(kScePadSetMainMode, port, 0, 1, 2);
        return;
    }
    if ((bits & 2) != 0)
    {
        mode = static_cast<uint8>(bits & 0xfd);
        guest_call<int32_t>(kScePadSetMainMode, port, 0, (bits & 8) == 0 ? 1 : 0, 3);
        return;
    }
    if ((bits & 0x10) == 0)
        return;
    if (guest_call<int32_t>(kScePadInfoPressMode, port, 0) == 0)
        return;
    if (guest_call<int32_t>(kScePadEnterPressMode, port, 0) == 0)
        return;
    mode = static_cast<uint8>(mode & 0xef);
}

bool_ InputLatched(uint8 key, uint8 interest, uint8 vals)
{
    uint8 &k = in().inactiveBuffer[key];
    const uint8 bits = k;
    if ((bits & 2) == 0 || (bits & interest) != vals)
        return 0;
    k = static_cast<uint8>(bits & 0xfd);
    return 1;
}

bool_ InputHit(uint8 key)
{
    inputClass &v = in();
    if ((v.inactiveBuffer[key] & 0x20) == 0)
        return 0;
    v.activeBuffer[key] = static_cast<uint8>(v.activeBuffer[key] & 0xdf);
    v.inactiveBuffer[key] = static_cast<uint8>(v.inactiveBuffer[key] & 0xdf);
    return 1;
}

void InputReset()
{
    inputClass &v = in();
    for (uint32_t i = 0; i < 32; ++i)
    {
        v.activeBuffer[i] = 0;
        v.inactiveBuffer[i] = 0;
    }
}

void InputResetAllHit()
{
    inputClass &v = in();
    for (uint32_t i = 0; i < 32; ++i)
    {
        v.activeBuffer[i] = static_cast<uint8>(v.activeBuffer[i] & 0xdf);
        v.inactiveBuffer[i] = static_cast<uint8>(v.inactiveBuffer[i] & 0xdf);
    }
}

void InputResetHit(uint8 key)
{
    inputClass &v = in();
    v.activeBuffer[key] = static_cast<uint8>(v.activeBuffer[key] & 0xdf);
    v.inactiveBuffer[key] = static_cast<uint8>(v.inactiveBuffer[key] & 0xdf);
}

void InputSnapshot()
{
    GuestFrame frame(0x20);
    InputHandler();
    inputClass &v = in();
    std::memcpy(v.inactiveBuffer, v.activeBuffer, 32);
    for (uint8 &key : v.activeBuffer)
        key = static_cast<uint8>(key & 0xfd);
}

bool_ InputPadPresent(eInputPad ePad)
{
    return in().padState[static_cast<uint32_t>(ePad) < 5u ? 0 : 1] != 0 ? 1 : 0;
}

namespace {

// The shape of InputDualShockPresent and the three pressure-mode functions: a ready pad's port asked one question.
template <typename Ask>
bool_ askReadyPad(eInputPad ePad, Ask ask)
{
    GuestFrame frame(0x30);
    // The original copies the port table into its frame; later callers' uninitialised locals can see those words.
    std::memcpy(frame.local<uint8_t>(0).get(), guest_host(kPadPorts), 0x20);
    const uint16_t port = padPort(ePad);
    if (!padReady(in().padState[port]))
        return 0;
    return ask(port) != 0 ? 1 : 0;
}

} // namespace

bool_ InputDualShockPresent(eInputPad ePad)
{
    return askReadyPad(ePad, [](uint16_t port) { return guest_call<int32_t>(kScePadInfoMode, port, 0, 2, 0); });
}

bool_ InputPressureModePadPresent(eInputPad ePad)
{
    return askReadyPad(ePad, [](uint16_t port) { return guest_call<int32_t>(kScePadInfoPressMode, port, 0); });
}

bool_ InputPadEnterPressureMode(eInputPad ePad)
{
    return askReadyPad(ePad, [](uint16_t port) { return guest_call<int32_t>(kScePadEnterPressMode, port, 0); });
}

bool_ InputPadExitPressureMode(eInputPad ePad)
{
    return askReadyPad(ePad, [](uint16_t port) { return guest_call<int32_t>(kScePadExitPressMode, port, 0); });
}

void InputSetPadMode(eInputPad ePad, uint8 padMode)
{
    in().padMode[ePad == INPUT_MULTITAP_1 ? 0 : 1] = padMode;
}

void InputClearVibrationVals()
{
    inputClass &v = in();
    v.signalSent[0] = 0;
    v.motors[1].motor1 = 0;
    v.userShockVals[0].motor0 = 0;
    v.userShockVals[0].motor1 = 0;
    v.motors[0].motor0 = 0;
    v.motors[0].motor1 = 0;
    v.signalSent[1] = 0;
    v.userShockVals[1].motor0 = 0;
    v.userShockVals[1].motor1 = 0;
    v.motors[1].motor0 = 0;
}

bool_ InputTestAnyKeyHeld()
{
    for (const uint8 key : in().inactiveBuffer)
        if ((key & 1) != 0)
            return 1;
    return 0;
}

uint8 InputGetPadID(eInputPad ePad)
{
    const uint8 *buffer = static_cast<uint32_t>(ePad) < 5u ? in().padBuffer1 : in().padBuffer2;
    return buffer[0] == 0 ? static_cast<uint8>(buffer[1] >> 4) : 0;
}

} // namespace rc

using rc::fuzz::inputController;
using rc::fuzz::inputKeys;
using rc::fuzz::inputPad;

RC_DECOMP_FN_SETUP(InputInit, 0x190908, inputPad)
RC_DECOMP_FN_SETUP(InputTerm, 0x190a88, inputPad)
RC_DECOMP_FN_SETUP(InputHandler, 0x190af8, inputPad)
RC_DECOMP_FN_SETUP(InputSetVibrateMode, 0x191268, inputController)
RC_DECOMP_FN_SETUP(InputSetPadMainMode, 0x1913c0, inputController)
RC_DECOMP_FN_SETUP(InputLatched, 0x191550, inputKeys)
RC_DECOMP_FN_SETUP(InputHit, 0x191598, inputKeys)
RC_DECOMP_FN_SETUP(InputReset, 0x191600, inputKeys)
RC_DECOMP_FN_SETUP(InputResetAllHit, 0x1916c8, inputKeys)
RC_DECOMP_FN_SETUP(InputResetHit, 0x1917b8, inputKeys)
RC_DECOMP_FN_SETUP(InputSnapshot, 0x191810, inputPad)
RC_DECOMP_FN_SETUP(InputPadPresent, 0x1918f8, inputPad)
RC_DECOMP_FN_SETUP(InputDualShockPresent, 0x191938, inputPad)
RC_DECOMP_FN_SETUP(InputPressureModePadPresent, 0x1919f0, inputPad)
RC_DECOMP_FN_SETUP(InputPadEnterPressureMode, 0x191aa0, inputPad)
RC_DECOMP_FN_SETUP(InputPadExitPressureMode, 0x191b50, inputPad)
RC_DECOMP_FN_SETUP(InputSetPadMode, 0x191c00, inputPad)
RC_DECOMP_FN_SETUP(InputClearVibrationVals, 0x191c40, inputKeys)
RC_DECOMP_FN_SETUP(InputTestAnyKeyHeld, 0x191c78, inputKeys)
RC_DECOMP_FN_SETUP(InputGetPadID, 0x191d30, inputPad)
