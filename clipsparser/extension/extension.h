#ifndef CLIPSPARSER_EXTENSION_H
#define CLIPSPARSER_EXTENSION_H

#include "smsdk_ext.h"
#include "clip_core.h"

#include <vector>

// A drawable line segment between two brush-face vertices, stored in game-native
// float precision because that is what gets handed back to SourcePawn.
struct Edge
{
    float a[3];
    float b[3];
};

// Holds the flattened result of one parsed map plus the filters used to build it.
class ClipCache
{
private:
    std::vector<Edge> _edges[clips::ClipTypeCount];
    int _brushCounts[clips::ClipTypeCount] = { 0 };
    int _bspVersion = 0;
    clips::ParseOptions _filters;

public:
    // Parse a .bsp and rebuild the edge lists, applying the current filters.
    // Returns false on a bad file.
    bool rebuild(const char* bspPath);

    void clear();

    // Config-driven extraction filters, set before rebuild().
    void clearFilters();
    void addMaterial(const char* substring);
    void addHammerId(int id);
    void addBrushBox(const float mins[3], const float maxs[3], int faceCount);
    void setShrink(float units);
    void setBrushTolerance(float units);

    int bspVersion() const
    {
        return _bspVersion;
    }

    int edgeCount(int type) const;
    int brushCount(int type) const;
    const Edge* edge(int type, int index) const;
};

class ClipsParserExt : public SDKExtension
{
public:
    bool SDK_OnLoad(char* error, size_t maxlen, bool late) override;
    void SDK_OnUnload() override;
    
    ClipCache cache;
};

extern ClipsParserExt g_ClipsParser;

#endif // CLIPSPARSER_EXTENSION_H
