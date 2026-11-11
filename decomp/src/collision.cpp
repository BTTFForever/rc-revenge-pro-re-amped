// Decompiled from the collision code: ACOL.c (queries against the level's collision grid), CARCOL.c (car spheres
// against the grid's polys) and TCOL.c (builds the grid for a track made in the Track Editor).
//
// A level's collision is a grid of 512-unit cells over the XZ plane. Each cell lists the polys that touch it, in two
// lists: the first holds the polys inside the cell, the second those that only reach its 128-unit border. Each list
// is sorted by the height of the polys' first vertex, highest first, so a search going down can stop at the first
// poly that lies below what it is looking for. Positions are floats in world units; normals are unit vectors.

#include "collision_fuzz.h"
#include "ee_math.h"

#include <algorithm>

#define F(value) ::rc::ee_float(value##f)

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);
void MemBlockFree(ptr32<memHandle> pHandle);
void MemBlockSet32(memHandle handle, uint32 value, uint32 size);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);
void ResTypeUnSetConstructDaemon(ptr32<char> pType);
ptr32<_sTSO> TSOFindByTag(uint32 tag);
void AColRCAConstructDaemon(ptr32<memHandle> pHandle);

namespace {

const ee_float kNoHit = F(-32767.0); // The height a down cast starts from, below any level.
const int kMaxContacts = 12;         // A sphere's contact list.

// A poly keeps its bounding box in fields with no other use: the padding of its vertices, and bbMaxZ.
ee_float polyMinX(const _sColPolyPSX &poly) { return poly.v[1].pad; }
ee_float polyMaxX(const _sColPolyPSX &poly) { return poly.v[2].pad; }
ee_float polyMinY(const _sColPolyPSX &poly) { return poly.v[0].pad; }
ee_float polyMinZ(const _sColPolyPSX &poly) { return poly.v[3].pad; }
ee_float polyMaxZ(const _sColPolyPSX &poly) { return poly.bbMaxZ; }
// The height a cell's lists are sorted by.
ee_float polySortY(const _sColPolyPSX &poly) { return poly.v[0].y; }

// max.s and min.s: plain comparisons of the two operands, in the instruction's order.
ee_float ee_max(ee_float a, ee_float b) { return std::max(a.v, b.v); }
ee_float ee_min(ee_float a, ee_float b) { return std::min(a.v, b.v); }

// sceVu0InnerProduct. VU0 clamps the result of each step, so it is done with the runtime's VU operations.
template <typename A, typename B>
ee_float vu0Dot(const A &a, const B &b)
{
    const __m128 product = PS2_VMUL(_mm_setr_ps(a.x, a.y, a.z, 0.0f), _mm_setr_ps(b.x, b.y, b.z, 0.0f));
    const __m128 y = _mm_shuffle_ps(product, product, _MM_SHUFFLE(1, 1, 1, 1));
    const __m128 z = _mm_shuffle_ps(product, product, _MM_SHUFFLE(2, 2, 2, 2));
    return _mm_cvtss_f32(PS2_VADD(PS2_VADD(product, y), z));
}

// The grid's cells are 512 units wide. The division rounds towards zero, so positions up to one cell outside the
// low edges still land in the first row or column.
int32_t cellCoord(ee_float pos, ee_float gridMin)
{
    return ee_ftoi(pos - gridMin) / 512;
}

ptr32<_sColGridCellPSX> cellAt(const _sColGridPSX &grid, int32_t cellX, int32_t cellZ)
{
    const uint32_t index = static_cast<uint32_t>(cellX) * static_cast<uint32_t>(grid.numCellsZ) + static_cast<uint32_t>(cellZ);
    return grid.pCell + static_cast<int32_t>(index);
}

ptr32<_sColGridCellPSX> cellAtPos(const _sColGridPSX &grid, ee_float x, ee_float z)
{
    return cellAt(grid, cellCoord(x, grid.min.x), cellCoord(z, grid.min.z));
}

// A point and a poly's vertices seen along one axis.
struct Flat
{
    ee_float a, b;
};

// Which side of the edge from one vertex to the next the point lies on.
bool rightOfEdge(Flat p, Flat from, Flat to)
{
    return (p.a - from.a) * (to.b - from.b) - (p.b - from.b) * (to.a - from.a) < F(0.0);
}

// True if the point is on the same side of every edge. The poly may wind either way; it has three vertices
// unless it says four.
bool insideFlatPoly(Flat p, const Flat v[4], uint8 numVerts)
{
    const int count = numVerts == 4 ? 4 : 3;
    const bool side = rightOfEdge(p, v[0], v[1]);
    for (int i = 1; i < count; ++i)
    {
        if (rightOfEdge(p, v[i], v[i + 1 == count ? 0 : i + 1]) != side)
            return false;
    }
    return true;
}

bool pointInPolyAlongX(const _sColCellVecPSX &p, const _sColPolyPSX &poly)
{
    const Flat v[4] = {{poly.v[0].y, poly.v[0].z}, {poly.v[1].y, poly.v[1].z}, {poly.v[2].y, poly.v[2].z}, {poly.v[3].y, poly.v[3].z}};
    return insideFlatPoly({p.y, p.z}, v, poly.numVerts);
}

bool pointInPolyAlongY(const _sColCellVecPSX &p, const _sColPolyPSX &poly)
{
    const Flat v[4] = {{poly.v[0].x, poly.v[0].z}, {poly.v[1].x, poly.v[1].z}, {poly.v[2].x, poly.v[2].z}, {poly.v[3].x, poly.v[3].z}};
    return insideFlatPoly({p.x, p.z}, v, poly.numVerts);
}

bool pointInPolyAlongZ(const _sColCellVecPSX &p, const _sColPolyPSX &poly)
{
    const Flat v[4] = {{poly.v[0].x, poly.v[0].y}, {poly.v[1].x, poly.v[1].y}, {poly.v[2].x, poly.v[2].y}, {poly.v[3].x, poly.v[3].y}};
    return insideFlatPoly({p.x, p.y}, v, poly.numVerts);
}

// The game's AColPointInPoly_NCLIP_DWO table: the test along the poly's major axis, the one its normal is closest
// to. 0 is Y (floors), 1 is X and 2 is Z (walls).
bool pointInPolyAlongMajorAxis(const _sColCellVecPSX &p, const _sColPolyPSX &poly, uint8 majorAxis)
{
    switch (majorAxis)
    {
    case 0:
        return pointInPolyAlongY(p, poly);
    case 1:
        return pointInPolyAlongX(p, poly);
    default:
        return pointInPolyAlongZ(p, poly);
    }
}

// Where the segment from start to end crosses the poly's front face, if it does. Only x, y and z of hit are written.
bool rayHitsPolyFront(const _sColCellVecPSX &start, const _sColCellVecPSX &end, const _sColPolyPSX &poly, _sColCellVecPSX &hit)
{
    sFVec dir;
    dir.x = end.x - start.x;
    dir.y = end.y - start.y;
    dir.z = end.z - start.z;
    const ee_float approach = vu0Dot(poly.n, dir);
    if (F(0.0) <= approach)
        return false; // Moving away from the face, or along it.

    sFVec toPoly;
    toPoly.x = poly.v[0].x - start.x;
    toPoly.y = poly.v[0].y - start.y;
    toPoly.z = poly.v[0].z - start.z;
    const ee_float height = vu0Dot(poly.n, toPoly);
    if (F(0.0) < height || height <= approach)
        return false; // Starts behind the plane, or ends before reaching it.

    const ee_float t = height / approach;
    hit.x = dir.x * t + start.x;
    hit.y = dir.y * t + start.y;
    hit.z = dir.z * t + start.z;
    return pointInPolyAlongMajorAxis(hit, poly, poly.ma);
}

// The nearest hit of a segment on the polys inside one cell. On a hit the segment is cut short, so a later poly
// counts only if it is nearer.
bool castSegmentInCell(const _sColGridCellPSX &cell, const _sColCellVecPSX &start, const _sColCellVecPSX &segmentEnd,
                       _sColCellVecPSX &hitOut, _sColNormalPSX &normalOut)
{
    const _sColGridPSX &grid = *G(acolG).pGrid;
    const int32_t count = static_cast<int32_t>(cell.list.num[0]);
    const ptr32<uint32> ids = grid.pIds + static_cast<int32_t>(cell.list.offset);
    _sColCellVecPSX end = segmentEnd;
    _sColCellVecPSX min, max;
    const auto setBounds = [&]
    {
        max.x = ee_max(end.x, start.x);
        min.x = ee_min(end.x, start.x);
        max.y = ee_max(end.y, start.y);
        min.y = ee_min(end.y, start.y);
        max.z = ee_max(end.z, start.z);
        min.z = ee_min(end.z, start.z);
    };
    setBounds();

    bool collided = false;
    for (int32_t i = 0; i < count; ++i)
    {
        const _sColPolyPSX &poly = *(grid.pPoly + static_cast<int32_t>(ids[i]));
        if (polySortY(poly) < min.y)
            break; // The rest of the list is lower still.
        _sColCellVecPSX hit;
        if (min.x <= polyMaxX(poly) && polyMinX(poly) <= max.x && min.z <= polyMaxZ(poly) && polyMinZ(poly) <= max.z &&
            polyMinY(poly) <= max.y && rayHitsPolyFront(start, end, poly, hit))
        {
            collided = true;
            end.x = hit.x;
            end.y = hit.y;
            end.z = hit.z;
            setBounds();
            // The original copies all 16 bytes of its hit, whose padding was never set.
            hitOut.x = hit.x;
            hitOut.y = hit.y;
            hitOut.z = hit.z;
            normalOut = poly.n;
        }
    }
    return collided;
}

int32_t clampToGrid(int32_t cell, int32_t numCells)
{
    if (cell < 0)
        return 0;
    return cell < numCells ? cell : numCells - 1;
}

// The highest upward-facing poly at or below a position, in the cell the position is in. Polys of material 0x1F are
// not ground.
struct GroundHit
{
    bool found;
    ee_float y;
};

template <typename OnBetter>
GroundHit findGroundBelow(const _sColGridPSX &grid, const _sColCellVecPSX &pos, OnBetter &&onBetter)
{
    // The plane distance uses the position as it was on entry.
    const sFVec entryPos = {pos.x, pos.y, pos.z};
    const _sColGridCellPSX &cell = *cellAtPos(grid, pos.x, pos.z);
    const ptr32<uint32> ids = grid.pIds + static_cast<int32_t>(cell.list.offset);
    ee_float best = kNoHit;
    for (uint32_t i = 0; i < cell.list.num[0]; ++i)
    {
        const _sColPolyPSX &poly = *(grid.pPoly + static_cast<int32_t>(ids[i]));
        const ee_float normalY = poly.n.y;
        if (!(F(0.0) < normalY))
            continue;
        if (best != kNoHit && polySortY(poly) < best)
            return {true, best}; // Sorted by height: nothing further down the list can be higher.
        if (polyMinY(poly) <= pos.y && pointInPolyAlongY(pos, poly))
        {
            const ee_float drop = -(vu0Dot(poly.n, entryPos) + poly.d) / normalY;
            if (drop < F(0.0) && best < drop + pos.y && poly.mat != 0x1F)
            {
                onBetter(poly);
                best = drop + pos.y;
            }
        }
    }
    return {best != kNoHit, best};
}

bool insideGridXZ(const _sColGridPSX &grid, ee_float x, ee_float z)
{
    return grid.min.x < x && x < grid.max.x && grid.min.z < z && z < grid.max.z;
}

// True if every poly inside the position's cell lies above the position, with 128 units and tol to spare.
bool nothingBelow(const _sColGridPSX &grid, const _sColCellVecPSX &pos, ee_float tol)
{
    const _sColGridCellPSX &cell = *cellAtPos(grid, pos.x, pos.z);
    const ptr32<uint32> ids = grid.pIds + static_cast<int32_t>(cell.list.offset);
    const ee_float top = pos.y + F(128.0) + tol;
    const int32_t count = static_cast<int32_t>(cell.list.num[0]);
    for (int32_t i = 0; i < count; ++i)
    {
        if (polyMinY(*(grid.pPoly + static_cast<int32_t>(ids[i]))) < top)
            return false;
    }
    return true;
}

// Adds a poly to a sphere's contacts. Returns false if the list is full.
bool addPolyContact(_sFPhysLibColSphere &sphere, ptr32<_sColPolyPSX> poly, const _sColNormalPSX &normal, ee_float depth)
{
    const int8 slot = sphere.totalCols;
    if (slot >= kMaxContacts)
        return false;
    sphere.totalCols = static_cast<int8>(sphere.totalCols + 1);
    sphere.polyPtr[slot] = poly;
    sphere.normal[slot].x = normal.x * F(4096.0);
    sphere.normal[slot].y = normal.y * F(4096.0);
    sphere.normal[slot].z = normal.z * F(4096.0);
    sphere.normal[slot].depth = depth;
    sphere.type[slot] = 1;
    return true;
}

bool boxesOverlapXZ(const _sColCellVecPSX &min, const _sColCellVecPSX &max, const _sColPolyPSX &poly)
{
    return min.x <= polyMaxX(poly) && polyMinX(poly) <= max.x && min.z <= polyMaxZ(poly) && polyMinZ(poly) <= max.z;
}

// A water surface against one car's spheres. A sphere under the surface gets a contact whose depth is how far its
// water radius reaches below the surface, measured straight down.
void collideWater(_sCarCol &car, ptr32<_sColPolyPSX> polyPtr)
{
    const _sColPolyPSX &poly = *polyPtr;
    _sFPhysLibColSphere *sphere = car.pSphere.get();
    for (int32_t left = car.numSpheres; left > 0; --left, ++sphere)
    {
        if ((sphere->colFlags & 4) != 0)
            continue; // Already in water.
        const _sColCellVecPSX &pos = sphere->pos16;
        if (!(pos.x <= polyMaxX(poly) && polyMinX(poly) <= pos.x && pos.z <= polyMaxZ(poly) && polyMinZ(poly) <= pos.z))
            continue;
        if (!pointInPolyAlongY(pos, poly))
            continue;
        ee_float depth = sphere->waterRadius - (vu0Dot(poly.n, pos) + poly.d) * F(256.0) / poly.n.y;
        if (depth < F(0.0))
            continue;
        if (F(65535.0) < depth)
            depth = F(65535.0);
        if (addPolyContact(*sphere, polyPtr, poly.n, depth))
        {
            // Keeps only one of the other flags, chosen by the poly's major axis, and marks the sphere as in water.
            sphere->colFlags = static_cast<int16>((static_cast<uint16_t>(sphere->colFlags) & (4u << (poly.ma & 31))) | 4u);
            sphere->numPolyCols = static_cast<int8>(sphere->numPolyCols + 1);
        }
    }
}

// A solid poly against one car's spheres. list is the cell list the poly came from: once a sphere is found to be
// above a poly of a list, bit `list` of its flags skips the rest of that list.
void collideSolid(_sCarCol &car, ptr32<_sColPolyPSX> polyPtr, int32_t list)
{
    const _sColPolyPSX &poly = *polyPtr;
    _sFPhysLibColSphere *sphere = car.pSphere.get();
    for (int32_t left = car.numSpheres; left > 0; --left, ++sphere)
    {
        if (((sphere->colFlags >> list) & 1) != 0)
            continue;
        if (polySortY(poly) < sphere->min.y)
        {
            sphere->colFlags = static_cast<int16>(static_cast<uint16_t>(sphere->colFlags) | (1u << list));
            continue;
        }
        if (!(boxesOverlapXZ(sphere->min, sphere->max, poly) && polyMinY(poly) <= sphere->max.y))
            continue;

        // Distance of the sphere's centre in front of the poly's plane, on the radius' scale.
        const ee_float distance = (vu0Dot(poly.n, sphere->pos16) + poly.d) * F(256.0);
        if (sphere->radius < distance)
            continue;

        bool touching;
        if (distance < -sphere->radius)
        {
            // The whole sphere is behind the plane. It counts only if it was in front, or touching, a step ago.
            const ee_float oldDistance = (vu0Dot(poly.n, sphere->oldPos16) + poly.d) * F(256.0);
            if (F(0.0) < oldDistance)
            {
                // It went through the plane this step: try where it is, where it was, and halfway between.
                touching = pointInPolyAlongMajorAxis(sphere->pos16, poly, poly.ma) ||
                           pointInPolyAlongMajorAxis(sphere->oldPos16, poly, poly.ma);
                if (!touching)
                {
                    _sColCellVecPSX middle;
                    middle.x = sphere->pos16.x + (sphere->oldPos16.x - sphere->pos16.x) * F(0.5);
                    middle.y = sphere->pos16.y + (sphere->oldPos16.y - sphere->pos16.y) * F(0.5);
                    middle.z = sphere->pos16.z + (sphere->oldPos16.z - sphere->pos16.z) * F(0.5);
                    touching = pointInPolyAlongMajorAxis(middle, poly, poly.ma);
                }
            }
            else
                touching = -oldDistance <= sphere->radius && pointInPolyAlongMajorAxis(sphere->oldPos16, poly, poly.ma);
        }
        else
            touching = pointInPolyAlongMajorAxis(sphere->pos16, poly, poly.ma);

        if (touching && addPolyContact(*sphere, polyPtr, poly.n, sphere->radius - distance))
            sphere->numPolyCols = static_cast<int8>(sphere->numPolyCols + 1);
    }
}

// The resource types of the two kinds of collision file, as the loader names them.
constexpr uint32_t kLevelCollisionType = 0x003281F8u; // "RCAF"
constexpr uint32_t kTrackCollisionType = 0x00327FA8u; // "TCAF"
constexpr uint32_t kAColRCAConstructDaemon = 0x0018E748u;

// The doors of the sci-fi levels are vertex animations. These are the two flags the triggers change; the names are
// what this code does with them.
constexpr uint32_t kDoorAnimRun = 0x40u, kDoorAnimReverse = 0x10u;

// TCOL.c: while the grid is built, each cell's two poly lists are linked lists of nodes from one pool.
constexpr uint32_t kLinkPoolBytes = 800000u;

template <typename T>
ptr32<memHandle> handleOf(ptr32<T> &pointer)
{
    return ptr32<memHandle>(guest_addr(&pointer));
}

template <typename T>
void allocTracked(ptr32<T> &pointer, uint32_t size)
{
    MemBlockAllocLo(handleOf(pointer), size);
    G(TColMemAllocedG) = static_cast<int32>(static_cast<uint32_t>(G(TColMemAllocedG)) + size);
}

template <typename T>
void freeTracked(ptr32<T> &pointer, uint32_t size)
{
    MemBlockFree(handleOf(pointer));
    G(TColMemAllocedG) = static_cast<int32>(static_cast<uint32_t>(G(TColMemAllocedG)) - size);
}

ptr32<_sTColLL> takeLink()
{
    const ptr32<_sTColLL> link = G(pTColLLPtrG) + G(TColNumLLPtrUsedG);
    G(TColNumLLPtrUsedG) = G(TColNumLLPtrUsedG) + 1;
    return link;
}

// Puts a poly at the front of one of a cell's lists.
void pushPoly(int32_t cellId, int32_t list, uint32_t polyId)
{
    _sTColLL &head = (G(pCellPolyIdsG)[list] + cellId).get()[0];
    const ptr32<_sTColLL> link = takeLink();
    link->pNext = head.pNext;
    link->id = polyId;
    head.pNext = link;
}

// A track module can be placed in quarter turns about the Y axis.
void rotateQuarterTurns(uint16 turns, ee_float &x, ee_float &z)
{
    const ee_float oldX = x, oldZ = z;
    switch (turns)
    {
    case 1:
        x = oldZ;
        z = -oldX;
        break;
    case 2:
        x = -oldX;
        z = -oldZ;
        break;
    case 3:
        z = oldX;
        x = -oldZ;
        break;
    default:
        break;
    }
}

// The original swaps poly ids through a float temporary. That changes nothing for an id below 2^24, which is far
// more polys than a track has; this is what it does to any value.
uint32 idThroughFloat(uint32 id)
{
    ee_float value;
    if (static_cast<int32_t>(id) >= 0)
        value = ee_itof(static_cast<int32_t>(id));
    else
    {
        value = ee_itof(static_cast<int32_t>((id >> 1) | (id & 1u)));
        value = value + value;
    }
    if (value.v < 1.0f)
        return 0;
    if (value.v >= 4294967296.0f)
        return 0xFFFFFFFFu;
    return static_cast<uint32>(value.v);
}

// Sorts one of a cell's lists by the height of each poly's first vertex, highest first. A bubble sort that carries
// the highest remaining poly to the front on each pass and stops after a pass with no swaps.
void sortPolysByHeight(uint32 *ids, uint32 count, const ptr32<_sColPolyPSX> polys)
{
    const int32_t last = static_cast<int32_t>(count) - 1;
    for (uint32 sorted = 1; sorted < count; ++sorted)
    {
        bool swapped = false;
        for (int32_t i = last; i >= static_cast<int32_t>(sorted); --i)
        {
            const uint32 upper = ids[i - 1];
            if (polySortY(*(polys + static_cast<int32_t>(upper))) < polySortY(*(polys + static_cast<int32_t>(ids[i]))))
            {
                ids[i - 1] = ids[i];
                ids[i] = idThroughFloat(upper);
                swapped = true;
            }
        }
        if (!swapped)
            break;
    }
}

} // namespace

// ACOL.c

void AColInit()
{
    ResTypeSetConstructDaemon(ptr32<char>(kLevelCollisionType), kAColRCAConstructDaemon, 2);
}

void AColTerm()
{
    ResTypeUnSetConstructDaemon(ptr32<char>(kLevelCollisionType));
}

bool_ AColPointInPolyX_NCLIP_DWO(ptr32<_sColCellVecPSX> pP, ptr32<_sColPolyPSX> pPoly)
{
    return pointInPolyAlongX(*pP, *pPoly);
}

bool_ AColPointInPolyY_NCLIP_DWO(ptr32<_sColCellVecPSX> pP, ptr32<_sColPolyPSX> pPoly)
{
    return pointInPolyAlongY(*pP, *pPoly);
}

bool_ AColPointInPolyY_NCLIP(ptr32<_sColCellVecPSX> pP, ptr32<_sColPolyPSX> pPoly)
{
    return pointInPolyAlongY(*pP, *pPoly);
}

bool_ AColPointInPolyZ_NCLIP_DWO(ptr32<_sColCellVecPSX> pP, ptr32<_sColPolyPSX> pPoly)
{
    return pointInPolyAlongZ(*pP, *pPoly);
}

// Finds the ground under a position given in whole units. Writes the point, the ground's normal on the 4096 scale
// and its material.
bool_ AColDownCastInt(ptr32<SVECTOR> pPosI, ptr32<SVECTOR> pPoiI, ptr32<SVECTOR> pNormalI, ptr32<uint8> pMatId)
{
    const _sColGridPSX &grid = *G(acolG).pGrid;
    _sColCellVecPSX pos;
    pos.x = ee_itof(pPosI->vx);
    pos.y = ee_itof(pPosI->vy);
    pos.z = ee_itof(pPosI->vz);
    if (!insideGridXZ(grid, pos.x, pos.z))
        return 0;

    const GroundHit ground = findGroundBelow(grid, pos, [&](const _sColPolyPSX &poly)
    {
        *pMatId = poly.mat;
        pNormalI->vx = static_cast<int16>(ee_ftoi(poly.n.x * F(4096.0)));
        pNormalI->vy = static_cast<int16>(ee_ftoi(poly.n.y * F(4096.0)));
        pNormalI->vz = static_cast<int16>(ee_ftoi(poly.n.z * F(4096.0)));
    });
    if (!ground.found)
        return 0;
    pPoiI->vy = static_cast<int16>(ee_ftoi(ground.y));
    pPoiI->vx = static_cast<int16>(ee_ftoi(pos.x));
    pPoiI->vz = static_cast<int16>(ee_ftoi(pos.z));
    return 1;
}

// Finds the ground under a position. Writes the point, the ground's unit normal and its material.
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal, ptr32<uint8> pMatId)
{
    const _sColGridPSX &grid = *G(acolG).pGrid;
    if (!insideGridXZ(grid, pPos->x, pPos->z))
        return 0;

    const GroundHit ground = findGroundBelow(grid, *pPos, [&](const _sColPolyPSX &poly)
    {
        pNormal->x = poly.n.x;
        pNormal->y = poly.n.y;
        pNormal->z = poly.n.z;
        *pMatId = poly.mat;
    });
    if (!ground.found)
        return 0;
    const ee_float x = pPos->x;
    pPoi->y = ground.y;
    pPoi->x = x;
    pPoi->z = pPos->z;
    return 1;
}

// The nearest hit of a segment in one cell. Nothing in the game calls this or AColGeneralRayCast: the camera has
// its own copy.
bool_ AColCellCastCell(ptr32<_sColGridCellPSX> pCell, ptr32<_sColCellVecPSX> pStart, ptr32<_sColCellVecPSX> pEnd,
                       ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal)
{
    const _sColCellVecPSX start = *pStart, end = *pEnd;
    return castSegmentInCell(*pCell, start, end, *pPoi, *pNormal);
}

// The nearest hit of a segment on the level, trying every cell in the rectangle the segment spans.
bool_ AColGeneralRayCast(ptr32<_sColCellVecPSX> pS, ptr32<_sColCellVecPSX> pE, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal)
{
    const _sColGridPSX &grid = *G(acolG).pGrid;
    const _sColCellVecPSX start = *pS;
    _sColCellVecPSX end = *pE;

    const int32_t endX = cellCoord(end.x, grid.min.x), endZ = cellCoord(end.z, grid.min.z);
    const int32_t startX = cellCoord(start.x, grid.min.x), startZ = cellCoord(start.z, grid.min.z);
    const int32_t firstX = clampToGrid(std::min(startX, endX), grid.numCellsX);
    const int32_t firstZ = clampToGrid(std::min(startZ, endZ), grid.numCellsZ);
    const int32_t lastX = clampToGrid(std::max(startX, endX), grid.numCellsX);
    const int32_t lastZ = clampToGrid(std::max(startZ, endZ), grid.numCellsZ);

    bool collided = false;
    for (int32_t x = firstX; x <= lastX; ++x)
    {
        for (int32_t z = firstZ; z <= lastZ; ++z)
        {
            _sColCellVecPSX hit;
            if (castSegmentInCell(*cellAt(grid, x, z), start, end, hit, *pNormal))
            {
                collided = true;
                end.x = hit.x;
                end.y = hit.y;
                end.z = hit.z;
                // The original copies 16 bytes here too; the padding it copies was never set.
                pPoi->x = hit.x;
                pPoi->y = hit.y;
                pPoi->z = hit.z;
            }
        }
    }
    return collided;
}

void AColLvlInit() {}

void AColLvlTerm() {}

bool_ AColRay_Poly_1Way(ptr32<_sColCellVecPSX> pS, ptr32<_sColCellVecPSX> pE, ptr32<_sColPolyPSX> pP, ptr32<_sColCellVecPSX> pPoi)
{
    return rayHitsPolyFront(*pS, *pE, *pP, *pPoi);
}

// The point at t along a ray, and whether it is inside the poly.
bool_ AColPointInPoly_RayPoly(ptr32<_sColCellVecPSX> pDir, ptr32<_sColCellVecPSX> pStart, ptr32<_sColPolyPSX> pPoly,
                              ptr32<_sColCellVecPSX> pPoi, ee_float t)
{
    pPoi->x = pDir->x * t + pStart->x;
    pPoi->y = pDir->y * t + pStart->y;
    pPoi->z = pDir->z * t + pStart->z;
    return pointInPolyAlongMajorAxis(*pPoi, *pPoly, pPoly->ma);
}

ptr32<_sColGridCellPSX> AColGetCell(ptr32<_sColGridPSX> pGrid, ee_float x, ee_float z)
{
    return cellAtPos(*pGrid, x, z);
}

int32 AColGetCellIdFromXZ(ptr32<_sColGridPSX> pGrid, int32 x, int32 z)
{
    return static_cast<int32>(static_cast<uint32_t>(x) * static_cast<uint32_t>(pGrid->numCellsZ) + static_cast<uint32_t>(z));
}

// True if a position is off the grid, or has nothing of its cell below it: every poly there is above it.
bool_ AColIsPositionOutsideWorld(ptr32<_sColCellVecPSX> pPos, ee_float tol)
{
    const _sColGridPSX &grid = *G(acolG).pGrid;
    if (!insideGridXZ(grid, pPos->x, pPos->z))
        return 1;
    return nothingBelow(grid, *pPos, tol);
}

bool_ AColIsPositionUnderWorld(ptr32<_sColCellVecPSX> pPos, ee_float tol)
{
    return nothingBelow(*G(acolG).pGrid, *pPos, tol);
}

bool_ AColIsPositionOutsideWorldExtentsXZ(ptr32<_sColCellVecPSX> pPos)
{
    return !insideGridXZ(*G(acolG).pGrid, pPos->x, pPos->z);
}

// The cell index of a position without the rounding towards zero the other lookups have, so it is only right for
// positions on the grid.
int32 AColCellIdFromPosUnbounded(ptr32<_sColGridPSX> pGrid, ee_float x, ee_float z)
{
    const uint32_t cellX = static_cast<uint32_t>(ee_ftoi(x - pGrid->min.x) >> 9);
    const uint32_t cellZ = static_cast<uint32_t>(ee_ftoi(z - pGrid->min.z) >> 9);
    return static_cast<int32>(cellX * static_cast<uint32_t>(pGrid->numCellsZ) + cellZ);
}

// Called by the resource loader when a level's collision file (type "RCAF") has been loaded.
void AColRCAConstructDaemon(ptr32<memHandle> pHandle)
{
    G(acolG).pGrid = pHandle->cast<_sColGridPSX>();
}

// CARCOL.c

// Collides the cars with the level. CarColColDetCars has grouped the cars by the cell they are in: each of the
// first numCells entries of carColHead is a cell and the chain of cars in it. Every contact found is added to the
// sphere's list for the physics to respond to.
void CarColDetect(int32_t numCells)
{
    const _sColGridPSX &grid = *G(acolG).pGrid;
    const ptr32<_sColPolyPSX> polys = grid.pPoly;
    const ptr32<_sColGridCellPSX> cells = grid.pCell;
    const ptr32<uint32> gridIds = grid.pIds;

    for (int32_t head = 0; head != numCells; ++head)
    {
        const _sCarColLL &group = G(carColG).carColHead[head];
        const _sColGridCellPSX &cell = *(cells + static_cast<int32_t>(group.cellId));
        const int32_t numInside = static_cast<int32_t>(cell.list.num[0]);
        const int32_t numPolys = numInside + static_cast<int32_t>(cell.list.num[1]);
        const ptr32<uint32> ids = gridIds + static_cast<int32_t>(cell.list.offset);

        for (int32_t n = 0; n < numPolys; ++n)
        {
            const ptr32<_sColPolyPSX> polyPtr = polys + static_cast<int32_t>(ids[n]);
            const _sColPolyPSX &poly = *polyPtr;
            const int32_t list = n < numInside ? 0 : 1;
            const bool water = poly.gid == 2;
            int32_t carsTested = 0;

            for (ptr32<_sCarCol> carPtr = group.pTail; carPtr; carPtr = carPtr->pPrev)
            {
                _sCarCol &car = *carPtr;
                if (car.dead[list] != 0)
                    continue;
                const _sOColObj &bounds = *car.pOcol;
                if (polySortY(poly) < bounds.min.y)
                {
                    car.dead[list] = 1; // The car is above everything left in this list.
                    continue;
                }
                ++carsTested;
                if (!water && !(polyMinY(poly) <= bounds.max.y))
                    continue;
                if (!boxesOverlapXZ(bounds.min, bounds.max, poly))
                    continue;
                if (water)
                    collideWater(car, polyPtr);
                else
                    collideSolid(car, polyPtr, list);
            }

            // Every car is above the rest of the border list, so the cell is finished.
            if (!water && carsTested == 0 && list != 0)
                break;
        }
    }
}

void CarColInit() {}

void CarColTerm() {}

void CarColLvlInit()
{
    for (uint32 &state : G(carColG).triggerState)
        state = 0;
}

void CarColLvlTerm() {}

void CarColInitObj(ptr32<_sObject> pObject)
{
    _sCarCol &carCol = *pObject->pCarCol;
    carCol.dead[1] = 0;
    carCol.dead[0] = 0;
    carCol.pOcol = ptr32<_sOColObj>(guest_addr(&pObject->oCol));
    const _sFPhysLibCar &physics = *pObject->pPhysics;
    carCol.pNext = nullptr;
    carCol.pPrev = nullptr;
    carCol.pSphere = physics.pSphere;
    carCol.numSpheres = static_cast<uint8>(physics.numSpheres);
}

// A debug display that was compiled out.
void ColDebugInfoRenderPolys(int32_t) {}

// Opens or closes one of the eight doors of a sci-fi level. The two halves of those levels use the same doors in a
// different order.
void CarColTriggerScifiAnim(int32_t door, int32_t level, bool_ open)
{
    if (G(frontEndVarG).active == 2)
        return;

    static constexpr uint32 kDoorTags[8] = {0x100005FD, 0x10000617, 0x100005FC, 0x10000618, 0x10000612, 0x10000613, 0x1000060F, 0x10000614};
    static constexpr uint32 kDoorTagsSwapped[8] = {0x100005FC, 0x10000618, 0x100005FD, 0x10000617, 0x1000060F, 0x10000613, 0x10000612, 0x10000614};
    // The game only passes doors 1 to 8. For any other the original looks up whatever its fourth argument register held.
    const uint32 tag = (level == 0x14 || level == 0x44 ? kDoorTagsSwapped : kDoorTags)[(door - 1) & 7];

    // The scenery object's shape begins with a pointer to its vertex animation.
    const ptr32<_sTSO> door3d = TSOFindByTag(tag);
    _sVTXAnim &anim = **door3d->obj.pTOB.cast<ptr32<_sVTXAnim>>();
    if (open)
        anim.vtxFlags = (anim.vtxFlags | kDoorAnimRun) & ~kDoorAnimReverse;
    else if ((anim.vtxFlags & kDoorAnimRun) == 0)
        anim.vtxFlags = anim.vtxFlags | kDoorAnimRun | kDoorAnimReverse;
}

// The sci-fi levels have two sets of four doors. Each car records which of the eight trigger areas it is in:
// areas 0 and 1 open the first pair of doors of the first set and, with areas 2 and 3, the second pair; areas 4 to 7
// do the same for the second set. A set closes when no car is in any of its areas.
void CarColCheckScifiTriggers()
{
    uint32 *state = G(carColG).triggerState; // Indexed by door, 1 to 8.
    const int32_t level = G(mainVarG).level;
    const auto openDoor = [&](int32_t door)
    {
        if (state[door] == 0)
            CarColTriggerScifiAnim(door, level, 1);
        state[door] = 1;
    };

    bool closeFirstSet = true, closeSecondSet = true;
    const sObjectClass &objects = G(objectVarG);
    for (int32_t i = static_cast<int32_t>(objects.noObjects) - 1; i != -1; --i)
    {
        const bool_ *in = objects.list[i]->inTrigger;
        for (int32_t set = 0; set < 2; ++set)
        {
            const bool_ *area = in + set * 4;
            const int32_t firstDoor = 1 + set * 4;
            bool &closeSet = set == 0 ? closeFirstSet : closeSecondSet;
            for (int32_t side = 0; side < 2; ++side)
            {
                if (area[side] == 0)
                    continue;
                openDoor(firstDoor + side);
                closeSet = false;
                if (area[side + 2] != 0)
                    openDoor(firstDoor + side + 2);
            }
            if (area[2] != 0 || area[3] != 0)
                closeSet = false;
        }
    }

    for (int32_t set = 0; set < 2; ++set)
    {
        if (!(set == 0 ? closeFirstSet : closeSecondSet))
            continue;
        const int32_t firstDoor = 1 + set * 4;
        for (int32_t door = firstDoor; door < firstDoor + 4; ++door)
        {
            if (state[door] == 0)
                CarColTriggerScifiAnim(door, level, 0);
        }
        for (int32_t door = firstDoor; door < firstDoor + 4; ++door)
            state[door] = 0;
    }
}

// TCOL.c
//
// A Track Editor track is a list of modules, each a piece of track with its own collision mesh, placed at an offset,
// turned in quarter turns and optionally mirrored. TColBuildLvlCollision turns them into one poly list and a grid over
// it like the one a normal level loads from its file.

void TColInit()
{
    G(TColMemAllocedG) = 0;
    G(TColNumLLPtrUsedG) = 0;
    ResTypeSetConstructDaemon(ptr32<char>(kTrackCollisionType), 0, 2);
}

void TColTerm()
{
    _sColGridPSX &grid = G(gridG);
    if (grid.pCell)
        MemBlockFree(handleOf(grid.pCell));
    if (grid.pIds)
        MemBlockFree(handleOf(grid.pIds));
    if (grid.pVec)
        MemBlockFree(handleOf(grid.pVec));
    if (G(tcolG).pPolyList)
        MemBlockFree(handleOf(G(tcolG).pPolyList));
    G(tcolG).pPolyList = nullptr;
    grid.pVec = nullptr;
    grid.pCell = nullptr;
    grid.pIds = nullptr;
    grid.pPoly = nullptr;
    G(pVecG) = nullptr;
    G(TColNumLLPtrUsedG) = 0;
    ResTypeUnSetConstructDaemon(ptr32<char>(kTrackCollisionType));
}

// Copies every module's polys into the track's poly list, moved to where the module is, and finds the track's
// extents in X and Z. The debug data gives this an int result, but it returns nothing meaningful.
void TColMakePolyList()
{
    SVECTOR &minExt = G(minExtG), &maxExt = G(maxExtG);
    minExt.vx = minExt.vy = minExt.vz = 0x7FFF;
    maxExt.vx = maxExt.vy = maxExt.vz = -0x7FFF;

    int32_t firstPoly = 0;
    for (int32_t module = 0; module < G(numModulesG); ++module)
    {
        const _sColMesh &mesh = G(tcolG).pTCAMeshList->pMesh[G(pModIdsG)[module]];
        _sColPolyPSX *dst = (G(tcolG).pPolyList + firstPoly).get();
        const _sColPolyPSX *src = mesh.pPoly.get();
        for (uint32 p = 0; p < mesh.numPolys; ++p, ++dst, ++src)
        {
            dst->numVerts = src->numVerts;
            for (int32_t v = 0; v < dst->numVerts; ++v)
            {
                const bool mirrored = G(pMFlagsG)[module] != 0;
                // Mirroring turns the poly over, so its vertices are taken in the opposite order to keep it facing out.
                int32_t from = v;
                if (mirrored && dst->numVerts == 4 && (v == 1 || v == 3))
                    from = 4 - v;
                else if (mirrored && dst->numVerts != 4 && (v == 1 || v == 2))
                    from = 3 - v;
                _sColCellVertPSX &vertex = dst->v[v];
                vertex.x = src->v[from].x;
                vertex.y = src->v[from].y;
                vertex.z = src->v[from].z;
                if (mirrored)
                    vertex.x = -vertex.x;
                rotateQuarterTurns(G(pRFlagsG)[module], vertex.x, vertex.z);
                const SVECTOR &offset = G(pOffG)[module];
                vertex.x = vertex.x + ee_itof(offset.vx);
                vertex.y = vertex.y + ee_itof(offset.vy);
                vertex.z = vertex.z + ee_itof(offset.vz);
            }

            dst->n.x = src->n.x;
            dst->n.y = src->n.y;
            dst->n.z = src->n.z;
            if (G(pMFlagsG)[module] != 0)
                dst->n.x = -dst->n.x;
            const uint16 turns = G(pRFlagsG)[module];
            rotateQuarterTurns(turns, dst->n.x, dst->n.z);
            dst->sid = turns >= 1 && turns <= 3 ? static_cast<int16>(turns) : 0;

            // The axis the normal is closest to: the point-in-poly tests look along it.
            if (F(0.56735027) < ee_abs(dst->n.y))
                dst->ma = 0;
            else
                dst->ma = F(0.56735027) < ee_abs(dst->n.x) ? 1 : 2;
            dst->mat = src->mat;
            dst->gid = src->gid;
            dst->d = -(dst->v[0].x * dst->n.x + dst->v[0].y * dst->n.y + dst->v[0].z * dst->n.z);

            // Walls facing +X or +Z are wound the other way round.
            if ((dst->ma == 1 && F(0.0) < dst->n.x) || (dst->ma == 2 && F(0.0) < dst->n.z))
                std::swap(dst->v[1], dst->v[dst->numVerts == 3 ? 2 : 3]);

            // The poly's bounding box, kept in its vertices' padding.
            dst->v[1].pad = F(32767.0);
            dst->v[2].pad = F(-32767.0);
            dst->v[3].pad = F(32767.0);
            dst->v[0].pad = F(32767.0);
            dst->bbMaxZ = F(-32767.0);
            for (int32_t v = 0; v < dst->numVerts; ++v)
            {
                dst->v[1].pad = ee_min(dst->v[v].x, dst->v[1].pad);
                dst->v[0].pad = ee_min(dst->v[v].y, dst->v[0].pad);
                dst->v[3].pad = ee_min(dst->v[v].z, dst->v[3].pad);
                dst->v[2].pad = ee_max(dst->v[v].x, dst->v[2].pad);
                dst->bbMaxZ = ee_max(dst->v[v].z, dst->bbMaxZ);
            }

            minExt.vx = static_cast<int16>(ee_ftoi(ee_min(polyMinX(*dst), ee_itof(minExt.vx))));
            minExt.vz = static_cast<int16>(ee_ftoi(ee_min(polyMinZ(*dst), ee_itof(minExt.vz))));
            maxExt.vx = static_cast<int16>(ee_ftoi(ee_max(polyMaxX(*dst), ee_itof(maxExt.vx))));
            maxExt.vz = static_cast<int16>(ee_ftoi(ee_max(polyMaxZ(*dst), ee_itof(maxExt.vz))));
        }
        firstPoly += static_cast<int32_t>(mesh.numPolys);
    }
}

// Counts the track's polys, allocates the poly list and fills it.
void TColBuildPolyList()
{
    _sTcol &tcol = G(tcolG);
    tcol.numPolys = 0;
    for (int32_t module = 0; module < G(numModulesG); ++module)
        tcol.numPolys = tcol.numPolys + tcol.pTCAMeshList->pMesh[G(pModIdsG)[module]].numPolys;
    tcol.pPolyList = nullptr;
    allocTracked(tcol.pPolyList, tcol.numPolys * 0x60u);
    TColMakePolyList();
}

// Sizes the grid: the track's extents and one more cell on every side.
void TColBuildGrid()
{
    _sColGridPSX &grid = G(gridG);
    const ee_float cell = F(512.0), perCell = F(0.001953125);
    const ee_float minZ = (ee_itof(G(minExtG).vz) - cell) * perCell * cell;
    const ee_float maxZ = (ee_itof(G(maxExtG).vz) + cell) * perCell * cell;
    const ee_float minX = (ee_itof(G(minExtG).vx) - cell) * perCell * cell;
    const ee_float maxX = (ee_itof(G(maxExtG).vx) + cell) * perCell * cell;

    // The original copies min.y and max.y from two stack words it never set. On the game's only path here those are
    // the upper halves of two 64-bit registers that TColMakePolyList saved, which hold 32-bit values: zero.
    grid.min.y = F(0.0);
    grid.max.y = F(0.0);
    grid.extra.z = F(128.0);
    grid.pVec = G(pVecG);
    grid.numVectors = 4;
    grid.min.x = minX;
    grid.numPolys = static_cast<int32>(G(tcolG).numPolys);
    grid.cellSize.x = cell;
    grid.cellSize.z = cell;
    grid.min.z = minZ;
    grid.max.z = maxZ;
    grid.max.x = maxX;
    grid.cellSize.y = F(0.0);
    grid.extra.x = F(128.0);
    grid.extra.y = F(0.0);
    grid.numCellsX = ee_ftoi((maxX - minX) * perCell) + 1;
    grid.numCellsZ = ee_ftoi((maxZ - minZ) * perCell) + 1;
    grid.totalCells = static_cast<int32>(static_cast<uint32_t>(grid.numCellsX) * static_cast<uint32_t>(grid.numCellsZ));
}

// Works out which cells each poly touches and writes every cell's two id lists.
void TColComputeCellResidency()
{
    _sColGridPSX &grid = G(gridG);
    const _sTcol &tcol = G(tcolG);
    auto &cellLists = G(pCellPolyIdsG);
    int32_t totalInstances = 0;

    allocTracked(grid.pCell, static_cast<uint32_t>(grid.totalCells) * 16u);
    allocTracked(G(pTColLLPtrG), kLinkPoolBytes);
    const uint32_t listBytes0 = static_cast<uint32_t>(grid.totalCells) * 8u;
    allocTracked(cellLists[0], listBytes0);
    const uint32_t listBytes1 = static_cast<uint32_t>(grid.totalCells) * 8u;
    allocTracked(cellLists[1], listBytes1);
    MemBlockSet32(cellLists[0].cast<void>(), 0, static_cast<uint32_t>(grid.totalCells) << 3);
    MemBlockSet32(cellLists[1].cast<void>(), 0, static_cast<uint32_t>(grid.totalCells) << 3);
    MemBlockSet32(G(pTColLLPtrG).cast<void>(), 0, kLinkPoolBytes);
    MemBlockSet32(grid.pCell.cast<void>(), 0, static_cast<uint32_t>(grid.totalCells) << 4);

    const ee_float border = F(128.0), perCell = F(0.001953125);
    for (uint16 p = 0; p < tcol.numPolys; ++p)
    {
        const _sColPolyPSX &poly = tcol.pPolyList[p];
        const ee_float minX = polyMinX(poly), maxX = polyMaxX(poly), minZ = polyMinZ(poly), maxZ = polyMaxZ(poly);
        // The cells the poly could touch, with one to spare each way.
        const int32_t startX = clampToGrid(ee_ftoi(((minX - border) - grid.min.x) * perCell - F(1.0)), grid.numCellsX);
        const int32_t startZ = clampToGrid(ee_ftoi(((minZ - border) - grid.min.z) * perCell - F(1.0)), grid.numCellsZ);
        const int32_t endX = clampToGrid(ee_ftoi(((maxX + border) - grid.min.x) * perCell + F(1.0)), grid.numCellsX);
        const int32_t endZ = clampToGrid(ee_ftoi(((maxZ + border) - grid.min.z) * perCell + F(1.0)), grid.numCellsZ);

        int32_t x = startX;
        do
        {
            int32_t z = startZ;
            do
            {
                const int32_t cellId = AColGetCellIdFromXZ(ptr32<_sColGridPSX>(guest_addr(&grid)), x, z);
                // The cell with its border.
                const ee_float lowX = (grid.min.x + ee_itof(x << 9)) - border;
                const ee_float lowZ = (grid.min.z + ee_itof(z << 9)) - border;
                const ee_float highX = lowX + F(512.0) + F(256.0);
                const ee_float highZ = lowZ + F(512.0) + F(256.0);
                if (minX <= highX && minZ <= highZ && lowX <= maxX && lowZ <= maxZ)
                {
                    const bool inside = minX <= highX - border && minZ <= highZ - border && lowX + border <= maxX && lowZ + border <= maxZ;
                    const int32_t list = inside ? 0 : 1;
                    ++totalInstances;
                    pushPoly(cellId, list, p);
                    uint32 &count = (grid.pCell + cellId)->list.num[list];
                    count = count + 1;
                }
                ++z;
            } while (z <= endZ);
            ++x;
        } while (x <= endX);
    }

    allocTracked(grid.pIds, static_cast<uint32_t>(totalInstances) * 4u);
    allocTracked(G(pVecG), 0x40u);
    // The directions a module can face, for the polys' sid.
    _sColCellVecPSX *vectors = G(pVecG).get();
    grid.pVec = G(pVecG);
    vectors[0].x = F(0.0);
    vectors[0].y = F(0.0);
    vectors[0].z = F(4096.0);
    vectors[1].x = F(4096.0);
    vectors[1].y = F(0.0);
    vectors[1].z = F(0.0);
    vectors[2].x = F(0.0);
    vectors[2].y = F(0.0);
    vectors[2].z = F(-4096.0);
    vectors[3].x = F(-4096.0);
    vectors[3].y = F(0.0);
    vectors[3].z = F(0.0);

    // Each cell's linked lists become a run of ids: the polys inside it, then those on its border.
    int32_t offset = 0;
    for (uint16 cellId = 0; cellId < grid.totalCells; ++cellId)
    {
        _sColGridCellPSX &cell = grid.pCell[cellId];
        cell.list.offset = static_cast<uint32>(offset);
        for (int32_t list = 0; list < 2; ++list)
        {
            ptr32<_sTColLL> link = cellLists[list][cellId].pNext;
            for (uint32 i = 0; i < cell.list.num[list]; ++i, link = link->pNext)
                *(grid.pIds + offset++) = static_cast<uint16>(link->id);
        }
    }
    grid.numIds = totalInstances;
    G(numIdsG) = grid.numIds;

    freeTracked(G(pTColLLPtrG), kLinkPoolBytes);
    freeTracked(cellLists[0], listBytes0);
    freeTracked(cellLists[1], listBytes1);
}

void TColSortPolysInCells()
{
    const _sColGridPSX &grid = G(gridG);
    const ptr32<uint32> ids = grid.pIds;
    const ptr32<_sColPolyPSX> polys = grid.pPoly;
    _sColGridCellPSX *cell = grid.pCell.get();
    for (int32_t i = 0; i < grid.totalCells; ++i, ++cell)
    {
        const uint32 offset = cell->list.offset;
        sortPolysByHeight((ids + static_cast<int32_t>(offset)).get(), cell->list.num[0], polys);
        sortPolysByHeight((ids + static_cast<int32_t>(offset + cell->list.num[0])).get(), cell->list.num[1], polys);
    }
}

void TColLvlInit()
{
    G(TColNumLLPtrUsedG) = 0;
}

void TColLvlTerm() {}

// Builds the collision for a Track Editor track and makes it the level's. The last two arguments are not used.
void TColBuildLvlCollision(uint16 numModules, ptr32<uint16> pModIds, ptr32<SVECTOR> pOff, ptr32<uint16> pRFlags,
                           ptr32<uint16> pMFlags, ptr32<SVECTOR>, ptr32<SVECTOR>)
{
    G(numModulesG) = numModules;
    G(pModIdsG) = pModIds;
    G(pOffG) = pOff;
    G(pRFlagsG) = pRFlags;
    G(pMFlagsG) = pMFlags;
    TColBuildPolyList();
    TColBuildGrid();
    TColComputeCellResidency();
    G(acolG).pGrid = ptr32<_sColGridPSX>(guest_addr(&G(gridG)));
    G(gridG).pPoly = G(tcolG).pPolyList;
    TColSortPolysInCells();
    G(offG) = *G(pOffG);
}

bool_ TColBoxesOverlapXZ(ptr32<_sColCellVecPSX> pMinA, ptr32<_sColCellVecPSX> pMaxA, ptr32<_sColCellVecPSX> pMinB, ptr32<_sColCellVecPSX> pMaxB)
{
    return pMinA->x <= pMaxB->x && pMinA->z <= pMaxB->z && pMinB->x <= pMaxA->x && pMinB->z <= pMaxA->z;
}

ptr32<_sTColLL> TColGetNextLLPtr()
{
    return takeLink();
}

// The id at a position in one of a cell's linked lists.
uint16 TColGetPolyId(int32_t cellId, int32_t extra, int32_t polyId)
{
    ptr32<_sTColLL> link = (G(pCellPolyIdsG)[extra] + cellId)->pNext;
    for (; polyId != 0; --polyId)
        link = link->pNext;
    return static_cast<uint16>(link->id);
}

void TColAddPolyId(int32_t cellId, int32_t extra, int32_t polyId)
{
    pushPoly(cellId, extra, static_cast<uint32_t>(polyId));
}

} // namespace rc

RC_DECOMP_FN_NOFUZZ(AColInit, 0x18ccd0)
RC_DECOMP_FN_NOFUZZ(AColTerm, 0x18cce8)
RC_DECOMP_FN_SETUP(AColPointInPolyX_NCLIP_DWO, 0x18ccf8, rc::fuzz::pointAndPoly)
RC_DECOMP_FN_SETUP(AColPointInPolyY_NCLIP_DWO, 0x18ced0, rc::fuzz::pointAndPoly)
RC_DECOMP_FN_SETUP(AColPointInPolyY_NCLIP, 0x18d0b0, rc::fuzz::pointAndPoly)
RC_DECOMP_FN_SETUP(AColPointInPolyZ_NCLIP_DWO, 0x18d288, rc::fuzz::pointAndPoly)
RC_DECOMP_FN_SETUP(AColDownCastInt, 0x18d468, rc::fuzz::downCastInt)
RC_DECOMP_FN_SETUP(AColDownCast, 0x18d8b0, rc::fuzz::downCast)
RC_DECOMP_FN_SETUP(AColGeneralRayCast, 0x18db98, rc::fuzz::generalRayCast)
RC_DECOMP_FN_SETUP(AColCellCastCell, 0x18df38, rc::fuzz::cellCastCell)
RC_DECOMP_FN(AColLvlInit, 0x18e1c8)
RC_DECOMP_FN(AColLvlTerm, 0x18e1d0)
RC_DECOMP_FN_SETUP(AColRay_Poly_1Way, 0x18e1d8, rc::fuzz::rayAndPoly)
RC_DECOMP_FN_SETUP(AColPointInPoly_RayPoly, 0x18e360, rc::fuzz::pointOnRayAndPoly)
RC_DECOMP_FN(AColGetCell, 0x18e3e0)
RC_DECOMP_FN(AColGetCellIdFromXZ, 0x18e490)
RC_DECOMP_FN_SETUP(AColIsPositionOutsideWorld, 0x18e4a0, rc::fuzz::positionAndTolerance)
RC_DECOMP_FN_SETUP(AColIsPositionUnderWorld, 0x18e5d0, rc::fuzz::positionAndTolerance)
RC_DECOMP_FN_SETUP(AColIsPositionOutsideWorldExtentsXZ, 0x18e6a8, rc::fuzz::positionAndTolerance)
RC_DECOMP_FN(AColCellIdFromPosUnbounded, 0x18e710)
RC_DECOMP_FN(AColRCAConstructDaemon, 0x18e748)

RC_DECOMP_FN_SETUP(CarColDetect, 0x1185a0, rc::fuzz::carsInCells)
RC_DECOMP_FN(CarColInit, 0x119c08)
RC_DECOMP_FN(CarColTerm, 0x119c10)
RC_DECOMP_FN(CarColLvlInit, 0x119c18)
RC_DECOMP_FN(CarColLvlTerm, 0x119c48)
RC_DECOMP_FN_SETUP(CarColInitObj, 0x119c50, rc::fuzz::objectForCarCol)
RC_DECOMP_FN(ColDebugInfoRenderPolys, 0x119c88)
RC_DECOMP_FN_SETUP(CarColTriggerScifiAnim, 0x119438, rc::fuzz::scifiDoor)
RC_DECOMP_FN_SETUP(CarColCheckScifiTriggers, 0x119570, rc::fuzz::scifiTriggers)

RC_DECOMP_FN_NOFUZZ(TColInit, 0x144ed0)
RC_DECOMP_FN_NOFUZZ(TColTerm, 0x144ef0)
RC_DECOMP_FN_SETUP(TColMakePolyList, 0x144f98, rc::fuzz::trackModules)
RC_DECOMP_FN_SETUP_HEAP(TColBuildPolyList, 0x1456c8, rc::fuzz::trackModulesOnHeap)
RC_DECOMP_FN_SETUP(TColBuildGrid, 0x145768, rc::fuzz::trackExtents)
RC_DECOMP_FN_SETUP_HEAP(TColComputeCellResidency, 0x1458a0, rc::fuzz::trackPolysForGrid)
RC_DECOMP_FN_SETUP(TColSortPolysInCells, 0x146018, rc::fuzz::unsortedCells)
RC_DECOMP_FN(TColLvlInit, 0x146318)
RC_DECOMP_FN(TColLvlTerm, 0x146320)
RC_DECOMP_FN_SETUP_HEAP(TColBuildLvlCollision, 0x146328, rc::fuzz::trackToBuild)
RC_DECOMP_FN(TColBoxesOverlapXZ, 0x146398)
RC_DECOMP_FN(TColGetNextLLPtr, 0x146400)
RC_DECOMP_FN_SETUP(TColGetPolyId, 0x146420, rc::fuzz::cellLinkLists)
RC_DECOMP_FN_SETUP(TColAddPolyId, 0x1464a8, rc::fuzz::cellLinkLists)
