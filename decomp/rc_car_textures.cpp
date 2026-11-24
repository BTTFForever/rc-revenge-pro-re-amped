// Cars past the 8th whose textures do not fit in GS memory: texture banks (rc_cars.h, NOTES.md "Car limits").
//
// The level's textures and the first 8 cars' stay where the game puts them. Of the further car types, as many as fit
// with room left for the largest of the rest are loaded as usual; the rest ("banked") are each loaded in turn into one
// shared region (the slot) at the top of the textures, and the host keeps a copy of the slot after each one
// (GS::saveTextureBank). Every shape fixed while a banked car loads is remembered with its bank; before such a shape
// is drawn (RendObjRender, and its transparent triangles in the sorted alpha chain) the frame's DMA chain gets a small
// packet that writes the bank's number to an unused GS register (kGSTextureBankRegister), and the GS frontend copies
// that bank back into the slot unless it holds it already. Everything after the marker in the stream (TEX0 and its
// palette load, the track batches) sees the bank's textures, as if they had always been there.
#include "rc_cars.h"
#include "rc_decomp.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_texture_bank.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rc {
u32 Rdr_WhichProg();                 // rdr.cpp
void Rdr_UploadProg(ptr32<u32> pProg); // rdr.cpp
void Rdr_AddDmaCall(ptr32<void> pPacket);
} // namespace rc

namespace rc::cars {

namespace {

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

constexpr uint32_t kGsEnd = 0x400000u, kPage = 0x2000u;
constexpr uint32_t kShapeProgram = sym::TRdr_VUShapeProg, kTrackProgramEnd = sym::EnvRdr_VUShapeProg;
// The marker packets (a copy of RdrState's packet layout: ret tag, STCYCL and UNPACK of 7 quadwords, a GIF tag of six
// A+D writes, then MSCAL and FLUSHA), two per bank: one starting the shape renderer's register-kick routine (0x2050),
// one the shiny pass renderer's (0x2370), whichever is loaded when it is sent.
constexpr uint32_t kPacketBytes = 0x90u;
uint32_t packetAddress(int bank, int variant) { return kBankPackets + (static_cast<uint32_t>(bank) * 2u + static_cast<uint32_t>(variant)) * kPacketBytes; }
static_assert(kBankPackets + kMaxBanks * 2u * 0x90u <= kGuestEnd, "the extension block is full");

bool enabled()
{
    // Opt-in (RC_CAR_TEXTURE_BANKS=1) until finished and tested.
    static const bool on = [] { const char *v = std::getenv("RC_CAR_TEXTURE_BANKS"); return v && *v == '1'; }();
    return on;
}

bool logOn()
{
    static const bool on = [] { const char *v = std::getenv("RC_CARS_LOG"); return v && *v && *v != '0'; }();
    return on;
}

struct State
{
    bool planned = false;     // the plan for this load is made (at the first car past the 8th)
    int residentLeft = 0;     // further new types still loaded the usual way
    bool banking = false;     // the rest go to banks
    uint32_t slot = 0;        // the shared region's start (bytes, page aligned)
    uint32_t slotEnd = 0;     // the highest end a bank reached
    int banks = 0;            // banks saved
    int loading = -1;         // the bank being loaded, -1: none
    bool overflow = false;    // the bank being loaded passed the end of GS memory
    bool full = false;        // a car did not fit: it and those after it are left out
    std::unordered_map<uint32_t, int> shapes; // shape (sRdrVUShape) -> bank
    std::unordered_map<uint32_t, int> alpha;  // this view's sorted transparent triangles of banked shapes -> bank
    int last = -1;            // the bank the chain last asked for this frame
    uint64_t markers = 0;     // markers sent (the log)
};
State g;

uint32_t costOf(int place) { return textureCost(place); }

void writePackets()
{
    for (int bank = 0; bank < kMaxBanks; ++bank)
        for (int variant = 0; variant < 2; ++variant)
        {
            uint8_t *p = static_cast<uint8_t *>(guest_host(packetAddress(bank, variant)));
            std::memset(p, 0, kPacketBytes);
            const auto put32 = [p](uint32_t off, uint32_t v) { std::memcpy(p + off, &v, 4); };
            const auto put64 = [p](uint32_t off, uint64_t v) { std::memcpy(p + off, &v, 8); };
            put64(0x00, 0x60000008ull); // ret, 8 quadwords
            put32(0x08, 0x1000404u);    // STCYCL 4,4
            put32(0x0c, 0x6c078000u);   // UNPACK V4-32, 7 quadwords, to the double buffer
            put64(0x10, 0x1000000000008006ull); // GIF tag: 6 loops, end of packet, one register (A+D)
            put64(0x18, 0xeull);
            put64(0x20, static_cast<uint64_t>(bank));
            put64(0x28, kGSTextureBankRegister);
            for (uint32_t r = 1; r < 6; ++r)
            {
                put64(0x20 + r * 0x10u, 0);
                put64(0x28 + r * 0x10u, 0x7fu); // no-op
            }
            put32(0x80, variant == 0 ? (0x2050u >> 3) | 0x14000000u : (0x2370u >> 3) | 0x14000000u); // MSCAL
            put32(0x84, 0x13000000u); // FLUSHA
        }
}

} // namespace

void texturesReset()
{
    if (g.banks != 0 && g_runtime)
        g_runtime->gs().clearTextureBanks();
    if (logOn() && g.markers != 0)
        std::fprintf(stderr, "[cars] texture banks: %llu markers sent\n", static_cast<unsigned long long>(g.markers));
    g = State{};
}

// RC_CAR_BANK_TEST=<n> (a test aid): every car type from car n on goes to a bank, even where it would fit (n = 0: all of
// them, so every car drawn needs its bank).
int bankTestFirst()
{
    static const int first = [] { const char *v = std::getenv("RC_CAR_BANK_TEST"); return v && *v ? std::atoi(v) : -1; }();
    return first;
}

CarTextures carTextures(int place, int total)
{
    const int first = bankTestFirst();
    if (first >= 0 ? place < first : (place < 8 || place < static_cast<int>(G(mainVarG).noHuman)))
        return CarTextures::Resident;
    if (!enabled())
        return roomForCar(place) ? CarTextures::Resident : CarTextures::Drop;
    if (g.full)
        return CarTextures::Drop;
    const sCarListClass &list = G(carListVarG);
    for (int j = 0; j < place; ++j)
        if (list.car[j].type == list.car[place].type)
            return CarTextures::Resident; // its model is loaded already (perhaps banked: its shapes carry their bank)
    if (!g.planned)
    {
        // The new types from here on, in order, and what each takes (rc_cars.cpp's measured table, or the reserve).
        g.planned = true;
        std::vector<uint32_t> costs;
        for (int i = place; i < total; ++i)
        {
            bool seen = false;
            for (int j = 0; j < i && !seen; ++j)
                seen = list.car[j].type == list.car[i].type;
            if (!seen)
                costs.push_back(costOf(i));
        }
        const uint32_t top = G(nextTexAddr), room = top < kGsEnd ? kGsEnd - top : 0u;
        // The most types loaded as usual such that the largest of the rest still fits in the region after them.
        int resident = 0;
        uint32_t sum = 0;
        for (int n = static_cast<int>(costs.size()); n >= 0; --n)
        {
            uint32_t s = 0, largest = 0;
            for (int j = 0; j < n; ++j)
                s += costs[static_cast<size_t>(j)];
            for (size_t j = static_cast<size_t>(n); j < costs.size(); ++j)
                largest = std::max(largest, costs[j]);
            if (s + (largest != 0 ? largest + kPage : 0u) <= room)
            {
                resident = n;
                sum = s;
                break;
            }
        }
        if (first >= 0)
            resident = 0;
        g.residentLeft = resident;
        if (logOn() || resident < static_cast<int>(costs.size()))
            std::fprintf(stderr, "[cars] level %u: GS textures at 0x%x, %zu more car types (estimated 0x%x for the first %d), %d loaded as usual, %d share one region\n",
                         G(mainVarG).level, top, costs.size(), sum, resident, resident, static_cast<int>(costs.size()) - resident);
    }
    if (g.residentLeft > 0)
    {
        --g.residentLeft;
        return CarTextures::Resident;
    }
    if (g.banks >= kMaxBanks)
        return CarTextures::Drop;
    if (!g.banking)
    {
        g.banking = true;
        g.slot = (G(nextTexAddr) + kPage - 1u) & ~(kPage - 1u);
        g.slotEnd = g.slot;
        writePackets();
    }
    // The region must take this type (the estimate), or the car is left out.
    if (g.slot >= kGsEnd || kGsEnd - g.slot < costOf(place))
        return CarTextures::Drop;
    G(nextTexAddr) = g.slot;
    g.loading = g.banks;
    g.overflow = false;
    return CarTextures::Banked;
}

bool carLoaded(int place)
{
    if (g.loading < 0)
        return true;
    const int bank = g.loading;
    g.loading = -1;
    const uint32_t end = G(nextTexAddr);
    if (g.overflow || end > kGsEnd)
    {
        // Some of its textures were not uploaded: the car cannot race.
        for (auto it = g.shapes.begin(); it != g.shapes.end();)
            it = it->second == bank ? g.shapes.erase(it) : std::next(it);
        G(nextTexAddr) = g.slotEnd;
        g.full = true;
        std::fprintf(stderr, "[cars] level %u: car %d's textures do not fit in the shared region (0x%x-0x%x)\n", G(mainVarG).level, place, g.slot, end);
        return false;
    }
    const uint32_t pages = (end - g.slot + kPage - 1u) / kPage;
    g.slotEnd = std::max(g.slotEnd, g.slot + pages * kPage);
    if (g_runtime && pages != 0)
        g_runtime->gs().saveTextureBank(bank, g.slot / kPage, pages);
    ++g.banks;
    // Whatever is placed after the cars goes above the largest bank.
    G(nextTexAddr) = g.slotEnd;
    if (logOn() || bank == 0)
        std::fprintf(stderr, "[cars] level %u: car %d (type %d) in texture bank %d, 0x%x-0x%x\n", G(mainVarG).level, place,
                     static_cast<int>(G(carListVarG).car[place].type), bank, g.slot, end);
    return true;
}

bool bankUpload(uint32_t address, uint32_t size)
{
    if (g.loading < 0)
        return true;
    if (address + size > kGsEnd)
    {
        g.overflow = true;
        return false;
    }
    return true;
}

void bankTexture(uint32_t texture)
{
    if (g.loading >= 0 && at<uint32_t>(texture + 4u) > 1u)
        std::fprintf(stderr, "[cars] warning: an animated texture (0x%x) in texture bank %d\n", texture, g.loading);
}

void bankShape(uint32_t shape)
{
    if (g.loading >= 0)
        g.shapes[shape] = g.loading;
}

int shapeBank(uint32_t shape)
{
    if (g.shapes.empty())
        return -1;
    const auto it = g.shapes.find(shape);
    return it == g.shapes.end() ? -1 : it->second;
}

void texturesFrameStart() { g.last = -1; }

void useBank(int bank)
{
    // RC_CAR_BANK_NOMARK=1 (a test aid): no markers, so banked cars show whichever bank the region holds.
    static const bool noMarkers = [] { const char *v = std::getenv("RC_CAR_BANK_NOMARK"); return v && *v == '1'; }();
    if (bank < 0 || bank == g.last || noMarkers)
        return;
    g.last = bank;
    int variant = 0;
    const uint32_t program = Rdr_WhichProg();
    if (program == kTrackProgramEnd)
        variant = 1;
    else if (program != kShapeProgram)
        Rdr_UploadProg(ptr32<u32>(kShapeProgram));
    Rdr_AddDmaCall(ptr32<void>(packetAddress(bank, variant)));
    ++g.markers;
}

void noteAlpha(uint32_t triangle, int bank)
{
    if (bank >= 0)
        g.alpha[triangle] = bank;
}

int alphaBank(uint32_t triangle)
{
    static const bool noMarkers = [] { const char *v = std::getenv("RC_CAR_BANK_NOMARK"); return v && *v == '1'; }();
    if (g.alpha.empty() || noMarkers)
        return -1;
    const auto it = g.alpha.find(triangle);
    return it == g.alpha.end() ? -1 : it->second;
}

uint32_t alphaMarker(int bank)
{
    g.last = bank;
    ++g.markers;
    return packetAddress(bank, 0);
}

int lastBank() { return g.last; }

void alphaDone()
{
    if (logOn() && !g.alpha.empty())
    {
        static int logged = 0;
        if (logged++ < 3)
            std::fprintf(stderr, "[cars] texture banks: %zu transparent triangles of banked cars in a chain\n", g.alpha.size());
    }
    g.alpha.clear();
}

} // namespace rc::cars
