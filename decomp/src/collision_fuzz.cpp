// Builds random but valid collision data for the random test. See collision_fuzz.h.
//
// The scratch area starts as random bytes, so every field not set here is random.

#include "collision_fuzz.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rc::fuzz {

namespace {

// Where things go in the 0x8000-byte scratch area.
constexpr uint32_t kGrid = 0x0000u, kCells = 0x0100u, kIds = 0x0200u, kVectors = 0x0500u, kPolys = 0x0600u;
constexpr uint32_t kArgs = 0x1800u; // Arguments, 16 bytes apart.
constexpr uint32_t kCarCols = 0x2000u, kCarBounds = 0x2100u, kSpheres = 0x2400u;
constexpr uint32_t kObject = 0x4000u, kObjectCarCol = 0x5000u, kObjectPhysics = 0x5800u;

constexpr int kCellsPerSide = 4, kNumPolys = 40, kMaxInside = 7, kMaxBorder = 3;
constexpr float kCell = 512.0f, kGridMin = -1024.0f, kGridMax = 1024.0f;

struct Rng
{
    uint64_t &state;

    uint32_t next() { return nextFuzz(state); }
    int below(int count) { return static_cast<int>(next() % static_cast<uint32_t>(count)); }
    bool chance(int percent) { return below(100) < percent; }
    float range(float low, float high) { return low + (high - low) * (static_cast<float>(next() >> 8) / 16777216.0f); }
};

struct Vec
{
    float x, y, z;
};

Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

Vec normalised(Vec v)
{
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x / length, v.y / length, v.z / length};
}

void setArg(R5900Context *ctx, int index, uint32_t value)
{
    SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value));
}

// A convex poly with three or four vertices in a plane, as the level files hold them: bounds in the padding, the
// plane's distance in d, and the axis the normal is closest to in ma. Returns its middle.
Vec makePoly(Rng &rng, _sColPolyPSX &poly, float spread = 900.0f)
{
    const Vec centre = {rng.range(-spread, spread), rng.range(-100.0f, 500.0f), rng.range(-spread, spread)};
    Vec normal;
    if (rng.chance(65))
        normal = normalised({rng.range(-0.5f, 0.5f), 1.0f, rng.range(-0.5f, 0.5f)}); // Floor.
    else
    {
        const float heading = rng.range(0.0f, 6.2831853f);
        normal = normalised({std::cos(heading), rng.range(-0.4f, 0.4f), std::sin(heading)}); // Wall.
    }
    if (rng.chance(10))
        normal = {-normal.x, -normal.y, -normal.z};

    const Vec across = normalised(cross(normal, std::fabs(normal.y) < 0.9f ? Vec{0.0f, 1.0f, 0.0f} : Vec{1.0f, 0.0f, 0.0f}));
    const Vec along = cross(normal, across);
    const int count = rng.chance(50) ? 4 : 3;
    const float radius = rng.range(200.0f, spread), turn = rng.range(0.0f, 6.2831853f);
    const bool reversed = rng.chance(50);
    Vec corners[4];
    int highest = 0;
    for (int i = 0; i < count; ++i)
    {
        const float angle = turn + (static_cast<float>(reversed ? count - i : i) + rng.range(-0.3f, 0.3f)) * 6.2831853f / static_cast<float>(count);
        const float u = radius * std::cos(angle), w = radius * std::sin(angle);
        corners[i] = {centre.x + across.x * u + along.x * w, centre.y + across.y * u + along.y * w, centre.z + across.z * u + along.z * w};
        if (corners[i].y > corners[highest].y)
            highest = i;
    }

    poly.n.x = normal.x;
    poly.n.y = normal.y;
    poly.n.z = normal.z;
    Vec min = corners[0], max = corners[0];
    for (int i = 0; i < count; ++i)
    {
        const Vec corner = corners[(highest + i) % count]; // The highest comes first: cells sort by it.
        poly.v[i].x = corner.x;
        poly.v[i].y = corner.y;
        poly.v[i].z = corner.z;
        min = {std::min(min.x, corner.x), std::min(min.y, corner.y), std::min(min.z, corner.z)};
        max = {std::max(max.x, corner.x), std::max(max.y, corner.y), std::max(max.z, corner.z)};
    }
    poly.v[0].pad = min.y;
    poly.v[1].pad = min.x;
    poly.v[2].pad = max.x;
    poly.v[3].pad = min.z;
    poly.bbMaxZ = max.z;
    poly.numVerts = static_cast<uint8>(count);
    poly.gid = rng.chance(25) ? 2 : static_cast<uint8>(rng.below(2)); // 2 is a water surface.
    poly.ma = std::fabs(normal.y) > 0.567f ? 0 : std::fabs(normal.x) > 0.567f ? 1 : 2;
    static constexpr uint8 kMaterials[] = {0, 5, 0x12, 0x13, 0x1F};
    poly.mat = kMaterials[rng.below(5)];
    poly.d = -(normal.x * corners[highest].x + normal.y * corners[highest].y + normal.z * corners[highest].z);
    poly.sid = static_cast<int16>(rng.below(4));
    return centre;
}

// A level of 4 by 4 cells with polys scattered over it, installed as the game's collision grid.
struct World
{
    _sColGridPSX *grid;
    _sColPolyPSX *polys;
    Vec centres[kNumPolys];
};

World buildWorld(Rng &rng, uint32_t scratch)
{
    World world;
    world.grid = static_cast<_sColGridPSX *>(guest_host(scratch + kGrid));
    world.polys = static_cast<_sColPolyPSX *>(guest_host(scratch + kPolys));
    for (int i = 0; i < kNumPolys; ++i)
        world.centres[i] = makePoly(rng, world.polys[i]);

    _sColGridPSX &grid = *world.grid;
    grid.min.x = kGridMin;
    grid.min.z = kGridMin;
    grid.max.x = kGridMax;
    grid.max.z = kGridMax;
    grid.numCellsX = kCellsPerSide;
    grid.numCellsZ = kCellsPerSide;
    grid.totalCells = kCellsPerSide * kCellsPerSide;
    grid.numPolys = kNumPolys;
    grid.numVectors = 4;
    grid.pCell = ptr32<_sColGridCellPSX>(scratch + kCells);
    grid.pPoly = ptr32<_sColPolyPSX>(scratch + kPolys);
    grid.pIds = ptr32<uint32>(scratch + kIds);
    grid.pVec = ptr32<_sColCellVecPSX>(scratch + kVectors);

    // Each cell lists the polys over it, then those over its 128-unit border only, each list highest first.
    _sColGridCellPSX *cells = grid.pCell.get();
    uint32 *ids = grid.pIds.get();
    uint32_t used = 0;
    for (int cellX = 0; cellX < kCellsPerSide; ++cellX)
    {
        for (int cellZ = 0; cellZ < kCellsPerSide; ++cellZ)
        {
            const float lowX = kGridMin + kCell * static_cast<float>(cellX), lowZ = kGridMin + kCell * static_cast<float>(cellZ);
            std::vector<uint32_t> lists[2];
            for (uint32_t id = 0; id < static_cast<uint32_t>(kNumPolys); ++id)
            {
                const _sColPolyPSX &poly = world.polys[id];
                const auto overlaps = [&](float border)
                {
                    return poly.v[1].pad.v <= lowX + kCell + border && lowX - border <= poly.v[2].pad.v &&
                           poly.v[3].pad.v <= lowZ + kCell + border && lowZ - border <= poly.bbMaxZ.v;
                };
                if (overlaps(0.0f))
                    lists[0].push_back(id);
                else if (overlaps(128.0f))
                    lists[1].push_back(id);
            }
            _sColGridCellPSX &cell = cells[cellX * kCellsPerSide + cellZ];
            cell.list.offset = used;
            for (int list = 0; list < 2; ++list)
            {
                std::vector<uint32_t> &chosen = lists[list];
                const size_t limit = list == 0 ? kMaxInside : kMaxBorder;
                while (chosen.size() > limit)
                    chosen.erase(chosen.begin() + rng.below(static_cast<int>(chosen.size())));
                std::sort(chosen.begin(), chosen.end(), [&](uint32_t a, uint32_t b) { return world.polys[a].v[0].y.v > world.polys[b].v[0].y.v; });
                cell.list.num[list] = static_cast<uint32>(chosen.size());
                for (const uint32_t id : chosen)
                    ids[used++] = id;
            }
        }
    }
    G(acolG).pGrid = ptr32<_sColGridPSX>(scratch + kGrid);
    return world;
}

_sColCellVecPSX &vecArg(uint32_t scratch, int index)
{
    return *static_cast<_sColCellVecPSX *>(guest_host(scratch + kArgs + 16u * static_cast<uint32_t>(index)));
}

uint32_t argAddress(uint32_t scratch, int index)
{
    return scratch + kArgs + 16u * static_cast<uint32_t>(index);
}

void setVec(_sColCellVecPSX &out, Vec v)
{
    out.x = v.x;
    out.y = v.y;
    out.z = v.z;
}

// Mostly on the grid, sometimes off it.
Vec worldPosition(Rng &rng)
{
    const float reach = rng.chance(85) ? 1000.0f : 1300.0f;
    return {rng.range(-reach, reach), rng.range(-300.0f, 1200.0f), rng.range(-reach, reach)};
}

Vec nearPoly(Rng &rng, Vec centre, float reach)
{
    return {centre.x + rng.range(-reach, reach), centre.y + rng.range(-reach, reach), centre.z + rng.range(-reach, reach)};
}

// A segment that often passes down through the level.
void segment(Rng &rng, Vec &start, Vec &end)
{
    start = worldPosition(rng);
    if (rng.chance(70))
    {
        start.y = rng.range(300.0f, 1500.0f);
        end = {start.x + rng.range(-600.0f, 600.0f), rng.range(-800.0f, 200.0f), start.z + rng.range(-600.0f, 600.0f)};
    }
    else
        end = worldPosition(rng);
}

// AColCellCastCell copies a 16-byte point whose last word it never set, so that word is whatever its stack held.
// This makes the stack hold what the destination has already, so that leaving the destination alone matches.
void matchUnsetPadding(R5900Context *ctx, uint32_t framesBelowSp, const _sColCellVecPSX &destination)
{
    constexpr uint32_t kCellCastFrame = 0xF0u, kHitPadding = 0x4Cu;
    const uint32_t word = getRegU32(ctx, 29) - framesBelowSp - kCellCastFrame + kHitPadding;
    std::memcpy(guest_host(word), &destination.pad, 4u);
}

// The doors of a sci-fi level: eight scenery objects, each pointing through its shape to a vertex animation.
void buildDoors(Rng &rng, uint32_t scratch)
{
    constexpr uint32_t kHeader = 0x0000u, kShapes = 0x0400u, kAnims = 0x0800u;
    static constexpr uint32 kTags[8] = {0x100005FD, 0x10000617, 0x100005FC, 0x10000618, 0x10000612, 0x10000613, 0x1000060F, 0x10000614};
    _sTSOHeader &header = *static_cast<_sTSOHeader *>(guest_host(scratch + kHeader));
    header.numObj = 8;
    _sTSO *objects = static_cast<_sTSO *>(guest_host(scratch + kHeader + 4u));
    const int first = rng.below(8);
    for (int i = 0; i < 8; ++i)
    {
        objects[i].tag = kTags[(first + i) % 8];
        objects[i].obj.pTOB = ptr32<_sTOB>(scratch + kShapes + 4u * static_cast<uint32_t>(i));
        *static_cast<uint32_t *>(guest_host(scratch + kShapes + 4u * static_cast<uint32_t>(i))) = scratch + kAnims + 0x100u * static_cast<uint32_t>(i);
    }
    G(tsoVarG).pTSOHeader = ptr32<_sTSOHeader>(scratch + kHeader);
    G(frontEndVarG).active = rng.chance(10) ? 2 : static_cast<uint8>(rng.below(2));
    static constexpr uint8 kLevels[4] = {0x14, 0x2C, 0x44, 0x5C};
    G(mainVarG).level = kLevels[rng.below(4)];
}

// A track of up to four modules, each using one of four collision meshes, as TEditGenerateTCol passes them.
struct Track
{
    static constexpr uint32_t kMeshList = 0x0000u, kMeshes = 0x0010u, kSourcePolys = 0x0100u;
    static constexpr uint32_t kModIds = 0x1000u, kOffsets = 0x1010u, kTurns = 0x1040u, kMirrors = 0x1050u, kPolyList = 0x2000u;
    static constexpr int kNumMeshes = 4, kPolysPerMesh = 6;
    uint16 numModules;
};

Track buildTrack(Rng &rng, uint32_t scratch)
{
    _sColMeshList &list = *static_cast<_sColMeshList *>(guest_host(scratch + Track::kMeshList));
    list.numMeshes = Track::kNumMeshes;
    list.pMesh = ptr32<_sColMesh>(scratch + Track::kMeshes);
    for (int mesh = 0; mesh < Track::kNumMeshes; ++mesh)
    {
        const uint32_t polys = scratch + Track::kSourcePolys + 0x60u * static_cast<uint32_t>(mesh * Track::kPolysPerMesh);
        list.pMesh[mesh].numPolys = static_cast<uint32>(rng.below(Track::kPolysPerMesh + 1));
        list.pMesh[mesh].pPoly = ptr32<_sColPolyPSX>(polys);
        for (uint32 p = 0; p < list.pMesh[mesh].numPolys; ++p)
            makePoly(rng, static_cast<_sColPolyPSX *>(guest_host(polys))[p], 400.0f);
    }

    Track track;
    track.numModules = static_cast<uint16>(rng.below(5));
    uint16 *modIds = static_cast<uint16 *>(guest_host(scratch + Track::kModIds));
    SVECTOR *offsets = static_cast<SVECTOR *>(guest_host(scratch + Track::kOffsets));
    uint16 *turns = static_cast<uint16 *>(guest_host(scratch + Track::kTurns));
    uint16 *mirrors = static_cast<uint16 *>(guest_host(scratch + Track::kMirrors));
    for (int module = 0; module < 4; ++module)
    {
        modIds[module] = static_cast<uint16>(rng.below(Track::kNumMeshes));
        offsets[module].vx = static_cast<int16>(rng.below(8) * 512 - 2048);
        offsets[module].vy = static_cast<int16>(rng.below(3) * 64);
        offsets[module].vz = static_cast<int16>(rng.below(8) * 512 - 2048);
        turns[module] = static_cast<uint16>(rng.below(5)); // 4 is not a turn the editor makes; it is treated as none.
        mirrors[module] = static_cast<uint16>(rng.below(2));
    }

    G(tcolG).pTCAMeshList = ptr32<_sColMeshList>(scratch + Track::kMeshList);
    G(tcolG).pPolyList = ptr32<_sColPolyPSX>(scratch + Track::kPolyList);
    G(numModulesG) = track.numModules;
    G(pModIdsG) = ptr32<uint16>(scratch + Track::kModIds);
    G(pOffG) = ptr32<SVECTOR>(scratch + Track::kOffsets);
    G(pRFlagsG) = ptr32<uint16>(scratch + Track::kTurns);
    G(pMFlagsG) = ptr32<uint16>(scratch + Track::kMirrors);
    return track;
}

// The Track Editor's grid, set to the random level, with nothing allocated yet.
void installAsTrackGrid(const World &world, uint32_t scratch)
{
    G(gridG) = *world.grid;
    G(tcolG).pPolyList = ptr32<_sColPolyPSX>(scratch + kPolys);
    G(tcolG).numPolys = kNumPolys;
    G(TColNumLLPtrUsedG) = 0;
}

} // namespace

void pointAndPoly(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const Vec centre = makePoly(rng, *static_cast<_sColPolyPSX *>(guest_host(scratch + kPolys)));
    setVec(vecArg(scratch, 0), nearPoly(rng, centre, 700.0f));
    setArg(ctx, 0, argAddress(scratch, 0));
    setArg(ctx, 1, scratch + kPolys);
}

void rayAndPoly(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const Vec centre = makePoly(rng, *static_cast<_sColPolyPSX *>(guest_host(scratch + kPolys)));
    setVec(vecArg(scratch, 0), nearPoly(rng, centre, 800.0f));
    setVec(vecArg(scratch, 1), nearPoly(rng, centre, 800.0f));
    setArg(ctx, 0, argAddress(scratch, 0));
    setArg(ctx, 1, argAddress(scratch, 1));
    setArg(ctx, 2, scratch + kPolys);
    setArg(ctx, 3, argAddress(scratch, 2));
}

void pointOnRayAndPoly(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const Vec centre = makePoly(rng, *static_cast<_sColPolyPSX *>(guest_host(scratch + kPolys)));
    setVec(vecArg(scratch, 0), {rng.range(-500.0f, 500.0f), rng.range(-500.0f, 500.0f), rng.range(-500.0f, 500.0f)});
    setVec(vecArg(scratch, 1), nearPoly(rng, centre, 500.0f));
    setArg(ctx, 0, argAddress(scratch, 0));
    setArg(ctx, 1, argAddress(scratch, 1));
    setArg(ctx, 2, scratch + kPolys);
    setArg(ctx, 3, argAddress(scratch, 2));
    ctx->f[12] = rng.range(-0.5f, 1.5f);
}

void downCast(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildWorld(rng, scratch);
    setVec(vecArg(scratch, 0), worldPosition(rng));
    for (int i = 0; i < 4; ++i)
        setArg(ctx, i, argAddress(scratch, i)); // Position, point found, normal, material.
}

void downCastInt(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildWorld(rng, scratch);
    const Vec position = worldPosition(rng);
    SVECTOR &whole = *static_cast<SVECTOR *>(guest_host(argAddress(scratch, 0)));
    whole.vx = static_cast<int16>(position.x);
    whole.vy = static_cast<int16>(position.y);
    whole.vz = static_cast<int16>(position.z);
    for (int i = 0; i < 4; ++i)
        setArg(ctx, i, argAddress(scratch, i));
}

void generalRayCast(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildWorld(rng, scratch);
    Vec start, end;
    segment(rng, start, end);
    setVec(vecArg(scratch, 0), start);
    setVec(vecArg(scratch, 1), end);
    for (int i = 0; i < 4; ++i)
        setArg(ctx, i, argAddress(scratch, i)); // Start, end, point found, normal.
    constexpr uint32_t kRayCastFrame = 0x110u;
    matchUnsetPadding(ctx, kRayCastFrame, vecArg(scratch, 2));
}

void cellCastCell(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildWorld(rng, scratch);
    Vec start, end;
    segment(rng, start, end);
    setVec(vecArg(scratch, 0), start);
    setVec(vecArg(scratch, 1), end);
    // Usually the cell the segment starts in.
    int cellX = rng.below(kCellsPerSide), cellZ = rng.below(kCellsPerSide);
    if (rng.chance(80))
    {
        cellX = std::clamp(static_cast<int>((start.x - kGridMin) / kCell), 0, kCellsPerSide - 1);
        cellZ = std::clamp(static_cast<int>((start.z - kGridMin) / kCell), 0, kCellsPerSide - 1);
    }
    setArg(ctx, 0, scratch + kCells + 16u * static_cast<uint32_t>(cellX * kCellsPerSide + cellZ));
    for (int i = 0; i < 4; ++i)
        setArg(ctx, i + 1, argAddress(scratch, i));
    matchUnsetPadding(ctx, 0u, vecArg(scratch, 2));
}

void positionAndTolerance(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildWorld(rng, scratch);
    setVec(vecArg(scratch, 0), worldPosition(rng));
    setArg(ctx, 0, argAddress(scratch, 0));
    ctx->f[12] = rng.range(-100.0f, 300.0f);
}

// Up to three cells, each with a chain of one or two cars, as CarColColDetCars leaves them for CarColDetect.
void carsInCells(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch);
    _sCarCol *carCols = static_cast<_sCarCol *>(guest_host(scratch + kCarCols));
    _sOColObj *bounds = static_cast<_sOColObj *>(guest_host(scratch + kCarBounds));
    _sFPhysLibColSphere *spheres = static_cast<_sFPhysLibColSphere *>(guest_host(scratch + kSpheres));
    constexpr int kSpheresPerCar = 3;

    const int numCells = rng.below(4);
    int car = 0;
    for (int head = 0; head < numCells; ++head)
    {
        _sCarColLL &group = G(carColG).carColHead[head];
        const int cellX = rng.below(kCellsPerSide), cellZ = rng.below(kCellsPerSide);
        group.cellId = static_cast<uint32>(cellX * kCellsPerSide + cellZ);
        const _sColGridCellPSX &cell = world.grid->pCell.get()[group.cellId];
        const uint32_t numInCell = cell.list.num[0] + cell.list.num[1];
        ptr32<_sCarCol> previous = nullptr;
        const int chain = (car < 3 && rng.chance(40)) ? 2 : 1;
        for (int link = 0; link < chain; ++link, ++car)
        {
            _sCarCol &carCol = carCols[car];
            carCol.dead[0] = rng.chance(10) ? 1 : 0;
            carCol.dead[1] = rng.chance(10) ? 1 : 0;
            carCol.numSpheres = static_cast<uint8>(1 + rng.below(kSpheresPerCar));
            carCol.pOcol = ptr32<_sOColObj>(guest_addr(&bounds[car]));
            carCol.pSphere = ptr32<_sFPhysLibColSphere>(guest_addr(&spheres[car * kSpheresPerCar]));
            carCol.pPrev = previous;
            previous = ptr32<_sCarCol>(guest_addr(&carCol));

            // The car sits in its cell, usually close to one of the cell's polys.
            Vec carPos = {kGridMin + kCell * (static_cast<float>(cellX) + rng.range(0.0f, 1.0f)), rng.range(-200.0f, 700.0f),
                          kGridMin + kCell * (static_cast<float>(cellZ) + rng.range(0.0f, 1.0f))};
            if (numInCell != 0u && rng.chance(75))
            {
                const uint32_t id = world.grid->pIds.get()[cell.list.offset + static_cast<uint32_t>(rng.below(static_cast<int>(numInCell)))];
                const _sColPolyPSX &poly = world.polys[id];
                const float out = rng.range(-120.0f, 160.0f);
                const Vec on = nearPoly(rng, world.centres[id], 250.0f);
                // Dropped onto the poly's plane, then moved off it along the normal.
                const Vec normal = {poly.n.x.v, poly.n.y.v, poly.n.z.v};
                const float off = normal.x * on.x + normal.y * on.y + normal.z * on.z + poly.d.v;
                carPos = {on.x - normal.x * (off - out), on.y - normal.y * (off - out), on.z - normal.z * (off - out)};
            }

            Vec min = {1e9f, 1e9f, 1e9f}, max = {-1e9f, -1e9f, -1e9f};
            for (int s = 0; s < carCol.numSpheres; ++s)
            {
                _sFPhysLibColSphere &sphere = spheres[car * kSpheresPerCar + s];
                const Vec pos = nearPoly(rng, carPos, 60.0f);
                const float reach = rng.chance(30) ? 300.0f : 40.0f; // Sometimes fast enough to pass through a poly.
                const float radius = rng.range(10.0f, 120.0f);
                setVec(sphere.pos16, pos);
                setVec(sphere.oldPos16, nearPoly(rng, pos, reach));
                setVec(sphere.min, {pos.x - radius, pos.y - radius, pos.z - radius});
                setVec(sphere.max, {pos.x + radius, pos.y + radius, pos.z + radius});
                sphere.radius = radius * 256.0f;
                sphere.waterRadius = rng.range(10.0f, 200.0f) * 256.0f;
                sphere.colFlags = rng.chance(70) ? 0 : static_cast<int16>(rng.below(8));
                sphere.totalCols = rng.chance(80) ? static_cast<int8>(rng.below(3)) : static_cast<int8>(9 + rng.below(4));
                sphere.numPolyCols = static_cast<int8>(rng.below(3));
                min = {std::min(min.x, pos.x - radius), std::min(min.y, pos.y - radius), std::min(min.z, pos.z - radius)};
                max = {std::max(max.x, pos.x + radius), std::max(max.y, pos.y + radius), std::max(max.z, pos.z + radius)};
            }
            setVec(bounds[car].min, min);
            setVec(bounds[car].max, max);
        }
        group.pTail = previous;
    }
    setArg(ctx, 0, static_cast<uint32_t>(numCells));
}

void objectForCarCol(R5900Context *ctx, uint64_t &, uint32_t scratch)
{
    _sObject &object = *static_cast<_sObject *>(guest_host(scratch + kObject));
    object.pCarCol = ptr32<_sCarCol>(scratch + kObjectCarCol);
    object.pPhysics = ptr32<_sFPhysLibCar>(scratch + kObjectPhysics);
    setArg(ctx, 0, scratch + kObject);
}

void scifiDoor(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildDoors(rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(1 + rng.below(8)));
    setArg(ctx, 1, G(mainVarG).level);
    setArg(ctx, 2, static_cast<uint32_t>(rng.below(2)));
}

// Up to four cars, each in a few of the eight trigger areas, and doors that are open or closed.
void scifiTriggers(R5900Context *, uint64_t &state, uint32_t scratch)
{
    constexpr uint32_t kObjects = 0x2000u, kObjectSpacing = 0x440u;
    Rng rng{state};
    buildDoors(rng, scratch);
    sObjectClass &objects = G(objectVarG);
    objects.noObjects = static_cast<uint32>(rng.below(5));
    const int busy = rng.below(60); // How likely a car is to be in an area.
    for (uint32_t i = 0; i < objects.noObjects; ++i)
    {
        objects.list[i] = ptr32<_sObject>(scratch + kObjects + kObjectSpacing * i);
        for (bool_ &in : objects.list[i]->inTrigger)
            in = rng.chance(busy) ? 1 : 0;
    }
    for (uint32 &door : G(carColG).triggerState)
        door = rng.chance(40) ? 1 : 0;
}

void trackModules(R5900Context *, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildTrack(rng, scratch);
}

void trackModulesOnHeap(R5900Context *, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildTrack(rng, scratch);
    G(tcolG).pPolyList = nullptr; // TColBuildPolyList allocates it.
}

void trackToBuild(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const Track track = buildTrack(rng, scratch);
    G(tcolG).pPolyList = nullptr;
    G(TColNumLLPtrUsedG) = 0;
    setArg(ctx, 0, track.numModules == 0 ? 1u : track.numModules); // The editor never builds an empty track.
    setArg(ctx, 1, scratch + Track::kModIds);
    setArg(ctx, 2, scratch + Track::kOffsets);
    setArg(ctx, 3, scratch + Track::kTurns);
    setArg(ctx, 4, scratch + Track::kMirrors);
    setArg(ctx, 5, 0u);
    setArg(ctx, 6, 0u);
}

void trackExtents(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    G(minExtG).vx = static_cast<int16>(rng.below(20000) - 20000);
    G(minExtG).vz = static_cast<int16>(rng.below(20000) - 20000);
    G(maxExtG).vx = static_cast<int16>(rng.below(20000));
    G(maxExtG).vz = static_cast<int16>(rng.below(20000));
    G(tcolG).numPolys = rng.next() % 5000u;
    G(pVecG) = ptr32<_sColCellVecPSX>(rng.next() & 0x01FFFFF0u);
    // TColBuildGrid copies two words of its stack frame that it never set. In the game they are zero, left there by
    // TColMakePolyList, and the decompiled function says so; this leaves the same.
    constexpr uint32_t kFrame = 0x20u;
    const uint32_t zero = 0u;
    std::memcpy(guest_host(getRegU32(ctx, 29) - kFrame + 0x4u), &zero, 4u);
    std::memcpy(guest_host(getRegU32(ctx, 29) - kFrame + 0x14u), &zero, 4u);
}

void trackPolysForGrid(R5900Context *, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch);
    installAsTrackGrid(world, scratch);
    G(gridG).pCell = nullptr;
    G(gridG).pIds = nullptr;
    G(gridG).pVec = nullptr;
}

// A level whose cell lists are in no particular order.
void unsortedCells(R5900Context *, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch);
    installAsTrackGrid(world, scratch);
    uint32 *ids = world.grid->pIds.get();
    const _sColGridCellPSX *cells = world.grid->pCell.get();
    for (int cell = 0; cell < kCellsPerSide * kCellsPerSide; ++cell)
    {
        uint32 *list = ids + cells[cell].list.offset;
        for (int which = 0; which < 2; list += cells[cell].list.num[which], ++which)
        {
            for (uint32 i = cells[cell].list.num[which]; i > 1; --i)
                std::swap(list[i - 1], list[rng.below(static_cast<int>(i))]);
        }
    }
}

// Eight cells with a linked list of polys in each of their two lists, as TColComputeCellResidency builds them.
void cellLinkLists(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    constexpr uint32_t kHeads = 0x0000u, kPool = 0x0100u;
    constexpr int kCellCount = 8, kShortest = 3;
    Rng rng{state};
    _sTColLL *pool = static_cast<_sTColLL *>(guest_host(scratch + kPool));
    int used = 0;
    for (int list = 0; list < 2; ++list)
    {
        G(pCellPolyIdsG)[list] = ptr32<_sTColLL>(scratch + kHeads + 0x40u * static_cast<uint32_t>(list));
        _sTColLL *heads = G(pCellPolyIdsG)[list].get();
        for (int cell = 0; cell < kCellCount; ++cell)
        {
            heads[cell].pNext = nullptr;
            for (int length = kShortest + rng.below(6); length > 0; --length, ++used)
            {
                pool[used].id = rng.next();
                pool[used].pNext = heads[cell].pNext;
                heads[cell].pNext = ptr32<_sTColLL>(guest_addr(&pool[used]));
            }
        }
    }
    G(pTColLLPtrG) = ptr32<_sTColLL>(scratch + kPool);
    G(TColNumLLPtrUsedG) = used;
    setArg(ctx, 0, static_cast<uint32_t>(rng.below(kCellCount)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.below(2)));
    setArg(ctx, 2, static_cast<uint32_t>(rng.below(kShortest)));
}

} // namespace rc::fuzz
