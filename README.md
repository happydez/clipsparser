# clipsparser

Draws a map's invisible geometry (clips, nodraw brushes, ladders,
buttons, invisible func_brush/func_wall) as coloured beams, so players
can see exactly what they can collide with.

## Install

Build the extension. Copy everything under `cstrike/` onto the server's `cstrike/` folder.
Make sure the materials are reachable over fastdl. Compile the plugin.

## Per-map config

No config is written automatically. A map with no config file uses built-in
defaults. To customise a map, create `addons/sourcemod/configs/clipsparser/<map>.cfg` by hand.
When `<map>.cfg` present it fully replaces the defaults.

```
"clips"
{
  // overrides cvars 
  "clips_draw_radius"  "2048"
  "clips_refresh_time" "1.0"
  "clips_max_edges"    "512"
  "clips_width_min"    "0.1"
  "clips_width_max"    "10.0"
  "clips_sort_nearest" "1"

  // pull every brush inward by this many units so edges
  // sit inside the shape instead of flush against walls/
  // skybox (avoids z-fighting). A brush thinner than
  // 2*shrink is drawn un-shrunk instead of vanishing.
  "shrink" "1"

  // 1: only treat a texture as invisible if its name contains "tools"
  // (alongside nodraw/invisible), so e.g. "akno/aknodraw_01" is not mistaken for a
  // clip.
  // 0: accept any nodraw/invisible substring (bad? behaviour).
  // Overrides the clips_strict_parse cvar for this map.
  "strict_parse" "1"

  "types"   // which clip types to cache on this map
  {
    "clip_player"     "1"
    "clip_monster"    "1"
    "clip_both"       "1"
    "world_nodraw"    "1"
    "ent_invisible"   "1"
    "ent_button"      "1"
    "ent_ladder"      "1"
    "ent_other"       "1"
    "custom"          "1"
  }

  // Brushes are auto-detected as invisible via the compiler nodraw flag or a tool
  // texture (name contains "tools" plus "nodraw"/"invisible"). A texture that looks
  // invisible by name but lacks "tools" (e.g. "akno/aknodraw_01") is NOT matched and
  // is logged to the server console; if it really is a clip, list it here.
  "materials"   // also draw brushes made entirely of these textures
  {
    "tools/toolsinvisible" "1"
  }

  "hammerids"   // also draw these brush entities by hammer id
  {
    "1234567" "1"
  }

  // Draw brush entities by classname (shown as the Custom type). An entry is
  // either "classname" "1" (every entity of that class) or "classname" { ... }
  // listing entity keyvalues that must ALL match. Key/value are matched case-
  // insensitively against the keyvalues that survived compilation, so look them up
  // in the compiled map (e.g. a Stripper dump), NOT the Hammer FGD labels; the key
  // name differs per entity (func_lod uses "Solid", func_brush uses "Solidity").
  "classnames"
  {
    // func_lod: "Solid" is 0 (Solid) or 1 (Not Solid). Non-solid LOD brushes have
    // no collision, so usually you only want the solid ones.
    "func_lod"
    {
      "Solid" "0"
    }

    // func_brush: "Solidity" is 0 (Toggle), 1 (Never Solid) or 2 (Always Solid).
    "func_brush"
    {
      "Solidity" "2"
    }

    // No conditions; draw every entity of this class.
    "func_wall" "1"
  }

  "exclude_hammerids"   // never draw these brush entities, even if matched above
  {
    "7654321" "1"
  }

  "exclude_regions"   // do NOT draw clips whose centre is inside this box
  {
    "1" { "mins" "-512 -512 0"  "maxs" "512 512 256" }
  }

  "extra_boxes"   // draw a wireframe box here (shown as the Custom type)
  {
    "1" { "mins" "128 256 0"  "maxs" "256 512 128" }
  }

  // draw one specific world brush by its extent (Custom)
  // read "center", "size" (and the face count) from
  // Hammer's status bar; "faces" is optional (0 = any)
  "brushes"
  {      
    "1" { "center" "-1488 -10416 -816"  "size" "592 2 288"  "faces" "6" }
    "2" { "center" "-1584 -10560 -832"  "size" "592 2 256"  "faces" "6" }
    "3" { "center" "-1536 -3528 -864"  "size" "736 145 1"  "faces" "6" }
  }
}
```
