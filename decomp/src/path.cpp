// Decompiled from PATH.c: the front end's camera paths. The front-end level data holds a null-terminated list of
// sCameraPath (pPaths: length, group, speed, id, then the camera tags) and pathLinks (FRONT.c) a table of
// (from, to, via, via2) tags for pairs with no direct path. pathDetailsG.constructedPath (an sCameraPathX of up to
// 100 tags) is the path the camera flies: SetPath/PathCopy copy one path into it, PathBuilder chains paths (direct or
// through a via tag) from the current tag to a destination, PathSmoother and PathOptimize tidy it. The camera calls
// PathPilotNodeCallback at each node and PathPilotEndCallback at the end, which builds and starts the next leg.
//
// Written from the instructions. Every tag list is indexed as the original does, including the word before the
// tags (index -1: pathIdTag) for an empty list, so all reads are raw guest words.

#include "path_fuzz.h"

namespace rc {

void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options); // res.cpp
void ResTypeUnSetConstructDaemon(ptr32<char> pType);                                       // res.cpp
void CameraTerminatePath(ptr32<_sCamera> pCamera);                                         // camera.cpp
void CameraSetNewPath(ptr32<_sCamera> pCamera, uint32 fp, uint32 fpNode, ptr32<sCameraPath> pPath); // camera.cpp

ptr32<sCameraPath> GetPath(uint32 startTag, uint32 endTag);
ptr32<sCameraPath> GetViaPath(uint32 startTag, uint32 endTag);
void SetCameraToThisTag(uint32 tag, ptr32<_sCamData> pCameras);
void PathSmoother(uint32 tag1, uint32 tag2, uint32 findTag, uint32 replaceTag);
void PathOptimize();

namespace {

constexpr uint32_t kDetails = 0x1ee578u;            // pathDetailsG
constexpr uint32_t kCurrentTag = kDetails + 0x0u, kActivePath = kDetails + 0x4u, kFinalDestTag = kDetails + 0x8u;
constexpr uint32_t kBuilt = kDetails + 0x24u;       // constructedPath: length (byte), +1 its high byte, +2 speed, +4 id
constexpr uint32_t kBuiltTags = kDetails + 0x2cu;   // constructedPath.startTag
constexpr uint32_t kFrontEnd = 0x1eca58u;           // frontEndVarG: pFELevelData, +0x288 destTag, +0x2be pilot mode
constexpr uint32_t kPathLinks = 0x1ebef0u;
constexpr uint32_t kCameraPtr = 0x1eab20u;          // cameraVarG.pCamera
constexpr uint32_t kCameraVars = 0x1e9fb0u;         // cameraVarG.camera
constexpr uint32_t kPathType = 0x327f48u;
constexpr uint32_t kPathConstructDaemon = 0x13ce88u, kPathPilotEndCallback = 0x13c380u, kPathPilotNodeCallback = 0x13ca78u;
constexpr uint32_t kMat3nBuildXYZ = 0x19e768u;

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t word(uint32_t address) { return at<uint32_t>(address); }
uint32_t &builtTag(int32_t i) { return at<uint32_t>(kBuiltTags + static_cast<uint32_t>(i) * 4u); }
// A path's tag i (startTag[i]); i = len - 1 is its last.
uint32_t pathTag(uint32_t path, int32_t i) { return word(path + 8u + static_cast<uint32_t>(i) * 4u); }
uint32_t lastTag(uint32_t path) { return pathTag(path, static_cast<int32_t>(at<uint8_t>(path)) - 1); }
uint32_t paths() { return word(word(kFrontEnd) + 0x34u); }

// The constructed path's header from a path: its length (one byte), id 0, its speed.
void copyHeader(uint32_t path)
{
    at<uint8_t>(kBuilt) = at<uint8_t>(path);
    at<uint32_t>(kBuilt + 4u) = 0;
    at<uint16_t>(kBuilt + 2u) = at<uint16_t>(path + 2u);
}

// The path's tags forward up to its terminating 0, or backward from its last: returns the count written; the
// 0 terminator follows and finalDestTag is the last copied (the word before the tags if none).
int32_t copyForward(uint32_t path)
{
    int32_t n = 0;
    for (uint32_t tag = pathTag(path, 0); tag != 0; tag = pathTag(path, n))
        builtTag(n++) = tag;
    return n;
}

int32_t copyBackward(uint32_t path)
{
    int32_t n = 0;
    for (int32_t i = static_cast<int32_t>(at<uint8_t>(path)) - 1; i >= 0; --i)
        builtTag(n++) = pathTag(path, i);
    return n;
}

void finish(int32_t n)
{
    builtTag(n) = 0;
    at<uint32_t>(kFinalDestTag) = builtTag(n - 1);
}

// Finds the path joining the two tags (either way round) in the front end's list, or 0.
uint32_t findPath(uint32_t tagA, uint32_t tagB)
{
    const uint32_t list = paths();
    for (uint32_t i = 0; word(list + i * 4u) != 0; ++i)
    {
        const uint32_t path = word(list + i * 4u);
        const uint32_t first = pathTag(path, 0), last = lastTag(path);
        if ((first == tagA && last == tagB) || (first == tagB && last == tagA))
            return path;
    }
    return 0;
}

} // namespace

// The camera tag's entry of the camera list, or 0.
ptr32<_sSingleCamera> GetCameraFromTag(uint32 tag, ptr32<_sCamData> pCameras)
{
    const int32_t count = pCameras->numCams;
    for (int32_t i = 0; i < count; ++i)
        if (word(pCameras.address + 4u + static_cast<uint32_t>(i) * 0x14u) == tag)
            return ptr32<_sSingleCamera>(pCameras.address + 4u + static_cast<uint32_t>(i) * 0x14u);
    return ptr32<_sSingleCamera>(nullptr);
}

void PathPilotNodeCallback(ptr32<void>, uint32 pathPos)
{
    at<uint32_t>(kCurrentTag) = word(word(kActivePath) + 8u + pathPos * 4u);
}

// Puts the front end's camera at a tag (the camera found, or whatever guest address 0 holds if none).
void SetCameraToThisTag(uint32 tag, ptr32<_sCamData> pCameras)
{
    const uint32_t camera = word(kCameraPtr);
    const uint32_t entry = GetCameraFromTag(tag, pCameras).address;
    GuestFrame frame(0x30);
    at<uint32_t>(kCurrentTag) = tag;
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint16_t heading = at<uint16_t>(entry + 4u);
    at<int32_t>(sp + 0x10u) = at<int32_t>(entry + 8u); // the dead cameraPos local
    at<uint16_t>(sp + 2u) = static_cast<uint16_t>(-heading);
    at<int32_t>(sp + 0x14u) = at<int32_t>(entry + 0xcu);
    at<uint16_t>(sp + 0u) = at<uint16_t>(entry + 6u);
    at<uint16_t>(sp + 4u) = 0;
    at<int32_t>(sp + 0x18u) = at<int32_t>(entry + 0x10u);
    at<int32_t>(camera + 0xcu) = at<int32_t>(entry + 8u);
    at<int32_t>(camera + 0x10u) = at<int32_t>(entry + 0xcu);
    at<int32_t>(camera + 0x14u) = at<int32_t>(entry + 0x10u);
    guest_call(kMat3nBuildXYZ, camera + 0x24u, sp);
}

void PathCopy(ptr32<sCameraPath> pPhysPath, int32 direction)
{
    copyHeader(pPhysPath.address);
    int32_t n = 0;
    if (direction == 3)
        n = copyForward(pPhysPath.address);
    else if (direction == 4)
        n = copyBackward(pPhysPath.address);
    finish(n);
}

void PathEmpty()
{
    at<uint8_t>(kBuilt) = 0;
    builtTag(0) = 0;
    at<uint16_t>(kBuilt + 2u) = 0;
    at<uint32_t>(kBuilt + 4u) = 0;
}

// Counts down the cameras and does nothing else (the body was compiled out).
void CameraPitchFixup(ptr32<_sCamData> pCameras)
{
    (void)static_cast<int32_t>(pCameras->numCams);
}

// Replaces findTag between tag1 and tag2 (either order) with replaceTag.
void PathSmoother(uint32 tag1, uint32 tag2, uint32 findTag, uint32 replaceTag)
{
    if (at<uint8_t>(kBuilt) < 3)
        return;
    for (int32_t i = 0; i < static_cast<int32_t>(at<uint8_t>(kBuilt)) - 3; ++i)
    {
        if (builtTag(i + 1) != findTag)
            continue;
        const uint32_t before = builtTag(i), after = builtTag(i + 2);
        if ((before == tag1 && after == tag2) || (before == tag2 && after == tag1))
            builtTag(i + 1) = replaceTag;
    }
}

ptr32<sCameraPath> GetPath(uint32 startTag, uint32 endTag)
{
    if (word(paths()) == 0)
        return ptr32<sCameraPath>(nullptr);
    return ptr32<sCameraPath>(findPath(startTag, endTag));
}

int32 CheckPathDirection(ptr32<sCameraPath> pPath, uint32 startTag, uint32 endTag)
{
    if (pathTag(pPath.address, 0) == endTag && lastTag(pPath.address) == startTag)
        return 0x1001;
    return 0x1000;
}

// The path resource's tag count (the words from +8 up to the first 0) into its first byte.
void PathConstructDaemon(ptr32<memHandle> pHandle)
{
    const uint32_t path = pHandle->address;
    uint32_t count = 0;
    while (word(path + 8u + count * 4u) != 0)
        ++count;
    at<uint8_t>(path) = static_cast<uint8_t>(count);
}

void PathInit()
{
    ResTypeSetConstructDaemon(ptr32<char>(kPathType), kPathConstructDaemon, 2);
}

void PathTerm()
{
    ResTypeUnSetConstructDaemon(ptr32<char>(kPathType));
}

// A path from startTag to endTag through a via tag of pathLinks: the path between startTag and the via tag. Once a
// link has matched, every later entry searches again with that via tag (the original never clears its flag).
ptr32<sCameraPath> GetViaPath(uint32 startTag, uint32 endTag)
{
    bool match = false;
    uint32_t via = 0;
    for (uint32_t i = 0; word(kPathLinks + i * 16u) != 0; ++i)
    {
        const uint32_t link = kPathLinks + i * 16u;
        if (word(link) == startTag && word(link + 4u) == endTag)
        {
            via = word(link + 8u);
            match = true;
        }
        else if (word(link) == endTag && word(link + 4u) == startTag)
        {
            via = word(link + 12u);
            match = true;
        }
        if (!match)
            continue;
        const uint32_t list = paths();
        if (word(list) == 0)
            continue;
        const uint32_t found = findPath(startTag, via);
        if (found != 0)
            return ptr32<sCameraPath>(found);
    }
    return ptr32<sCameraPath>(nullptr);
}

// Copies the path joining the two tags into the constructed path, forward (pilot mode 3, returns 1) or backward
// (mode 4, returns 0x1001); 0 if there is none.
int32 SetPath(uint32 startTag, uint32 endTag)
{
    const uint32_t list = paths();
    for (uint32_t i = 0; word(list + i * 4u) != 0; ++i)
    {
        const uint32_t path = word(list + i * 4u);
        const uint32_t first = pathTag(path, 0), last = lastTag(path);
        if (first == startTag && last == endTag)
        {
            copyHeader(path);
            finish(copyForward(path));
            at<int16_t>(kFrontEnd + 0x2beu) = 3;
            return 1;
        }
        if (first == endTag && last == startTag)
        {
            copyHeader(path);
            finish(copyBackward(path));
            at<int16_t>(kFrontEnd + 0x2beu) = 4;
            return 0x1001;
        }
    }
    return 0;
}

// Cuts loops: a tag seen again later marks everything after its first visit up to the repeat as removed (-1); the
// removed tags are then squeezed out. The first and last two tags are never the start of a loop.
void PathOptimize()
{
    const int32_t n = at<uint8_t>(kBuilt);
    if (n < 3)
        return;
    for (int32_t i = 0; i < n - 2; ++i)
    {
        const uint32_t tag = builtTag(i);
        if (tag == 0xffffffffu)
            continue;
        for (int32_t j = i + 1; j < n - 1; ++j)
            if (builtTag(j) == tag)
                for (int32_t k = i + 1; k <= j; ++k)
                    builtTag(k) = 0xffffffffu;
    }
    int32_t w = 0;
    for (int32_t i = 0; i < n; ++i)
    {
        const uint32_t tag = builtTag(i);
        if (tag == 0xffffffffu)
            at<uint8_t>(kBuilt) = static_cast<uint8_t>(at<uint8_t>(kBuilt) - 1);
        else
            builtTag(w++) = tag;
    }
}

// Chains paths from the current tag to destTag: direct ones, or through pathLinks; returns 1 if it got stuck (no
// path onward), 0 when the chain reaches destTag (or it is already there).
int32 PathBuilder(uint32 destTag)
{
    at<uint16_t>(kBuilt + 2u) = 0;
    uint32_t start = word(kCurrentTag);
    if (start == destTag)
        return 0;
    uint32_t path = 0;
    for (;;)
    {
        if (path != 0 && (lastTag(path) == destTag || pathTag(path, 0) == destTag))
        {
            at<uint32_t>(kFinalDestTag) = destTag;
            return 0;
        }
        uint32_t n = at<uint8_t>(kBuilt);
        path = GetPath(start, destTag).address;
        if (path == 0 && start != destTag)
        {
            path = GetViaPath(start, destTag).address;
            if (path == 0)
            {
                at<uint32_t>(kFinalDestTag) = destTag;
                return 1;
            }
        }
        // The first leg sets the speed (doubled while the length's high byte is 0).
        if (at<uint16_t>(kBuilt + 2u) == 0)
        {
            const uint16_t speed = at<uint16_t>(path + 2u);
            at<uint16_t>(kBuilt + 2u) = at<uint8_t>(kBuilt + 1u) == 0 ? static_cast<uint16_t>(speed << 1) : speed;
        }
        const auto add = [&](uint32_t tag)
        {
            if (n == 0 || builtTag(static_cast<int32_t>(n) - 1) != tag)
                builtTag(static_cast<int32_t>(n++)) = tag;
        };
        if (pathTag(path, 0) == start)
        {
            if (start != 0)
                for (int32_t i = 0; start != 0; start = pathTag(path, ++i))
                    add(start);
        }
        else
        {
            for (int32_t i = static_cast<int32_t>(at<uint8_t>(path)) - 1; i >= 0; --i)
                add(pathTag(path, i));
        }
        builtTag(static_cast<int32_t>(n)) = 0;
        start = builtTag(static_cast<int32_t>(n) - 1);
        at<uint8_t>(kBuilt) = static_cast<uint8_t>(n);
    }
}

// The end of a leg: the camera is at the path's last tag; builds the next leg to finalDestTag and starts it.
void PathPilotEndCallback(ptr32<void>)
{
    {
        GuestFrame frame(0x30);
        const uint32_t active = word(kActivePath);
        uint32_t tag = word(kCurrentTag);
        if (active != 0)
            tag = at<uint8_t>(active) == 0 ? 0u : lastTag(active);
        if (tag != 0)
            at<uint32_t>(kCurrentTag) = tag;
        at<uint8_t>(kBuilt) = 0;
        builtTag(0) = 0;
        at<uint16_t>(kBuilt + 2u) = 0;
        at<uint32_t>(kBuilt + 4u) = 0;
        if (PathBuilder(word(kFinalDestTag)) == 1)
        {
            CameraTerminatePath(ptr32<_sCamera>(word(kCameraPtr)));
            SetCameraToThisTag(word(kFinalDestTag), ptr32<_sCamData>(word(word(kFrontEnd) + 0x30u)));
        }
        if (word(kCurrentTag) == word(kFinalDestTag) || at<uint8_t>(word(kActivePath)) == 0)
            return;
        PathSmoother(0x100000e4u, 0x100000dbu, 0x100000dau, 0x100000adu);
        PathOptimize();
        at<uint32_t>(kFrontEnd + 0x288u) = word(kFinalDestTag);
    }
    CameraSetNewPath(ptr32<_sCamera>(kCameraVars), kPathPilotEndCallback, kPathPilotNodeCallback,
                     ptr32<sCameraPath>(word(kActivePath))); // a tail call
}

} // namespace rc

RC_DECOMP_FN_SETUP(GetCameraFromTag, 0x13ca18, rc::fuzz::pathCameras)
RC_DECOMP_FN_SETUP(PathPilotNodeCallback, 0x13ca78, rc::fuzz::pathNode)
RC_DECOMP_FN_SETUP(SetCameraToThisTag, 0x13ca98, rc::fuzz::pathCameras)
RC_DECOMP_FN_SETUP(PathCopy, 0x13cb70, rc::fuzz::pathCopy)
RC_DECOMP_FN_SETUP(PathEmpty, 0x13cc60, rc::fuzz::pathBuilt)
RC_DECOMP_FN_SETUP(CameraPitchFixup, 0x13cc80, rc::fuzz::pathPitch)
RC_DECOMP_FN_SETUP(PathSmoother, 0x13ccf0, rc::fuzz::pathSmoother)
RC_DECOMP_FN_SETUP(GetPath, 0x13cdb8, rc::fuzz::pathTags)
RC_DECOMP_FN_SETUP(CheckPathDirection, 0x13ce48, rc::fuzz::pathDirection)
RC_DECOMP_FN_SETUP(PathConstructDaemon, 0x13ce88, rc::fuzz::pathDaemon)
RC_DECOMP_FN_SETUP_HEAP(PathInit, 0x13c5f8, rc::fuzz::pathNone)
RC_DECOMP_FN_SETUP_HEAP(PathTerm, 0x13c610, rc::fuzz::pathNone)
RC_DECOMP_FN_SETUP(GetViaPath, 0x13c4b8, rc::fuzz::pathTags)
RC_DECOMP_FN_SETUP(SetPath, 0x13c620, rc::fuzz::pathTags)
RC_DECOMP_FN_SETUP(PathOptimize, 0x13c7d8, rc::fuzz::pathBuilt)
RC_DECOMP_FN_SETUP(PathBuilder, 0x13c170, rc::fuzz::pathBuilder)
RC_DECOMP_FN_SETUP_HEAP(PathPilotEndCallback, 0x13c380, rc::fuzz::pathEnd)
