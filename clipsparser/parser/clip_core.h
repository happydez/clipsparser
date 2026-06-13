#ifndef CLIP_CORE_H
#define CLIP_CORE_H

#include <vector>
#include <string>

namespace clips
{

struct Vec3
{
    double x, y, z;
};

struct Face
{
    int brushIndex;
    int sideIndex;
    std::vector<Vec3> vertices;
};

// How a brush is categorised for drawing. The clip/ladder/nodraw types come
// from brush contents; the ent_* types come from the owning brush entity.
enum ClipType
{
    Clip_PlayerClip,
    Clip_MonsterClip,
    Clip_BothClip,
    Clip_WorldNoDraw,
    Clip_EntInvisible,
    Clip_EntButton,
    Clip_EntLadder,
    Clip_EntOther,
    Clip_Custom, // brushes matched by a config material/hammerid filter
    ClipTypeCount,
};

// A position-based brush selector: an axis-aligned box (mins/maxs in any order)
// plus an optional required face count. faceCount 0 means "match any face count".
struct BrushMatch
{
    Vec3 mins;
    Vec3 maxs;
    int faceCount;
};

// Extra extraction rules supplied from the per map config. A brush is added to
// Clip_Custom if all of its faces use one of these materials (substring match),
// if it belongs to a brush entity whose hammerid is listed, if it belongs to a
// brush entity whose classname is listed, or if it matches one of the brushBoxes
// by extent. All exist to pull in geometry the built-in heuristics don't cover.
struct ParseOptions
{
    std::vector<std::string> materials; // e.g. "tools/toolsinvisible"
    std::vector<int> hammerIds; // editor entity ids of brush entities

    // Brush-entity classnames to pull in, e.g. "func_lod". Compared case-
    // insensitively against the owning entity's classname. Only applies to brushes
    // not already claimed by the built-in classification, and (like hammerIds) only
    // to entities that survive compilation as their own brush model; world and
    // func_detail brushes have no hammerid in the compiled map.
    std::vector<std::string> classnames;

    // World geometry loses its hammerid when compiled, so the only way to single
    // out a specific wall/block is by position. A brush is drawn when its bounding
    // box lies INSIDE one of these boxes (expanded by brushBoxTolerance) and, if
    // faceCount is set, its face count matches too. Containment means one box sized
    // to a grouped func_detail picks up every member brush; a box sized to a single
    // brush selects just that brush. Author the centre/size from Hammer.
    std::vector<BrushMatch> brushBoxes;

    // Shrink every brush inward by this many units before building its faces, so
    // the drawn edges sit inside the shape instead of flush against neighbouring
    // surfaces. 0 leaves geometry exactly on the brush faces.
    double shrink = 0.0;

    // Per-corner tolerance (units) when matching a brush against brushBoxes. The
    // match is done on the UNSHRUNK bounds, so this only needs to absorb authoring
    // rounding, not the shrink amount.
    double brushBoxTolerance = 1.0;
};

struct ParseResult
{
    bool ok = false;
    int bspVersion = 0;
    int brushCount = 0;
    int worldBrushes = 0;
    int entityBrushes = 0;
    int playerClips = 0;
    int monsterClips = 0;

    std::vector<Face> faces[ClipTypeCount];
};

// Parse a .bsp and reconstruct every clip face. On failure ParseResult::ok is
// false and the face lists are empty. The options add config-driven material
// and hammerid filters on top of the built-in classification.
ParseResult parseBspClips(const char* bspPath, const ParseOptions& options = {});

// Stable short name for a clip type, e.g. "clip_player". Used for filenames and
// log lines.
const char* clipTypeName(ClipType type);

}

#endif // CLIP_CORE_H
