// Decompiled from TEDIT.c: the cursor engine. TEditAdjustCursorModule walks one edit module from a grid position,
// direction and height (drawing it in 3D and on the 2D map, placing its tiles, or drawing it on a map only) and moves
// the position to the module's far end; TEditBuildTransitionCursorModules fills the cursor's module list with the
// pieces that take the track from one slope to another. See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"

namespace rc {

using namespace tedit;

// Walks the module's units (a copy of its covered-unit and tile arrays, turned over for a mirrored piece) in the
// order its direction gives and, for each covered unit, by `flag`: 1 draws its tile and its picture on the 2D map,
// 2 places it on the grid, 4 draws the 2D map's picture, 8 the hud map's. Then the height, the direction and the
// position move to the module's end. Returns the module's output slope.
//
// The loop counters are bytes, as the original's: a module wider than 127 never ends.
eSlope TEditAdjustCursorModule(ptr32<_sCursorModule> pCursorModule, ptr32<int16> xPos, ptr32<int16> yPos,
                               ptr32<eDirection> direction, ptr32<int16> elevation, uint8 flag)
{
    GuestFrame frame(0x100);
    const uint32_t sp = frame.local<uint8_t>(0).address; // trackUnit 0x0, pPosArray 0x10, pTileArray 0x14
    const uint32_t desc = at<uint32_t>(pCursorModule.address + 4u);
    const uint32_t module = te().pEditModuleList.address + at<uint16_t>(desc) * 0x18u;
    const uint32_t x = xPos.address, y = yPos.address, elev = elevation.address, dir = direction.address;
    at<uint32_t>(sp + 0x10u) = 0;
    at<uint32_t>(sp + 0x14u) = 0;
    const int32_t size = static_cast<int8_t>(at<uint16_t>(module) * at<uint16_t>(module + 2u));
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x10u, size);
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x14u, size);
    const uint32_t theme = te().trackDesc.themeTypeNdx;
    const int32_t width = at<uint16_t>(module);
    const int32_t dirChange = at<int8_t>(module + 4u);
    const int32_t height = at<uint16_t>(module + 2u);
    int32_t vHeight = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(at<int16_t>(module + 6u))) << 24) >> 16;
    int32_t outputSlope = at<int32_t>(module + 0xcu);

    const auto copyUnit = [&](int32_t to, int32_t from)
    {
        at<uint8_t>(at<uint32_t>(sp + 0x10u) + static_cast<uint32_t>(to)) = at<uint8_t>(at<uint32_t>(module + 0x10u) + static_cast<uint32_t>(from));
        at<uint8_t>(at<uint32_t>(sp + 0x14u) + static_cast<uint32_t>(to)) =
            at<uint8_t>(at<uint32_t>(module + 0x14u) + (static_cast<uint32_t>(from) << 2));
    };
    for (int8_t i = 0; i < width; i = static_cast<int8_t>(i + 1))
    {
        if (height == 0)
            continue;
        int8_t j = 0;
        do
        {
            const int32_t index = width * i + j;
            j = static_cast<int8_t>(j + 1);
            copyUnit(index, index);
        } while (j < height);
    }
    if ((at<uint16_t>(desc + 2u) & 1u) != 0)
    {
        // A mirrored piece: its rows from the far end, slopes swapped and negated, and it starts from its top.
        const int32_t half = static_cast<uint32_t>(height) >> 1;
        for (int8_t i = 0; i < width; i = static_cast<int8_t>(i + 1))
        {
            if (half == 0)
                continue;
            int8_t j = 0;
            do
            {
                const int32_t from = width * (height - i - 1) + j, to = width * i + j;
                j = static_cast<int8_t>(j + 1);
                copyUnit(to, from);
            } while (j < half);
        }
        at<uint16_t>(elev) = static_cast<uint16_t>(static_cast<int16_t>(-vHeight) + at<uint16_t>(elev));
        outputSlope = -at<int32_t>(module + 8u);
        vHeight = 0;
    }
    const uint32_t sprites = TEditGetTileSprPtr(at<uint8_t>(desc)).address;
    const uint32_t startDirection = at<uint32_t>(dir);
    const bool mirrored = (at<uint16_t>(desc + 2u) & 1u) != 0;
    uint32_t height16 = at<uint16_t>(elev); // the height as the original keeps it in a register

    // One unit of the module, at the position plus (dx, dy).
    const auto unit = [&](int32_t index, int32_t dx, int32_t dy, uint32_t orientation, uint32_t mapFlag)
    {
        const uint32_t i = static_cast<uint32_t>(index);
        if (at<int8_t>(at<uint32_t>(sp + 0x10u) + i) == 0)
            return;
        const uint8_t tile = at<uint8_t>(at<uint32_t>(sp + 0x14u) + i);
        const uint32_t map = (orientation | mapFlag) & 0xffu;
        const auto px = [&] { return static_cast<int16_t>(at<uint16_t>(x) + dx); };
        const auto py = [&] { return static_cast<int16_t>(at<uint16_t>(y) + dy); };
        const auto setUnit = [&]
        {
            at<uint16_t>(sp + 2u) = static_cast<uint16_t>(height16);
            at<uint16_t>(sp) = tile;
            at<uint32_t>(sp + 4u) = orientation;
        };
        switch (flag)
        {
        case 1:
        {
            const int32_t tx = at<int16_t>(x) + dx, ty = at<int16_t>(y) + dy;
            setUnit();
            guest_call(sym::TileRender, tx, ty, sp, theme);
            TEditRender2DModule(px(), py(), at<int8_t>(sprites + i), at<int16_t>(elev), static_cast<uint8_t>(map));
            break;
        }
        case 2:
            TEditPlaceModule(px(), py(), tile, static_cast<int16_t>(height16), static_cast<eDirection>(orientation));
            break;
        case 4:
            setUnit();
            TEditRender2DModule(px(), py(), at<int8_t>(sprites + i), static_cast<int16_t>(height16), static_cast<uint8_t>(map));
            break;
        case 8:
            setUnit();
            TEditRender2DHudMapModule(px(), py(), at<int8_t>(sprites + i), static_cast<int16_t>(height16), static_cast<uint8_t>(map));
            break;
        default:
            return;
        }
        height16 = at<uint16_t>(elev);
    };
    const auto next = [](int8_t v) { return static_cast<int8_t>(v + 1); };
    const auto prev = [](int8_t v) { return static_cast<int8_t>(v - 1); };
    const bool sharp = (static_cast<uint32_t>(dirChange) & 0xffu) >= 2u; // a left turn (direction change 3) and anything else
    const auto setX = [&](int32_t value) { at<uint16_t>(x) = static_cast<uint16_t>(value); };
    const auto setY = [&](int32_t value) { at<uint16_t>(y) = static_cast<uint16_t>(value); };
    const auto finish = [&]
    {
        at<uint16_t>(elev) = static_cast<uint16_t>(vHeight + static_cast<int32_t>(height16));
        at<uint32_t>(dir) = at<uint32_t>(dir) + static_cast<uint32_t>(dirChange);
    };
    const auto gx = [&] { return static_cast<int32_t>(at<uint16_t>(x)); };
    const auto gy = [&] { return static_cast<int32_t>(at<uint16_t>(y)); };

    switch (startDirection)
    {
    case 0: // north
    {
        const uint32_t o = mirrored ? 2u : 0u;
        for (int8_t i = 0; i < width; i = next(i))
        {
            if (height == 0)
                continue;
            int8_t j = 0;
            do
            {
                if (!sharp)
                    unit(width * j + i, i, height - 1 - j, o, 0);
                else
                    unit(width * j + i, -(width - 1 - i), height - 1 - j, o, 0x80);
                j = next(j);
            } while (j < height);
        }
        finish();
        if (dirChange == 0)
        {
            setX(gx() + width - 1);
            setY(height + gy());
        }
        else if (dirChange == 1)
        {
            setX(width + gx());
            setY(gy() + height - 1);
        }
        else if (dirChange == 3)
        {
            setX(gx() - width);
            setY(gy() + height - 1);
        }
        break;
    }
    case 1: // east
    {
        const uint32_t o = mirrored ? 3u : 1u;
        if (!sharp)
        {
            for (int8_t i = static_cast<int8_t>(height - 1); i >= 0; i = prev(i))
            {
                if (width == 0)
                    continue;
                int8_t j = 0;
                do
                {
                    unit(width * i + j, height - 1 - i, -j, o, 0);
                    j = next(j);
                } while (j < width);
            }
        }
        else
        {
            for (int8_t i = 0; i < width; i = next(i))
            {
                if (height == 0)
                    continue;
                int8_t j = 0;
                do
                {
                    unit(width * j + i, height - 1 - j, width - 1 - i, o, 0x80);
                    j = next(j);
                } while (j < height);
            }
        }
        finish();
        if (dirChange == 0)
        {
            setX(width + gx());
            setY(gy() + height - 1);
        }
        else if (dirChange == 1)
        {
            setX(gx() + width - 1);
            setY(gy() - height);
        }
        else if (dirChange == 3)
        {
            setX(gx() + height - 1);
            setY(width + gy());
        }
        break;
    }
    case 2: // south
    {
        const uint32_t o = mirrored ? 0u : 2u;
        for (int8_t i = static_cast<int8_t>(width - 1); i >= 0; i = prev(i))
        {
            if (!sharp)
            {
                if (height == 0)
                    continue;
                int8_t j = 0;
                do
                {
                    unit(i * height + j, -j, -(width - 1 - i), o, 0);
                    j = next(j);
                } while (j < height);
            }
            else
            {
                for (int8_t j = static_cast<int8_t>(height - 1); j >= 0; j = prev(j))
                    unit(j * width + i, width - 1 - i, -(height - 1 - j), o, 0x80);
            }
        }
        finish();
        if (dirChange == 0)
        {
            setX(gx() + width - 1);
            setY(gy() - height);
        }
        else if (dirChange == 1)
        {
            setX(gx() - width);
            setY(gy() - (height - 1));
        }
        else if (dirChange == 3)
        {
            setX(width + gx());
            setY(gy() - (height - 1));
        }
        break;
    }
    case 3: // west
    {
        const uint32_t o = mirrored ? 1u : 3u;
        for (int8_t i = static_cast<int8_t>(width - 1); i >= 0; i = prev(i))
        {
            if (!sharp)
            {
                if (height == 0)
                    continue;
                int8_t j = 0;
                do
                {
                    unit(i * height + j, -(width - 1 - i), j, o, 0);
                    j = next(j);
                } while (j < height);
            }
            else
            {
                for (int8_t j = static_cast<int8_t>(height - 1); j >= 0; j = prev(j))
                    unit(i * height + j, -(width - 1 - i), -(height - 1 - j), o, 0x80);
            }
        }
        finish();
        if (dirChange == 0)
        {
            setX(gx() - width);
            setY(gy() + height - 1);
        }
        else if (dirChange == 1)
        {
            setX(gx() - (width - 1));
            setY(height + gy());
        }
        else if (dirChange == 3)
        {
            setX(gx() - (width - 1));
            setY(gy() - height);
        }
        break;
    }
    default:
        break;
    }
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x10u);
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x14u);
    at<uint32_t>(dir) = at<uint32_t>(dir) & 3u;
    return static_cast<eSlope>(outputSlope);
}

// The cursor's pieces for a change of slope: one transition piece between flat and a slope or between two slopes of
// the same side (transitionArrayG: in, out, the piece and the piece that does it the other way up), two through flat
// from an incline to a decline or back.
//
// A pair the table does not hold (the same slope twice: its callers never ask) takes the piece's id from a register
// the function never set: the caller's $t3, $s0, $t1, $t0 and $s3, or $t2 and $s4, by the case.
void TEditBuildTransitionCursorModules(eSlope currentSlope, eSlope desiredSlope)
{
    R5900Context &ctx = g_runtime->eeScheduler().runningContext();
    const uint32_t t0 = getRegU32(&ctx, 8), t1 = getRegU32(&ctx, 9), t2 = getRegU32(&ctx, 10), t3 = getRegU32(&ctx, 11);
    const uint32_t s0 = getRegU32(&ctx, 16), s3 = getRegU32(&ctx, 19), s4 = getRegU32(&ctx, 20);
    GuestFrame frame(0xa0);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t table = sym::transitionArrayG;
    const auto find = [&](uint32_t in, uint32_t out, bool turned, uint32_t leftover)
    {
        for (uint32_t i = 0; i < 6u; ++i)
            if (at<uint8_t>(table + i * 4u) == in && at<uint8_t>(table + i * 4u + 1u) == out)
                return static_cast<uint32_t>(at<uint8_t>(table + i * 4u + (turned ? 3u : 2u)));
        return leftover & 0xffu;
    };
    const auto add = [&](uint32_t handle, uint32_t id)
    {
        at<uint32_t>(handle) = 0;
        guest_call<int32_t>(sym::MemBlockAllocLo, handle, 8);
        const uint32_t node = at<uint32_t>(handle);
        uint32_t last = te().cursor.pCursorModuleList.address;
        at<uint32_t>(node + 4u) = kEditModuleList + id * 4u;
        at<uint32_t>(node) = 0;
        if (last == 0)
        {
            te().cursor.pCursorModuleList = ptr32<_sCursorModule>(node);
            return;
        }
        while (at<uint32_t>(last) != 0)
            last = at<uint32_t>(last);
        at<uint32_t>(last) = node;
    };
    const int32_t current = currentSlope, desired = desiredSlope;
    const uint32_t c = static_cast<uint32_t>(current), d = static_cast<uint32_t>(desired);
    if (current == 0 || desired == 0)
    {
        // From or to flat: a decline is the incline's piece the other way up.
        uint32_t in, out;
        bool turned = false;
        if (current == 0)
        {
            in = desired < 0 ? 0u - d : 0u;
            out = desired < 0 ? 0u : d;
            turned = desired < 0;
        }
        else
        {
            in = current < 0 ? 0u : c;
            out = current < 0 ? 0u - c : 0u;
            turned = current < 0;
        }
        add(sp, find(in, out, turned, t3));
    }
    else if (current > 0 && desired > 0)
        add(sp + 4u, find(c, d, false, s0));
    else if (current < 0 && desired < 0)
        add(sp + 8u, find(0u - d, 0u - c, true, t1));
    else if (current > 0)
    {
        add(sp + 0xcu, find(c, 0, false, t0));
        add(sp + 0x10u, find(0u - d, 0, true, s3));
    }
    else
    {
        add(sp + 0x14u, find(0, 0u - c, true, t2));
        add(sp + 0x18u, find(0, d, false, s4));
    }
}

} // namespace rc

RC_DECOMP_FN_SETUP(TEditAdjustCursorModule, rc::fuzz::teditAdjust)
RC_DECOMP_FN_SETUP(TEditBuildTransitionCursorModules, rc::fuzz::teditTransition)
