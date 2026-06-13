#include "clip_core.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "lzma/LzmaDec.h"

namespace clips
{
namespace
{

// A VBSP file is a header followed by 64 lumps, each lump being a flat array of
// fixed-size records at some offset in the file. We only touch the handful of
// lumps needed to rebuild brushes and tell them apart.
enum LumpIndex
{
    Lump_Entities           = 0,   // the entity keyvalue text block
    Lump_Planes             = 1,   // dPlane[] - exact face planes
    Lump_TexData            = 2,   // dTexData[] - links a face to its material name
    Lump_Nodes              = 5,   // dNode[] - BSP tree interior nodes
    Lump_TexInfo            = 6,   // dTexInfo[] - per-face surface flags + texdata index
    Lump_Leaves             = 10,  // dLeaf[] - BSP tree leaves (hold brush lists)
    Lump_Models             = 14,  // dModel[] - model 0 is the world, 1.. are brush entities
    Lump_LeafBrushes        = 17,  // uint16[] - brush indices referenced by leaves
    Lump_Brushes            = 18,  // dBrush[] - one record per collision brush
    Lump_BrushSides         = 19,  // dBrushSide[] - the sides (planes) of each brush
    Lump_TexDataStringData  = 43,  // char[] - packed material name strings
    Lump_TexDataStringTable = 44,  // int[] - offsets into the string data
    HeaderLumpCount         = 64,
};

// Brush flags
enum BrushContents
{
    Contents_Solid          = 0x1,
    Contents_PlayerClip     = 0x10000,
    Contents_MonsterClip    = 0x20000,
    Contents_Ladder         = 0x20000000,
};

// Per surface flags stored in dTexInfo.flags.
enum SurfaceFlags
{
    Surf_NoDraw = 0x80, // face is never rendered (the classic invisible clip)
};

// Render mode is an entity keyvalue, not a surface flag. func_wall / func_brush
// that are made invisible keep ordinary textures on their brush faces and only
// carry rendermode 10 (kRenderNone) on the entity, so this is the only way to
// recognise them.
enum RenderMode
{
    kRenderNone = 10,
};

#pragma pack(push, 1)

struct LumpDirEntry
{
    int32_t fileOffset;
    int32_t length;
    int32_t version;
    char fourCC[4];
};

struct BspHeader
{
    int32_t ident; // 'VBSP' as little-endian bytes
    int32_t version; // 19 or 20 for CS:S
    LumpDirEntry lumps[HeaderLumpCount];
    int32_t mapRevision;
};

struct dPlane
{
    float normal[3];
    float distance;
    int32_t type;
};

struct dBrush
{
    int32_t firstSide;
    int32_t numSides;
    int32_t contents;
};

struct dBrushSide
{
    uint16_t planeNum;
    int16_t texInfo;
    int16_t dispInfo;
    int16_t bevel;
};

struct dTexInfo
{
    float textureVecs[2][4];
    float lightmapVecs[2][4];
    int32_t flags;
    int32_t texData;
};

struct dTexData
{
    float reflectivity[3];
    int32_t nameStringTableId;
    int32_t width, height;
    int32_t viewWidth, viewHeight;
};

struct dModel
{
    float mins[3], maxs[3];
    float origin[3];
    int32_t headNode;
    int32_t firstFace, numFaces;
};

struct dNode
{
    int32_t planeNum;
    int32_t children[2]; // >= 0 is a node index, < 0  leaf (-1 - child)
    int16_t mins[3], maxs[3];
    uint16_t firstFace, numFaces;
    int16_t area;
    int16_t padding;
};

#pragma pack(pop)

// Everything geometric runs in double precision. The brushes themselves use
// floats, but the winding gets carved from a polygon the size of the whole
// coordinate space, and clipping a 500k-unit quad down to a few units with
// floats accumulates enough error to drop or duplicate vertices.
Vec3 operator+(Vec3 a, Vec3 b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

Vec3 operator-(Vec3 a, Vec3 b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

Vec3 operator*(Vec3 a, double scale)
{
    return { a.x * scale, a.y * scale, a.z * scale };
}

double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

double length(Vec3 a)
{
    return std::sqrt(dot(a, a));
}

Vec3 normalized(Vec3 a)
{
    double len = length(a);
    if (len > 0.0)
    {
        return a * (1.0 / len);
    }

    return a;
}

// A plane as outward normal + distance, matching the dPlane convention.
struct Plane
{
    Vec3 normal;
    double distance;
};

// Half the size of the starting quad. 2^19 comfortably covers the +-32768 world.
constexpr double kBaseWindingRange = 524288.0;

// Distance below which a point counts as "on" a clip plane rather than across it.
constexpr double kPlaneOnEpsilon = 0.01;

// Vertices closer than this are treated as the same point and merged.
constexpr double kMergeEpsilon = 0.1;

// Faces with less area than this are discarded as slivers.
constexpr double kMinFaceArea = 0.1;

// A correctly bounded face never reaches this far out; the Source world is
// +-32768, so a vertex past this means clipping failed to close the volume.
constexpr double kWorldGuard = 65536.0;

// A large quad on the given plane, used as the starting shape before clipping.
std::vector<Vec3> makeBaseWinding(const Plane& plane)
{
    // Pick whichever world axis is least parallel to the normal as the seed for
    // an in-plane basis, so the basis never collapses.
    Vec3 normal = plane.normal;
    double ax = std::fabs(normal.x);
    double ay = std::fabs(normal.y);
    double az = std::fabs(normal.z);

    Vec3 seed = { 0, 0, 1 };
    if (az >= ax && az >= ay)
    {
        seed = { 1, 0, 0 };
    }

    Vec3 up = normalized(seed - normal * dot(seed, normal));
    Vec3 right = cross(up, normal);

    Vec3 center = normal * plane.distance;
    up = up * kBaseWindingRange;
    right = right * kBaseWindingRange;

    return { center - right + up, center + right + up, center + right - up, center - right - up };
}

// Clip the polygon to the half-space behind the plane (Sutherland-Hodgman).
void clipToHalfspace(std::vector<Vec3>& poly, const Plane& plane)
{
    if (poly.empty())
    {
        return;
    }

    const size_t count = poly.size();
    std::vector<double> distances(count);
    for (size_t i = 0; i < count; i++)
    {
        distances[i] = dot(plane.normal, poly[i]) - plane.distance;
    }

    std::vector<Vec3> result;
    result.reserve(count + 1);

    for (size_t i = 0; i < count; i++)
    {
        size_t next = (i + 1) % count;
        bool insideHere = distances[i] <= kPlaneOnEpsilon;
        bool insideNext = distances[next] <= kPlaneOnEpsilon;

        if (insideHere)
        {
            result.push_back(poly[i]);
        }

        // If the edge crosses the plane, add the exact intersection point.
        if (insideHere != insideNext)
        {
            double t = distances[i] / (distances[i] - distances[next]);
            result.push_back(poly[i] + (poly[next] - poly[i]) * t);
        }
    }

    poly.swap(result);
}

// Drop duplicate and collinear vertices, then reject anything degenerate.
void cleanupWinding(std::vector<Vec3>& poly)
{
    std::vector<Vec3> merged;
    for (const Vec3& v : poly)
    {
        if (!merged.empty() && length(v - merged.back()) < kMergeEpsilon)
        {
            continue;
        }

        merged.push_back(v);
    }

    while (merged.size() >= 2 && length(merged.front() - merged.back()) < kMergeEpsilon)
    {
        merged.pop_back();
    }

    poly.swap(merged);
    if (poly.size() < 3)
    {
        poly.clear();
        return;
    }

    // Remove a vertex whenever its two adjacent edges are nearly collinear.
    bool removedAny = true;
    while (removedAny && poly.size() > 3)
    {
        removedAny = false;
        for (size_t i = 0; i < poly.size(); i++)
        {
            size_t prev = (i + poly.size() - 1) % poly.size();
            size_t next = (i + 1) % poly.size();
            Vec3 incoming = normalized(poly[i] - poly[prev]);
            Vec3 outgoing = normalized(poly[next] - poly[i]);
            if (length(cross(incoming, outgoing)) < 1e-3)
            {
                poly.erase(poly.begin() + i);
                removedAny = true;
                break;
            }
        }
    }

    if (poly.size() < 3)
    {
        poly.clear();
        return;
    }

    double area = 0.0;
    for (size_t i = 1; i + 1 < poly.size(); i++)
    {
        area += length(cross(poly[i] - poly[0], poly[i + 1] - poly[0])) * 0.5;
    }

    if (area < kMinFaceArea)
    {
        poly.clear();
    }
}

// Round coordinates that sit a hair off an integer, so output stays clean and
// stable across runs without distorting genuinely fractional (rotated) brushes.
double snapToGrid(double v)
{
    double rounded = std::round(v);
    if (std::fabs(v - rounded) < 0.01)
    {
        return rounded;
    }

    return v;
}

// A leftover from failed clipping would render as a long stray line across the
// map, so drop any winding that escapes the world bounds.
bool windingOutOfBounds(const std::vector<Vec3>& poly)
{
    for (const Vec3& v : poly)
    {
        if (std::fabs(v.x) > kWorldGuard || std::fabs(v.y) > kWorldGuard || std::fabs(v.z) > kWorldGuard)
        {
            return true;
        }
    }

    return false;
}


// Allocator the LZMA decoder uses for its internal probability tables.
void* lzmaAlloc(void* p, size_t size)
{
    (void)p;
    return std::malloc(size);
}

void lzmaFree(void* p, void* address)
{
    (void)p;
    std::free(address);
}

// A decompressed lump never legitimately exceeds this; a larger claim means a
// corrupt header, so we refuse it instead of trying a huge allocation.
constexpr size_t kMaxLumpSize = 256u * 1024u * 1024u;

// Loads a .bsp into memory and exposes typed views over its lumps. Lumps stored
// LZMA-compressed (Valve lump compression) are transparently decompressed on
// first access and cached, so the rest of the parser never sees the difference.
class BspFile
{
private:
    std::vector<char> _bytes;
    const BspHeader* _header = nullptr;

    mutable std::vector<char> _lumpCache[HeaderLumpCount];
    mutable bool _decompTried[HeaderLumpCount] = {};
    mutable bool _decompOk[HeaderLumpCount] = {};

    // Decompress a Valve LZMA lump into out. Layout: uint32 'LZMA', uint32
    // uncompressed size, uint32 compressed size, 5 property bytes, then the raw
    // LZMA stream. Returns false (and leaves out empty) on any inconsistency.
    bool decompressLump(int index, std::vector<char>& out) const
    {
        const char* p = _bytes.data() + _header->lumps[index].fileOffset;
        int len = _header->lumps[index].length;
        if (len < 17)
        {
            return false;
        }

        uint32_t actualSize = 0;
        uint32_t lzmaSize = 0;
        std::memcpy(&actualSize, p + 4, 4);
        std::memcpy(&lzmaSize, p + 8, 4);

        if ((size_t)17 + (size_t)lzmaSize > (size_t)len)
        {
            return false;
        }

        if (actualSize == 0 || actualSize > kMaxLumpSize)
        {
            return false;
        }

        out.resize(actualSize);

        ISzAlloc alloc = { lzmaAlloc, lzmaFree };
        SizeT destLen = actualSize;
        SizeT srcLen = lzmaSize;
        ELzmaStatus status;
        SRes res = LzmaDecode(reinterpret_cast<Byte*>(out.data()), &destLen, reinterpret_cast<const Byte*>(p + 17), &srcLen, reinterpret_cast<const Byte*>(p + 12), 5, LZMA_FINISH_END, &status, &alloc);

        if (res != SZ_OK || destLen != (SizeT)actualSize)
        {
            out.clear();
            return false;
        }

        return true;
    }

public:
    // A pointer + length view of a lump's data, already decompressed if needed.
    struct LumpView
    {
        const char* data;
        int length;
    };

    bool load(const char* path)
    {
        FILE* file = std::fopen(path, "rb");
        if (!file)
        {
            return false;
        }

        std::fseek(file, 0, SEEK_END);
        long size = std::ftell(file);
        std::fseek(file, 0, SEEK_SET);

        _bytes.resize(size > 0 ? size : 0);
        bool readOk = size > 0 && std::fread(_bytes.data(), 1, size, file) == (size_t)size;
        std::fclose(file);

        if (!readOk || _bytes.size() < sizeof(BspHeader))
        {
            return false;
        }

        _header = reinterpret_cast<const BspHeader*>(_bytes.data());

        return std::memcmp(&_header->ident, "VBSP", 4) == 0;
    }

    int version() const
    {
        return _header->version;
    }

    const LumpDirEntry& lump(int index) const
    {
        return _header->lumps[index];
    }

    // A lump is usable only if its declared region lies entirely within the file.
    // Anything else (corrupt header, truncated file, bogus offset) is rejected so
    // the readers below never run off the end of the buffer.
    bool lumpValid(int index) const
    {
        if (!_header || index < 0 || index >= HeaderLumpCount)
        {
            return false;
        }

        const LumpDirEntry& l = _header->lumps[index];
        if (l.fileOffset < 0 || l.length < 0)
        {
            return false;
        }

        return (size_t)l.fileOffset + (size_t)l.length <= _bytes.size();
    }

    // Source can store a lump LZMA-compressed, in which case its data begins with
    // the Valve 'LZMA' magic. We don't decompress, so such a lump is treated as
    // unreadable and the caller skips the whole map rather than reading garbage.
    bool lumpCompressed(int index) const
    {
        if (!lumpValid(index) || _header->lumps[index].length < 4)
        {
            return false;
        }

        return std::memcmp(_bytes.data() + _header->lumps[index].fileOffset, "LZMA", 4) == 0;
    }

    // The usable bytes of a lump; the raw region, or the decompressed buffer for
    // an LZMA lump. Returns {nullptr, 0} when the lump is invalid or a compressed
    // lump fails to decompress, so every reader degrades to "no data".
    LumpView lumpView(int index) const
    {
        if (!lumpValid(index))
        {
            return { nullptr, 0 };
        }

        if (!lumpCompressed(index))
        {
            return { _bytes.data() + _header->lumps[index].fileOffset, _header->lumps[index].length };
        }

        if (!_decompTried[index])
        {
            _decompTried[index] = true;
            _decompOk[index] = decompressLump(index, _lumpCache[index]);
        }

        if (!_decompOk[index])
        {
            return { nullptr, 0 };
        }

        return { _lumpCache[index].data(), (int)_lumpCache[index].size() };
    }

    template <typename T>
    const T* records(int index) const
    {
        return reinterpret_cast<const T*>(lumpView(index).data);
    }

    template <typename T>
    int recordCount(int index) const
    {
        return lumpView(index).length / (int)sizeof(T);
    }

    const char* rawLump(int index) const
    {
        return lumpView(index).data;
    }

    int lumpLength(int index) const
    {
        return lumpView(index).length;
    }
};


// The entity that owns a brush submodel ("model" "*N"), as far as we read it.
struct EntityInfo
{
    std::string classname;
    int rendermode = -1;
    int renderamt = -1;
    int hammerId = -1;
    Vec3 origin = { 0.0, 0.0, 0.0 };

    // Every keyvalue of the entity that survived compilation, keys lower-cased.
    // Lets the config gate a classname selector on arbitrary fields (e.g. func_lod
    // "solid" "1") without the parser having to know each entity's schema.
    std::unordered_map<std::string, std::string> keyvalues;
};

// Reads brushes, rebuilds their geometry and maps them to the entity that owns
// them, all from the loaded BSP lumps.
class BspMap
{
private:
    // Leaves are the only version-dependent struct we read. In leaf lump version
    // 0 each leaf carries an extra 24-byte ambient light cube at the end, making
    // it 56 bytes instead of 32. The fields we need (the brush list) sit at the
    // same offsets in both, so we only adjust the stride.
    void measureLeafLump()
    {
        _leafData = _bsp.rawLump(Lump_Leaves);
        _leafStride = (_bsp.lump(Lump_Leaves).version == 0) ? 56 : 32;
        _leafCount = (_leafData != nullptr) ? (_bsp.lumpLength(Lump_Leaves) / _leafStride) : 0;
    }

    static constexpr int kLeafFirstBrushOffset = 24; // uint16 firstLeafBrush
    static constexpr int kLeafNumBrushesOffset = 26; // uint16 numLeafBrushes

    void collectBrushesUnderNode(int nodeIndex, std::unordered_set<int>& out) const
    {
        std::unordered_set<int> visited;
        std::vector<int> stack{ nodeIndex };
        while (!stack.empty())
        {
            int index = stack.back();
            stack.pop_back();

            if (index >= 0)
            {
                if (index >= _nodeCount || !visited.insert(index).second)
                {
                    continue;
                }

                stack.push_back(_nodes[index].children[0]);
                stack.push_back(_nodes[index].children[1]);
                continue;
            }

            // A negative child encodes a leaf as (-1 - index).
            int leaf = -1 - index;
            if (leaf < 0 || leaf >= _leafCount)
            {
                continue;
            }

            const char* record = _leafData + (size_t)leaf * _leafStride;
            uint16_t firstBrush = readU16(record + kLeafFirstBrushOffset);
            uint16_t numBrushes = readU16(record + kLeafNumBrushesOffset);
            for (int k = 0; k < numBrushes; k++)
            {
                int ref = firstBrush + k;
                if (ref >= 0 && ref < _leafBrushCount)
                {
                    out.insert(_leafBrushes[ref]);
                }
            }
        }
    }

    void parseEntitiesForBrushModels(std::unordered_map<int, EntityInfo>& out) const
    {
        const char* text = _bsp.rawLump(Lump_Entities);
        int length = _bsp.lumpLength(Lump_Entities);
        if (text == nullptr || length <= 0)
        {
            return;
        }

        EntityInfo current;
        std::string model;
        bool insideEntity = false;

        for (int i = 0; i < length; )
        {
            char c = text[i];

            if (c == '{')
            {
                insideEntity = true;
                current = EntityInfo();
                model.clear();
                i++;
            }
            else if (c == '}')
            {
                // A brush entity references its geometry as model "*N".
                if (insideEntity && model.size() > 1 && model[0] == '*' && !current.classname.empty())
                {
                    out[std::atoi(model.c_str() + 1)] = current;
                }

                insideEntity = false;
                i++;
            }
            else if (c == '"')
            {
                std::string key = readQuoted(text, length, i);
                std::string value = readQuoted(text, length, i);
                current.keyvalues[toLowerStr(key.c_str())] = value;
                if (key == "classname")
                {
                    current.classname = value;
                }
                else if (key == "model")
                {
                    model = value;
                }
                else if (key == "rendermode")
                {
                    current.rendermode = std::atoi(value.c_str());
                }
                else if (key == "renderamt")
                {
                    current.renderamt = std::atoi(value.c_str());
                }
                else if (key == "origin")
                {
                    std::sscanf(value.c_str(), "%lf %lf %lf", &current.origin.x, &current.origin.y, &current.origin.z);
                }
                else if (key == "hammerid")
                {
                    current.hammerId = std::atoi(value.c_str());
                }
            }
            else
            {
                i++;
            }
        }
    }

    // Read the next "..."-quoted token, advancing i past the closing quote.
    static std::string readQuoted(const char* text, int length, int& i)
    {
        while (i < length && text[i] != '"')
        {
            i++;
        }

        if (i >= length)
        {
            return {};
        }

        int start = ++i;
        while (i < length && text[i] != '"')
        {
            i++;
        }

        std::string token(text + start, i - start);
        if (i < length)
        {
            i++;
        }

        return token;
    }

    static uint16_t readU16(const char* p)
    {
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));

        return v;
    }

    static std::string toLowerStr(const char* s)
    {
        std::string out = s;
        for (char& c : out)
        {
            c = (char)std::tolower((unsigned char)c);
        }

        return out;
    }

    // A material counts as invisible by name when it carries a "nodraw"/"invisible"
    // token. In strict mode (the default) it must ALSO carry the "tools" marker, so
    // player nicknames or art textures that merely contain the substring (e.g.
    // "akno/aknodraw_01") are not mistaken for clips; non-strict mode drops that
    // requirement. Genuine nodraw faces are also caught by the Surf_NoDraw flag,
    // independent of this heuristic.
    bool materialIsInvisible(const char* material) const
    {
        std::string name = toLowerStr(material);
        if (_strictInvisible && name.find("tools") == std::string::npos)
        {
            return false;
        }

        return name.find("invisible") != std::string::npos || name.find("nodraw") != std::string::npos;
    }

    // A material that looks invisible by name but lacks the "tools" marker, so it is
    // deliberately not auto-classified. Surfaced to the server log so an author can
    // add it to the config 'materials' block if it really is a clip.
    static bool materialIsSuspicious(const char* material)
    {
        std::string name = toLowerStr(material);
        bool looksInvisible = name.find("invisible") != std::string::npos || name.find("nodraw") != std::string::npos;

        return looksInvisible && name.find("tools") == std::string::npos;
    }

    const BspFile& _bsp;

    // When true, materialIsInvisible requires the "tools" marker; set from
    // the config's strict_parse before parsing.
    bool _strictInvisible = true;

    const dPlane* _planes = nullptr;
    const dBrush* _brushes = nullptr;
    const dBrushSide* _brushSides = nullptr;
    const dTexInfo* _texInfos = nullptr;
    const dTexData* _texDatas = nullptr;
    const dModel* _models = nullptr;
    const dNode* _nodes = nullptr;
    const uint16_t* _leafBrushes = nullptr;
    const int32_t* _stringTable = nullptr;
    const char* _stringData = nullptr;

    int _planeCount = 0, _brushCount = 0, _brushSideCount = 0;
    int _texInfoCount = 0, _texDataCount = 0;
    int _modelCount = 0, _nodeCount = 0, _leafBrushCount = 0, _stringTableCount = 0;
    int _stringDataLength = 0;

    const char* _leafData = nullptr;
    int _leafStride = 32;
    int _leafCount = 0;

public:
    explicit BspMap(const BspFile& bsp) : _bsp(bsp)
    {
        _planes = bsp.records<dPlane>(Lump_Planes);
        _brushes = bsp.records<dBrush>(Lump_Brushes);
        _brushSides = bsp.records<dBrushSide>(Lump_BrushSides);
        _texInfos = bsp.records<dTexInfo>(Lump_TexInfo);
        _texDatas = bsp.records<dTexData>(Lump_TexData);
        _models = bsp.records<dModel>(Lump_Models);
        _nodes = bsp.records<dNode>(Lump_Nodes);
        _leafBrushes = bsp.records<uint16_t>(Lump_LeafBrushes);
        _stringTable = bsp.records<int32_t>(Lump_TexDataStringTable);
        _stringData = bsp.rawLump(Lump_TexDataStringData);
        _planeCount = bsp.recordCount<dPlane>(Lump_Planes);
        _brushCount = bsp.recordCount<dBrush>(Lump_Brushes);
        _brushSideCount = bsp.recordCount<dBrushSide>(Lump_BrushSides);
        _texInfoCount = bsp.recordCount<dTexInfo>(Lump_TexInfo);
        _texDataCount = bsp.recordCount<dTexData>(Lump_TexData);
        _modelCount = bsp.recordCount<dModel>(Lump_Models);
        _nodeCount = bsp.recordCount<dNode>(Lump_Nodes);
        _leafBrushCount = bsp.recordCount<uint16_t>(Lump_LeafBrushes);
        _stringTableCount = bsp.recordCount<int32_t>(Lump_TexDataStringTable);
        _stringDataLength = bsp.lumpLength(Lump_TexDataStringData);

        measureLeafLump();
    }

    int brushCount() const
    {
        return _brushCount;
    }

    void setStrictInvisible(bool strict)
    {
        _strictInvisible = strict;
    }

    const dBrush& brush(int index) const
    {
        return _brushes[index];
    }

    int sideFlags(const dBrushSide& side) const
    {
        if (side.texInfo < 0 || side.texInfo >= _texInfoCount)
        {
            return 0;
        }

        return _texInfos[side.texInfo].flags;
    }

    // The material name of a brush side, or "" when it has no texinfo (bevels).
    const char* sideMaterial(const dBrushSide& side) const
    {
        if (side.texInfo < 0 || side.texInfo >= _texInfoCount)
        {
            return "";
        }

        int texData = _texInfos[side.texInfo].texData;
        if (texData < 0 || texData >= _texDataCount)
        {
            return "";
        }

        int nameId = _texDatas[texData].nameStringTableId;
        if (nameId < 0 || nameId >= _stringTableCount)
        {
            return "";
        }

        int offset = _stringTable[nameId];
        if (_stringData == nullptr || offset < 0 || offset >= _stringDataLength)
        {
            return "";
        }

        return _stringData + offset;
    }

    // True only when the brush's side range lies fully inside the brush-sides
    // lump, so the per-side loops below never read out of bounds.
    bool sidesInRange(const dBrush& b) const
    {
        return b.numSides >= 0 && b.firstSide >= 0 && b.firstSide + b.numSides <= _brushSideCount;
    }

    // True when every real (non-bevel) side of the brush is invisible, either by
    // the nodraw surface flag or by an invisible tool material. That is what an
    // "invisible solid" block looks like: collidable but never rendered.
    bool brushIsFullyInvisible(int brushIndex) const
    {
        const dBrush& b = _brushes[brushIndex];
        if (!sidesInRange(b))
        {
            return false;
        }

        bool sawRealSide = false;
        for (int i = 0; i < b.numSides; i++)
        {
            const dBrushSide& side = _brushSides[b.firstSide + i];
            if (side.bevel)
            {
                continue;
            }

            sawRealSide = true;
            bool invisible = (sideFlags(side) & Surf_NoDraw) || materialIsInvisible(sideMaterial(side));
            if (!invisible)
            {
                return false;
            }
        }

        return sawRealSide;
    }

    // True when every real face uses one of the given material substrings, i.e.
    // the whole brush is "made of" those textures (the config material filter).
    bool brushMatchesMaterials(int brushIndex, const std::vector<std::string>& wanted) const
    {
        if (wanted.empty())
        {
            return false;
        }

        const dBrush& b = _brushes[brushIndex];
        if (!sidesInRange(b))
        {
            return false;
        }

        bool sawRealSide = false;
        for (int i = 0; i < b.numSides; i++)
        {
            const dBrushSide& side = _brushSides[b.firstSide + i];
            if (side.bevel)
            {
                continue;
            }

            sawRealSide = true;
            std::string material = toLowerStr(sideMaterial(side));
            bool hit = false;
            for (const std::string& w : wanted)
            {
                if (!w.empty() && material.find(w) != std::string::npos)
                {
                    hit = true;
                    break;
                }
            }

            if (!hit)
            {
                return false;
            }
        }

        return sawRealSide;
    }

    // Scan every real brush side and collect (deduplicated, lower-cased) the names
    // of materials that look invisible but lack the "tools" marker, so the server
    // can warn about textures we intentionally skipped.
    void collectSuspiciousMaterials(std::unordered_set<std::string>& out) const
    {
        for (int bi = 0; bi < brushCount(); bi++)
        {
            const dBrush& b = _brushes[bi];
            if (!sidesInRange(b))
            {
                continue;
            }

            for (int i = 0; i < b.numSides; i++)
            {
                const dBrushSide& side = _brushSides[b.firstSide + i];
                if (side.bevel)
                {
                    continue;
                }

                const char* material = sideMaterial(side);
                if (materialIsSuspicious(material))
                {
                    out.insert(toLowerStr(material));
                }
            }
        }
    }

    // Rebuild every renderable face polygon of one brush. `shrink` moves every
    // side plane inward by that many units, eroding the brush so its faces end
    // up inside the shape rather than flush against neighbouring surfaces.
    std::vector<Face> buildBrushFaces(int brushIndex, double shrink) const
    {
        const dBrush& b = _brushes[brushIndex];
        if (!sidesInRange(b))
        {
            return {};
        }

        // Collect a plane for every side. Bevel sides are extra collision planes
        // that aren't real faces, but they still bound the brush, so we clip
        // against all of them and only emit faces for the non-bevel sides. This
        // guarantees the winding is fully closed.
        std::vector<Plane> planes;
        std::vector<int> sideIndices;
        std::vector<bool> bevel;
        planes.reserve(b.numSides);
        sideIndices.reserve(b.numSides);
        bevel.reserve(b.numSides);

        for (int i = 0; i < b.numSides; i++)
        {
            const dBrushSide& side = _brushSides[b.firstSide + i];

            // A side plane index out of range means the brush data is broken;
            // skip the whole brush rather than read an arbitrary plane.
            if (side.planeNum >= _planeCount)
            {
                return {};
            }

            const dPlane& p = _planes[side.planeNum];

            // Side normals point outward and the interior is dot(n,x) <= dist, so
            // lowering dist by `shrink` pulls the plane toward the interior.
            planes.push_back({ { p.normal[0], p.normal[1], p.normal[2] }, p.distance - shrink });
            sideIndices.push_back(i);
            bevel.push_back(side.bevel != 0);
        }

        std::vector<Face> faces;
        for (size_t i = 0; i < planes.size(); i++)
        {
            if (bevel[i])
            {
                continue;
            }

            std::vector<Vec3> winding = makeBaseWinding(planes[i]);
            for (size_t j = 0; j < planes.size() && !winding.empty(); j++)
            {
                if (j != i)
                {
                    clipToHalfspace(winding, planes[j]);
                }
            }

            cleanupWinding(winding);
            if (winding.size() < 3 || windingOutOfBounds(winding))
            {
                continue;
            }

            Face face;
            face.brushIndex = brushIndex;
            face.sideIndex = sideIndices[i];
            for (const Vec3& v : winding)
            {
                face.vertices.push_back({ snapToGrid(v.x), snapToGrid(v.y), snapToGrid(v.z) });
            }

            faces.push_back(std::move(face));
        }

        return faces;
    }

    // Map every brush to the model that owns it. Model 0 is the world; models
    // 1.. are brush entities (func_brush, func_button, triggers, ...). World
    // brushes are claimed first, so a brush only ever maps to its real owner.
    std::unordered_map<int, int> mapBrushesToModels() const
    {
        std::unordered_map<int, int> owner;
        for (int model = 0; model < _modelCount; model++)
        {
            std::unordered_set<int> brushesInModel;
            collectBrushesUnderNode(_models[model].headNode, brushesInModel);
            for (int brush : brushesInModel)
            {
                owner.emplace(brush, model);
            }
        }

        return owner;
    }

    // Map a submodel index (model "*N") to the entity that uses it.
    std::unordered_map<int, EntityInfo> mapModelsToEntities() const
    {
        std::unordered_map<int, EntityInfo> result;
        parseEntitiesForBrushModels(result);

        return result;
    }
};


std::string toLower(std::string s)
{
    for (char& c : s)
    {
        c = (char)std::tolower((unsigned char)c);
    }

    return s;
}

bool entityIsInvisible(const EntityInfo& e)
{
    return e.rendermode == kRenderNone || e.renderamt == 0;
}

/*
 * Decide which list a brush belongs in, or -1 to skip it entirely.
 */
int classifyBrush(const BspMap& map, int brushIndex, int ownerModel, const EntityInfo* owner)
{
    const dBrush& b = map.brush(brushIndex);
    int contents = b.contents;

    // Ladders carry CONTENTS_LADDER and are usually merged into the world model
    // (the func_ladder entity is dropped at compile time), so go by contents.
    if (contents & Contents_Ladder)
    {
        return Clip_EntLadder;
    }

    int clip = contents & (Contents_PlayerClip | Contents_MonsterClip);
    if (clip == (Contents_PlayerClip | Contents_MonsterClip))
    {
        return Clip_BothClip;
    }

    if (clip == Contents_MonsterClip)
    {
        return Clip_MonsterClip;
    }

    if (clip == Contents_PlayerClip)
    {
        return Clip_PlayerClip;
    }

    if (ownerModel == 0)
    {
        // World (and merged func_detail) solid brushes that are fully invisible.
        if ((contents & Contents_Solid) && map.brushIsFullyInvisible(brushIndex))
        {
            return Clip_WorldNoDraw;
        }

        return -1;
    }

    if (owner == nullptr)
    {
        return -1;
    }

    const std::string& cls = owner->classname;
    if (cls == "func_button")
    {
        return Clip_EntButton;
    }

    if (cls == "func_brush" || cls == "func_wall")
    {
        // Invisible either via the entity rendermode or via nodraw face materials.
        if (entityIsInvisible(*owner) || map.brushIsFullyInvisible(brushIndex))
        {
            return Clip_EntInvisible;
        }

        return -1;
    }

    return -1;
}

// True if the brush should also go into Clip_Custom because of a config filter.
bool brushMatchesCustom(const BspMap& map, int brushIndex, const EntityInfo* owner, const ParseOptions& options)
{
    if (map.brushMatchesMaterials(brushIndex, options.materials))
    {
        return true;
    }

    if (owner != nullptr && owner->hammerId >= 0)
    {
        for (int id : options.hammerIds)
        {
            if (id == owner->hammerId)
            {
                return true;
            }
        }
    }

    // classname
    if (owner != nullptr && !owner->classname.empty())
    {
        for (const ClassnameFilter& f : options.classnames)
        {
            if (toLower(f.classname) != owner->classname)
            {
                continue;
            }

            // No conditions: every entity of this classname matches. Otherwise the
            // entity must carry each required keyvalue with the listed value.
            bool allMatch = true;
            for (const auto& cond : f.require)
            {
                auto it = owner->keyvalues.find(toLower(cond.first)); // keys stored lower-cased
                if (it == owner->keyvalues.end() || toLower(it->second) != toLower(cond.second))
                {
                    allMatch = false;
                    break;
                }
            }

            if (allMatch)
            {
                return true;
            }
        }
    }

    return false;
}

// True if this brush's owning entity is on the config's exclude-by-hammerid list,
// in which case the brush is dropped before any other rule gets to claim it.
bool hammerIdExcluded(const EntityInfo* owner, const ParseOptions& options)
{
    if (owner == nullptr || owner->hammerId < 0)
    {
        return false;
    }

    for (int id : options.excludeHammerIds)
    {
        if (id == owner->hammerId)
        {
            return true;
        }
    }

    return false;
}

// Build a brush's faces, falling back to the unshrunk shape when shrinking
// collapsed a thin brush to nothing.
std::vector<Face> buildFacesWithFallback(const BspMap& map, int brushIndex, double shrink)
{
    std::vector<Face> faces = map.buildBrushFaces(brushIndex, shrink);
    if (faces.empty() && shrink > 0.0)
    {
        faces = map.buildBrushFaces(brushIndex, 0.0);
    }

    return faces;
}

// True if the brush should be drawn for one of the selectors: its bounding box
// (built from its face vertices and shifted into world space by the owner origin)
// lies INSIDE the selector box, expanded by `tolerance` on each side, and - when
// the selector sets a face count - the brush has exactly that many faces.
//
// Containment (rather than exact-corner equality) lets one selector box pick up
// every brush of a grouped func_detail: you author the group's overall
// centre/size from Hammer and each member brush, being inside that box, is drawn.
// A box sized to a single brush still selects just that brush.
bool brushBoxMatches(const std::vector<Face>& faces, const Vec3& originOffset, const std::vector<BrushMatch>& boxes, double tolerance)
{
    Vec3 mn = {  1e30,  1e30,  1e30 };
    Vec3 mx = { -1e30, -1e30, -1e30 };

    for (const Face& face : faces)
    {
        for (const Vec3& v : face.vertices)
        {
            mn.x = (v.x < mn.x) ? v.x : mn.x;
            mn.y = (v.y < mn.y) ? v.y : mn.y;
            mn.z = (v.z < mn.z) ? v.z : mn.z;
            mx.x = (v.x > mx.x) ? v.x : mx.x;
            mx.y = (v.y > mx.y) ? v.y : mx.y;
            mx.z = (v.z > mx.z) ? v.z : mx.z;
        }
    }

    mn.x += originOffset.x; mn.y += originOffset.y; mn.z += originOffset.z;
    mx.x += originOffset.x; mx.y += originOffset.y; mx.z += originOffset.z;

    int faceCount = (int)faces.size();

    for (const BrushMatch& m : boxes)
    {
        if (m.faceCount > 0 && faceCount != m.faceCount)
        {
            continue;
        }

        // Normalise the selector corners (mins/maxs may be given in any order).
        double loX = ((m.mins.x < m.maxs.x) ? m.mins.x : m.maxs.x) - tolerance;
        double loY = ((m.mins.y < m.maxs.y) ? m.mins.y : m.maxs.y) - tolerance;
        double loZ = ((m.mins.z < m.maxs.z) ? m.mins.z : m.maxs.z) - tolerance;
        double hiX = ((m.mins.x > m.maxs.x) ? m.mins.x : m.maxs.x) + tolerance;
        double hiY = ((m.mins.y > m.maxs.y) ? m.mins.y : m.maxs.y) + tolerance;
        double hiZ = ((m.mins.z > m.maxs.z) ? m.mins.z : m.maxs.z) + tolerance;

        if (mn.x >= loX && mn.y >= loY && mn.z >= loZ &&
            mx.x <= hiX && mx.y <= hiY && mx.z <= hiZ)
        {
            return true;
        }
    }

    return false;
}

}

ParseResult parseBspClips(const char* bspPath, const ParseOptions& options)
{
    ParseResult result;

    BspFile bsp;
    if (!bsp.load(bspPath))
    {
        return result;
    }

    // Refuse maps we cannot read safely; any lump needed to rebuild geometry that
    // is out of range, or LZMA-compressed and fails to decompress. lumpView()
    // returns null data in those cases; rather than read garbage (and crash the
    // server) we leave result.ok false and draw nothing on such a map.
    const int essentialLumps[] = {
        Lump_Planes, Lump_Brushes, Lump_BrushSides,
        Lump_Models, Lump_Nodes, Lump_Leaves, Lump_LeafBrushes,
    };
    for (int idx : essentialLumps)
    {
        if (bsp.lumpView(idx).data == nullptr)
        {
            return result;
        }
    }

    BspMap map(bsp);
    map.setStrictInvisible(options.strictInvisible);
    std::unordered_map<int, int> brushModel = map.mapBrushesToModels();
    std::unordered_map<int, EntityInfo> modelEntities = map.mapModelsToEntities();

    result.ok = true;
    result.bspVersion = bsp.version();
    result.brushCount = map.brushCount();

    for (int i = 0; i < map.brushCount(); i++)
    {
        const dBrush& b = map.brush(i);
        int model = brushModel.count(i) ? brushModel[i] : 0;

        if (model == 0)
        {
            result.worldBrushes++;
        }
        else
        {
            result.entityBrushes++;
        }

        if (b.contents & Contents_PlayerClip)
        {
            result.playerClips++;
        }

        if (b.contents & Contents_MonsterClip)
        {
            result.monsterClips++;
        }

        EntityInfo owner;
        const EntityInfo* ownerPtr = nullptr;
        auto it = modelEntities.find(model);
        if (it != modelEntities.end())
        {
            owner = it->second;
            owner.classname = toLower(owner.classname);
            ownerPtr = &owner;
        }

        if (hammerIdExcluded(ownerPtr, options))
        {
            continue;
        }

        Vec3 originOffset = ownerPtr ? owner.origin : Vec3{ 0.0, 0.0, 0.0 };

        std::vector<Face> faces;

        int type = classifyBrush(map, i, model, ownerPtr);
        if (type < 0)
        {
            if (brushMatchesCustom(map, i, ownerPtr, options))
            {
                type = Clip_Custom;
            }
            else if (!options.brushBoxes.empty())
            {
                // Position-based selection. Match against the UNSHRUNK bounds so a
                // centre/size authored from Hammer lines up exactly; shrink only
                // affects the geometry we draw, never the match.
                std::vector<Face> matchFaces = buildFacesWithFallback(map, i, 0.0);

                if (matchFaces.empty() || !brushBoxMatches(matchFaces, originOffset, options.brushBoxes, options.brushBoxTolerance))
                {
                    continue;
                }

                type = Clip_Custom;
            }
            else
            {
                continue;
            }
        }

        // Shrinking erodes the brush by `shrink` on every side, so a brush thinner
        // than 2*shrink (a 1-2 unit wall clip, say) collapses to nothing; the
        // builder falls back to drawing it unshrunk in that case.
        faces = buildFacesWithFallback(map, i, options.shrink);

        if (faces.empty())
        {
            continue;
        }

        // Brush entity geometry is stored relative to the entity origin; shift it
        // into world space (origin is 0 for the world and most entities).
        if (ownerPtr)
        {
            for (Face& face : faces)
            {
                for (Vec3& v : face.vertices)
                {
                    v.x += owner.origin.x;
                    v.y += owner.origin.y;
                    v.z += owner.origin.z;
                }
            }
        }

        std::vector<Face>& destination = result.faces[type];
        destination.insert(destination.end(), std::make_move_iterator(faces.begin()), std::make_move_iterator(faces.end()));
    }

    // Only meaningful in strict mode: in non-strict mode these textures are drawn,
    // so there is nothing to warn about.
    if (options.strictInvisible)
    {
        std::unordered_set<std::string> suspicious;
        map.collectSuspiciousMaterials(suspicious);
        result.suspiciousMaterials.assign(suspicious.begin(), suspicious.end());
    }

    return result;
}

const char* clipTypeName(ClipType type)
{
    switch (type)
    {
        case Clip_PlayerClip:   return "clip_player";
        case Clip_MonsterClip:  return "clip_monster";
        case Clip_BothClip:     return "clip_both";
        case Clip_WorldNoDraw:  return "world_nodraw";
        case Clip_EntInvisible: return "ent_invisible";
        case Clip_EntButton:    return "ent_button";
        case Clip_EntLadder:    return "ent_ladder";
        case Clip_EntOther:     return "ent_other";
        case Clip_Custom:       return "custom";
        default:                return "unknown";
    }
}

}
