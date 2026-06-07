#include <sourcemod>
#include <sdktools>
#include <clientprefs>
#include <clipsparser>
#include <convar_class>

#pragma semicolon 1
#pragma newdecls required

#define TYPE_COUNT view_as<int>(ClipType_Count)
#define MAX_COLORS 64
#define DEFAULT_WIDTH 0.8

// Built-in fallbacks for the beam materials, used when 
// configs/clipsparser/downloads.cfg is missing.
#define DEFAULT_BEAM_MATERIAL "materials/clipsparser/beam.vmt"
#define DEFAULT_IGNOREZ_MATERIAL "materials/clipsparser/ignorez_beam.vmt"
#define DEFAULT_BEAM_TEXTURE "materials/clipsparser/beam.vtf"

// menu pagination
#define MENU_PAGE_ITEMS 7
#define PageOf(%1) (((%1) / MENU_PAGE_ITEMS) * MENU_PAGE_ITEMS)

#define CHAT_COLOR_ON  "\x07A8CF9A"
#define CHAT_COLOR_OFF "\x07DE9B8F"

Convar gCV_Radius;
Convar gCV_Refresh;
Convar gCV_WidthMin;
Convar gCV_WidthMax;
Convar gCV_DefaultShrink;
Convar gCV_MaxEdges;
Convar gCV_SortNearest;
Convar gCV_ColText;
Convar gCV_ColVal;
Convar gCV_Commands;

bool gB_CommandsRegistered; // toggle commands from clips_commands registered once

int gI_BeamModel;
int gI_IgnoreZBeam;

char gS_BeamMaterial[PLATFORM_MAX_PATH];
char gS_IgnoreZMaterial[PLATFORM_MAX_PATH];

// Static map data, rebuilt every map.
bool gB_MapTypeEnabled[TYPE_COUNT];
ArrayList gA_Edges[TYPE_COUNT];
ArrayList gA_ExcludeRegions;
int gI_BrushCount[TYPE_COUNT];
int gI_CursorType;
int gI_CursorIndex;
int gI_TotalEdges;
int gI_DrawnThisCycle[MAXPLAYERS + 1];
int gI_CycleFrame;
char gS_MapName[64];

float gF_DrawRadius;
float gF_RefreshTime;
int gI_MaxEdges;
float gF_WidthMin;
float gF_WidthMax;
bool gB_SortNearest;

ArrayList gA_DrawPlan[MAXPLAYERS + 1];
int gI_PlanPos[MAXPLAYERS + 1];

// Client settings
bool gB_Show[MAXPLAYERS + 1];
bool gB_IgnoreZ[MAXPLAYERS + 1];
bool gB_Optimize[MAXPLAYERS + 1];
bool gB_TypeOn[MAXPLAYERS + 1][TYPE_COUNT];
int gI_ColorIdx[MAXPLAYERS + 1][TYPE_COUNT];
float gF_Width[MAXPLAYERS + 1][TYPE_COUNT];
int gI_MenuType[MAXPLAYERS + 1];
int gI_MainPos[MAXPLAYERS + 1];
int gI_TypesPos[MAXPLAYERS + 1];

Cookie gC_kShow;
Cookie gC_kIgnoreZ;
Cookie gC_kOptimize;
Cookie gC_kFlags;
Cookie gC_kColors;
Cookie gC_kWidths;

char gS_TypeLabel[TYPE_COUNT][] = {
    "Player clips", "Monster clips", "Combined clips", "World nodraw",
    "Invisible brushes", "Buttons", "Ladders", "Other entities", "Custom"
};

// Menu Colour
int gI_ColorCount = 0;
int gI_ColorTable[MAX_COLORS][4];
char gS_ColorName[MAX_COLORS][64];

char gS_ColText[16];
char gS_ColVal[16];

int gI_DefaultColor[TYPE_COUNT] = { 8, 5, 10, 2, 7, 9, 11, 1, 5 };
bool gB_DefaultOn[TYPE_COUNT] = { true, true, true, true, true, true, true, true, true };

public Plugin myinfo = {
    name        = "Clips Parser",
    author      = "happydez",
    description = "Draws parsed invisible clip geometry",
    version     = "1.1.0",
    url         = "https://github.com/happydez/clipsparser"
};

public APLRes AskPluginLoad2(Handle myself, bool late, char[] error, int err_max)
{
    MarkNativeAsOptional("Clips_Parse");
    MarkNativeAsOptional("Clips_Clear");
    MarkNativeAsOptional("Clips_ClearFilters");
    MarkNativeAsOptional("Clips_AddMaterialFilter");
    MarkNativeAsOptional("Clips_AddHammerId");
    MarkNativeAsOptional("Clips_AddBrushBox");
    MarkNativeAsOptional("Clips_SetShrink");
    MarkNativeAsOptional("Clips_GetEdgeCount");
    MarkNativeAsOptional("Clips_GetEdge");
    MarkNativeAsOptional("Clips_GetBrushCount");
    MarkNativeAsOptional("Clips_GetBspVersion");

    return APLRes_Success;
}

public void OnPluginStart()
{
    gCV_Radius = new Convar("clips_draw_radius", "4096", "Always only draw clip edges within this many units of a viewer (applies whether optimization is on or off).", _, true, 0.0);
    gCV_Refresh = new Convar("clips_refresh_time", "1.0", "Seconds for one full redraw of all visible edges. Higher = fewer packets/sec.", _, true, 0.2, true, 5.0);
    gCV_WidthMin = new Convar("clips_width_min", "0.1", "Minimum beam width a player can set (cannot go below 0).", _, true, 0.0);
    gCV_WidthMax = new Convar("clips_width_max", "10.0", "Maximum beam width a player can set.", _, true, 0.1);
    gCV_DefaultShrink = new Convar("clips_default_shrink", "0.0", "Shrink (units) applied on maps with no custom config;\na custom config's own shrink overrides this.", _, true, 0.0);
    gCV_MaxEdges = new Convar("clips_max_edges", "1024", "Max clip edges drawn to one player per refresh cycle (0 = unlimited).\nCaps client load in extremely dense areas.", _, true, 0.0);
    gCV_SortNearest = new Convar("clips_sort_nearest", "0", "When clips_max_edges caps drawing, draw the NEAREST clips first (1)\ninstead of arbitrary (0). Costs a per-cycle sort.", _, true, 0.0, true, 1.0);
    gCV_ColText = new Convar("clips_color_text", "e8dccc", "Chat message text colour (hex RRGGBB).");
    gCV_ColVal = new Convar("clips_color_value", "9c948a", "Chat message value/highlight colour (hex RRGGBB).");
    gCV_Commands = new Convar("clips_commands", "sm_showclips;sm_showplayerclips;sm_clips", "Command names (semicolon-separated) that toggle clip drawing on/off.");
    Convar.AutoExecConfig();

    gF_DrawRadius = gCV_Radius.FloatValue;
    gF_RefreshTime = gCV_Refresh.FloatValue;
    gI_MaxEdges = gCV_MaxEdges.IntValue;
    gF_WidthMin = gCV_WidthMin.FloatValue;
    gF_WidthMax = gCV_WidthMax.FloatValue;
    gB_SortNearest = gCV_SortNearest.BoolValue;

    LoadColors();

    gC_kShow = new Cookie("clips_show", "Clip drawing on/off", CookieAccess_Protected);
    gC_kIgnoreZ = new Cookie("clips_ignorez", "Draw clips through walls", CookieAccess_Protected);
    gC_kOptimize = new Cookie("clips_optimize", "Cull to view/radius", CookieAccess_Protected);
    gC_kFlags = new Cookie("clips_flags", "Per-type enable bitmask", CookieAccess_Protected);
    gC_kColors = new Cookie("clips_colors", "Per-type colour indices", CookieAccess_Protected);
    gC_kWidths = new Cookie("clips_widths", "Per-type beam widths", CookieAccess_Protected);

    RegConsoleCmd("sm_clipmenu", Cmd_Menu, "Open the clip settings menu.");

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        gA_Edges[t] = new ArrayList(6);
    }

    gA_ExcludeRegions = new ArrayList(6);

    for (int c = 0; c < sizeof(gA_DrawPlan); c++)
    {
        gA_DrawPlan[c] = new ArrayList(8);
    }

    for (int c = 1; c <= MaxClients; c++)
    {
        if (AreClientCookiesCached(c))
        {
            LoadClientSettings(c);
        }
    }
}

public void OnConfigsExecuted()
{
    if (!gB_CommandsRegistered)
    {
        RegToggleCommands();
    }
}

void RegToggleCommands()
{
    char buf[256];
    gCV_Commands.GetString(buf, sizeof(buf));

    char names[16][32];
    int count = ExplodeString(buf, ";", names, sizeof(names), sizeof(names[]));

    for (int i = 0; i < count; i++)
    {
        TrimString(names[i]);

        if (names[i][0] != '\0')
        {
            RegConsoleCmd(names[i], Cmd_Toggle, "Toggle clip drawing on/off.");
        }
    }

    gB_CommandsRegistered = true;
}

public void OnMapStart()
{
    LoadDownloadsConfig();

    gI_BeamModel = PrecacheModel(gS_BeamMaterial, true);
    gI_IgnoreZBeam = PrecacheModel(gS_IgnoreZMaterial, true);

    CacheChatColors();

    ClearMapData();

    if (GetFeatureStatus(FeatureType_Native, "Clips_Parse") != FeatureStatus_Available)
    {
        LogError("clipsparser extension is not loaded; nothing will be drawn.");

        return;
    }

    GetCurrentMap(gS_MapName, sizeof(gS_MapName));

    LoadMapConfig(gS_MapName);

    char bspPath[PLATFORM_MAX_PATH];
    Format(bspPath, sizeof(bspPath), "maps/%s.bsp", gS_MapName);

    if (!Clips_Parse(bspPath))
    {
        LogError("Clips_Parse failed for %s", bspPath);

        return;
    }

    CacheEdges();

    PrintToServer("[clipsparser] %s: BSP v%d, %d edges cached.", gS_MapName, Clips_GetBspVersion(), gI_TotalEdges);
}

void ClearMapData()
{
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        gA_Edges[t].Clear();
        gB_MapTypeEnabled[t] = gB_DefaultOn[t];
        gI_BrushCount[t] = 0;
    }

    gA_ExcludeRegions.Clear();
    gI_CursorType = 0;
    gI_CursorIndex = 0;
    gI_TotalEdges = 0;

    gI_CycleFrame = 0;

    for (int c = 0; c < sizeof(gA_DrawPlan); c++)
    {
        gA_DrawPlan[c].Clear();
        gI_PlanPos[c] = 0;
    }
}


void LoadMapConfig(const char[] map)
{
    char path[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, path, sizeof(path), "configs/clipsparser/%s.cfg", map);

    Clips_ClearFilters();

    gF_DrawRadius = gCV_Radius.FloatValue;
    gF_RefreshTime = gCV_Refresh.FloatValue;
    gI_MaxEdges = gCV_MaxEdges.IntValue;
    gF_WidthMin = gCV_WidthMin.FloatValue;
    gF_WidthMax = gCV_WidthMax.FloatValue;
    gB_SortNearest = gCV_SortNearest.BoolValue;

    if (!FileExists(path))
    {
        ApplyDefaultConfig();

        return;
    }

    KeyValues kv = new KeyValues("clips");

    if (!kv.ImportFromFile(path))
    {
        LogError("Failed to read config %s; using defaults.", path);
        delete kv;

        return;
    }

    gF_DrawRadius = kv.GetFloat("clips_draw_radius", gF_DrawRadius);
    gF_RefreshTime = kv.GetFloat("clips_refresh_time", gF_RefreshTime);
    gI_MaxEdges = kv.GetNum("clips_max_edges", gI_MaxEdges);
    gF_WidthMin = kv.GetFloat("clips_width_min", gF_WidthMin);
    gF_WidthMax = kv.GetFloat("clips_width_max", gF_WidthMax);
    gB_SortNearest = kv.GetNum("clips_sort_nearest", gB_SortNearest ? 1 : 0) != 0;

    Clips_SetShrink(kv.GetFloat("shrink", gCV_DefaultShrink.FloatValue));

    if (kv.JumpToKey("types"))
    {
        for (int t = 0; t < TYPE_COUNT; t++)
        {
            char name[32];
            TypeKey(t, name, sizeof(name));
            gB_MapTypeEnabled[t] = kv.GetNum(name, gB_DefaultOn[t] ? 1 : 0) != 0;
        }

        kv.GoBack();
    }

    ReadRegions(kv, "exclude_regions", gA_ExcludeRegions);
    ReadExtraBoxes(kv);
    ReadBrushBoxes(kv);
    ReadFilters(kv);

    delete kv;
}

// Single out specific world geometry (which has no hammerid) by its extent. Each
// entry gives the wall's centre and size as read from Hammer; the
// box is handed to the extension, which draws the brush whose bounds match it.
void ReadBrushBoxes(KeyValues kv)
{
    if (!kv.JumpToKey("brushes"))
    {
        return;
    }

    if (kv.GotoFirstSubKey())
    {
        do
        {
            float center[3], size[3];
            kv.GetVector("center", center);
            kv.GetVector("size", size);

            float mins[3], maxs[3];
            for (int i = 0; i < 3; i++)
            {
                mins[i] = center[i] - size[i] * 0.5;
                maxs[i] = center[i] + size[i] * 0.5;
            }

            // Optional "faces" (Hammer's "solid with N faces") tightens the match.
            int faces = kv.GetNum("faces", 0);
            Clips_AddBrushBox(mins, maxs, faces);
        }
        while (kv.GotoNextKey());

        kv.GoBack();
    }

    kv.GoBack();
}

void ReadRegions(KeyValues kv, const char[] section, ArrayList into)
{
    if (!kv.JumpToKey(section))
    {
        return;
    }

    if (kv.GotoFirstSubKey())
    {
        do
        {
            float mins[3], maxs[3];
            kv.GetVector("mins", mins);
            kv.GetVector("maxs", maxs);

            float box[6];
            box[0] = mins[0]; box[1] = mins[1]; box[2] = mins[2];
            box[3] = maxs[0]; box[4] = maxs[1]; box[5] = maxs[2];
            into.PushArray(box);
        }
        while (kv.GotoNextKey());

        kv.GoBack();
    }

    kv.GoBack();
}

void ReadExtraBoxes(KeyValues kv)
{
    if (!kv.JumpToKey("extra_boxes"))
    {
        return;
    }

    if (kv.GotoFirstSubKey())
    {
        do
        {
            float mins[3], maxs[3];
            kv.GetVector("mins", mins);
            kv.GetVector("maxs", maxs);
            PushBoxEdges(mins, maxs);
        }
        while (kv.GotoNextKey());

        kv.GoBack();
    }

    kv.GoBack();
}

void ReadFilters(KeyValues kv)
{
    if (kv.JumpToKey("materials"))
    {
        if (kv.GotoFirstSubKey(false))
        {
            do
            {
                char material[PLATFORM_MAX_PATH];
                kv.GetSectionName(material, sizeof(material));

                if (material[0])
                {
                    Clips_AddMaterialFilter(material);
                }
            }
            while (kv.GotoNextKey(false));

            kv.GoBack();
        }

        kv.GoBack();
    }

    if (kv.JumpToKey("hammerids"))
    {
        if (kv.GotoFirstSubKey(false))
        {
            do
            {
                char idText[32];
                kv.GetSectionName(idText, sizeof(idText));

                if (idText[0])
                {
                    Clips_AddHammerId(StringToInt(idText));
                }
            }
            while (kv.GotoNextKey(false));

            kv.GoBack();
        }

        kv.GoBack();
    }
}

void ApplyDefaultConfig()
{
    Clips_SetShrink(gCV_DefaultShrink.FloatValue);

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        gB_MapTypeEnabled[t] = gB_DefaultOn[t];
    }

    Clips_AddMaterialFilter("tools/toolsinvisible");
}

void CacheEdges()
{
    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (!gB_MapTypeEnabled[t])
        {
            continue;
        }

        int count = Clips_GetEdgeCount(view_as<ClipType>(t));

        for (int i = 0; i < count; i++)
        {
            float a[3], b[3];

            if (!Clips_GetEdge(view_as<ClipType>(t), i, a, b))
            {
                continue;
            }

            if (EdgeExcluded(a, b))
            {
                continue;
            }

            float edge[6];
            edge[0] = a[0]; edge[1] = a[1]; edge[2] = a[2];
            edge[3] = b[0]; edge[4] = b[1]; edge[5] = b[2];
            gA_Edges[t].PushArray(edge);
        }
    }

    gI_TotalEdges = 0;

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        gI_TotalEdges += gA_Edges[t].Length;
        gI_BrushCount[t] = gB_MapTypeEnabled[t] ? Clips_GetBrushCount(view_as<ClipType>(t)) : 0;
    }
}

bool EdgeExcluded(const float a[3], const float b[3])
{
    if (gA_ExcludeRegions.Length == 0)
    {
        return false;
    }

    float mid[3];

    for (int k = 0; k < 3; k++)
    {
        mid[k] = (a[k] + b[k]) * 0.5;
    }

    for (int r = 0; r < gA_ExcludeRegions.Length; r++)
    {
        float box[6];
        gA_ExcludeRegions.GetArray(r, box);

        if (mid[0] >= box[0] && mid[0] <= box[3] &&
            mid[1] >= box[1] && mid[1] <= box[4] &&
            mid[2] >= box[2] && mid[2] <= box[5])
        {
            return true;
        }
    }

    return false;
}

void PushBoxEdges(const float mins[3], const float maxs[3])
{
    float corner[8][3];

    for (int i = 0; i < 8; i++)
    {
        corner[i][0] = (i & 1) ? maxs[0] : mins[0];
        corner[i][1] = (i & 2) ? maxs[1] : mins[1];
        corner[i][2] = (i & 4) ? maxs[2] : mins[2];
    }

    int edges[12][2] = 
    {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };

    int custom = view_as<int>(ClipType_Custom);

    for (int i = 0; i < 12; i++)
    {
        float edge[6];
        edge[0] = corner[edges[i][0]][0]; edge[1] = corner[edges[i][0]][1]; edge[2] = corner[edges[i][0]][2];
        edge[3] = corner[edges[i][1]][0]; edge[4] = corner[edges[i][1]][1]; edge[5] = corner[edges[i][1]][2];
        gA_Edges[custom].PushArray(edge);
    }

    gB_MapTypeEnabled[custom] = true;
}

public void OnGameFrame()
{
    if (gI_TotalEdges == 0)
    {
        return;
    }

    int viewers[MAXPLAYERS + 1];
    static float eyes[MAXPLAYERS + 1][3];
    static float viewDir[MAXPLAYERS + 1][3];
    int viewerCount = 0;

    for (int c = 1; c <= MaxClients; c++)
    {
        if (!gB_Show[c] || !IsClientInGame(c) || IsFakeClient(c))
        {
            continue;
        }

        float eye[3], angles[3], dir[3];
        GetClientEyePosition(c, eye);
        GetClientEyeAngles(c, angles);
        GetAngleVectors(angles, dir, NULL_VECTOR, NULL_VECTOR);

        viewers[viewerCount] = c;
        eyes[viewerCount][0] = eye[0]; eyes[viewerCount][1] = eye[1]; eyes[viewerCount][2] = eye[2];
        viewDir[viewerCount][0] = dir[0]; viewDir[viewerCount][1] = dir[1]; viewDir[viewerCount][2] = dir[2];
        viewerCount++;
    }

    if (viewerCount == 0)
    {
        return;
    }

    float radiusSq = gF_DrawRadius * gF_DrawRadius;
    float refresh = gF_RefreshTime;
    float life = refresh + 0.2;

    int framesPerCycle = RoundToCeil(refresh / GetTickInterval());

    if (framesPerCycle < 1)
    {
        framesPerCycle = 1;
    }

    int maxEdges = gI_MaxEdges;
    bool sortNearest = gB_SortNearest && maxEdges > 0;

    if (gI_CycleFrame <= 0)
    {
        gI_CycleFrame = framesPerCycle;

        for (int c = 1; c <= MaxClients; c++)
        {
            gI_DrawnThisCycle[c] = 0;
        }

        if (sortNearest)
        {
            for (int v = 0; v < viewerCount; v++)
            {
                BuildDrawPlan(viewers[v], eyes[v], viewDir[v], radiusSq, maxEdges, gB_Optimize[viewers[v]]);
            }
        }
    }

    gI_CycleFrame--;

    if (sortNearest)
    {
        DrawFromPlans(viewers, viewerCount, life, framesPerCycle);

        return;
    }

    int budget = (gI_TotalEdges + framesPerCycle - 1) / framesPerCycle;

    for (int processed = 0; processed < budget; processed++)
    {
        int type;
        float a[3], b[3];

        if (!NextEdge(type, a, b))
        {
            break;
        }

        float mid[3];

        for (int k = 0; k < 3; k++)
        {
            mid[k] = (a[k] + b[k]) * 0.5;
        }

        for (int v = 0; v < viewerCount; v++)
        {
            int client = viewers[v];

            if (!gB_TypeOn[client][type])
            {
                continue;
            }

            float d[3];
            d[0] = mid[0] - eyes[v][0];
            d[1] = mid[1] - eyes[v][1];
            d[2] = mid[2] - eyes[v][2];

            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > radiusSq)
            {
                continue;
            }

            if (gB_Optimize[client] && d[0] * viewDir[v][0] + d[1] * viewDir[v][1] + d[2] * viewDir[v][2] < 0.0)
            {
                continue;
            }

            if (maxEdges > 0 && gI_DrawnThisCycle[client] >= maxEdges)
            {
                continue;
            }

            float w = gF_Width[client][type];
            int beam = gB_IgnoreZ[client] ? gI_IgnoreZBeam : gI_BeamModel;
            TE_SetupBeamPoints(a, b, beam, 0, 0, 0, life, w, w, 0, 0.0, gI_ColorTable[gI_ColorIdx[client][type]], 0);
            TE_SendToClient(client);
            gI_DrawnThisCycle[client]++;
        }
    }
}

void BuildDrawPlan(int client, const float eye[3], const float dir[3], float radiusSq, int maxEdges, bool optimize)
{
    ArrayList plan = gA_DrawPlan[client];
    plan.Clear();

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (!gB_TypeOn[client][t])
        {
            continue;
        }

        ArrayList list = gA_Edges[t];
        int n = list.Length;

        for (int i = 0; i < n; i++)
        {
            float edge[6];
            list.GetArray(i, edge);

            float d[3];
            d[0] = (edge[0] + edge[3]) * 0.5 - eye[0];
            d[1] = (edge[1] + edge[4]) * 0.5 - eye[1];
            d[2] = (edge[2] + edge[5]) * 0.5 - eye[2];
            float distSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];

            if (distSq > radiusSq)
            {
                continue;
            }

            if (optimize && d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2] < 0.0)
            {
                continue;
            }

            float entry[8];
            entry[0] = distSq;
            entry[1] = edge[0]; entry[2] = edge[1]; entry[3] = edge[2];
            entry[4] = edge[3]; entry[5] = edge[4]; entry[6] = edge[5];
            entry[7] = float(t);
            plan.PushArray(entry);
        }
    }

    if (plan.Length > maxEdges)
    {
        plan.SortCustom(SortPlanByDistance);
        plan.Resize(maxEdges);
    }

    gI_PlanPos[client] = 0;
}

int SortPlanByDistance(int index1, int index2, Handle array, Handle hndl)
{
    ArrayList plan = view_as<ArrayList>(array);
    float d1 = plan.Get(index1, 0);
    float d2 = plan.Get(index2, 0);

    if (d1 < d2)
    {
        return -1;
    }

    if (d1 > d2)
    {
        return 1;
    }

    return 0;
}

void DrawFromPlans(const int[] viewers, int viewerCount, float life, int framesPerCycle)
{
    for (int v = 0; v < viewerCount; v++)
    {
        int client = viewers[v];
        ArrayList plan = gA_DrawPlan[client];
        int planSize = plan.Length;
        if (planSize == 0)
        {
            continue;
        }

        int planBudget = (planSize + framesPerCycle - 1) / framesPerCycle;
        int start = gI_PlanPos[client];
        int end = start + planBudget;

        for (int i = start; i < end && i < planSize; i++)
        {
            float entry[8];
            plan.GetArray(i, entry);

            int type = RoundToNearest(entry[7]);
            float a[3], b[3];
            a[0] = entry[1]; a[1] = entry[2]; a[2] = entry[3];
            b[0] = entry[4]; b[1] = entry[5]; b[2] = entry[6];

            float w = gF_Width[client][type];
            int beam = gB_IgnoreZ[client] ? gI_IgnoreZBeam : gI_BeamModel;
            TE_SetupBeamPoints(a, b, beam, 0, 0, 0, life, w, w, 0, 0.0, gI_ColorTable[gI_ColorIdx[client][type]], 0);
            TE_SendToClient(client);
        }

        gI_PlanPos[client] = end;
    }
}

bool NextEdge(int &type, float a[3], float b[3])
{
    for (int guard = 0; guard < TYPE_COUNT; guard++)
    {
        ArrayList list = gA_Edges[gI_CursorType];

        if (gI_CursorIndex >= list.Length)
        {
            gI_CursorIndex = 0;
            gI_CursorType = (gI_CursorType + 1) % TYPE_COUNT;

            continue;
        }

        float edge[6];
        list.GetArray(gI_CursorIndex, edge);
        a[0] = edge[0]; a[1] = edge[1]; a[2] = edge[2];
        b[0] = edge[3]; b[1] = edge[4]; b[2] = edge[5];
        type = gI_CursorType;
        gI_CursorIndex++;

        return true;
    }

    return false;
}

public void OnClientCookiesCached(int client)
{
    LoadClientSettings(client);
}

public void OnClientDisconnect(int client)
{
    gB_Show[client] = false;
}

void LoadClientSettings(int client)
{
    char buf[256];

    gC_kShow.Get(client, buf, sizeof(buf));
    gB_Show[client] = (buf[0] != '\0') ? (StringToInt(buf) != 0) : false;

    gC_kIgnoreZ.Get(client, buf, sizeof(buf));
    gB_IgnoreZ[client] = (buf[0] != '\0') ? (StringToInt(buf) != 0) : false;

    gC_kOptimize.Get(client, buf, sizeof(buf));
    gB_Optimize[client] = (buf[0] != '\0') ? (StringToInt(buf) != 0) : false;

    gC_kFlags.Get(client, buf, sizeof(buf));
    int flags = (buf[0] != '\0') ? StringToInt(buf) : DefaultFlags();

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        gB_TypeOn[client][t] = (flags & (1 << t)) != 0;
    }

    gC_kColors.Get(client, buf, sizeof(buf));

    if (buf[0] != '\0')
    {
        char parts[TYPE_COUNT][8];
        int n = ExplodeString(buf, ",", parts, TYPE_COUNT, sizeof(parts[]));

        for (int t = 0; t < TYPE_COUNT; t++)
        {
            gI_ColorIdx[client][t] = (t < n) ? ClampColor(StringToInt(parts[t])) : gI_DefaultColor[t];
        }
    }
    else
    {
        for (int t = 0; t < TYPE_COUNT; t++)
        {
            gI_ColorIdx[client][t] = gI_DefaultColor[t];
        }
    }

    gC_kWidths.Get(client, buf, sizeof(buf));

    if (buf[0] != '\0')
    {
        char parts[TYPE_COUNT][8];
        int n = ExplodeString(buf, ",", parts, TYPE_COUNT, sizeof(parts[]));

        for (int t = 0; t < TYPE_COUNT; t++)
        {
            gF_Width[client][t] = (t < n) ? ClampWidth(StringToFloat(parts[t])) : DEFAULT_WIDTH;
        }
    }
    else
    {
        for (int t = 0; t < TYPE_COUNT; t++)
        {
            gF_Width[client][t] = DEFAULT_WIDTH;
        }
    }
}

int DefaultFlags()
{
    int flags = 0;

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (gB_DefaultOn[t])
        {
            flags |= (1 << t);
        }
    }

    return flags;
}

void ResetSettings(int client)
{
    gB_IgnoreZ[client] = false;
    gB_Optimize[client] = false;

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        gB_TypeOn[client][t] = gB_DefaultOn[t];
        gI_ColorIdx[client][t] = gI_DefaultColor[t];
        gF_Width[client][t] = DEFAULT_WIDTH;
    }

    SaveBool(gC_kIgnoreZ, client, gB_IgnoreZ[client]);
    SaveBool(gC_kOptimize, client, gB_Optimize[client]);
    SaveFlags(client);
    SaveColors(client);
    SaveWidths(client);
}

void SaveShow(int client)
{
    SaveBool(gC_kShow, client, gB_Show[client]);
}

void SaveBool(Cookie cookie, int client, bool value)
{
    char buf[8];
    IntToString(value ? 1 : 0, buf, sizeof(buf));
    cookie.Set(client, buf);
}

void SaveFlags(int client)
{
    int flags = 0;

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (gB_TypeOn[client][t])
        {
            flags |= (1 << t);
        }
    }

    char buf[16];
    IntToString(flags, buf, sizeof(buf));
    gC_kFlags.Set(client, buf);
}

void SaveColors(int client)
{
    char buf[128], piece[8];

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        IntToString(gI_ColorIdx[client][t], piece, sizeof(piece));

        if (t > 0)
        {
            StrCat(buf, sizeof(buf), ",");
        }

        StrCat(buf, sizeof(buf), piece);
    }

    gC_kColors.Set(client, buf);
}

void SaveWidths(int client)
{
    char buf[128], piece[8];

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        Format(piece, sizeof(piece), "%.1f", gF_Width[client][t]);

        if (t > 0)
        {
            StrCat(buf, sizeof(buf), ",");
        }

        StrCat(buf, sizeof(buf), piece);
    }

    gC_kWidths.Set(client, buf);
}

int ClampColor(int idx)
{
    if (idx < 0)
    {
        return 0;
    }

    if (idx >= gI_ColorCount)
    {
        return gI_ColorCount > 0 ? gI_ColorCount - 1 : 0;
    }

    return idx;
}

float ClampWidth(float w)
{
    float lo = gF_WidthMin;
    float hi = gF_WidthMax;

    if (lo < 0.0)
    {
        lo = 0.0;
    }

    if (hi < lo)
    {
        hi = lo;
    }

    if (w < lo)
    {
        return lo;
    }

    if (w > hi)
    {
        return hi;
    }

    return w;
}

public Action Cmd_Toggle(int client, int args)
{
    if (client < 1)
    {
        return Plugin_Handled;
    }

    gB_Show[client] = !gB_Show[client];
    SaveShow(client);

    char msg[160];
    FormatEx(msg, sizeof(msg), "%sClips %s(!clipmenu) %sare now %s%s",
        gS_ColText, gS_ColVal, gS_ColText,
        gB_Show[client] ? CHAT_COLOR_ON : CHAT_COLOR_OFF,
        gB_Show[client] ? "enabled" : "disabled");
    ChatToOne(client, msg);

    return Plugin_Handled;
}

public Action Cmd_Menu(int client, int args)
{
    if (client > 0)
    {
        ShowClipsMenu(client);
    }

    return Plugin_Handled;
}

void ShowClipsMenu(int client, int firstItem = 0)
{
    Menu menu = new Menu(ClipsMenu_Handler);

    char maxEdgesStr[16];
    if (gI_MaxEdges > 0)
    {
        IntToString(gI_MaxEdges, maxEdgesStr, sizeof(maxEdgesStr));
    }
    else
    {
        strcopy(maxEdgesStr, sizeof(maxEdgesStr), "unlimited");
    }

    char title[512];
    Format(title, sizeof(title),
        "Clips menu - %s\n \nRadius: %d | Refresh: %.1fs\nMax edges: %s | Sort nearest: %s\n \nplayer %d | monster %d | combined %d\nnodraw %d | invisible %d | buttons %d\nladders %d | other %d | custom %d\n \n",
        gS_MapName, RoundToNearest(gF_DrawRadius), gF_RefreshTime,
        maxEdgesStr, gB_SortNearest ? "on" : "off",
        gI_BrushCount[0], gI_BrushCount[1], gI_BrushCount[2],
        gI_BrushCount[3], gI_BrushCount[4], gI_BrushCount[5],
        gI_BrushCount[6], gI_BrushCount[7], gI_BrushCount[8]);
    menu.SetTitle(title);

    char item[64];
    Format(item, sizeof(item), "Enabled: %s", gB_Show[client] ? "[+]" : "[-]");
    menu.AddItem("toggle", item);
    Format(item, sizeof(item), "Show through walls: %s", gB_IgnoreZ[client] ? "[+]" : "[-]");
    menu.AddItem("ignorez", item);
    Format(item, sizeof(item), "Optimize (only what you look at): %s", gB_Optimize[client] ? "[+]" : "[-]");
    menu.AddItem("optimize", item);
    menu.AddItem("colors", "Colour & width settings");
    menu.AddItem("reset", "Reset to default\n ");

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        char info[8];
        IntToString(t, info, sizeof(info));
        Format(item, sizeof(item), "%s: %s", gS_TypeLabel[t], gB_TypeOn[client][t] ? "[+]" : "[-]");
        menu.AddItem(info, item, gB_MapTypeEnabled[t] ? ITEMDRAW_DEFAULT : ITEMDRAW_DISABLED);
    }

    menu.ExitButton = true;
    menu.DisplayAt(client, firstItem, MENU_TIME_FOREVER);
}

public int ClipsMenu_Handler(Menu menu, MenuAction action, int param1, int param2)
{
    if (action == MenuAction_End)
    {
        delete menu;

        return 0;
    }

    if (action != MenuAction_Select)
    {
        return 0;
    }

    char info[16];
    menu.GetItem(param2, info, sizeof(info));
    int pos = PageOf(param2);

    if (StrEqual(info, "toggle"))
    {
        gB_Show[param1] = !gB_Show[param1];
        SaveShow(param1);
        ShowClipsMenu(param1, pos);
    }
    else if (StrEqual(info, "ignorez"))
    {
        gB_IgnoreZ[param1] = !gB_IgnoreZ[param1];
        SaveBool(gC_kIgnoreZ, param1, gB_IgnoreZ[param1]);
        ShowClipsMenu(param1, pos);
    }
    else if (StrEqual(info, "optimize"))
    {
        gB_Optimize[param1] = !gB_Optimize[param1];
        SaveBool(gC_kOptimize, param1, gB_Optimize[param1]);
        ShowClipsMenu(param1, pos);
    }
    else if (StrEqual(info, "colors"))
    {
        gI_MainPos[param1] = pos;
        ShowTypesMenu(param1);
    }
    else if (StrEqual(info, "reset"))
    {
        gI_MainPos[param1] = pos;
        ShowResetConfirmMenu(param1);
    }
    else
    {
        int type = StringToInt(info);
        gB_TypeOn[param1][type] = !gB_TypeOn[param1][type];
        SaveFlags(param1);
        ShowClipsMenu(param1, pos);
    }

    return 0;
}

void ShowResetConfirmMenu(int client)
{
    Menu menu = new Menu(ResetConfirmMenu_Handler);
    menu.SetTitle("Reset all clip settings to default?\n \n");
    menu.AddItem("yes", "Yes");
    menu.AddItem("no", "No");

    menu.ExitButton = true;
    menu.Display(client, MENU_TIME_FOREVER);
}

public int ResetConfirmMenu_Handler(Menu menu, MenuAction action, int param1, int param2)
{
    if (action == MenuAction_End)
    {
        delete menu;

        return 0;
    }

    if (action != MenuAction_Select)
    {
        return 0;
    }

    char info[8];
    menu.GetItem(param2, info, sizeof(info));

    if (StrEqual(info, "yes"))
    {
        ResetSettings(param1);

        char msg[160];
        FormatEx(msg, sizeof(msg), "%sClips %s(!clipmenu) %ssettings reset to default", gS_ColText, gS_ColVal, gS_ColText);
        ChatToOne(param1, msg);
    }

    ShowClipsMenu(param1, gI_MainPos[param1]);

    return 0;
}

void ShowTypesMenu(int client, int firstItem = 0)
{
    Menu menu = new Menu(TypesMenu_Handler);
    menu.SetTitle("Clip types\n \n");

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        char info[8], item[96];
        IntToString(t, info, sizeof(info));
        Format(item, sizeof(item), "%s: %s [%s, w%.1f]%s",
            gS_TypeLabel[t],
            gB_TypeOn[client][t] ? "ON" : "OFF",
            gS_ColorName[gI_ColorIdx[client][t]],
            gF_Width[client][t],
            gB_MapTypeEnabled[t] ? "" : " (off on this map)");
        menu.AddItem(info, item);
    }

    menu.ExitBackButton = true;
    menu.DisplayAt(client, firstItem, MENU_TIME_FOREVER);
}

public int TypesMenu_Handler(Menu menu, MenuAction action, int param1, int param2)
{
    if (action == MenuAction_End)
    {
        delete menu;

        return 0;
    }

    if (action == MenuAction_Cancel)
    {
        if (param2 == MenuCancel_ExitBack)
        {
            ShowClipsMenu(param1, gI_MainPos[param1]);
        }

        return 0;
    }

    if (action != MenuAction_Select)
    {
        return 0;
    }

    char info[8];
    menu.GetItem(param2, info, sizeof(info));
    gI_MenuType[param1] = StringToInt(info);
    gI_TypesPos[param1] = PageOf(param2);
    ShowTypeMenu(param1);

    return 0;
}

void ShowTypeMenu(int client, int firstItem = 0)
{
    int t = gI_MenuType[client];

    Menu menu = new Menu(TypeMenu_Handler);
    char title[64];
    Format(title, sizeof(title), "%s\n \n", gS_TypeLabel[t]);
    menu.SetTitle(title);

    char item[64];
    Format(item, sizeof(item), "Enabled: %s", gB_TypeOn[client][t] ? "[+]" : "[-]");
    menu.AddItem("toggle", item);
    Format(item, sizeof(item), "Colour: %s", gS_ColorName[gI_ColorIdx[client][t]]);
    menu.AddItem("color", item);
    Format(item, sizeof(item), "Width: %.1f", gF_Width[client][t]);
    menu.AddItem("width", item);

    menu.ExitBackButton = true;
    menu.DisplayAt(client, firstItem, MENU_TIME_FOREVER);
}

public int TypeMenu_Handler(Menu menu, MenuAction action, int param1, int param2)
{
    if (action == MenuAction_End)
    {
        delete menu;

        return 0;
    }

    if (action == MenuAction_Cancel)
    {
        if (param2 == MenuCancel_ExitBack)
        {
            ShowTypesMenu(param1, gI_TypesPos[param1]);
        }

        return 0;
    }

    if (action != MenuAction_Select)
    {
        return 0;
    }

    char info[16];
    menu.GetItem(param2, info, sizeof(info));
    int pos = PageOf(param2);

    if (StrEqual(info, "toggle"))
    {
        int t = gI_MenuType[param1];
        gB_TypeOn[param1][t] = !gB_TypeOn[param1][t];
        SaveFlags(param1);
        ShowTypeMenu(param1, pos);
    }
    else if (StrEqual(info, "color"))
    {
        ShowColorMenu(param1);
    }
    else if (StrEqual(info, "width"))
    {
        ShowWidthMenu(param1);
    }

    return 0;
}

void ShowColorMenu(int client, int firstItem = 0)
{
    int t = gI_MenuType[client];

    Menu menu = new Menu(ColorMenu_Handler);
    char title[64];
    Format(title, sizeof(title), "%s - colour\n \n", gS_TypeLabel[t]);
    menu.SetTitle(title);

    for (int c = 0; c < gI_ColorCount; c++)
    {
        char info[8];
        IntToString(c, info, sizeof(info));
        int style = (c == gI_ColorIdx[client][t]) ? ITEMDRAW_DISABLED : ITEMDRAW_DEFAULT;
        menu.AddItem(info, gS_ColorName[c], style);
    }

    menu.ExitBackButton = true;
    menu.DisplayAt(client, firstItem, MENU_TIME_FOREVER);
}

public int ColorMenu_Handler(Menu menu, MenuAction action, int param1, int param2)
{
    if (action == MenuAction_End)
    {
        delete menu;

        return 0;
    }

    if (action == MenuAction_Cancel)
    {
        if (param2 == MenuCancel_ExitBack)
        {
            ShowTypeMenu(param1);
        }

        return 0;
    }

    if (action != MenuAction_Select)
    {
        return 0;
    }

    char info[8];
    menu.GetItem(param2, info, sizeof(info));
    gI_ColorIdx[param1][gI_MenuType[param1]] = ClampColor(StringToInt(info));
    SaveColors(param1);
    ShowColorMenu(param1, PageOf(param2));

    return 0;
}

void ShowWidthMenu(int client, int firstItem = 0)
{
    int t = gI_MenuType[client];

    Menu menu = new Menu(WidthMenu_Handler);
    char title[96];
    Format(title, sizeof(title), "%s - width\n \nCurrent Width: %.1f\n \n", gS_TypeLabel[t], gF_Width[client][t]);
    menu.SetTitle(title);

    menu.AddItem("0.1", "+ 0.1");
    menu.AddItem("-0.1", "- 0.1");
    menu.AddItem("1.0", "+ 1.0");
    menu.AddItem("-1.0", "- 1.0");

    menu.ExitBackButton = true;
    menu.DisplayAt(client, firstItem, MENU_TIME_FOREVER);
}

public int WidthMenu_Handler(Menu menu, MenuAction action, int param1, int param2)
{
    if (action == MenuAction_End)
    {
        delete menu;

        return 0;
    }

    if (action == MenuAction_Cancel)
    {
        if (param2 == MenuCancel_ExitBack)
        {
            ShowTypeMenu(param1);
        }

        return 0;
    }

    if (action != MenuAction_Select)
    {
        return 0;
    }

    char info[8];
    menu.GetItem(param2, info, sizeof(info));
    int t = gI_MenuType[param1];
    gF_Width[param1][t] = ClampWidth(gF_Width[param1][t] + StringToFloat(info));
    SaveWidths(param1);
    ShowWidthMenu(param1, PageOf(param2));

    return 0;
}

void CacheChatColors()
{
    gCV_ColText.GetString(gS_ColText, sizeof(gS_ColText));
    gCV_ColVal.GetString(gS_ColVal, sizeof(gS_ColVal));

    Format(gS_ColText, sizeof(gS_ColText), "\x07%s", gS_ColText);
    Format(gS_ColVal, sizeof(gS_ColVal), "\x07%s", gS_ColVal);
}

void ChatToOne(int client, const char[] msg)
{
    if (client <= 0 || !IsClientInGame(client))
    {
        return;
    }

    Handle h = StartMessageOne("SayText2", client, USERMSG_RELIABLE | USERMSG_BLOCKHOOKS);

    if (GetFeatureStatus(FeatureType_Native, "GetUserMessageType") == FeatureStatus_Available && GetUserMessageType() == UM_Protobuf)
    {
        Protobuf pb = UserMessageToProtobuf(h);
        pb.SetInt("ent_idx", client);
        pb.SetBool("chat", true);
        pb.SetString("msg_name", msg);

        for (int i = 0; i < 4; i++)
        {
            pb.AddString("params", "");
        }
    }
    else
    {
        BfWrite bf = UserMessageToBfWrite(h);
        bf.WriteByte(client);
        bf.WriteByte(true);
        bf.WriteString(msg);
    }

    EndMessage();
}

// Beam materials and the files clients must download, from
// configs/clipsparser/downloads.cfg. Falls back to the built-in defaults when the config is missing.
void LoadDownloadsConfig()
{
    strcopy(gS_BeamMaterial, sizeof(gS_BeamMaterial), DEFAULT_BEAM_MATERIAL);
    strcopy(gS_IgnoreZMaterial, sizeof(gS_IgnoreZMaterial), DEFAULT_IGNOREZ_MATERIAL);

    char path[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, path, sizeof(path), "configs/clipsparser/downloads.cfg");

    KeyValues kv = new KeyValues("downloads");

    if (!kv.ImportFromFile(path))
    {
        delete kv;

        AddFileToDownloadsTable(DEFAULT_BEAM_MATERIAL);
        AddFileToDownloadsTable(DEFAULT_IGNOREZ_MATERIAL);
        AddFileToDownloadsTable(DEFAULT_BEAM_TEXTURE);
        return;
    }

    kv.GetString("beam", gS_BeamMaterial, sizeof(gS_BeamMaterial), gS_BeamMaterial);
    kv.GetString("ignorez_beam", gS_IgnoreZMaterial, sizeof(gS_IgnoreZMaterial), gS_IgnoreZMaterial);

    char downloads[PLATFORM_MAX_PATH * 8];
    kv.GetString("downloads", downloads, sizeof(downloads));

    char files[16][PLATFORM_MAX_PATH];
    int count = ExplodeString(downloads, ";", files, sizeof(files), sizeof(files[]));

    for (int i = 0; i < count; i++)
    {
        TrimString(files[i]);

        if (files[i][0] != '\0')
        {
            AddFileToDownloadsTable(files[i]);
        }
    }

    delete kv;
}

void AddBuiltinColor(const char[] name, int r, int g, int b)
{
    if (gI_ColorCount >= MAX_COLORS)
    {
        return;
    }

    strcopy(gS_ColorName[gI_ColorCount], 64, name);
    gI_ColorTable[gI_ColorCount][0] = r;
    gI_ColorTable[gI_ColorCount][1] = g;
    gI_ColorTable[gI_ColorCount][2] = b;
    gI_ColorTable[gI_ColorCount][3] = 255;
    gI_ColorCount++;
}

void LoadBuiltinColors()
{
    gI_ColorCount = 0;
    AddBuiltinColor("White", 255, 255, 255);
    AddBuiltinColor("Black", 0, 0, 0);
    AddBuiltinColor("Blue", 0, 0, 255);
    AddBuiltinColor("Light Blue", 173, 216, 230);
    AddBuiltinColor("Brown", 139, 69, 19);
    AddBuiltinColor("Cyan", 0, 255, 255);
    AddBuiltinColor("Green", 0, 255, 0);
    AddBuiltinColor("Dark Green", 0, 100, 0);
    AddBuiltinColor("Red", 255, 0, 0);
    AddBuiltinColor("Orange", 255, 165, 0);
    AddBuiltinColor("Yellow", 255, 255, 0);
    AddBuiltinColor("Pink", 255, 192, 203);
    AddBuiltinColor("Light Pink", 255, 182, 193);
    AddBuiltinColor("Purple", 128, 0, 128);
}

void WriteDefaultColors(const char[] path)
{
    char dir[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, dir, sizeof(dir), "configs/clipsparser");

    if (!DirExists(dir))
    {
        CreateDirectory(dir, FPERM_U_READ | FPERM_U_WRITE | FPERM_U_EXEC | FPERM_G_READ | FPERM_G_EXEC | FPERM_O_READ | FPERM_O_EXEC);
    }

    LoadBuiltinColors();

    KeyValues kv = new KeyValues("colors");

    for (int c = 0; c < gI_ColorCount; c++)
    {
        char val[32];
        Format(val, sizeof(val), "%d %d %d %d",
            gI_ColorTable[c][0], gI_ColorTable[c][1], gI_ColorTable[c][2], gI_ColorTable[c][3]);
        kv.SetString(gS_ColorName[c], val);
    }

    kv.Rewind();
    kv.ExportToFile(path);
    delete kv;
}

// Each entry is "Name" "r g b [a]"; alpha defaults to 255.
void LoadColors()
{
    char path[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, path, sizeof(path), "configs/clipsparser/colors.cfg");

    if (!FileExists(path))
    {
        WriteDefaultColors(path);
    }

    gI_ColorCount = 0;
    KeyValues kv = new KeyValues("colors");

    if (kv.ImportFromFile(path) && kv.GotoFirstSubKey(false))
    {
        do
        {
            if (gI_ColorCount >= MAX_COLORS)
            {
                break;
            }

            char name[64], val[64];
            kv.GetSectionName(name, sizeof(name));
            kv.GetString(NULL_STRING, val, sizeof(val));

            char parts[4][8];
            int n = ExplodeString(val, " ", parts, 4, sizeof(parts[]));

            if (n < 3 || name[0] == '\0')
            {
                continue;
            }

            strcopy(gS_ColorName[gI_ColorCount], 64, name);
            gI_ColorTable[gI_ColorCount][0] = StringToInt(parts[0]);
            gI_ColorTable[gI_ColorCount][1] = StringToInt(parts[1]);
            gI_ColorTable[gI_ColorCount][2] = StringToInt(parts[2]);
            gI_ColorTable[gI_ColorCount][3] = (n >= 4) ? StringToInt(parts[3]) : 255;
            gI_ColorCount++;
        }
        while (kv.GotoNextKey(false));
    }

    delete kv;

    if (gI_ColorCount == 0)
    {
        LoadBuiltinColors();
    }
}

void TypeKey(int type, char[] buffer, int maxlen)
{
    switch (view_as<ClipType>(type))
    {
        case ClipType_PlayerClip:   strcopy(buffer, maxlen, "clip_player");
        case ClipType_MonsterClip:  strcopy(buffer, maxlen, "clip_monster");
        case ClipType_BothClip:     strcopy(buffer, maxlen, "clip_both");
        case ClipType_WorldNoDraw:  strcopy(buffer, maxlen, "world_nodraw");
        case ClipType_EntInvisible: strcopy(buffer, maxlen, "ent_invisible");
        case ClipType_EntButton:    strcopy(buffer, maxlen, "ent_button");
        case ClipType_EntLadder:    strcopy(buffer, maxlen, "ent_ladder");
        case ClipType_EntOther:     strcopy(buffer, maxlen, "ent_other");
        case ClipType_Custom:       strcopy(buffer, maxlen, "custom");
        default:                    strcopy(buffer, maxlen, "unknown");
    }
}
