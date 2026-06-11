#if defined _clipsparser_included
#endinput
#endif
#define _clipsparser_included

// Must stay in lock-step with clips::ClipType in clip_core.h.
enum ClipType
{
    ClipType_PlayerClip = 0,    // world brushes with CONTENTS_PLAYERCLIP only
    ClipType_MonsterClip,       // world brushes with CONTENTS_MONSTERCLIP only
    ClipType_BothClip,          // world brushes with both clip contents
    ClipType_WorldNoDraw,       // world solid brushes that are fully invisible (incl. func_detail)
    ClipType_EntInvisible,      // brush-entity geometry that is clip or fully invisible
    ClipType_EntButton,         // func_button
    ClipType_EntLadder,         // func_ladder
    ClipType_EntOther,          // any other brush entity (triggers, visible func_brush, ...)
    ClipType_Custom,            // brushes pulled in by a config material/hammerid filter
    ClipType_Count
};

/**
 * Parse a compiled map and cache its reconstructed clip geometry. Replaces any
 * previously cached result. Call once when a map starts.
 *
 * @param bspPath   Path to the .bsp, relative to the game folder (e.g. "maps/bhop_abc.bsp") or absolute.
 * 
 * @return          True on success, false if the file is missing or not VBSP.
 */
native bool Clips_Parse(const char[] bspPath);

/**
 * Drop the cached result and free its memory. Safe to call when nothing is
 * cached. The extension also clears automatically on the next Clips_Parse.
 */
native void Clips_Clear();

/**
 * Clear all extraction filters. Call before configuring filters for a map.
 */
native void Clips_ClearFilters();

/**
 * Register a material filter. Any brush whose every face uses a material
 * containing this substring (case-insensitive, e.g. "tools/toolsinvisible")
 * is added to ClipType_Custom on the next Clips_Parse. Note: func_detail is
 * merged into the world at compile time, so its brushes are still matched here
 * by material even though their entity (and hammerid) no longer exist.
 */
native void Clips_AddMaterialFilter(const char[] substring);

/**
 * Register an entity hammerid. Brushes of the brush entity with
 * this id are added to ClipType_Custom. Only works for entities that survive as
 * their own brush model (func_brush, func_wall, ...); world and
 * func_detail brushes have no hammerid in the compiled map.
 */
native void Clips_AddHammerId(int hammerid);

/**
 * Register a brush bounding box (world coordinates). Every brush whose own
 * bounding box lies INSIDE mins/maxs (expanded by the brush tolerance) is added
 * to ClipType_Custom. This singles out world geometry, which has no hammerid once
 * compiled: read the centre and size from Hammer and pass mins = centre - size/2,
 * maxs = centre + size/2. Because the match is containment, a box sized to a
 * grouped func_detail picks up all its member brushes, while a box sized to one
 * brush selects just that brush.
 *
 * @param faces     Optional required face count (Hammer's "solid with N faces");
 *                  0 matches any. Set it to tighten the match for a single brush.
 */
native void Clips_AddBrushBox(const float mins[3], const float maxs[3], int faces = 0);

/**
 * Shrink every brush inward by this many units before its faces are built, so
 * the drawn edges sit inside the shape instead of flush against neighbouring
 * surfaces (walls, skybox). 0 (default) leaves geometry on the brush faces.
 * Set before Clips_Parse.
 */
native void Clips_SetShrink(float units);

/**
 * Per-corner tolerance (units) for matching brushes against Clips_AddBrushBox
 * selectors. The match runs on the brush's unshrunk bounds, so this only needs to
 * absorb authoring rounding, not the shrink amount. Default 1.0. Set before
 * Clips_Parse.
 */
native void Clips_SetBrushTolerance(float units);

/**
 * Number of edges available for a clip type after a successful parse.
 *
 * @param type      Clip type to query.
 * 
 * @return          Edge count, or 0 if nothing is cached for that type.
 */
native int Clips_GetEdgeCount(ClipType type);

/**
 * Fetch one edge (a line segment between two brush-face vertices).
 *
 * @param type      Clip type to read from.
 * @param index     Edge index in range [0, Clips_GetEdgeCount(type)].
 * @param a         Receives the first endpoint.
 * @param b         Receives the second endpoint.
 * 
 * @return          True if the index was valid and the points were written.
 */
native bool Clips_GetEdge(ClipType type, int index, float a[3], float b[3]);

/**
 * Number of distinct brushes that contributed edges of a clip type. Handy for
 * status output ("drawing N player-clip brushes").
 */
native int Clips_GetBrushCount(ClipType type);

/**
 * The VBSP version of the currently cached map (19 or 20), or 0 if none.
 */
native int Clips_GetBspVersion();

public Extension __ext_clipsparser =
{
    name = "clipsparser",
    file = "clipsparser.ext",
    autoload = 1,
    required = 0,
};
