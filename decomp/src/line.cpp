// Decompiled from LINE.c: debug lines (the route nodes, collision and camera views use them), drawn straight to the
// GS through GFX_P2's packet buffer as line primitives.

#include "ee_math.h"

namespace rc {

namespace {

uint64_t *quadword(int32_t index) { return reinterpret_cast<uint64_t *>((G(gGfxP2).pDmaBuff + index).get()); }

// The state block before a line: no texture, TEX1, PABE off, then ALPHA (blended for flag 1).
void lineState(bool blended)
{
    sGfxP2 &gfx = G(gGfxP2);
    uint64_t *q = quadword(gfx.buffIdx);
    q[0] = 0x2000000000000001ull;
    q[1] = 0xe0;
    q[2] = (static_cast<uint64_t>(blended) << 6) | 1; // PRIM: line, alpha blending for flag 1
    q[3] = 0;
    q[4] = 0;
    q[5] = 0x49;
    q[6] = 0x1000000000000001ull;
    q[7] = 0xe;
    q[8] = blended ? 0x4000000021ull : 0x8000000029ull;
    q[9] = 0x42;
}

// One vertex (RGBAQ, XYZ2) under the open GIF tag, or a new tag if there is none or it is full.
void lineVertex(uint8 red, uint8 green, uint8 blue, uint64_t xy, uint64_t z)
{
    sGfxP2 &gfx = G(gGfxP2);
    int32_t at = gfx.buffIdx;
    int32_t tag = gfx.gifTagIdx;
    if (tag == -1 || 0x7ffc < (*quadword(tag) & 0x7fff))
    {
        uint64_t *t = quadword(at);
        t[0] = 0x2000000000000000ull;
        t[1] = 0x51; // RGBAQ, XYZ2
        tag = gfx.gifTagIdx = at;
        ++at;
    }
    uint64_t *q = quadword(at);
    q[0] = red | (static_cast<uint64_t>(green) << 32);
    q[1] = blue;
    uint64_t &header = *quadword(tag);
    header = (header & 0xffffffffffff0000ull) | ((header & 0x7fff) + 1);
    gfx.buffIdx = at + 2;
    q[3] = z;
    q[2] = xy;
}

uint64_t screenXY(int32_t x, int32_t y)
{
    const auto c = [](int32_t v) { return static_cast<uint64_t>(ee_ftoi(ee_itof(v) * ee_float(16.0f))) & 0xffffu; };
    return c(x + 0x6c0) | (c(y + 0x780) << 32);
}

uint64_t lineDepth(ee_float z, uint32_t scaleAddress)
{
    return static_cast<uint64_t>(static_cast<int64_t>(ee_ftoi((ee_float(1.0f) - z) * *static_cast<ee_float *>(guest_host(scaleAddress)))));
}

} // namespace

// A line between two screen points. The state block is followed by a gap of two quadwords before the line's GIF
// tag, as in the original (buffIdx moves 3 for a 5-quadword block, then the tag goes 2 further on).
void LineRender2D(ptr32<_sLine2D> pLine, f32 otz)
{
    sGfxP2 &gfx = G(gGfxP2);
    const bool blended = (pLine->flags & 1) != 0;
    lineState(blended);
    gfx.buffIdx = gfx.buffIdx + 3;
    gfx.gifTagIdx = gfx.buffIdx + 2;
    const _sLine2D &line = *pLine;
    G(gLastTex) = ptr32<_sGfxTexture>(0xffffffffu); // No texture set.
    uint64_t *tag = quadword(gfx.gifTagIdx);
    tag[0] = 0x2000000000000000ull;
    tag[1] = 0x51;
    tag[2] = line.red | (static_cast<uint64_t>(line.green) << 32);
    tag[3] = line.blue;
    tag[0] = (tag[0] & 0xffffffffffff0000ull) | ((tag[0] & 0x7fff) + 1);
    gfx.buffIdx = gfx.buffIdx + 5;
    tag[5] = lineDepth(otz, 0x327960u);
    tag[4] = screenXY(line.x0, line.y0);
    lineVertex(line.red, line.green, line.blue, screenXY(line.x1, line.y1), lineDepth(otz, 0x327964u));
}

// A line in the world, seen through the current camera: clipped against the four sides of the view, projected,
// and drawn at the front of the depth buffer for flag 2 (the default), or brought forward for flag 8.
void LineRender3D(ptr32<_sLine3D> pLine)
{
    const _sLine3D &line = *pLine;
    const sRdrCamera &cam = G(rdrCameraG);
    const sMat33 &m = cam.mat;
    const ee_float ex = ee_itof(line.end.vx) - cam.pos.x, ey = ee_itof(line.end.vy) - cam.pos.y;
    const ee_float sy = ee_itof(line.start.vy) - cam.pos.y, sx = ee_itof(line.start.vx) - cam.pos.x;
    const ee_float ez = ee_itof(line.end.vz) - cam.pos.z;
    uint64_t flags = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(line.flags)));
    if (flags == 0)
        flags = 2;
    const ee_float sz = ee_itof(line.start.vz) - cam.pos.z;
    // Both ends in camera space; the direction stays as it was before clipping.
    ee_float s_z = sx * m.Zx + sy * m.Zy + sz * m.Zz;
    ee_float s_x = sx * m.Xx + sy * m.Xy + sz * m.Xz;
    ee_float e_y = ex * m.Yx + ey * m.Yy + ez * m.Yz;
    ee_float s_y = sx * m.Yx + sy * m.Yy + sz * m.Yz;
    ee_float e_x = ex * m.Xx + ey * m.Xy + ez * m.Xz;
    ee_float e_z = ex * m.Zx + ey * m.Zy + ez * m.Zz;
    const ee_float dy = s_y - e_y, dx = s_x - e_x, dz = s_z - e_z;
    const sVec3 *planes = &cam.normRight;
    for (int i = 0; i < 4; ++i)
    {
        const ee_float nx = planes[i].x, ny = planes[i].y, nz = planes[i].z;
        const ee_float ds = -(s_x * nx + s_y * ny + s_z * nz);
        const ee_float de = -(e_x * nx + e_y * ny + e_z * nz);
        if (ds.v < 0.0f)
        {
            if (de.v < 0.0f)
                return; // Wholly outside this side.
            const ee_float t = ds / (dx * nx + dy * ny + dz * nz);
            s_z = s_z + dz * t;
            s_y = s_y + dy * t;
            s_x = s_x + dx * t;
        }
        else if (de.v < 0.0f)
        {
            const ee_float t = de / (dx * nx + dy * ny + dz * nz);
            e_z = e_z + dz * t;
            e_x = e_x + dx * t;
            e_y = e_y + dy * t;
        }
    }
    const ee_float one = ee_float(1.0f);
    const ee_float invS = one / (1.0f <= s_z.v ? s_z : one);
    ee_float zS = one - invS;
    const ee_float invE = one / (1.0f <= e_z.v ? e_z : one);
    const ee_float pxS = s_x * invS * cam.dValX, pyS = s_y * invS * cam.dValY;
    ee_float zE = one - invE;
    const ee_float pxE = e_x * invE * cam.dValX, pyE = e_y * invE * cam.dValY;
    if ((flags & 2) != 0)
    {
        zS = 0.0f;
        zE = 0.0f;
    }
    else if ((flags & 8) != 0)
    {
        const ee_float nudge = *static_cast<ee_float *>(guest_host(0x327968u));
        zS = (zS - nudge) - nudge;
    }
    sGfxP2 &gfx = G(gGfxP2);
    lineState((flags & 1) != 0);
    gfx.buffIdx = gfx.buffIdx + 3;
    gfx.gifTagIdx = -1;
    gfx.buffIdx = gfx.buffIdx + 2;
    G(gLastTex) = ptr32<_sGfxTexture>(0xffffffffu);
    const auto project = [](ee_float x, ee_float y) {
        return (static_cast<uint64_t>(ee_ftoi((x + ee_float(2048.0f)) * ee_float(16.0f))) & 0xffffu) |
               ((static_cast<uint64_t>(ee_ftoi((ee_float(2048.0f) - y) * ee_float(16.0f))) & 0xffffu) << 32);
    };
    lineVertex(line.red, line.green, line.blue, project(pxS, pyS), lineDepth(zS, 0x32796cu));
    lineVertex(line.red, line.green, line.blue, project(pxE, pyE), lineDepth(zE, 0x327970u));
}

} // namespace rc

RC_DECOMP_FN(LineRender2D, 0x184e08)
RC_DECOMP_FN(LineRender3D, 0x185158)
