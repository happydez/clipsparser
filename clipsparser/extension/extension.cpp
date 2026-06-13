#include "extension.h"

#include <unordered_set>
#include <string>
#include <algorithm>
#include <cmath>

ClipsParserExt g_ClipsParser;

SDKExtension* g_pExtensionIface = &g_ClipsParser;

static std::string edgeKey(int brushIndex, const clips::Vec3& a, const clips::Vec3& b)
{
    int p[3] = { (int)lround(a.x), (int)lround(a.y), (int)lround(a.z) };
    int q[3] = { (int)lround(b.x), (int)lround(b.y), (int)lround(b.z) };
    if (std::lexicographical_compare(q, q + 3, p, p + 3))
    {
        std::swap_ranges(p, p + 3, q);
    }

    int key[7] = { brushIndex, p[0], p[1], p[2], q[0], q[1], q[2] };

    return std::string(reinterpret_cast<const char*>(key), sizeof(key));
}

void ClipCache::clear()
{
    for (int t = 0; t < clips::ClipTypeCount; t++)
    {
        _edges[t].clear();
        _brushCounts[t] = 0;
    }

    _bspVersion = 0;
}

void ClipCache::clearFilters()
{
    _filters.materials.clear();
    _filters.hammerIds.clear();
    _filters.classnames.clear();
    _filters.brushBoxes.clear();
    _filters.shrink = 0.0;
    _filters.brushBoxTolerance = 1.0;
}

void ClipCache::addMaterial(const char* substring)
{
    if (substring && substring[0])
    {
        _filters.materials.push_back(substring);
    }
}

void ClipCache::addHammerId(int id)
{
    _filters.hammerIds.push_back(id);
}

void ClipCache::addClassname(const char* classname)
{
    if (classname && classname[0])
    {
        _filters.classnames.push_back(classname);
    }
}

void ClipCache::addBrushBox(const float mins[3], const float maxs[3], int faceCount)
{
    clips::BrushMatch m;
    m.mins = { mins[0], mins[1], mins[2] };
    m.maxs = { maxs[0], maxs[1], maxs[2] };
    m.faceCount = faceCount;
    _filters.brushBoxes.push_back(m);
}

void ClipCache::setShrink(float units)
{
    _filters.shrink = units;
}

void ClipCache::setBrushTolerance(float units)
{
    _filters.brushBoxTolerance = units;
}

bool ClipCache::rebuild(const char* bspPath)
{
    clips::ParseResult parsed = clips::parseBspClips(bspPath, _filters);
    if (!parsed.ok) {
        return false;
    }

    clear();
    _bspVersion = parsed.bspVersion;

    for (int t = 0; t < clips::ClipTypeCount; t++)
    {
        std::unordered_set<int> brushes;
        std::unordered_set<std::string> seen;

        for (const clips::Face& face : parsed.faces[t])
        {
            brushes.insert(face.brushIndex);

            const size_t n = face.vertices.size();
            for (size_t i = 0; i < n; i++)
            {
                const clips::Vec3& v0 = face.vertices[i];
                const clips::Vec3& v1 = face.vertices[(i + 1) % n];

                if (!seen.insert(edgeKey(face.brushIndex, v0, v1)).second)
                {
                    continue;
                }

                Edge e;
                e.a[0] = (float)v0.x; e.a[1] = (float)v0.y; e.a[2] = (float)v0.z;
                e.b[0] = (float)v1.x; e.b[1] = (float)v1.y; e.b[2] = (float)v1.z;
                _edges[t].push_back(e);
            }
        }

        _brushCounts[t] = (int)brushes.size();
    }

    return true;
}

int ClipCache::edgeCount(int type) const
{
    if (type < 0 || type >= clips::ClipTypeCount)
    {
        return 0;
    }

    return (int)_edges[type].size();
}

int ClipCache::brushCount(int type) const
{
    if (type < 0 || type >= clips::ClipTypeCount)
    {
        return 0;
    }

    return _brushCounts[type];
}

const Edge* ClipCache::edge(int type, int index) const
{
    if (type < 0 || type >= clips::ClipTypeCount)
    {
        return nullptr;
    }

    if (index < 0 || index >= (int)_edges[type].size())
    {
        return nullptr;
    }

    return &_edges[type][index];
}

static cell_t Native_Parse(IPluginContext* pContext, const cell_t* params)
{
    char* given;
    pContext->LocalToString(params[1], &given);

    // A relative path like "maps/x.bsp" is resolved against the game folder so
    // callers do not have to know where the server is installed; an absolute
    // path is used as-is.
    char full[PLATFORM_MAX_PATH];
    if (given[0] && given[1] == ':')
    {
        ke::SafeStrcpy(full, sizeof(full), given);
    }
    else if (given[0] == '/')
    {
        ke::SafeStrcpy(full, sizeof(full), given);
    }
    else
    {
        smutils->BuildPath(Path_Game, full, sizeof(full), "%s", given);
    }

    return g_ClipsParser.cache.rebuild(full) ? 1 : 0;
}

static cell_t Native_Clear(IPluginContext* pContext, const cell_t* params)
{
    g_ClipsParser.cache.clear();

    return 0;
}

static cell_t Native_ClearFilters(IPluginContext* pContext, const cell_t* params)
{
    g_ClipsParser.cache.clearFilters();

    return 0;
}

static cell_t Native_AddMaterialFilter(IPluginContext* pContext, const cell_t* params)
{
    char* material;
    pContext->LocalToString(params[1], &material);
    g_ClipsParser.cache.addMaterial(material);

    return 0;
}

static cell_t Native_AddHammerId(IPluginContext* pContext, const cell_t* params)
{
    g_ClipsParser.cache.addHammerId(params[1]);

    return 0;
}

static cell_t Native_AddClassnameFilter(IPluginContext* pContext, const cell_t* params)
{
    char* classname;
    pContext->LocalToString(params[1], &classname);
    g_ClipsParser.cache.addClassname(classname);

    return 0;
}

static cell_t Native_AddBrushBox(IPluginContext* pContext, const cell_t* params)
{
    cell_t* mins;
    cell_t* maxs;
    pContext->LocalToPhysAddr(params[1], &mins);
    pContext->LocalToPhysAddr(params[2], &maxs);

    float fmins[3];
    float fmaxs[3];
    for (int i = 0; i < 3; i++)
    {
        fmins[i] = sp_ctof(mins[i]);
        fmaxs[i] = sp_ctof(maxs[i]);
    }

    g_ClipsParser.cache.addBrushBox(fmins, fmaxs, params[3]);

    return 0;
}

static cell_t Native_SetShrink(IPluginContext* pContext, const cell_t* params)
{
    g_ClipsParser.cache.setShrink(sp_ctof(params[1]));

    return 0;
}

static cell_t Native_SetBrushTolerance(IPluginContext* pContext, const cell_t* params)
{
    g_ClipsParser.cache.setBrushTolerance(sp_ctof(params[1]));

    return 0;
}

static cell_t Native_GetEdgeCount(IPluginContext* pContext, const cell_t* params)
{
    return g_ClipsParser.cache.edgeCount(params[1]);
}

static cell_t Native_GetEdge(IPluginContext* pContext, const cell_t* params)
{
    const Edge* e = g_ClipsParser.cache.edge(params[1], params[2]);
    if (!e)
    {
        return 0;
    }

    cell_t* a;
    cell_t* b;
    pContext->LocalToPhysAddr(params[3], &a);
    pContext->LocalToPhysAddr(params[4], &b);

    for (int i = 0; i < 3; i++)
    {
        a[i] = sp_ftoc(e->a[i]);
        b[i] = sp_ftoc(e->b[i]);
    }

    return 1;
}

static cell_t Native_GetBrushCount(IPluginContext* pContext, const cell_t* params)
{
    return g_ClipsParser.cache.brushCount(params[1]);
}

static cell_t Native_GetBspVersion(IPluginContext* pContext, const cell_t* params)
{
    return g_ClipsParser.cache.bspVersion();
}

static const sp_nativeinfo_t s_Natives[] = 
{
    { "Clips_Parse",                Native_Parse },
    { "Clips_Clear",                Native_Clear },
    { "Clips_ClearFilters",         Native_ClearFilters },
    { "Clips_AddMaterialFilter",    Native_AddMaterialFilter },
    { "Clips_AddHammerId",          Native_AddHammerId },
    { "Clips_AddClassnameFilter",   Native_AddClassnameFilter },
    { "Clips_AddBrushBox",          Native_AddBrushBox },
    { "Clips_SetShrink",            Native_SetShrink },
    { "Clips_SetBrushTolerance",    Native_SetBrushTolerance },
    { "Clips_GetEdgeCount",         Native_GetEdgeCount },
    { "Clips_GetEdge",              Native_GetEdge },
    { "Clips_GetBrushCount",        Native_GetBrushCount },
    { "Clips_GetBspVersion",        Native_GetBspVersion },
    { nullptr,                      nullptr },
};

bool ClipsParserExt::SDK_OnLoad(char* error, size_t maxlen, bool late)
{
    sharesys->AddNatives(myself, s_Natives);
    sharesys->RegisterLibrary(myself, "clipsparser");

    return true;
}

void ClipsParserExt::SDK_OnUnload()
{
    cache.clear();
}
