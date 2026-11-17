// Decompiled from JIS.c: converting between ASCII and the Japanese text codes (JIS) the game's strings can use. A JIS
// character is a 16-bit code stored high byte first; ASCII characters 0x20 to 0x7f have codes in a table.

#include "rc_decomp.h"

namespace rc {

namespace {

const uint32_t kAsciiToJISTable = sym::asciiToJISTable;

uint16_t tableEntry(uint32_t index) { return *static_cast<uint16_t *>(guest_host(kAsciiToJISTable + index * 2u)); }

uint16_t swapBytes(uint16_t code) { return static_cast<uint16_t>((code << 8) | (code >> 8)); }

} // namespace

uint16 ASCIIToJIS(uint8 ascii_code)
{
    if (static_cast<uint8_t>(ascii_code - 0x20) < 0x60)
        return tableEntry(static_cast<uint8_t>(ascii_code - 0x20));
    return 0;
}

// The ASCII character for a code, or '?' if the table (which ends in a zero) does not have it.
char JISToASCII(uint16 jis)
{
    int32_t ascii = 0x20;
    for (uint32_t index = 0; tableEntry(index) != 0; ++index)
    {
        if (tableEntry(index) == jis)
            return static_cast<char>(ascii);
        ascii = static_cast<int8_t>(ascii + 1);
    }
    return '?';
}

// Converts up to jisLength characters, then pads with the code for a space.
void ASCIIStringToJISPadded(ptr32<char> pASCII, ptr32<uint16> pJIS, int32 jisLength)
{
    const char *ascii = pASCII.get();
    uint16_t *jis = pJIS.get();
    while (*ascii != 0 && jisLength != 0)
    {
        *jis++ = swapBytes(ASCIIToJIS(static_cast<uint8_t>(*ascii++)));
        --jisLength;
    }
    const uint16_t space = swapBytes(ASCIIToJIS(' '));
    while (jisLength != 0)
    {
        *jis++ = space;
        --jisLength;
    }
}

// Converts up to jisLength characters and ends with a zero if there is room.
void ASCIIStringToJIS(ptr32<char> pASCII, ptr32<uint16> pJIS, int32 jisLength)
{
    const char *ascii = pASCII.get();
    uint16_t *jis = pJIS.get();
    while (*ascii != 0 && jisLength != 0)
    {
        *jis++ = swapBytes(ASCIIToJIS(static_cast<uint8_t>(*ascii++)));
        --jisLength;
    }
    if (0 < jisLength)
        *jis = 0;
}

// Takes one byte for a character up to 0x80 and two (high byte first) above that. The last place in the output is
// kept for the ending zero.
void JISStringToASCII(ptr32<uint16> pJIS, ptr32<char> pASCII, int32 asciiLength)
{
    uint32_t in = pJIS.address;
    char *out = pASCII.get();
    const auto byteAt = [](uint32_t address) { return *static_cast<uint8_t *>(guest_host(address)); };
    uint8_t b;
    do
    {
        b = 0;
        if (1 < asciiLength)
            b = byteAt(in);
        if (b < 0x81)
        {
            *out = static_cast<char>(b);
            in += 1;
        }
        else
        {
            const uint8_t low = byteAt(in + 1);
            in += 2;
            *out = JISToASCII(static_cast<uint16_t>((b << 8) | low));
        }
        ++out;
        --asciiLength;
    } while (b != 0);
    if (0 < asciiLength)
        *out = 0;
}

} // namespace rc

namespace rc::fuzz {

namespace {

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

uint16_t tableCode(uint64_t &state)
{
    for (;;)
    {
        const uint16_t code = *static_cast<uint16_t *>(guest_host(sym::asciiToJISTable + (nextFuzz(state) % 0x60u) * 2u));
        if (code != 0)
            return code;
    }
}

} // namespace

// ASCII text of 0 to 30 characters (some past 0x7f), the output at scratch + 0x400, and a length of 1 to 40.
void jisFromAscii(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t text = scratch + 0x100;
    const uint32_t length = nextFuzz(state) % 31u;
    for (uint32_t i = 0; i < length; ++i)
        *static_cast<uint8_t *>(guest_host(text + i)) = static_cast<uint8_t>(1 + nextFuzz(state) % 0xfeu);
    *static_cast<uint8_t *>(guest_host(text + length)) = 0;
    setArg(ctx, 0, text);
    setArg(ctx, 1, scratch + 0x400);
    setArg(ctx, 2, 1 + nextFuzz(state) % 40u);
}

// JIS text of 0 to 20 characters: table codes, plain bytes and bytes above 0x80 with a random second byte. The
// output at scratch + 0x400 and a length of 0 to 45.
void asciiFromJis(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t text = scratch + 0x100;
    const uint32_t length = nextFuzz(state) % 21u;
    uint32_t at = text;
    for (uint32_t i = 0; i < length; ++i)
    {
        const uint32_t pick = nextFuzz(state) % 10u;
        if (pick < 5)
        {
            const uint16_t code = tableCode(state);
            *static_cast<uint8_t *>(guest_host(at++)) = static_cast<uint8_t>(code >> 8);
            *static_cast<uint8_t *>(guest_host(at++)) = static_cast<uint8_t>(code);
        }
        else if (pick < 8)
            *static_cast<uint8_t *>(guest_host(at++)) = static_cast<uint8_t>(1 + nextFuzz(state) % 0x7fu);
        else
        {
            *static_cast<uint8_t *>(guest_host(at++)) = static_cast<uint8_t>(0x81 + nextFuzz(state) % 0x7fu);
            *static_cast<uint8_t *>(guest_host(at++)) = static_cast<uint8_t>(nextFuzz(state));
        }
    }
    *static_cast<uint16_t *>(guest_host(at)) = 0;
    setArg(ctx, 0, text);
    setArg(ctx, 1, scratch + 0x400);
    setArg(ctx, 2, nextFuzz(state) % 46u);
}

} // namespace rc::fuzz

RC_DECOMP_FN(ASCIIToJIS)
RC_DECOMP_FN(JISToASCII)
RC_DECOMP_FN_SETUP(ASCIIStringToJISPadded, rc::fuzz::jisFromAscii)
RC_DECOMP_FN_SETUP(ASCIIStringToJIS, rc::fuzz::jisFromAscii)
RC_DECOMP_FN_SETUP(JISStringToASCII, rc::fuzz::asciiFromJis)
