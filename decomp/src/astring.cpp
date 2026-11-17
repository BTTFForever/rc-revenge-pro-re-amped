// Decompiled from ASTRING.c: laying out text in the menus. A string is cut into words at spaces, tabs and line breaks
// and filled into lines no wider than the menu's width; "}" starts a two-character code (an icon or a colour) that
// has no width of its own, or 32 pixels for an icon.

#include "rc_decomp.h"

#include <cstring>

namespace rc {

uint32 FontLength(ptr32<char> pString);              // font.cpp
ptr32<char> FontPrint(int16 x, int16 y, ptr32<char> pString);
int32_t AStringCentre(int32_t startX, int32_t width, ptr32<char> pText, int32_t stringWidth);

namespace {

constexpr uint32_t kSpaceString = (sym::sec_sdata + 0xc0u); // " ", the string the space's width is measured on.

bool endsWord(char c) { return c == 0 || c == '\n' || c == '\r' || c == ' ' || c == '\t'; }

} // namespace

// Where the next line begins: the width of the space between two words.
int32_t AStringPrepareWrap(ptr32<aStringTemp> pDestStrings, ptr32<char> pString, ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0x140);
    char *const word = frame.local<char>(0).get();
    const ptr32<char> shavedAddress = frame.local<char>(0x40);
    char *const shaved = shavedAddress.get();
    const uint32_t dest = pDestStrings.address;
    const auto lineAt = [&](int32_t index) { return static_cast<char *>(guest_host(dest + static_cast<uint32_t>(index) * 60u)); };
    const auto countAt = [&](int32_t index) { return static_cast<char *>(guest_host(dest + 0x960u + static_cast<uint32_t>(index))); };
    char &whiteSpace = G(whiteSpace)[0];

    int32_t shaveLength = 0;
    const int32_t spaceSize = static_cast<int32_t>(FontLength(ptr32<char>(kSpaceString)));
    int32_t x = pHeader->x; // How far along the current line.
    bool tryAgain = false;
    *lineAt(0) = 0;
    *lineAt(1) = 0;
    *countAt(0) = 0;
    *countAt(1) = 0;
    int32_t line = 0;
    if (pString.address != 0)
    {
        char *current = pString.get();
        int32_t lineBefore = 0;
        do
        {
            shaveLength = 0;
            int32_t length = 0, skipped = 0;
            char *p = current;
            if (!endsWord(*current))
            {
                char c = *current;
                while (c != '@')
                {
                    if (c == '}')
                        ++skipped;
                    else if (length < 1 || p[-1] != '}')
                        shaved[length - skipped] = *p;
                    else
                    {
                        ++skipped;
                        shaveLength += 0x20;
                    }
                    word[length] = *p;
                    ++length;
                    p = current + length;
                    c = *p;
                    if (endsWord(c))
                        break;
                }
            }
            word[length] = 0;
            shaved[length - skipped] = 0;
            if (*p == 0)
                current = nullptr;
            else
            {
                current = current + length + 1;
                whiteSpace = *p;
            }
            const uint32_t wordWidth = FontLength(shavedAddress);
            const int32_t headerX = pHeader->x;
            line = lineBefore;
            if (static_cast<int32_t>(pHeader->width) < static_cast<int32_t>(static_cast<uint32_t>(x - headerX) + wordWidth + static_cast<uint32_t>(shaveLength)))
            {
                line = lineBefore + 1;
                *lineAt(lineBefore + 1) = 0;
                *lineAt(lineBefore + 2) = 0;
                *countAt(lineBefore + 1) = 0;
                *countAt(lineBefore + 2) = 0;
                x = headerX;
            }
            x = static_cast<int32_t>(static_cast<uint32_t>(x) + wordWidth + static_cast<uint32_t>(shaveLength));
            std::strcat(lineAt(line), word);
            *countAt(line) = static_cast<char>(*countAt(line) + 1);
            bool newLine = false;
            if (whiteSpace == '\r')
                newLine = true;
            else if (whiteSpace < 0xe)
                newLine = whiteSpace == '\n';
            else if (whiteSpace == ' ')
            {
                if (static_cast<int32_t>(pHeader->width) < (x - pHeader->x) + spaceSize)
                    newLine = true;
                else
                {
                    std::strcat(lineAt(line), &whiteSpace);
                    x += spaceSize;
                }
            }
            if (newLine)
            {
                x = pHeader->x;
                *lineAt(line + 1) = 0;
                *countAt(line + 1) = 0;
                *countAt(line + 2) = 0;
                ++line;
            }
            lineBefore = line;
        } while (current != nullptr);
    }
    // Spaces and line breaks at the end of a line are dropped.
    for (int32_t i = 0; i < line; ++i)
    {
        for (;;)
        {
            char *text = lineAt(i);
            const size_t length = std::strlen(text);
            if (!(0 < static_cast<long>(length)))
                break;
            const char last = text[length - 1];
            if (last != ' ' && last != '\r' && last != '\n')
                break;
            text[length - 1] = 0;
        }
    }
    // A line that is one word and still too wide widens the menu, and the whole text is laid out again.
    for (int32_t i = 0; i < line; ++i)
    {
        const uint32_t width = FontLength(ptr32<char>(dest + static_cast<uint32_t>(i) * 60u));
        if (*countAt(i) == 1 && static_cast<int32_t>(pHeader->width) < static_cast<int32_t>(width))
        {
            pHeader->width = static_cast<uint16>(width);
            tryAgain = true;
        }
    }
    if (tryAgain)
        return AStringPrepareWrap(pDestStrings, pString, pHeader);
    if (*countAt(line) == 0)
        --line;
    return line + 1;
}

// Prints a wrapped string at (posX, posY), a line at a time, centred if the menu's flags ask for it. Returns the
// number of lines.
int32_t AStringWrap(int32_t posX, int32_t posY, ptr32<char> pString, ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0xa20);
    const ptr32<aStringTemp> prePrinted = frame.local<aStringTemp>(0);
    const int32_t lines = AStringPrepareWrap(prePrinted, pString, pHeader);
    int32_t x = pHeader->x;
    for (int32_t i = 0; i < lines; ++i)
    {
        const ptr32<char> text(prePrinted.address + static_cast<uint32_t>(i) * 60u);
        if (prePrinted->wordCount[i] == 0 || std::strlen(text.get()) == 0)
            return lines;
        if ((pHeader->flags & 4u) != 0)
            x = AStringCentre(posX, pHeader->width, text, -1);
        FontPrint(static_cast<int16>(x), static_cast<int16>(posY), text);
        posY += pHeader->changeY;
    }
    return lines;
}

// The x that centres a string of this width in a box starting at startX; the width is measured if -1.
int32_t AStringCentre(int32_t startX, int32_t width, ptr32<char> pText, int32_t stringWidth)
{
    if (stringWidth == -1)
        stringWidth = static_cast<int32_t>(FontLength(pText));
    return startX + (width - stringWidth) / 2;
}

} // namespace rc

namespace rc::fuzz {

void fontText(R5900Context *ctx, uint64_t &state, uint32_t scratch); // font.cpp

namespace {

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

// A menu header at scratch + 0x2000: its x, width, line spacing and flags are the ones the wrapping reads.
uint32_t menuHeader(uint64_t &state, uint32_t scratch)
{
    const uint32_t header = scratch + 0x2000;
    auto &h = *static_cast<sAMenuHeader *>(guest_host(header));
    h.x = static_cast<int16>(nextFuzz(state) % 120u);
    h.width = static_cast<uint16>(20 + nextFuzz(state) % 300u);
    h.changeY = static_cast<uint8>(8 + nextFuzz(state) % 24u);
    h.flags = static_cast<uint16>(nextFuzz(state));
    return header;
}

} // namespace

// a0 the destination (an aStringTemp at scratch + 0x1000), a1 the text, a2 the header.
void astringPrepareArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fontText(ctx, state, scratch);
    setArg(ctx, 0, scratch + 0x1000);
    setArg(ctx, 1, scratch + 0x100);
    setArg(ctx, 2, menuHeader(state, scratch));
}

// a0 x, a1 y, a2 the text, a3 the header.
void astringWrapArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fontText(ctx, state, scratch);
    setArg(ctx, 0, nextFuzz(state) % 300u);
    setArg(ctx, 1, nextFuzz(state) % 400u);
    setArg(ctx, 2, scratch + 0x100);
    setArg(ctx, 3, menuHeader(state, scratch));
}

// a0 start, a1 width, a2 the text, a3 the string's width or -1.
void astringCentreArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fontText(ctx, state, scratch);
    setArg(ctx, 0, nextFuzz(state) % 300u);
    setArg(ctx, 1, nextFuzz(state) % 400u);
    setArg(ctx, 2, scratch + 0x100);
    setArg(ctx, 3, (nextFuzz(state) & 1u) != 0u ? 0xffffffffu : nextFuzz(state) % 500u);
}

} // namespace rc::fuzz

RC_DECOMP_FN_SETUP(AStringPrepareWrap, rc::fuzz::astringPrepareArgs)
RC_DECOMP_FN_SETUP_HEAP(AStringWrap, rc::fuzz::astringWrapArgs)
RC_DECOMP_FN_SETUP(AStringCentre, rc::fuzz::astringCentreArgs)
