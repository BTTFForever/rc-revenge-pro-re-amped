// rc_replay's vk consumer, the chained replay: see frame_vk.h.
#include "frame_vk.h"

#include "png.h"

#include "../../render/frame/frame_reader.h"
#include "../../render/gs2d/gs2d_vk.h"
#include "../../render/gs2d/gs_plan.h"
#include "../../render/tex/gs_layout.h"

#include "rhi.h"
#include "vk/rhi_vk.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace rc::rec;
using namespace rc::rd;

namespace rc::replay
{
    namespace
    {
        constexpr uint32_t kW = Gs2dVk::kWidth, kH = Gs2dVk::kHeight, kPixels = kW * kH;
        constexpr uint32_t kZbp = 0xa0u;
        using Clock = std::chrono::steady_clock;

        double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

        // Colour targets 0x0 and 0x50 (CT32) and Z at kZbp (the low 24 bits), 640 x 256 each, from a picture of GS memory.
        void readPlanes(const uint8_t *mem, std::vector<uint32_t> (&planes)[3])
        {
            const LayoutTables &lt = layoutTables();
            for (uint32_t p = 0; p < 3; ++p)
            {
                planes[p].resize(kPixels);
                for (uint32_t y = 0; y < kH; ++y)
                    for (uint32_t x = 0; x < kW; ++x)
                    {
                        const uint32_t a = p < 2 ? gsAddr32(lt, p ? 0x50u * 32u : 0u, 10u, x, y) : gsAddrZ32(lt, kZbp * 32u, 10u, x, y);
                        uint32_t v;
                        std::memcpy(&v, mem + (a & (kGsBytes - 1u)), 4);
                        planes[p][y * kW + x] = p < 2 ? v : (v & 0xFFFFFFu);
                    }
            }
        }

        void copyCmd(DrawList &to, const DrawList &from, const DrawCmd &c)
        {
            DrawCmd &n = to.add(c.op, from.data(c), c.payloadBytes);
            const uint32_t offset = n.payloadOffset;
            n = c;
            n.payloadOffset = offset;
        }

        FrameReader::Options readerOptions()
        {
            FrameReader::Options o;
            o.gif.seed = true;          // GSRG at the first keyframe: every register is known from there on
            o.gif.cullOffscreen = true; // the reference's own early exits (RENDERER_3D_DESIGN.md 8)
            o.gif.path1Prims = true;    // K2 is drawn
            o.gif.frameUploads = true;  // uploads into the frame buffers, in order with the draws
            return o;
        }
    }

    struct FrameVkCheck::Impl
    {
        FrameVkOptions opt;
        FrameReader reader{readerOptions()};
        Gs2dVk vk;
        bool rhiUp = false, up = false, seeded = false;
        std::string adapter;
        uint32_t validationErrors = 0;
        uint32_t height = 240;
        bool exact = false, europe = false;

        // The cycle in progress.
        uint64_t cycle = 0;
        std::vector<uint32_t> cuts; // command counts after each channel 1 kick (the depth fault's cut points)
        std::string order;          // the cycle's kicks: '1' VIF1, '3' a drawing PATH3 kick, 'u' an upload into a frame buffer, 'c' the clear
        double cycleReaderMs = 0.0;
        uint64_t path1KicksHere = 0;
        std::set<uint32_t> texturesSent; // the texture fault: ids given to the GPU

        // Totals.
        uint64_t cycles = 0, cyclesHashed = 0, hashEqual = 0, cyclesRefi = 0, refiEqual = 0, refiPixels = 0, refiPixelsDiffer = 0, cyclesNoRef = 0;
        uint64_t refiChannelDiff[5] = {}; // R, G, B values off by 1, 2, 3, 4 or more; alpha values that differ
        int refiMaxDiff = 0;
        uint64_t cyclesFailing = 0, otherFbp = 0, fbpMismatch = 0, drawFailures = 0, readFailures = 0, seeds = 0, seedFailures = 0;
        uint64_t keyframesJudged = 0, keyframesEqual = 0, keyPixelsDiffer[3] = {};
        uint64_t cyclesK2 = 0, cyclesK1Only = 0, cyclesUploadOnly = 0, cyclesEmpty = 0;
        uint64_t path1Kicks = 0, path3Kicks = 0, faultCyclesApplied = 0, faultTexturesDropped = 0, faultClearsDropped = 0;
        uint64_t draws = 0, barriers = 0, prims = 0, lists = 0, maxBarriers = 0, maxDraws = 0, listBytes = 0, maxListBytes = 0;
        double readerMs = 0.0, path1Ms = 0.0, maxPath1Ms = 0.0, buildMs = 0.0, gpuMs = 0.0, maxGpuMs = 0.0, drawWallMs = 0.0, readbackMs = 0.0;
        uint64_t gpuSamples = 0, gpuK2Samples = 0;
        double gpuK2Ms = 0.0, maxGpuK2Ms = 0.0;
        int64_t firstFail = -1, lastPass = -1;
        uint32_t pngsWritten = 0;
        std::vector<std::string> failures;
        uint64_t failuresDropped = 0;
        Clock::time_point t0 = Clock::now();

        void fail(const std::string &line)
        {
            if (failures.size() < 24u)
                failures.push_back(line);
            else
                ++failuresDropped;
        }
        bool faultOn(const char *name) const
        {
            if (opt.fault != name || opt.faultCycle < 0 || static_cast<int64_t>(cycle) < opt.faultCycle)
                return false;
            return opt.faultCycles == 0u || cycle < static_cast<uint64_t>(opt.faultCycle) + opt.faultCycles;
        }
        bool seedTargets(const uint8_t *mem);
        bool zeroDepth();
        bool drawCycle(const DrawList &list);
        void compareKeyframe(const Keyframe &kf, const uint8_t *mem);
    };

    FrameVkCheck::FrameVkCheck(const FrameVkOptions &options) : m(std::make_unique<Impl>()) { m->opt = options; }

    FrameVkCheck::~FrameVkCheck()
    {
        if (m->rhiUp)
        {
            m->vk.destroy();
            rhi_shutdown();
        }
    }

    bool FrameVkCheck::start(std::string *why)
    {
        RhiDeviceDesc desc{};
        desc.sdlWindow = nullptr;
        desc.vsync = false;
        desc.debugLayers = m->opt.validation;
        desc.appName = "rc_replay vk";
        if (!rhi_init(&desc))
        {
            if (why)
                *why = "rhi_init failed (no Vulkan device?)";
            return false;
        }
        m->rhiUp = true;
        m->adapter = rhi_adapter_name() ? rhi_adapter_name() : "?";
        Gs2dVk::Options o;
        o.blend = m->opt.blend == "ff" ? Gs2dVk::kBlendFixed : m->opt.blend == "read" ? Gs2dVk::kBlendRead : Gs2dVk::kBlendAuto;
        m->vk.setOptions(o);
        if (!m->vk.create(why))
            return false;
        m->up = true;
        std::printf("vk: %s, validation %s, blend %s (%s)%s%s\n", m->adapter.c_str(), rhi_limits()->validationLayer ? "on" : "off",
                    m->vk.readMode() ? "read" : "fixed function", m->vk.readMode() ? "exact" : "D-2D-1: not expected to be equal",
                    m->opt.fault.empty() ? "" : "; planted fault ", m->opt.fault.c_str());
        return true;
    }

    bool FrameVkCheck::Impl::seedTargets(const uint8_t *mem)
    {
        std::vector<uint32_t> planes[3];
        readPlanes(mem, planes);
        // seed() writes the depth with each target: the second call leaves the Z plane as read.
        return vk.seed(0u, planes[0].data(), planes[2].data()) && vk.seed(1u, planes[1].data(), planes[2].data());
    }

    bool FrameVkCheck::Impl::zeroDepth()
    {
        std::vector<uint32_t> c[2];
        const std::vector<uint32_t> zero(kPixels, 0u);
        return vk.readColour(0u, c[0]) && vk.readColour(1u, c[1]) && vk.seed(0u, c[0].data(), zero.data()) && vk.seed(1u, c[1].data(), zero.data());
    }

    void FrameVkCheck::Impl::compareKeyframe(const Keyframe &kf, const uint8_t *mem)
    {
        ++keyframesJudged;
        std::vector<uint32_t> ref[3], got[3];
        readPlanes(mem, ref);
        if (!vk.readColour(0u, got[0]) || !vk.readColour(1u, got[1]) || !vk.readDepth(got[2]))
        {
            ++readFailures;
            fail("keyframe at cycle " + std::to_string(kf.header.cycle) + ": read-back failed (or a depth that is not a whole Z)");
            return;
        }
        static const char *const name[3] = {"target 0x0", "target 0x50", "Z"};
        uint64_t differ[3] = {};
        std::string text;
        for (uint32_t p = 0; p < 3; ++p)
        {
            int fx = -1, fy = -1;
            for (uint32_t i = 0; i < kPixels; ++i)
                if (got[p][i] != ref[p][i] && differ[p]++ == 0)
                {
                    fx = static_cast<int>(i % kW);
                    fy = static_cast<int>(i / kW);
                }
            keyPixelsDiffer[p] += differ[p];
            if (differ[p])
            {
                char line[160];
                std::snprintf(line, sizeof line, "%s%s %" PRIu64 " pixels (first (%d, %d) vk %08x keyframe %08x)", text.empty() ? "" : "; ", name[p], differ[p], fx,
                              fy, got[p][fy * kW + fx], ref[p][fy * kW + fx]);
                text += line;
                if (!opt.pngDir.empty() && pngsWritten < 12u)
                {
                    ++pngsWritten;
                    std::filesystem::create_directories(opt.pngDir);
                    const std::string base = opt.pngDir + "/keyframe_" + std::to_string(kf.header.cycle) + "_" + (p == 0 ? "fb00" : p == 1 ? "fb50" : "z");
                    std::vector<uint8_t> pic(static_cast<size_t>(kPixels) * 4u);
                    auto plane = [&](const std::vector<uint32_t> &v)
                    {
                        for (uint32_t i = 0; i < kPixels; ++i)
                        {
                            // Z as a grey ramp of its high bytes (the low byte in blue).
                            const uint32_t w = p < 2 ? v[i] : ((v[i] >> 16) & 0xFFu) | (((v[i] >> 8) & 0xFFu) << 8) | ((v[i] & 0xFFu) << 16);
                            std::memcpy(&pic[i * 4u], &w, 4);
                        }
                    };
                    plane(got[p]);
                    writePng(base + "_vk.png", pic.data(), kW, kH);
                    plane(ref[p]);
                    writePng(base + "_ref.png", pic.data(), kW, kH);
                    for (uint32_t i = 0; i < kPixels; ++i)
                    {
                        const bool d = got[p][i] != ref[p][i];
                        pic[i * 4u] = d ? 0xFFu : 0u;
                        pic[i * 4u + 1u] = pic[i * 4u + 2u] = 0u;
                    }
                    writePng(base + "_diff.png", pic.data(), kW, kH);
                }
            }
        }
        if (text.empty())
            ++keyframesEqual;
        else
            fail("keyframe at cycle " + std::to_string(kf.header.cycle) + ((kf.header.flags & kKeyFinal) ? " (final)" : "") + ": " + text);
        if (!opt.quiet)
            std::printf("keyframe at cycle %" PRIu64 "%s: targets and Z %s\n", static_cast<uint64_t>(kf.header.cycle), (kf.header.flags & kKeyFinal) ? " (final)" : "",
                        text.empty() ? "equal" : "DIFFER");
    }

    void FrameVkCheck::open(const FileInfo &info)
    {
        m->reader.open(info);
        if (info.header.height)
            m->height = info.header.height;
        m->exact = (info.header.flags & kFileExact) != 0;
        m->europe = info.header.gameVersion != 0;
        if (m->opt.dumpPath.empty())
            m->opt.dumpPath = info.path;
    }

    void FrameVkCheck::keyframe(const Keyframe &kf)
    {
        const StateChunk *g = kf.find(kStateGsMemory);
        const bool haveMem = g && g->bytes >= kGsMemBytes;
        const bool starts = !(kf.header.flags & kKeyFinal) && (!m->seeded || (kf.header.flags & kKeyAfterStateLoad));
        if (m->up && haveMem)
        {
            if (starts)
            {
                // Where a replay starts (the first keyframe, and one after a save-state load: the recording's machine was
                // replaced there). The only seeding.
                if (m->seedTargets(g->data))
                    ++m->seeds;
                else
                {
                    ++m->seedFailures;
                    m->fail("keyframe at cycle " + std::to_string(kf.header.cycle) + ": seeding the targets failed");
                }
                m->seeded = true;
                if (!m->opt.quiet)
                    std::printf("keyframe at cycle %" PRIu64 "%s: seeded\n", static_cast<uint64_t>(kf.header.cycle),
                                (kf.header.flags & kKeyAfterStateLoad) ? " (after a save-state load)" : "");
            }
            else if (m->seeded && m->exact && (kf.header.flags & kKeyVramExact))
                m->compareKeyframe(kf, g->data);
        }
        m->reader.keyframe(kf);
    }

    void FrameVkCheck::cycleBegin(uint64_t cycle)
    {
        m->cycle = cycle;
        m->cuts.clear();
        m->order.clear();
        m->cycleReaderMs = 0.0;
        m->path1KicksHere = 0;
        m->reader.cycleBegin(cycle);
    }

    void FrameVkCheck::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) { m->reader.hw(address, width, value, callerRa); }

    void FrameVkCheck::kick(const KickView &k, const RamView &ram)
    {
        const Clock::time_point t0 = Clock::now();
        const uint64_t guarded = m->reader.stats().guardedKicks;
        const size_t before = m->reader.list().cmds.size();
        m->reader.kick(k, ram);
        const double ms = msSince(t0);
        m->cycleReaderMs += ms;
        const DrawList &l = m->reader.list();
        if (k.channel == 1u)
        {
            ++m->path1Kicks;
            ++m->path1KicksHere;
            m->path1Ms += ms;
            m->maxPath1Ms = std::max(m->maxPath1Ms, ms);
            m->cuts.push_back(static_cast<uint32_t>(l.cmds.size()));
            m->order += '1';
            if (m->reader.stats().guardedKicks != guarded)
                m->fail("cycle " + std::to_string(k.cycle) + " kick " + std::to_string(k.index) + ": a guard stopped the VIF1 kick: " + m->reader.guardText());
        }
        else if (k.channel == 2u)
        {
            ++m->path3Kicks;
            // What the commands made so far say about this kick (an upload's IMAGE_BLIT may only appear at the next kick
            // or at the cycle's close: the cycle's class below reads the whole list).
            bool prims = false, clear = false, blit = false;
            for (size_t i = before; i < l.cmds.size(); ++i)
            {
                prims |= l.cmds[i].op == kOpScreenPrims;
                clear |= l.cmds[i].op == kOpClear;
                blit |= l.cmds[i].op == kOpImageBlit;
            }
            if (blit)
                m->order += 'u';
            if (clear)
                m->order += 'c';
            if (prims)
                m->order += '3';
        }
    }

    bool FrameVkCheck::Impl::drawCycle(const DrawList &full)
    {
        // The planted faults that change the list: copies with commands left out.
        const DrawList *list = &full;
        DrawList edited;
        std::vector<uint32_t> cutsEdited = cuts;
        const bool noClear = faultOn("clear"), noTexture = faultOn("texture");
        if (noClear || noTexture)
        {
            size_t nextCut = 0;
            for (size_t i = 0; i < full.cmds.size(); ++i)
            {
                while (nextCut < cuts.size() && cuts[nextCut] == i)
                    cutsEdited[nextCut++] = static_cast<uint32_t>(edited.cmds.size());
                const DrawCmd &c = full.cmds[i];
                if (noClear && c.op == kOpClear)
                {
                    ++faultClearsDropped;
                    continue;
                }
                if (noTexture && c.op == kOpTexture && texturesSent.count(c.a))
                {
                    ++faultTexturesDropped;
                    continue;
                }
                copyCmd(edited, full, c);
            }
            while (nextCut < cuts.size())
                cutsEdited[nextCut++] = static_cast<uint32_t>(edited.cmds.size());
            list = &edited;
        }
        for (const DrawCmd &c : full.cmds)
            if (c.op == kOpTexture)
                texturesSent.insert(c.a);

        auto account = [&](bool k2)
        {
            const Gs2dVk::ListStats &s = vk.lastList();
            ++lists;
            draws += s.draws;
            barriers += s.barriers;
            prims += s.prims;
            buildMs += s.buildMs;
            maxBarriers = std::max(maxBarriers, s.barriers);
            maxDraws = std::max(maxDraws, s.draws);
            if (s.gpuMs > 0.0)
            {
                ++gpuSamples;
                gpuMs += s.gpuMs;
                maxGpuMs = std::max(maxGpuMs, s.gpuMs);
                if (k2)
                {
                    ++gpuK2Samples;
                    gpuK2Ms += s.gpuMs;
                    maxGpuK2Ms = std::max(maxGpuK2Ms, s.gpuMs);
                }
            }
        };

        if (!faultOn("depth") || cutsEdited.empty())
        {
            if (list->cmds.empty())
                return true;
            const bool ok = vk.draw(*list);
            account(path1KicksHere != 0);
            return ok;
        }
        // The depth fault: the list in parts, the depth buffer zeroed after each channel 1 kick's commands. A later part
        // starts with the STATE in force (Gs2dVk keeps the textures, not the state, between lists).
        bool ok = true;
        size_t from = 0;
        const DrawCmd *stateCmd = nullptr;
        std::vector<uint32_t> ends = cutsEdited;
        ends.push_back(static_cast<uint32_t>(list->cmds.size()));
        for (size_t part = 0; part < ends.size(); ++part)
        {
            DrawList piece;
            if (stateCmd)
                copyCmd(piece, *list, *stateCmd);
            for (size_t i = from; i < ends[part]; ++i)
            {
                copyCmd(piece, *list, list->cmds[i]);
                if (list->cmds[i].op == kOpState)
                    stateCmd = &list->cmds[i];
            }
            from = ends[part];
            if (!piece.cmds.empty())
            {
                ok &= vk.draw(piece);
                account(false);
            }
            if (part + 1u < ends.size())
                ok &= zeroDepth();
        }
        return ok;
    }

    void FrameVkCheck::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        Clock::time_point t0 = Clock::now();
        m->reader.cycleEnd(info, refRgba);
        m->cycleReaderMs += msSince(t0);
        m->readerMs += m->cycleReaderMs;
        if (!m->up || !m->seeded)
            return;
        ++m->cycles;
        const DrawList &list = m->reader.list();
        const uint64_t bytes = list.payload.size() + list.cmds.size() * sizeof(DrawCmd);
        m->listBytes += bytes;
        m->maxListBytes = std::max(m->maxListBytes, bytes);
        bool anyPrims = false, anyBlit = false;
        for (const DrawCmd &c : list.cmds)
        {
            anyPrims |= c.op == kOpScreenPrims || c.op == kOpClear;
            anyBlit |= c.op == kOpImageBlit;
        }
        if (m->path1KicksHere)
            ++m->cyclesK2;
        else if (anyPrims)
            ++m->cyclesK1Only;
        else if (anyBlit)
            ++m->cyclesUploadOnly;
        else
            ++m->cyclesEmpty;
        const bool faulted = m->faultOn(m->opt.fault.c_str());
        m->faultCyclesApplied += faulted;

        t0 = Clock::now();
        const bool drew = m->drawCycle(list);
        m->drawWallMs += msSince(t0);
        const uint64_t cycle = info.end.cycle;
        if (!drew)
        {
            ++m->drawFailures;
            m->fail("cycle " + std::to_string(cycle) + ": Gs2dVk refused the list");
        }

        // The picture: the top H rows of target[DISPFB2.FBP].
        const uint32_t fbp = static_cast<uint32_t>(info.end.dispfb2 & 0x1FFu);
        const uint32_t w = kW, h = std::min(info.end.refHeight ? info.end.refHeight : m->height, kH);
        const bool hasHash = m->exact && info.end.refWidth == w && info.end.refHeight != 0u;
        if (!hasHash)
        {
            ++m->cyclesNoRef;
            return;
        }
        bool bad = false;
        std::string why;
        if (fbp != 0x0u && fbp != 0x50u)
        {
            ++m->otherFbp;
            bad = true;
            why = "DISPFB2 shows FBP 0x" + std::to_string(fbp) + ", neither target";
        }
        if (fbp != info.end.refFbp)
        {
            ++m->fbpMismatch;
            bad = true;
            why += std::string(why.empty() ? "" : "; ") + "DISPFB2's FBP is not the reference picture's";
        }
        uint32_t t = fbp == 0x50u ? 1u : 0u;
        if (m->faultOn("flip"))
            t ^= 1u;
        t0 = Clock::now();
        std::vector<uint32_t> words;
        if (!m->vk.readColour(t, words) || words.size() < static_cast<size_t>(kW) * h)
        {
            ++m->readFailures;
            m->fail("cycle " + std::to_string(cycle) + ": read-back failed");
            return;
        }
        std::vector<uint8_t> picture(static_cast<size_t>(w) * h * 4u);
        std::memcpy(picture.data(), words.data(), picture.size()); // kW == w: the rows are contiguous
        Hash64 hash;
        hash.update(picture.data(), picture.size());
        const uint64_t got = hash.finish();
        m->readbackMs += msSince(t0);
        ++m->cyclesHashed;
        const bool hashOk = got == info.end.refHash;
        m->hashEqual += hashOk;
        uint64_t differ = 0;
        int fx = -1, fy = -1;
        const bool hasRefi = refRgba != nullptr;
        if (hasRefi)
        {
            ++m->cyclesRefi;
            for (uint32_t i = 0; i < w * h; ++i)
                if (std::memcmp(&picture[i * 4u], refRgba + i * 4u, 4) != 0)
                {
                    if (differ++ == 0)
                    {
                        fx = static_cast<int>(i % w);
                        fy = static_cast<int>(i / w);
                    }
                    // By how much (the fixed-function fallback's report; with the exact blend there is nothing to count).
                    for (uint32_t c = 0; c < 4u; ++c)
                    {
                        const int d = std::abs(static_cast<int>(picture[i * 4u + c]) - static_cast<int>(refRgba[i * 4u + c]));
                        if (d)
                            ++m->refiChannelDiff[c == 3u ? 4 : std::min(d, 4) - 1];
                        m->refiMaxDiff = std::max(m->refiMaxDiff, d);
                    }
                }
            m->refiPixels += static_cast<uint64_t>(w) * h;
            m->refiPixelsDiffer += differ;
            m->refiEqual += differ == 0;
        }
        bad |= !hashOk || differ != 0;
        if (!m->opt.quiet)
            std::printf("cycle %" PRIu64 ": FBP 0x%x, kicks %s, %zu commands, hash %s%s\n", cycle, fbp, m->order.empty() ? "-" : m->order.c_str(), list.cmds.size(),
                        hashOk ? "equal" : "DIFFERS", hasRefi ? (differ ? ", stored picture DIFFERS" : ", stored picture equal") : "");
        if (!bad)
        {
            if (m->firstFail < 0)
                m->lastPass = static_cast<int64_t>(cycle);
            return;
        }
        ++m->cyclesFailing;
        if (m->firstFail < 0)
            m->firstFail = static_cast<int64_t>(cycle);
        char line[640];
        if (hasRefi && differ)
        {
            const uint8_t *n = &picture[(static_cast<size_t>(fy) * w + fx) * 4u], *r = refRgba + (static_cast<size_t>(fy) * w + fx) * 4u;
            std::snprintf(line, sizeof line, "cycle %" PRIu64 " (FBP 0x%x, kicks %s): %" PRIu64 " of %u pixels differ from the stored picture; first (%d, %d) vk %02x%02x%02x%02x reference %02x%02x%02x%02x%s%s",
                          cycle, fbp, m->order.c_str(), differ, w * h, fx, fy, n[0], n[1], n[2], n[3], r[0], r[1], r[2], r[3], why.empty() ? "" : "; ", why.c_str());
        }
        else if (!hashOk)
            std::snprintf(line, sizeof line,
                          "cycle %" PRIu64 " (FBP 0x%x, kicks %s): picture hash %016" PRIx64 ", reference %016" PRIx64 "; no stored picture: rc_replay '%s' --consumer emu --cycles %" PRIu64
                          "-%" PRIu64 " --png <dir> writes the reference%s%s",
                          cycle, fbp, m->order.c_str(), got, static_cast<uint64_t>(info.end.refHash), m->opt.dumpPath.c_str(), cycle, cycle, why.empty() ? "" : "; ", why.c_str());
        else
            std::snprintf(line, sizeof line, "cycle %" PRIu64 ": %s", cycle, why.c_str());
        m->fail(line);
        if (!m->opt.pngDir.empty() && m->pngsWritten < 12u)
        {
            ++m->pngsWritten;
            std::filesystem::create_directories(m->opt.pngDir);
            const std::string base = m->opt.pngDir + "/fail_" + std::to_string(cycle);
            writePng(base + "_vk.png", picture.data(), w, h);
            if (hasRefi)
            {
                writePng(base + "_ref.png", refRgba, w, h);
                std::vector<uint8_t> pic(picture.size(), 0u);
                for (uint32_t i = 0; i < w * h; ++i)
                {
                    // Red where the colour differs, blue where only the alpha does.
                    const bool rgb = std::memcmp(&picture[i * 4u], refRgba + i * 4u, 3) != 0, a = picture[i * 4u + 3u] != refRgba[i * 4u + 3u];
                    pic[i * 4u] = rgb ? 0xFFu : 0u;
                    pic[i * 4u + 2u] = !rgb && a ? 0xFFu : 0u;
                }
                writePng(base + "_diff.png", pic.data(), w, h);
            }
        }
    }

    bool FrameVkCheck::failed() const
    {
        const Gs2dVk::Stats &vs = m->vk.stats();
        return m->cyclesFailing || m->drawFailures || m->readFailures || m->seedFailures || m->validationErrors || m->keyframesJudged != m->keyframesEqual ||
               m->reader.stats().guardedKicks || !m->seeds || !m->cyclesHashed || vs.unsupported || vs.spritesSkipped || vs.beyondViewport ||
               m->reader.gif().stats().frameUploadsOther;
    }

    void FrameVkCheck::summary()
    {
        const bool layer = m->rhiUp && rhi_limits() && rhi_limits()->validationLayer;
        const bool read = m->up && m->vk.readMode();
        const Gs2dVk::Stats vs = m->vk.stats();
        if (m->rhiUp)
        {
            m->vk.destroy();
            m->validationErrors += rhi_vk_validation_error_count();
            rhi_shutdown();
            m->rhiUp = m->up = false;
        }
        const double wall = msSince(m->t0) / 1000.0;
        const Gif2dReader::Stats &gs = m->reader.gif().stats();
        const FrameReader::Stats &fs = m->reader.stats();
        const double n = m->cycles ? static_cast<double>(m->cycles) : 1.0;
        std::printf("vk: chained replay, %s, %s: seeded at %" PRIu64 " keyframe%s (the first, and after a save-state load), nothing seeded afterwards; %" PRIu64
                    " cycles (%" PRIu64 " with K2, %" PRIu64 " with PATH3 draws only, %" PRIu64 " with uploads only, %" PRIu64 " with nothing drawn), %" PRIu64 " VIF1 kicks, %" PRIu64
                    " PATH3 kicks\n",
                    m->europe ? "Europe" : "USA", read ? "exact blend" : "fixed-function blend", m->seeds, m->seeds == 1 ? "" : "s", m->cycles, m->cyclesK2, m->cyclesK1Only,
                    m->cyclesUploadOnly, m->cyclesEmpty, m->path1Kicks, m->path3Kicks);
        std::printf("vk: pictures at the flip: %" PRIu64 " of %" PRIu64 " hashes equal to END.refHash; stored pictures (REFI) %" PRIu64 " of %" PRIu64 " equal, %" PRIu64 " of %" PRIu64
                    " pixels differ; cycles without a reference %" PRIu64 "; failing cycles %" PRIu64,
                    m->hashEqual, m->cyclesHashed, m->refiEqual, m->cyclesRefi, m->refiPixelsDiffer, m->refiPixels, m->cyclesNoRef, m->cyclesFailing);
        if (m->firstFail >= 0)
            std::printf(" (first %" PRId64 ", last equal before it %" PRId64 ")", m->firstFail, m->lastPass);
        std::printf("; DISPFB2 on neither target %" PRIu64 ", FBP not the reference's %" PRIu64 "\n", m->otherFbp, m->fbpMismatch);
        if (m->refiPixelsDiffer)
            std::printf("vk: stored pictures, the differing values: R, G, B off by 1: %" PRIu64 ", by 2: %" PRIu64 ", by 3: %" PRIu64 ", by 4 or more: %" PRIu64 " (largest %d); alpha: %" PRIu64
                        " (of %" PRIu64 " colour values)\n",
                        m->refiChannelDiff[0], m->refiChannelDiff[1], m->refiChannelDiff[2], m->refiChannelDiff[3], m->refiMaxDiff, m->refiChannelDiff[4], m->refiPixels * 3u);
        std::printf("vk: later keyframes (targets 0x0 and 0x50 in R, G, B, A and Z, 640 x 256): %" PRIu64 " of %" PRIu64 " equal; pixels differing %" PRIu64 " / %" PRIu64 " / %" PRIu64 "\n",
                    m->keyframesEqual, m->keyframesJudged, m->keyPixelsDiffer[0], m->keyPixelsDiffer[1], m->keyPixelsDiffer[2]);
        std::printf("vk: reader: guarded VIF1 kicks %" PRIu64 "; primitives %" PRIu64 " (dropped %" PRIu64 ": FBW 0 %" PRIu64 ", unknown register %" PRIu64 ", guard %" PRIu64
                    "); culled %" PRIu64 " without area, %" PRIu64 " outside the scissor; MIPMAP primitives %" PRIu64 "; draws that needed a seeded register %" PRIu64
                    " (a default %" PRIu64 "); uploads into the frame buffers %" PRIu64 " in %" PRIu64 " blits, %" PRIu64 " not placed; textures sent %" PRIu64 " (%.1f MB)\n",
                    fs.guardedKicks, gs.prims, gs.dropped, gs.droppedFbw0, gs.droppedUnknown, gs.droppedGuard, gs.culledZeroArea, gs.culledOffscreen, gs.guardPrims[kGuardMipmap],
                    [&] { uint64_t s = 0; for (uint64_t v : gs.neededSeeded) s += v; return s; }(), [&] { uint64_t s = 0; for (uint64_t v : gs.neededDefault) s += v; return s; }(),
                    gs.frameUploads, gs.frameUploadBlits, gs.frameUploadsOther, gs.textureCmds, static_cast<double>(gs.textureBytes) / 1048576.0);
        {
            // Which registers those draws took from the keyframe's GSRG (never written by the stream since).
            std::string groups;
            for (uint32_t b = 0; b < kGsGroupCount; ++b)
                if (gs.neededSeeded[b] || gs.neededDefault[b])
                    groups += std::string(groups.empty() ? "" : ", ") + gsGroupName(b) + " " + std::to_string(gs.neededSeeded[b]) + (gs.neededDefault[b] ? " (default " + std::to_string(gs.neededDefault[b]) + ")" : "");
            std::printf("vk: reader: registers still the keyframe's GSRG at a draw, by group: %s; with a value other than the default: %s\n", groups.empty() ? "none" : groups.c_str(),
                        [&] { std::string t; for (uint32_t b = 0; b < kGsGroupCount; ++b) if (gs.neededSeededNotDefault[b]) t += std::string(t.empty() ? "" : ", ") + gsGroupName(b) + " " + std::to_string(gs.neededSeededNotDefault[b]); return t.empty() ? std::string("none") : t; }().c_str());
        }
        std::printf("vk: Gs2dVk: %" PRIu64 " lists, %" PRIu64 " draws (%.1f a cycle, largest %" PRIu64 "), %" PRIu64 " primitives, %" PRIu64 " barriers (%.2f a cycle, largest %" PRIu64
                    "), clears %" PRIu64 ", unsupported states %" PRIu64 ", sprites skipped %" PRIu64 ", vertices beyond the viewport %" PRIu64 ", triangles with a vertex Z >= 2^24 %" PRIu64
                    "; draw failures %" PRIu64 ", read-back failures %" PRIu64 "; validation errors %u%s\n",
                    m->lists, m->draws, static_cast<double>(m->draws) / n, m->maxDraws, m->prims, m->barriers, static_cast<double>(m->barriers) / n, m->maxBarriers, vs.clears,
                    vs.unsupported, vs.spritesSkipped, vs.beyondViewport, vs.zAbove24, m->drawFailures, m->readFailures, m->validationErrors, layer ? "" : " (layer off)");
        std::printf("vk: time: %.1f s for the run, %.2f ms a cycle: the reader %.2f ms (a VIF1 kick %.2f ms, largest %.2f), the draw %.2f ms (the list's build %.2f ms; waited for), "
                    "the read-back and hash %.2f ms; GPU %.3f ms a list (largest %.3f), with K2 %.3f ms (largest %.3f) [off-line: the GPU's clock is low, U3]; "
                    "list %.2f MB a cycle (largest %.2f MB)\n",
                    wall, wall * 1000.0 / n, m->readerMs / n, m->path1Kicks ? m->path1Ms / static_cast<double>(m->path1Kicks) : 0.0, m->maxPath1Ms, m->drawWallMs / n,
                    m->buildMs / n, m->readbackMs / n, m->gpuSamples ? m->gpuMs / static_cast<double>(m->gpuSamples) : 0.0, m->maxGpuMs,
                    m->gpuK2Samples ? m->gpuK2Ms / static_cast<double>(m->gpuK2Samples) : 0.0, m->maxGpuK2Ms, static_cast<double>(m->listBytes) / n / 1048576.0,
                    static_cast<double>(m->maxListBytes) / 1048576.0);
        if (!m->opt.fault.empty())
            std::printf("vk: planted fault %s from cycle %" PRId64 ": applied in %" PRIu64 " cycles (clears left out %" PRIu64 ", TEXTURE commands left out %" PRIu64 ")\n",
                        m->opt.fault.c_str(), m->opt.faultCycle, m->faultCyclesApplied, m->faultClearsDropped, m->faultTexturesDropped);
        for (const std::string &f : m->failures)
            std::printf("FAIL vk %s\n", f.c_str());
        if (m->failuresDropped)
            std::printf("FAIL vk ... and %" PRIu64 " more\n", m->failuresDropped);
    }
}
