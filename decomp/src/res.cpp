// Decompiled from RES.c (the resource loader) and TSO.c (scenery objects): the lookups the collision code needs.

#include "rc_decomp.h"

namespace rc {

// Finds a resource type by its name, such as "RCAF". The loader keeps pointers to the types sorted by name.
ptr32<rdsTypeListRecord> ResFindType(ptr32<char> pType)
{
    const resClass &res = G(resVarG);
    uint32_t low = 0, count = static_cast<uint32_t>(res.types);
    while (low < count)
    {
        const uint32_t middle = (low + count) >> 1;
        const ptr32<rdsTypeListRecord> record = res.pTypePtrs[middle];
        const int order = std::strcmp(pType.get(), record->type);
        if (order == 0)
            return record;
        if (order > 0)
            low = middle + 1;
        else
            count = middle;
    }
    return nullptr;
}

// Sets the function the loader calls when a resource of this type has been loaded.
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options)
{
    const ptr32<rdsTypeListRecord> type = ResFindType(pType);
    if (!type)
        return;
    resConstruct &construct = G(resVarG).pConstructDaemonsList[type->typeVal];
    construct.options = static_cast<uint16>(options);
    construct.constructDaemon.address = constructDaemon;
}

void ResTypeUnSetConstructDaemon(ptr32<char> pType)
{
    const ptr32<rdsTypeListRecord> type = ResFindType(pType);
    if (!type)
        return;
    resConstruct &construct = G(resVarG).pConstructDaemonsList[type->typeVal];
    construct.version = 0;
    construct.constructDaemon.address = 0;
    construct.options = 0;
}

// TSO.c: the level's scenery object with this tag, or null.
ptr32<_sTSO> TSOFindByTag(uint32 tag)
{
    const ptr32<_sTSOHeader> header = G(tsoVarG).pTSOHeader;
    const ptr32<_sTSO> objects(header.address + 4u);
    for (uint32_t i = 0; i < header->numObj; ++i)
    {
        if (objects[i].tag == tag)
            return objects + static_cast<int32_t>(i);
    }
    return nullptr;
}

} // namespace rc

RC_DECOMP_FN_NOFUZZ(ResFindType, 0x1ab920)
RC_DECOMP_FN_NOFUZZ(ResTypeSetConstructDaemon, 0x1ab878)
RC_DECOMP_FN_NOFUZZ(ResTypeUnSetConstructDaemon, 0x1ab8d8)
RC_DECOMP_FN_NOFUZZ(TSOFindByTag, 0x18cb78)
