#pragma once

// Everything derived from a GsState (RENDERER_2D_DESIGN.md sections 2.3, 2.4, 4 and 5): the only place that turns
// registers into drawing decisions. The Vulkan replay and the CPU model (step 7 batch 2) both call planState; the reader
// calls it to count the guards. No dependencies but gs_state.h, C++17.
//
// The GS rule this plans for is the reference's (the runtime's software GS, gs_cpu_raster_inline.h ShadePixel):
//   scissor; alpha test on As giving a write mask (AFAIL); Z test on ZTST alone (the reference ignores ZTE);
//   blend Cv = clamp(((A - B) * C >> 7) + D) unless PABE and As < 0x80; FBMSK; Z written unless ZMSK.

#include "gs_state.h"

#include <cstdint>

namespace rc::rd
{
    // Blend configurations (4.2): the pipeline always has srcColor = ONE, dstColor = SRC1_COLOR when blending.
    enum GsBlendConfig : uint8_t
    {
        kBlendOff = 0,    // disabled: out0 is the colour
        kBlendMix = 1,    // ADD: out0 + Cd * out1
        kBlendMixSub = 2, // REVERSE_SUBTRACT: Cd * out1 - out0
    };
    // Which source term the shader computes (4.2).
    enum GsSourceTerm : uint8_t
    {
        kTermExact = 0,  // a = |P(Cs)|, the GS rule evaluated in integers with Cd = 0 (and clamped when blend is off);
                         // the destination coefficient is the integer kd (0 or 1): integer exact
        kTermLinear = 1, // a = |ks| * Cs as a fraction of a level with the source bias of 4.3; out1 = kd = F/128 or
                         // (128 - F)/128
    };
    // A blend coefficient of Cs or Cd (ks, kd in 4.2) as a form in F = C / 128.
    enum GsCoef : uint8_t
    {
        kCoefZero = 0, kCoefOne = 1, kCoefF = 2, kCoefOneMinusF = 3, kCoefMinusF = 4, kCoefOnePlusF = 5, kCoefTwo = 6, kCoefMinusOne = 7,
    };
    // F's source (`factor`).
    enum GsFactor : uint8_t
    {
        kFactorNone = 0, kFactorAs = 1, kFactorFix = 2,
    };
    // Z tests: the GS's ZTST codes. RHI: NEVER, ALWAYS, GEQUAL, GREATER.
    enum GsDepthCompare : uint8_t
    {
        kDepthNever = 0, kDepthAlways = 1, kDepthGequal = 2, kDepthGreater = 3,
    };
    // Alpha tests: the GS's ATST codes.
    enum GsAlphaTest : uint8_t
    {
        kAtstNever = 0, kAtstAlways = 1, kAtstLess = 2, kAtstLequal = 3, kAtstEqual = 4, kAtstGequal = 5, kAtstGreater = 6, kAtstNotequal = 7,
    };
    // Colour mask bits.
    enum : uint8_t
    {
        kMaskR = 1, kMaskG = 2, kMaskB = 4, kMaskA = 8, kMaskRgb = 7, kMaskRgba = 15,
    };

    // Guards (2.4): states the renderer does not draw as the GS would. Each is counted by the reader; some drop the
    // draw (GsPlan::unsupported), the others draw without the feature.
    enum GsGuard : uint8_t
    {
        kGuardNone = 0,
        kGuardPabeBlend,  // PABE 1, ABE 1 without step 5's precondition: blended as if PABE were 0
        kGuardDestAlpha,  // ALPHA C = Ad (drawn with F = 0x80) or DATE 1 (ignored)
        kGuardFrame,      // FRAME not CT32 / FBW 10 / FBP 0x0 or 0x50 / FBMSK 0, 0xff000000 or 0x00ffffff: dropped
        kGuardZbuf,       // ZBUF not ZBP 0xa0 or a 16-bit Z format: dropped
        kGuardFba,        // FBA 1: drawn without it
        kGuardDthe,       // DTHE 1: drawn without dither
        kGuardColclamp,   // COLCLAMP 0: drawn clamped
        kGuardFge,        // fog: drawn without
        kGuardAa1,        // antialiasing: drawn without
        kGuardCtxt,       // context 2 (the state holds its registers; listed because RC never uses it)
        kGuardBlendForm,  // a destination coefficient of 1+F, -F, 2 or -1 (not expressible): drawn with blend off
        kGuardMipmap,     // TEX1 MXL > 0: drawn from level 0
        kGuardTexFormat,  // a texture format the cache does not decode: dropped
        kGuardZte,        // ZTE 0: drawn as the reference does (ZTST applies)
        kGuardCount
    };
    const char *gsGuardName(uint32_t guard);

    struct GsPlan
    {
        uint8_t target;        // 0: FBP 0x0, 1: FBP 0x50
        uint8_t blend;         // GsBlendConfig
        uint8_t term;          // GsSourceTerm
        uint8_t factor;        // GsFactor: F is As or FIX (or none)
        uint8_t fix;           // the FIX value (0x80 where C = Ad was replaced, DEST_ALPHA)
        uint8_t ks, kd;        // GsCoef: the coefficients of Cs and Cd (after FIX 0 / 0x80 made them integers)
        uint8_t abcd;          // ALPHA bits 0-7 for P(Cs) (kTermExact: a = gsSourcePart(abcd | fix << 32, Cs, As)); 0
                               // (P = Cs) when the blend is off by ABE 0 or by PABE under its precondition
        uint8_t exactBlend;    // the blend is integer exact for every pixel (4.3)
        uint8_t atst, aref;    // the alpha test as the shader evaluates it; ALWAYS when ATE is 0 or the test cannot
                               // change what is written
        uint8_t passColorMask, failColorMask; // kMask bits; fail 0 and failDepthWrite 0: a plain discard
        uint8_t passDepthWrite, failDepthWrite;
        uint8_t split;         // failing pixels write something other than passing ones and other than nothing, with a
                               // depth write involved: two draws per group of primitives (5.2)
        uint8_t alphaByBlend;  // RGB_ONLY with no depth write: one draw, alpha written through SRC1_ALPHA (5.2)
        uint8_t depthCompare;  // GsDepthCompare
        uint8_t tfx, tcc, linear, wrapU, wrapV;
        uint8_t unsupported;   // the guard that drops the draw, 0 when drawable
        uint16_t guards;       // bit per GsGuard that fired (1 << guard)
    };

    GsPlan planState(const GsState &s);

    // The GS's own source-only term P(Cs) = ((A - B) * C >> 7) + D with Cd = 0 (no clamp): what kTermExact computes.
    int gsSourcePart(uint64_t alpha, int cs, int as);
}
