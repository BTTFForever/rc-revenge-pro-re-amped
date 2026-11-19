#include "emu_state.h"

#include "ps2_capture_hooks.h"
#include "ps2_runtime.h"

#include <cstring>

namespace rc::rec
{
    static_assert(ps2x::capture::kGsRegCount == kGsRegCount);

    std::vector<StateChunk> KeyframeState::chunks(bool final) const
    {
        std::vector<StateChunk> out;
        out.push_back({kStateGsMemory, gsMemory.data(), gsMemory.size()});
        out.push_back({kStateGsPriv, reinterpret_cast<const uint8_t *>(&gsPriv), sizeof(gsPriv)});
        out.push_back({kStateGsRegs, reinterpret_cast<const uint8_t *>(gsRegs), sizeof(gsRegs)});
        if (!final)
        {
            out.push_back({kStateVu1Memory, vu1Memory.data(), vu1Memory.size()});
            out.push_back({kStateVu1Regs, reinterpret_cast<const uint8_t *>(&vu1Regs), sizeof(vu1Regs)});
            out.push_back({kStateVif1, reinterpret_cast<const uint8_t *>(&vif1), sizeof(vif1)});
            if (!emu.empty())
                out.push_back({kStateEmu, emu.data(), emu.size()});
        }
        return out;
    }

    bool captureKeyframeState(PS2Runtime &runtime, KeyframeState &out)
    {
        // snapshotGs first: it waits for the VU1/GIF threads (ps2x::asyncvif::sync), so VU1 and VIF1 are at rest after.
        if (!ps2x::capture::snapshotGs(runtime, out.gsMemory, out.gsRegs))
            return false;
        PS2Memory &memory = runtime.memory();
        const GSRegisters &g = memory.gs();
        out.gsPriv = {g.pmode, g.smode1, g.smode2, g.srfsh, g.synch1, g.synch2, g.syncv, g.dispfb1, g.display1, g.dispfb2,
                      g.display2, g.extbuf, g.extdata, g.extwrite, g.bgcolor, g.csr.load(), g.vsyncTick.load(), g.imr, g.busdir,
                      g.siglblid};
        out.vu1Memory.resize(PS2_VU1_CODE_SIZE + PS2_VU1_DATA_SIZE);
        std::memcpy(out.vu1Memory.data(), memory.getVU1Code(), PS2_VU1_CODE_SIZE);
        std::memcpy(out.vu1Memory.data() + PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE);
        const VU1State &vu = runtime.vu1().state();
        std::memcpy(out.vu1Regs.vf, vu.vf, sizeof(out.vu1Regs.vf));
        std::memcpy(out.vu1Regs.vi, vu.vi, sizeof(out.vu1Regs.vi));
        std::memcpy(out.vu1Regs.acc, vu.acc, sizeof(out.vu1Regs.acc));
        out.vu1Regs.q = vu.q;
        out.vu1Regs.p = vu.p;
        out.vu1Regs.i = vu.i;
        out.vu1Regs.r = vu.r;
        out.vu1Regs.pc = vu.pc;
        out.vu1Regs.mac = vu.mac;
        out.vu1Regs.clip = vu.clip;
        out.vu1Regs.status = vu.status;
        const VIFRegisters &v = memory.vif1_regs;
        static_assert(sizeof(VIFRegisters) == offsetof(Vif1Regs, pad));
        std::memcpy(&out.vif1, &v, sizeof(VIFRegisters));
        out.vif1.pad = 0;
        ps2x::savestate::saveGraphics(runtime, out.emu);
        return true;
    }

    uint32_t emuStateVersion() { return ps2x::savestate::graphicsStateVersion(); }

    bool loadKeyframeState(PS2Runtime &runtime, const Keyframe &kf, std::string &error)
    {
        const StateChunk *emu = kf.find(kStateEmu), *vram = kf.find(kStateGsMemory);
        if (!emu || !vram)
        {
            error = "the keyframe has no EMUS or GSVR chunk";
            return false;
        }
        try
        {
            ps2x::savestate::loadGraphics(runtime, emu->data, emu->bytes, vram->data, vram->bytes);
        }
        catch (const std::exception &e)
        {
            error = std::string("EMUS does not load: ") + e.what();
            return false;
        }
        return true;
    }
}
