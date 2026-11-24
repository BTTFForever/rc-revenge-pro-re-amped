// More cars in a race (rc_cars.h, NOTES.md "Car limits").
#include "rc_cars.h"
#include "rc_decomp.h"
#include "rc_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rc {
angle ArcTan(int32 x, int32 y); // maths.cpp
}

namespace rc::cars {

namespace {

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

static_assert(kMenuLines + 0x1000u <= kGuestEnd, "the extension block is full");

bool logOn()
{
    static const bool on = [] { const char *v = std::getenv("RC_CARS_LOG"); return v && *v && *v != '0'; }();
    return on;
}

int clampCount(int n, int low) { return std::clamp(n, low, kMaxCars); }

// Game Setup's two items, by their selection tags.
constexpr uint32_t kSingleTag = 0x1000028du, kMultiTag = 0x1000028eu;

// ---- Start grid ----
struct V
{
    float x, y, z;
};
V slotAt(uint32_t start, int k) { return {float(at<int16_t>(start + 8u * k)), float(at<int16_t>(start + 8u * k + 2u)), float(at<int16_t>(start + 8u * k + 4u))}; }
float flatLength(float x, float z) { return std::sqrt(x * x + z * z); }

// Headings of the added places (the game's angle units >> 4, as ObjectCreateCars stores them), by place.
int16_t g_heading[kMaxCars];
int g_gridCount = 0;

// The node the point is in (as AINodeGetNearestHeading looks), else the nearest by its centre; null without nodes.
ptr32<_sAINode> nearestNode(const V &p)
{
    const sAINodeClass &ai = G(aiNodeVarG);
    if (!ai.pNode || ai.noNodesSingle <= 0)
        return nullptr;
    ptr32<_sAINode> inside = nullptr, any = nullptr;
    float bestInside = 1e30f, bestAny = 1e30f;
    for (int32_t i = 0; i < ai.noNodesSingle; ++i)
    {
        const ptr32<_sAINode> node = ai.pNode + i;
        const SVECTOR &c = node->centre;
        const float dx = c.vx - p.x, dy = c.vy - p.y, dz = c.vz - p.z, d = dx * dx + dy * dy + dz * dz;
        if (d < bestAny)
            bestAny = d, any = node;
        const bool in = p.x >= node->boundsMin[0] && p.x <= node->boundsMax[0] && p.z >= node->boundsMin[1] && p.z <= node->boundsMax[1] &&
                        std::fabs(c.vy - p.y) < 400.0f;
        if (in && d < bestInside)
            bestInside = d, inside = node;
    }
    return inside ? inside : any;
}

// The place `back` units behind `base` along the AI's route, at the same distance from the route's line, and the
// route's direction there (x, z, unit length).
void behindAlongRoute(const V &base, float back, V &out, float &fx, float &fz)
{
    const ptr32<_sAINode> start = nearestNode(base);
    if (!start)
    {
        out = base;
        return;
    }
    const auto unit = [](float &x, float &z) { const float l = flatLength(x, z); if (l > 1e-3f) { x /= l; z /= l; } else { x = 0.0f; z = 1.0f; } };
    fx = start->link.svForwardDir[0].vx;
    fz = start->link.svForwardDir[0].vz;
    unit(fx, fz);
    const SVECTOR &c0 = start->centre;
    const float ox = base.x - c0.vx, oz = base.z - c0.vz;
    const float along = ox * fx + oz * fz;      // ahead of the node's centre
    const float side = ox * fz - oz * fx;       // to one side of the route's line (right = (fz, -fx))
    const float lift = base.y - c0.vy;          // above the route's centre
    float target = along - back;
    ptr32<_sAINode> node = start, child = nullptr;
    for (int guard = 0; target < 0.0f && guard < 256; ++guard)
    {
        const ptr32<_sAINode> prev = node->prev[0];
        if (!prev)
            break;
        const SVECTOR &a = prev->centre, &b = node->centre;
        float seg = flatLength(float(b.vx - a.vx), float(b.vz - a.vz));
        if (seg < 1.0f)
            seg = 1.0f;
        target += seg;
        child = node;
        node = prev;
    }
    float px, py, pz;
    if (!child)
    {
        px = c0.vx + fx * target;
        pz = c0.vz + fz * target;
        py = c0.vy;
    }
    else
    {
        const SVECTOR &a = node->centre, &b = child->centre;
        fx = float(b.vx - a.vx);
        fz = float(b.vz - a.vz);
        const float seg = std::max(1.0f, flatLength(fx, fz));
        unit(fx, fz);
        const float t = std::clamp(target / seg, 0.0f, 1.0f);
        px = a.vx + (b.vx - a.vx) * t;
        py = a.vy + (b.vy - a.vy) * t;
        pz = a.vz + (b.vz - a.vz) * t;
    }
    out = {px + fz * side, py + lift, pz - fx * side};
}

// ---- Results and ranking lines ----
constexpr uint32_t kLineBytes = 0x18u, kLineText = 48u;
// Per menu (results 0, ranking 1) and added place (9th = 0): the item, then its text group and four texts.
uint32_t lineItem(int menu, int extra) { return kMenuLines + static_cast<uint32_t>(menu * (kMaxCars - 8) + extra) * (kLineBytes + 16u + 4u * kLineText); }

} // namespace

int singleCount(int cardValue)
{
    const int s = settings::get().carsSingle;
    return s > 0 ? clampCount(s, 2) : cardValue;
}

int multiCount(int cardValue)
{
    const int s = settings::get().carsMulti;
    return s > 0 ? clampCount(s, 2) : cardValue;
}

int cupCount() { return std::max(8, std::min(singleCount(8), kMaxCars)); }

void entrySettings(uint32_t item)
{
    const ptr32<sMenuItemStd> p(item);
    if (!p->pMenuVar)
        return;
    sMenuItemVar &var = *p->pMenuVar;
    const sFrontGameConfig &c = G(frontEndVarG).gameStateData.gameConfig;
    if (p->selectionTag == kSingleTag)
    {
        var.maxValue = kMaxCars;
        var.value = singleCount(c.singleCarCount);
    }
    else if (p->selectionTag == kMultiTag)
    {
        var.maxValue = kMaxCars;
        var.value = multiCount(c.multiCarCount);
    }
}

void chosen(int single, int multi, int32_t &cardSingle, int32_t &cardMulti)
{
    settings::Settings s = settings::get();
    // Kept in settings.ini once a count the memory card cannot hold is chosen (or once one was), so a save keeps the
    // PS2's ranges; until then the memory card's counts stay in charge, as on the PS2.
    const bool keep = s.carsSingle != 0 || s.carsMulti != 0 || single > kPs2Single || multi > kPs2Multi;
    if (keep && (s.carsSingle != single || s.carsMulti != multi))
    {
        s.carsSingle = clampCount(single, 2);
        s.carsMulti = clampCount(multi, 2);
        settings::set(s);
    }
    cardSingle = std::min(single, kPs2Single);
    cardMulti = std::min(multi, kPs2Multi);
}

uint32_t startGrid(int count)
{
    const uint32_t start = at<uint32_t>(G(pLvlG).address + 0x30u); // pStartData
    g_gridCount = 0;
    if (start == 0 || count <= 8)
        return start;
    count = std::min(count, kMaxCars);
    std::memcpy(guest_host(kStartGrid), guest_host(start), 8u * 8u);
    // The rows' spacing: the level's grid is four rows of two.
    float spacing = 0.0f;
    for (int s = 0; s < 2; ++s)
    {
        const V a = slotAt(start, s), b = slotAt(start, 6 + s);
        spacing = std::max(spacing, flatLength(b.x - a.x, b.z - a.z) / 3.0f);
    }
    if (spacing < 200.0f)
        spacing = 600.0f;
    for (int k = 8; k < count; ++k)
    {
        const int side = k & 1, rows = (k - 6 - side) / 2;
        const V base = slotAt(start, 6 + side);
        V p = base;
        float fx = 0.0f, fz = 0.0f;
        behindAlongRoute(base, spacing * static_cast<float>(rows), p, fx, fz);
        if (fx == 0.0f && fz == 0.0f)
        {
            // No route: straight on behind the grid.
            const V front = slotAt(start, side);
            fx = base.x - front.x;
            fz = base.z - front.z;
            const float l = std::max(1.0f, flatLength(fx, fz));
            fx = -fx / l;
            fz = -fz / l;
            p = {base.x - fx * spacing * rows, base.y, base.z - fz * spacing * rows};
        }
        const uint32_t slot = kStartGrid + 8u * static_cast<uint32_t>(k);
        at<int16_t>(slot) = static_cast<int16_t>(std::lround(p.x));
        at<int16_t>(slot + 2u) = static_cast<int16_t>(std::lround(p.y));
        at<int16_t>(slot + 4u) = static_cast<int16_t>(std::lround(p.z));
        at<int16_t>(slot + 6u) = 0;
        // As ObjectCreateCars turns the level's places: from a back place towards the front one, turned round.
        const int32_t dz = static_cast<int32_t>(std::lround(-fz * 4096.0f)), dx = static_cast<int32_t>(std::lround(-fx * 4096.0f));
        g_heading[k] = static_cast<int16_t>((static_cast<int32_t>(ArcTan(dz, dx)) + 0x8000) >> 4);
        if (logOn())
            std::fprintf(stderr, "[cars] level %u place %d: %d %d %d (from %d %d %d, %.0f back), heading %d\n", G(mainVarG).level, k,
                         at<int16_t>(slot), at<int16_t>(slot + 2u), at<int16_t>(slot + 4u), int(base.x), int(base.y), int(base.z),
                         spacing * rows, g_heading[k]);
    }
    g_gridCount = count;
    return kStartGrid;
}

bool addedPlaceHeading(int place, int16_t &heading)
{
    if (place < 8 || place >= g_gridCount)
        return false;
    heading = g_heading[place];
    return true;
}

uint8_t racePoints(int place) { return place >= 0 && place < 8 ? at<uint8_t>(sym::racePointsG + static_cast<uint32_t>(place)) : 0; }

void extendResultMenus()
{
    const uint32_t menus[2] = {sym::pResultsMenuG, sym::pRankingMenuG};
    for (int m = 0; m < 2; ++m)
    {
        const uint32_t header = at<uint32_t>(menus[m]);
        if (header == 0)
            continue;
        // The title line, then the places; find the last.
        uint32_t last = at<uint32_t>(header + 4u), lines = 0;
        if (last == 0)
            continue;
        uint32_t line = at<uint32_t>(last + 8u);
        for (; line != 0; line = at<uint32_t>(line + 8u))
        {
            ++lines;
            last = line;
        }
        if (lines != 8 || (last >= kGuestBase && last < kGuestEnd))
            continue; // already done (a restart keeps the level), or not the layout expected
        for (int e = 0; e < kMaxCars - 8; ++e)
        {
            const uint32_t item = lineItem(m, e), group = item + kLineBytes, texts = group + 16u;
            std::memcpy(guest_host(item), guest_host(last), kLineBytes);
            at<uint32_t>(item) = group;
            at<uint32_t>(item + 8u) = 0;
            // Each language's text of the 8th line with its place number: "8\xaa....\xaa...." -> "10\xaa...\xaa....".
            const uint32_t from = at<uint32_t>(last);
            for (uint32_t lang = 0; lang < 4; ++lang)
            {
                const uint32_t dst = texts + lang * kLineText;
                at<uint32_t>(group + 4u * lang) = dst;
                const uint32_t src = at<uint32_t>(from + 4u * lang);
                const char *s = src ? static_cast<const char *>(guest_host(src)) : "8";
                char buf[kLineText];
                const int place = 9 + e;
                int n = std::snprintf(buf, sizeof buf, "%d", place);
                const char *rest = s;
                while (*rest >= '0' && *rest <= '9')
                    ++rest;
                // A two-digit place: one fill dot fewer, so the line keeps its length.
                bool dropped = place < 10;
                for (; *rest && n < static_cast<int>(kLineText) - 1; ++rest)
                {
                    if (!dropped && *rest == '.')
                    {
                        dropped = true;
                        continue;
                    }
                    buf[n++] = *rest;
                }
                buf[n] = 0;
                std::memcpy(guest_host(dst), buf, static_cast<size_t>(n) + 1u);
            }
            at<uint32_t>(last + 8u) = item;
            at<uint8_t>(header + 0x62u) = static_cast<uint8_t>(at<uint8_t>(header + 0x62u) + 1u); // menuItems
            last = item;
        }
    }
    // The ranking shows the cup's cars only (the results hide the lines past the race's count themselves).
    const uint32_t ranking = at<uint32_t>(sym::pRankingMenuG);
    if (ranking != 0 && at<uint32_t>(ranking + 4u) != 0)
    {
        const int n = static_cast<int>(at<uint8_t>(sym::mainVarG + 0x369u)) + at<uint8_t>(sym::mainVarG + 0x36au);
        int i = 0;
        for (uint32_t line = at<uint32_t>(at<uint32_t>(ranking + 4u) + 8u); line; line = at<uint32_t>(line + 8u), ++i)
        {
            uint16_t &flags = at<uint16_t>(line + 0x14u);
            flags = i < std::max(n, 8) ? static_cast<uint16_t>(flags & 0xffdfu) : static_cast<uint16_t>(flags | 0x20u);
        }
    }
}

uint32_t placeSprite(uint32_t res, int place) { return place >= 0 && place < 8 ? at<uint32_t>(res + 0x24u + 4u * static_cast<uint32_t>(place)) : 0u; }

namespace {
int g_chosenAI = -1, g_droppedAI = -1; // the computer cars chosen, and what dropCars left (-1: none dropped)
// GS memory one car type's textures took (NOTES.md "Car limits", the attract demo with 12 random cars on 30 levels'
// loads), by car type 1-19: on a land level, a boat level (the car without its parts, and its boat), a level of both;
// 0 where not measured, which counts as the most measured of that kind plus a margin (the reserve).
constexpr uint32_t kCarCost[20] = {0, 0xc500, 0x8d00, 0xb500, 0, 0xb000, 0x4d00, 0, 0, 0x9f00, 0x7f00, 0, 0x8b00, 0x9100, 0, 0, 0x14c00, 0xce00, 0xdb00, 0};
constexpr uint32_t kBoatCost[20] = {0, 0x6000, 0x8500, 0xfc00, 0, 0x3300, 0x2b00, 0, 0, 0x8e00, 0x7900, 0, 0x8d00, 0x10300, 0, 0, 0x6900, 0xaa00, 0xe200, 0};
constexpr uint32_t kBothCost[20] = {0, 0x10000, 0xa100, 0x18000, 0, 0xbb00, 0x5800, 0, 0, 0x10500, 0xc800, 0, 0xd700, 0x16f00, 0, 0, 0x19500, 0x10200, 0xe700, 0};
constexpr uint32_t kCarReserve = 0x17000u, kBoatReserve = 0x12000u, kBothReserve = 0x1b000u, kMargin = 0x1000u;
}

uint32_t textureCost(int place)
{
    const sCarListClass &list = G(carListVarG);
    const uint32_t level = G(mainVarG).level % 12u;
    const bool both = (level == 9 || level == 0xb) && G(mainVarG).level != 'a', boats = level == 5 || level == 0 || level == 8;
    const uint32_t type = static_cast<uint32_t>(list.car[place].type);
    const uint32_t known = type < 20u ? (both ? kBothCost : boats ? kBoatCost : kCarCost)[type] : 0u;
    return known != 0 ? known + kMargin : both ? kBothReserve : boats ? kBoatReserve : kCarReserve;
}

bool roomForCar(int place)
{
    const sCarListClass &list = G(carListVarG);
    if (place < 8 || place < static_cast<int>(G(mainVarG).noHuman))
        return true;
    for (int j = 0; j < place; ++j)
        if (list.car[j].type == list.car[place].type)
            return true; // its model and textures are loaded already
    const uint32_t reserve = textureCost(place);
    const uint32_t top = G(nextTexAddr);
    return top < 0x400000u && 0x400000u - top >= reserve;
}

void dropCars(int keep)
{
    mainClass &m = G(mainVarG);
    const int ai = std::max(0, keep - static_cast<int>(m.noHuman));
    if (g_chosenAI < 0)
        g_chosenAI = m.noAI;
    m.noAI = static_cast<uint8>(ai);
    G(aiVarG).noAIPlayers = ai;
    g_droppedAI = ai;
    std::fprintf(stderr, "[cars] level %u: GS texture memory full (0x%x used of 0x400000), racing %d cars instead of %d\n", m.level,
                 G(nextTexAddr), keep, static_cast<int>(m.noHuman) + g_chosenAI);
}

void restoreCount()
{
    mainClass &m = G(mainVarG);
    if (g_chosenAI >= 0 && m.noAI == g_droppedAI)
        m.noAI = static_cast<uint8>(g_chosenAI);
    g_chosenAI = g_droppedAI = -1;
}

void forgetCount() { g_chosenAI = g_droppedAI = -1; }

void fillCupList(int count)
{
    // RemoveDuplicates takes the computer's cars from this list in order, skipping the players' cars: entries up to
    // count - 1 (8 or fewer cars: the original 8, and no extra random numbers drawn).
    if (count <= 8)
        return;
    const int want = std::min({count + 2, kCupListSize, 19});
    const auto entry = [](int i) -> uint32_t & { return at<uint32_t>(kCupCarList + 4u * static_cast<uint32_t>(i)); };
    const auto listed = [&](uint32_t car, int upTo) { for (int j = 0; j < upTo; ++j) if (entry(j) == car) return true; return false; };
    for (int i = 8; i < want; ++i)
    {
        // Another car: one the player has unlocked if any is left, else any (there are 19).
        uint32_t car = 0;
        for (int pass = 0; pass < 2 && car == 0; ++pass)
        {
            uint32_t options[19];
            int n = 0;
            for (uint32_t c = 1; c <= 19; ++c)
                if (!listed(c, i) && (pass == 1 || guest_call<int32_t>(sym::CheckCarAvailability, c) == 1))
                    options[n++] = c;
            if (n != 0)
                car = options[static_cast<uint32_t>(guest_call<int32_t>(sym::rand)) % static_cast<uint32_t>(n)];
        }
        entry(i) = car != 0 ? car : 1u;
    }
    if (logOn())
    {
        std::fprintf(stderr, "[cars] cup list:");
        for (int i = 0; i < want; ++i)
            std::fprintf(stderr, " %u", entry(i));
        std::fprintf(stderr, "\n");
    }
}

} // namespace rc::cars
